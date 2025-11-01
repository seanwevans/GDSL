# GDSL Formal Verifier Specification

**Version:** 1.0 (v1 single-queue, single-device)  
**Status:** Implementation-ready  
**Last updated:** 2025-10-31

---

## 0. Conformance & Determinism

### Conformance Levels

The verifier implements three tiers of checking:

| Level | Name | Checks | Use Case |
|-------|------|--------|----------|
| 0 | Syntax validity | Opcode recognition, operand sizes, format | Fast compilation check |
| 1 | Phase correctness | Phase transitions, fence balance | Unit testing, runtime safety |
| 2 | Domain coherence | Barriers, domain transitions, snapshot safety | CI validation, formal proof |

**API:** `gdsl_verify(stream, level)` exposes this as a parameter. Level 2 is the default (full verification).

### Determinism

**Statement:** The verifier is **purely functional**. Given identical binary input and conformance level, it must produce identical diagnostics independent of:
- Hardware platform or OS
- Driver version or GPU vendor
- Runtime state or thread scheduling
- Call history or global state

**Implementation consequence:** The verifier maintains no mutable global state. All state lives in `gdsl_state_t` (allocated per invocation). Random seeding, if used, must be deterministic.

### Opcode Metadata

All opcodes have fixed size in v1. Instruction length is obtained from `gdsl_opcode_table[opcode].size`. This table is statically initialized and must be consistent across all verifier invocations.

---

## 1. Abstract Syntax

### Opcode Universe

```
op ∈ {
  /* Memory management */
  ALLOC_HEAP, FREE_HEAP, ALLOC_BUFFER, FREE_BUFFER,
  ALLOC_IMAGE, FREE_IMAGE, UPLOAD, DOWNLOAD,
  COPY_BUFFER, COPY_IMAGE, CLEAR,
  
  /* Pipeline & shaders */
  PIPE_CREATE, PIPE_DESTROY, PIPE_BIND, SET_VIEWPORT,
  SET_BLENDSTATE, SET_DEPTHSTATE, PUSH_CONSTANTS,
  SET_DESCRIPTOR, BIND_BUFFER, BIND_IMAGE, BIND_SAMPLER,
  
  /* Dispatch & rendering */
  DRAW, DRAW_INDEXED, DISPATCH, INDIRECT,
  BEGIN_PASS, END_PASS,
  
  /* Synchronization */
  BARRIER, EVENT_CREATE, EVENT_SET, EVENT_WAIT,
  FENCE_CREATE, FENCE_WAIT, SEMAPHORE_SIGNAL, SEMAPHORE_WAIT,
  TIMESTAMP, BARRIER_GLOBAL,
  
  /* Snapshot & continuation */
  YIELD, SNAPSHOT_BEGIN, SNAPSHOT_END, RESUME,
  CHECKPOINT, ASSERT_IDLE, BARRIER_TO_HOST,
  
  /* Metadata & debug */
  META, LABEL, MARKER_PUSH, MARKER_POP, LOG,
  QUERY_BEGIN, QUERY_END, READ_QUERY,
  
  /* Control flow */
  IF_EQ, IF_NE, IF_GT, IF_LT, ELSE, ENDIF,
  LOOP, ENDLOOP, CALL, RET, INCLUDE,
  
  /* Utilities */
  CONST_I32, CONST_F32, ADD, SUB, MUL, DIV,
  RAND_SEED, RAND_NEXT,
  
  /* System */
  SET_DEVICE, SET_QUEUE, SLEEP_MS, NOP,
  
  /* Terminators */
  BEGIN_STREAM, END_STREAM, SUBMIT, END_PROGRAM
}
```

### Opcode Extension Space

**Reserved:** Opcodes `0xC0–0xFF` are reserved for:
- Vendor-specific extensions (Metal, HIP, etc.)
- Private back-end instructions
- Future standardized extensions

**Verifier behavior:**
- Unknown opcodes (no entry in `gdsl_opcode_table`) are treated as **errors** by default.
- If `GDSL_FLAG_IGNORE_UNKNOWN` is set during compilation, unknown opcodes are treated as no-ops (phase and state unchanged).
- This allows forward compatibility: older verifiers can gracefully skip future opcodes.

### Instruction Format

```
instr ::= op [operands] [flags]

where:
  operands ∈ { uint32_t, uint64_t, label_id, fence_id, resource_id, ... }
  flags ∈ { auto_inferred, debug_only, ... }
```

### Stream

```
stream = [instr₀, instr₁, ..., instr_n]
```

A stream is a linearly-ordered sequence of instructions.

---

## 2. Abstract Machine (Γ)

### Invariants

The verifier maintains these invariants on `Γ` throughout execution:

| ID | Invariant | Description |
|:---|:----------|:------------|
| (I1) | `Γ.phase ∈ {Build, Record, Submitted, Idle}` | Phase is always one of four states |
| (I2) | `Γ.fd` is finite and unique | Each fence ID appears at most once per SUBMIT; no reuse |
| (I3) | Allocated resources are well-typed | `∀r allocated ⟹ domain ∈ {Device, Host, Coherent}` |
| (I4) | Pending transitions not double-booked | `∀r : pending_transition ≠ ∅ ⟹ (no second BARRIER before FENCE_WAIT)` |
| (I5) | Linear resource lifetime | Each resource ID allocated exactly once; freed exactly once; no reuse |

These invariants are cited in the proof of Snapshot Safety.

### Resource Lineage

Each resource ID has a **single linear lifetime**:

```
state: unallocated → (ALLOC_*) → allocated → (FREE_*) → deallocated
```

The verifier forbids:
- Allocation of a resource already allocated in the same stream
- Freeing a resource that was never allocated
- Reuse of a resource ID after `FREE_*` (in v1; v2 may pool IDs)

This is enforced by tracking `Γ.resources[r].allocated` as a boolean and consulting it on every access.

```
Γ = {
  phase ∈ {Build, Record, Submitted, Idle},
  
  fd : Multiset(FenceId),    // outstanding fences
  
  resources : ResourceId → {
    domain ∈ {Device, Host, Coherent},
    pending_transition : (domain → domain) | ∅,
    allocated : bool,
    persist_flag : bool,
    heap_id : HeapId | ∅
  },
  
  labels : LabelId → InstrIdx,  // mapping for CHECKPOINT references
  
  checkpoint_set : Set({
    label_id : LabelId,
    heap_merkle_root : Hash,
    pipeline_table_merkle_root : Hash,
    stream_ptr : InstrIdx
  })
}
```

### Domain Semantics

| Domain | Readable by | Writable by | Cached | Use |
|--------|------------|-----------|--------|-----|
| Device | GPU only | GPU only | GPU-local | Compute, render targets |
| Host | CPU only | CPU only | CPU cache | Staging, readback |
| Coherent | CPU & GPU | CPU & GPU | (driver-managed) | Persistent state |

### Phase Transitions (Legal Path)

```
    [Idle]
      ↓ BEGIN_STREAM
    [Record]
      ↓ SUBMIT
    [Submitted]
      ↓ FENCE_WAIT
    [Idle]
```

**All snapshot operations only valid in Idle.**  
**All command recording only valid in Record.**

### Pending Transition Model

When `BARRIER(r, src_domain, dst_domain)` is issued (in Record phase):
- The transition is **recorded** but not yet **applied**.
- `Γ.resources[r].pending_transition ← (src_domain → dst_domain)`

When `FENCE_WAIT(f)` is issued (in Submitted phase):
- All pending transitions are **committed**.
- For each `r` with `pending_transition ≠ ∅`:
  ```
  Γ.resources[r].domain ← pending_transition.dst
  Γ.resources[r].pending_transition ← ∅
  ```

This ensures the verifier is **side-effect-free** on the abstract machine—no state changes until fences are witnessed.

---

## 3. Judgment Rules (Γ ⊢ instr : Γ')

### Core Rules

#### Rule 1: BEGIN_STREAM

```
         Γ.phase ∈ {Idle, Build}
────────────────────────────────────────
Γ ⊢ BEGIN_STREAM : Γ[phase := Record]
```

**Effect:** Transition to Record phase, allowing command recording.

---

#### Rule 2: END_STREAM

```
         Γ.phase = Record
────────────────────────────────────────
Γ ⊢ END_STREAM : Γ[phase := Record]
```

**Effect:** Validate the stream buffer; no phase change. Used for nested stream validation.

---

#### Rule 3: SUBMIT

```
         Γ.phase = Record
         f_fresh ∉ Γ.fd
────────────────────────────────────────
Γ ⊢ SUBMIT : Γ[phase := Submitted, fd := Γ.fd ∪ {f_fresh}]
```

**Effect:** Transition to Submitted phase, introduce fresh fence token.

---

#### Rule 4: FENCE_WAIT

```
         Γ.phase = Submitted
         f ∈ Γ.fd
         Γ' = apply_pending_transitions(Γ, {r | pending_transition[r] ≠ ∅})
────────────────────────────────────────
Γ ⊢ FENCE_WAIT(f) : Γ'[phase := Idle, fd := Γ'.fd \ {f}]
```

**Effect:** 
- Transition to Idle phase
- Commit all pending domain transitions
- Consume fence token `f`

**Helper:** `apply_pending_transitions(Γ, R)`
```
for each r ∈ R:
  Γ.resources[r].domain ← Γ.resources[r].pending_transition.dst
  Γ.resources[r].pending_transition ← ∅
return Γ
```

---

#### Rule 5: BARRIER

```
         Γ.phase = Record
         r ∈ Γ.resources
         Γ.resources[r].domain = src_domain
         Γ.resources[r].pending_transition = ∅
────────────────────────────────────────────────────────────
Γ ⊢ BARRIER(r, src_domain, dst_domain) : 
    Γ[resources[r].pending_transition := (src_domain → dst_domain)]
```

**Effect:** Record a deferred domain transition. The actual transition happens at `FENCE_WAIT`.

**Precondition on pending_transition:** If `Γ.resources[r].pending_transition ≠ ∅`, the verifier rejects with:
```
Error: "BARRIER(#r): pending transition already queued
        (cannot layer barriers; issue FENCE_WAIT before second BARRIER)"
```

This prevents stacking unsatisfied barriers on the same resource.

**Note:** If `src_domain ≠ current domain`, verification fails with diagnostic.

---

#### Rule 5a: ASSERT_IDLE

```
         Γ.phase = Idle
────────────────────────────────────────
Γ ⊢ ASSERT_IDLE : Γ
```

**Effect:** No state change. Acts as a verifier-level assertion that current phase is Idle.

**Purpose:** Used by barrier-inference pass (v2) to document that fences have been waited. Verifier treats it as a no-op but can report it in diagnostics for clarity.

**Runtime:** Host-side assertion; if phase is not Idle at execution, runtime issues a warning or error.

---

#### Rule 5b: END_PROGRAM

```
         Γ.phase = Idle
         Γ.fd = ∅
────────────────────────────────────────
Γ ⊢ END_PROGRAM : Γ
```

**Effect:** No state change. Verifier checks that program termination is only reached in Idle with no outstanding fences.

**Error if violated:**
- `phase ≠ Idle` → "Program ended in non-Idle phase (Submitted)"
- `fd ≠ ∅` → "Program ended with outstanding fences: {f0, f1, ...}"

---

#### Rule 6: SNAPSHOT_BEGIN

```
         Γ.phase = Idle
         ∀r ∈ resources : r.persist_flag ⟹ r.domain = Host
         ∀r ∈ resources : r.persist_flag ⟹ r.pending_transition = ∅
────────────────────────────────────────────────────────────
Γ ⊢ SNAPSHOT_BEGIN(label) : Γ
```

**Effect:** No state change (Idle → Idle). Preconditions ensure snapshot safety.

**Error if violated:**
- `phase ≠ Idle` → "Snapshot in non-Idle phase"
- `∃r with persist_flag ∧ domain ≠ Host` → "Persistent resource still in Device domain"
- `∃r with pending_transition ≠ ∅` → "Pending barrier not committed"

---

### Memory Management Rules

#### Rule 7: ALLOC_BUFFER

```
         Γ.phase ∈ {Idle, Record}
         buf_id ∉ Γ.resources
         heap_id ∈ Γ.resources (if specified)
────────────────────────────────────────────────────────────
Γ ⊢ ALLOC_BUFFER(buf_id, heap_id, size, usage, flags) :
    Γ[resources[buf_id] := {
      domain := Device,
      allocated := true,
      persist_flag := (flags & PERSIST) ≠ 0,
      heap_id := heap_id
    }]
```

**Effect:** Register buffer in resource table; initial domain is Device.

**Note:** `PERSIST` flag determines whether buffer must be Host-readable at snapshots.

---

#### Rule 8: FREE_BUFFER

```
         Γ.phase ∈ {Idle, Record}
         buf_id ∈ Γ.resources
         Γ.resources[buf_id].allocated = true
────────────────────────────────────────────────────────────
Γ ⊢ FREE_BUFFER(buf_id) :
    Γ[resources[buf_id].allocated := false]
```

**Effect:** Mark resource as deallocated. Verifier does not reuse IDs.

---

### Command Recording Rules

#### Rule 9: DRAW

```
         Γ.phase = Record
────────────────────────────────────────
Γ ⊢ DRAW(first, count) : Γ
```

**Effect:** No state change. Treated as a command that must be recorded before SUBMIT.

---

### Checkpoint & Diff Rules

#### Rule 10: CHECKPOINT

```
         Γ.phase = Idle
         label_id ∉ Γ.labels
────────────────────────────────────────
Γ ⊢ CHECKPOINT(label_id, heap_merkle, pipe_merkle, stream_ptr) :
    Γ[checkpoint_set := Γ.checkpoint_set ∪ {
      label_id, heap_merkle, pipe_merkle, stream_ptr
    }]
```

**Effect:** Record metadata for later diff operations.

---

### Phase-Validity Check (Auxiliary Rule)

For any opcode `op`, define `valid_ops(phase)` as the set of legal operations:

```
valid_ops(Idle) = {
  ALLOC_HEAP, ALLOC_BUFFER, ALLOC_IMAGE,
  BEGIN_STREAM, SET_DEVICE, SET_QUEUE,
  SNAPSHOT_*, CHECKPOINT, DIFF, PATCH,
  YIELD, SLEEP_MS, NOP
}

valid_ops(Record) = {
  END_STREAM, FREE_BUFFER, FREE_IMAGE,
  UPLOAD, DOWNLOAD, COPY_BUFFER, COPY_IMAGE, CLEAR,
  PIPE_CREATE, PIPE_BIND, SET_VIEWPORT,
  PUSH_CONSTANTS, SET_DESCRIPTOR, BIND_BUFFER,
  DRAW, DRAW_INDEXED, DISPATCH, INDIRECT,
  BEGIN_PASS, END_PASS, BARRIER, EVENT_SET, EVENT_WAIT,
  LABEL, MARKER_PUSH, MARKER_POP, LOG,
  QUERY_BEGIN, QUERY_END, CONST_*, ADD, SUB, MUL,
  IF_*, LOOP, CALL, INCLUDE, NOP, SUBMIT
}

valid_ops(Submitted) = { } (empty—CPU must wait)

valid_ops(Build) = { } (transient, not exposed)
```

#### Rule 13: Unknown but Phase-Valid Opcode (Meta-Rule)

For all opcodes not explicitly ruled above:

```
         op ∉ {explicit rules}
         op ∈ valid_ops(Γ.phase)
────────────────────────────────────
Γ ⊢ op(...) : Γ  (state unchanged)
```

**Effect:** Opcodes not explicitly ruled but whose opcode ID matches `valid_ops(phase)` are treated as no-ops (no state mutation).

**Purpose:** Allows forward compatibility with unknown opcodes (e.g., vendor extensions) and graceful degradation when `GDSL_FLAG_IGNORE_UNKNOWN` is set.

---

## 4. Verification Algorithm (Single-Pass Pseudo-Code)

### Fast-Path: Pre-Verified Binaries

If the binary includes a `gdsl_header` with `verified_hash` field:

```
if (header.verified_hash != 0 && !(flags & GDSL_VERIFY_FORCE)) {
  // Fast path: skip verification
  out->is_valid = 1;
  out->error_count = 0;
  return 0;
}
// else: fall through to full verification
```

This avoids re-verification of binaries previously checked. Use `--force` flag to bypass cache.

### Report Structure

```c
typedef struct {
  int is_valid;                          // 1 if verification passed, 0 otherwise
  size_t error_count;                    // number of diagnostics
  const char **errors;                   // error messages (array of strings)
  
  uint64_t phase_mask;                   // bitmask of phases reached
  uint32_t conformance_level;            // level at which verification was run
  uint64_t flags;                        // GDSL_VERIFY_* flags
  
  struct {
    size_t instr_count;
    size_t resource_count;
    size_t fence_count;
  } telemetry;
} gdsl_verify_report_t;
```

### Diagnostic Severity Convention

Each diagnostic includes a severity level for future extensibility:

```c
typedef enum {
  GDSL_DIAG_ERROR,      // verification fails
  GDSL_DIAG_WARNING,    // verification passes but potential issue
  GDSL_DIAG_INFO        // informational (perf hints, debug)
} gdsl_diag_severity_t;
```

Currently (v1), only ERROR diagnostics exist. Warnings and info are reserved for future checks (e.g., performance hints).

### Implementation
typedef struct {
  uint32_t phase;      // Build=0, Record=1, Submitted=2, Idle=3
  multiset_t fences;   // outstanding fence IDs
  map_t resources;     // resource_id → resource_state
  set_t labels;
  set_t checkpoints;
  
  // error tracking
  vector_t errors;     // diagnostic messages
  int is_valid;
} gdsl_state_t;

typedef struct {
  uint32_t domain;
  uint32_t pending_transition;  // (src << 16) | dst, or 0
  bool allocated;
  bool persist_flag;
  uint32_t heap_id;
} resource_state_t;

int gdsl_verify(const void* bin, size_t len, gdsl_verify_report_t* out) {
  gdsl_state_t Γ = {
    .phase = GDSL_PHASE_IDLE,
    .fences = multiset_create(),
    .resources = map_create(),
    .labels = set_create(),
    .checkpoints = set_create(),
    .errors = vector_create(),
    .is_valid = 1
  };
  
  const uint8_t* stream = (const uint8_t*)bin;
  size_t instr_count = 0;
  
  for (size_t i = 0; i < len; /* advance by instr size */) {
    uint8_t opcode = stream[i];
    size_t instr_size = 0;
    
    // Dispatch on opcode
    switch (opcode) {
      
      case GDSL_BEGIN_STREAM: {
        if (Γ.phase != GDSL_PHASE_IDLE && 
            Γ.phase != GDSL_PHASE_BUILD) {
          error_push(&Γ.errors, 
            "BEGIN_STREAM: phase must be Idle or Build, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        Γ.phase = GDSL_PHASE_RECORD;
        instr_size = 1;
        break;
      }
      
      case GDSL_END_STREAM: {
        if (Γ.phase != GDSL_PHASE_RECORD) {
          error_push(&Γ.errors,
            "END_STREAM: phase must be Record, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        // No phase change
        instr_size = 1;
        break;
      }
      
      case GDSL_SUBMIT: {
        if (Γ.phase != GDSL_PHASE_RECORD) {
          error_push(&Γ.errors,
            "SUBMIT: phase must be Record, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        uint32_t fence_id = (uint32_t)instr_count;  // generate fresh ID
        multiset_add(&Γ.fences, fence_id);
        Γ.phase = GDSL_PHASE_SUBMITTED;
        instr_size = 1;
        break;
      }
      
      case GDSL_FENCE_WAIT: {
        if (Γ.phase != GDSL_PHASE_SUBMITTED) {
          error_push(&Γ.errors,
            "FENCE_WAIT: phase must be Submitted, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        
        uint32_t fence_id = read_u32_le(&stream[i + 1]);
        if (!multiset_contains(&Γ.fences, fence_id)) {
          error_push(&Γ.errors,
            "FENCE_WAIT: fence #%u not outstanding", fence_id);
          Γ.is_valid = 0;
          break;
        }
        
        // Apply pending transitions
        map_iter_t iter = map_iter_init(&Γ.resources);
        while (map_iter_next(&iter)) {
          resource_state_t* rs = (resource_state_t*)iter.value;
          if (rs->pending_transition != 0) {
            uint32_t dst = rs->pending_transition & 0xFFFF;
            rs->domain = dst;
            rs->pending_transition = 0;
          }
        }
        
        multiset_remove(&Γ.fences, fence_id);
        Γ.phase = GDSL_PHASE_IDLE;
        instr_size = 5;
        break;
      }
      
      case GDSL_BARRIER: {
        if (Γ.phase != GDSL_PHASE_RECORD) {
          error_push(&Γ.errors,
            "BARRIER: phase must be Record, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        
        uint32_t res_id = read_u32_le(&stream[i + 1]);
        uint32_t src_domain = read_u32_le(&stream[i + 5]);
        uint32_t dst_domain = read_u32_le(&stream[i + 9]);
        
        resource_state_t* rs = (resource_state_t*)map_get(&Γ.resources, res_id);
        if (!rs) {
          error_push(&Γ.errors,
            "BARRIER: resource #%u not allocated", res_id);
          Γ.is_valid = 0;
          break;
        }
        
        if (rs->domain != src_domain) {
          error_push(&Γ.errors,
            "BARRIER(#%u): expected domain %s, got %s",
            res_id, domain_name(src_domain), domain_name(rs->domain));
          Γ.is_valid = 0;
          break;
        }
        
        rs->pending_transition = (src_domain << 16) | dst_domain;
        instr_size = 13;
        break;
      }
      
      case GDSL_SNAPSHOT_BEGIN: {
        if (Γ.phase != GDSL_PHASE_IDLE) {
          error_push(&Γ.errors,
            "SNAPSHOT_BEGIN: phase must be Idle, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        
        // Check: all PERSIST resources in Host domain with no pending transitions
        map_iter_t iter = map_iter_init(&Γ.resources);
        while (map_iter_next(&iter)) {
          uint32_t res_id = (uint32_t)(uintptr_t)iter.key;
          resource_state_t* rs = (resource_state_t*)iter.value;
          
          if (rs->persist_flag) {
            if (rs->domain != GDSL_DOMAIN_HOST) {
              error_push(&Γ.errors,
                "SNAPSHOT_BEGIN: persistent resource #%u in %s domain "
                "(must be Host); insert BARRIER(%u, %s→Host) before SUBMIT",
                res_id, domain_name(rs->domain), res_id,
                domain_name(rs->domain));
              Γ.is_valid = 0;
            }
            
            if (rs->pending_transition != 0) {
              error_push(&Γ.errors,
                "SNAPSHOT_BEGIN: resource #%u has pending transition "
                "(not committed); add FENCE_WAIT before snapshot",
                res_id);
              Γ.is_valid = 0;
            }
          }
        }
        
        instr_size = 1;
        break;
      }
      
      case GDSL_ALLOC_BUFFER: {
        if (Γ.phase != GDSL_PHASE_IDLE && 
            Γ.phase != GDSL_PHASE_RECORD) {
          error_push(&Γ.errors,
            "ALLOC_BUFFER: phase must be Idle or Record, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        
        uint32_t buf_id = read_u32_le(&stream[i + 1]);
        uint32_t heap_id = read_u32_le(&stream[i + 5]);
        uint64_t size = read_u64_le(&stream[i + 9]);
        uint32_t usage = read_u32_le(&stream[i + 17]);
        uint32_t flags = read_u32_le(&stream[i + 21]);
        
        if (map_contains(&Γ.resources, (void*)(uintptr_t)buf_id)) {
          error_push(&Γ.errors,
            "ALLOC_BUFFER: resource #%u already allocated", buf_id);
          Γ.is_valid = 0;
          break;
        }
        
        resource_state_t rs = {
          .domain = GDSL_DOMAIN_DEVICE,
          .pending_transition = 0,
          .allocated = true,
          .persist_flag = (flags & GDSL_FLAG_PERSIST) != 0,
          .heap_id = heap_id
        };
        map_put(&Γ.resources, (void*)(uintptr_t)buf_id, &rs, sizeof(rs));
        
        instr_size = 25;
        break;
      }
      
      case GDSL_CHECKPOINT: {
        if (Γ.phase != GDSL_PHASE_IDLE) {
          error_push(&Γ.errors,
            "CHECKPOINT: phase must be Idle, got %s",
            phase_name(Γ.phase));
          Γ.is_valid = 0;
          break;
        }
        
        uint32_t label_id = read_u32_le(&stream[i + 1]);
        if (set_contains(&Γ.labels, (void*)(uintptr_t)label_id)) {
          error_push(&Γ.errors,
            "CHECKPOINT: label #%u already exists", label_id);
          Γ.is_valid = 0;
          break;
        }
        
        set_add(&Γ.labels, (void*)(uintptr_t)label_id);
        instr_size = 33;  // label + 3 hash fields
        break;
      }
      
      default: {
        // Opcodes not explicitly handled are treated as phase-valid no-ops
        if (!is_valid_opcode_in_phase(opcode, Γ.phase)) {
          error_push(&Γ.errors,
            "Opcode 0x%02x invalid in phase %s",
            opcode, phase_name(Γ.phase));
          Γ.is_valid = 0;
        }
        instr_size = opcode_size(opcode);  // from opcode metadata table
        break;
      }
    }
    
    if (!Γ.is_valid && !(flags & GDSL_VERIFY_CONTINUE)) {
      break;  // fail-fast (or continue if CONTINUE flag set)
    }
    
    i += instr_size;
    instr_count++;
  }
  
  // Final checks
  if (Γ.phase != GDSL_PHASE_IDLE) {
    error_push(&Γ.errors,
      "Stream did not reach Idle phase (final: %s); "
      "missing FENCE_WAIT or SUBMIT?",
      phase_name(Γ.phase));
    Γ.is_valid = 0;
  }
  
  if (!multiset_is_empty(&Γ.fences)) {
    error_push(&Γ.errors,
      "Outstanding fences not waited: {%s}",
      multiset_to_string(&Γ.fences));
    Γ.is_valid = 0;
  }
  
  // Populate output report
  out->is_valid = Γ.is_valid;
  out->error_count = vector_len(&Γ.errors);
  out->errors = (const char**)vector_data(&Γ.errors);
  
  return Γ.is_valid ? 0 : -1;
}
```

---

## 5. Safety Theorem & Proof Sketch

### Theorem (Snapshot Safety)

**Statement:**

> If stream `S` passes verification (`gdsl_verify(S) = 0`), then executing `S` to completion and invoking `SNAPSHOT_BEGIN` produces a coherent snapshot in which all `PERSIST`-flagged resources are host-readable.

**Formal:**

```
Theorem (Snapshot_Safety):
  Let S = [instr₀, ..., instr_n] be a stream.
  Let Γ₀ = initial_state().
  
  Assume: ∀i ∈ [0, n]:
    (Γᵢ₋₁ ⊢ instrᵢ : Γᵢ) ∧ verification_accepts(Γᵢ)
  
  Then:
    (1) Γₙ.phase = Idle
    (2) ∀r ∈ resources : r.persist_flag ⟹ Γₙ.resources[r].domain = Host
    (3) ∀r ∈ resources : r.pending_transition = ∅
    (4) Γₙ.fd = ∅
    
  Therefore:
    SNAPSHOT_BEGIN satisfies Rule 6 preconditions
    ⟹ snapshot is safe and coherent.
```

### Proof Sketch

**Step 1: Verification ensures phase-correct execution path.**

By the phase-transition rules, the only path from Idle to Idle is:

```
Idle --BEGIN_STREAM--> Record --SUBMIT--> Submitted --FENCE_WAIT--> Idle
```

Verification enforces this path by checking:
- Only BEGIN_STREAM can enter Record (Rule 1)
- Only SUBMIT can enter Submitted (Rule 3)
- Only FENCE_WAIT can re-enter Idle (Rule 4)

**Claim:** If `S` verifies and runs to completion, then `Γ_final.phase = Idle` and `Γ_final.fd = ∅`.

*Proof:* If any instruction is not matched by exactly one rule, verification rejects it (opcode out of phase). If SUBMIT introduces fence `f`, and `FENCE_WAIT(f)` consumes it, fences are balanced. If SUBMIT is reached without matching FENCE_WAIT, final phase is Submitted (fails Idle check). QED.

**Step 2: BARRIER + FENCE_WAIT enforce domain transitions.**

For each `PERSIST` resource `r`:
- If `r.domain ≠ Host` at any checkpoint, verification requires a `BARRIER(r, *, Host)` in Record phase (Rule 5).
- Rule 5 records `pending_transition ← (Device → Host)`.
- Rule 4 commits pending transitions only on FENCE_WAIT, setting `r.domain := Host`.
- Verification checks at SNAPSHOT_BEGIN (Rule 6) that `r.domain = Host`.

**Claim:** If `S` verifies and reaches SNAPSHOT_BEGIN, all PERSIST resources have `domain = Host`.

*Proof:* By induction on instruction sequence. Initially (Build), resources don't exist. On ALLOC_BUFFER (Rule 7), `domain := Device`. On BARRIER (Record phase, Rule 5), a transition is *recorded*. On FENCE_WAIT (Rule 4), transitions are *applied*. At SNAPSHOT_BEGIN (Rule 6), verification checks postcondition. QED.

**Step 3: No pending transitions remain.**

Rule 4 (FENCE_WAIT) clears all `pending_transition` fields.

**Claim:** If verification reaches SNAPSHOT_BEGIN, no resource has a non-empty pending transition.

*Proof:* A pending transition can only exist after BARRIER (Rule 5, Record phase). It is cleared by FENCE_WAIT (Rule 4). SNAPSHOT_BEGIN is only reachable in Idle (Rule 6 precondition), which follows FENCE_WAIT. QED.

**Step 4: Coherence conclusion.**

By steps 1–3:
- `Γ_final.phase = Idle`
- `∀r.persist_flag ⟹ domain = Host`
- `no pending_transition`
- `fd = ∅`

Therefore, Rule 6 preconditions hold, and the snapshot is coherent. The host CPU can safely read PERSIST resources without stale cache data. QED.

---

### Lemma (L1: Fence Completeness)

**Statement:** Every fence token introduced by SUBMIT is consumed exactly once by FENCE_WAIT.

```
Lemma (Fence_Completeness):
  ∀f ∈ Γ.fd : ∃! FENCE_WAIT(f) in stream S
```

**Proof:** By construction of the verifier's multiset tracking:
- Each SUBMIT adds a fresh `f` to `Γ.fd`.
- Each FENCE_WAIT removes exactly one fence.
- Verifier rejects if `FENCE_WAIT(f)` where `f ∉ Γ.fd`.
- Verifier rejects if program terminates with non-empty `Γ.fd`.

Therefore, every fence is both introduced and consumed, and uniquely so. QED.

**Use:** Lemma (L1) ensures that GPU work is always concluded before phase returns to Idle.

---

### Corollary (Reproducibility)

**Statement:** Given two independently verified streams `S₁` and `S₂` that produce identical checkpoint sets (same label IDs, identical heap merkle roots), their execution traces are observationally equivalent with respect to resource domains and fences.

```
Corollary (Reproducibility):
  Let S₁, S₂ both verify (level 2).
  Assume: checkpoints(S₁) = checkpoints(S₂)
          ∧ ∀c ∈ checkpoints : heap_merkle(S₁, c) = heap_merkle(S₂, c)
  
  Then:
    trace_domains(S₁) = trace_domains(S₂)
    ∧ trace_fences(S₁) ≡ trace_fences(S₂)
```

**Proof sketch:** By induction on instruction count:
- Checkpoint sets are identical ⟹ same stream structure.
- Verification enforces deterministic domain transitions (BARRIER + FENCE_WAIT).
- Fence tokens are generated deterministically (by SUBMIT order).
- Therefore, both streams evolve identically through state space.

**Use:** Enables deterministic replay and diff semantics. If two execution traces produce the same checkpoint, they are indistinguishable to the host.

---

## 6. Counterexamples with Diagnostics

### Bad Program 1: Missing BARRIER Before Snapshot

```
begin_stream
  alloc_buffer buf0, persist=1  // domain := Device
  draw ...
end_stream
submit                           // phase := Submitted
fence_wait f0                    // phase := Idle
snapshot_begin "checkpoint"      // ← FAILS HERE
```

**Diagnostic:**

```
VERIFICATION FAILED at instruction 7 (snapshot_begin "checkpoint")
  Line: snapshot_begin "checkpoint"
  
Error:
  SNAPSHOT_BEGIN: persistent resource #0 (buf0) in Device domain
  (must be Host); insert BARRIER(buf0, Device→Host) before SUBMIT
  
  Corrected sequence:
    barrier buf0 Device Host     // record in Record phase
    submit                       // fence introduced
    fence_wait f0                // pending transition applied
    snapshot_begin "checkpoint"  // now safe
```

---

### Bad Program 2: SNAPSHOT During Submitted Phase

```
begin_stream
  alloc_buffer buf0
  draw ...
end_stream
submit                    // phase := Submitted, fence f0
snapshot_begin "bad"      // ← FAILS (wrong phase)
fence_wait f0
```

**Diagnostic:**

```
VERIFICATION FAILED at instruction 6 (snapshot_begin "bad")
  Line: snapshot_begin "bad"
  
Error:
  SNAPSHOT_BEGIN: phase must be Idle, got Submitted
  
  Reason: GPU commands are still in flight.
  
  Fix: Add FENCE_WAIT before snapshot:
    submit
    fence_wait f0              // wait for GPU work
    snapshot_begin "bad"       // now phase is Idle
```

---

### Bad Program 3: Outstanding Fence at Stream End

```
begin_stream
  alloc_buffer buf0
  draw ...
end_stream
submit                    // phase := Submitted, fence f0 introduced
// ← No FENCE_WAIT; end of stream reached
```

**Diagnostic:**

```
VERIFICATION FAILED: stream did not complete successfully

Error:
  Stream did not reach Idle phase (final: Submitted);
  missing FENCE_WAIT?
  
Error:
  Outstanding fences not waited: {f0}
  
Fix: Add fence_wait before stream end:
  submit
  fence_wait f0
```

---

### Bad Program 4: BARRIER in Wrong Phase

```
begin_stream
  alloc_buffer buf0
end_stream
submit
barrier buf0 Device Host   // ← FAILS (not in Record phase)
```

**Diagnostic:**

```
VERIFICATION FAILED at instruction 5 (barrier buf0 Device Host)
  Line: barrier buf0 Device Host
  
Error:
  BARRIER: phase must be Record, got Submitted
  
  Reason: Barriers are command-buffer opcodes and must be recorded
          before SUBMIT, not after.
  
Fix: Move barrier before end_stream:
  begin_stream
    alloc_buffer buf0
    barrier buf0 Device Host    // in Record phase
  end_stream
  submit
  fence_wait f0
```

---

### Bad Program 5: Mismatched Domain Barrier

```
begin_stream
  alloc_buffer buf0           // domain := Device
  barrier buf0 Host Device    // ← FAILS (src ≠ current domain)
end_stream
```

**Diagnostic:**

```
VERIFICATION FAILED at instruction 3 (barrier buf0 Host Device)
  Line: barrier buf0 Host Device
  
Error:
  BARRIER(#0): expected domain Host, got Device
  
  Reason: Barrier operand (src_domain) must match resource's
          current domain.
  
Fix: Use correct source domain:
  barrier buf0 Device Host    // Device → Host (correct)
```

---

### Good Program (Verification Success)

```
begin_stream
  alloc_buffer buf0 persist=1     // domain := Device, persist_flag := true
  barrier buf0 Device Host        // record deferred transition
end_stream
submit                            // fence f0 generated; phase := Submitted
fence_wait f0                     // apply pending transitions; Γ.resources[buf0].domain := Host
                                  // phase := Idle
snapshot_begin "frame_complete"   // phase check: Idle ✓
                                  // PERSIST check: buf0 in Host domain ✓
                                  // no pending transitions ✓
snapshot_end
```

**Diagnostic:**

```
VERIFICATION SUCCESS
  Instructions: 8
  Resources allocated: 1
  Fences balanced: 1 submit ↔ 1 wait
  Final phase: Idle
  
Checkpoint "frame_complete" verified:
  - Persistent resource #0 (buf0) in Host domain
  - All barriers committed
  - Safe for snapshot and host read
```

---

## 7. Concurrency Model Notes

### Formal Assumptions (A1–A4)

The v1 verifier and theorem proofs assume:

| ID | Assumption | Purpose |
|:---|:-----------|:--------|
| A1 | One stream per device | Simplifies `Γ` structure; no need for per-stream tracking |
| A2 | One implicit queue per device | Defines phase progression serially |
| A3 | Host serializes SUBMIT/FENCE_WAIT | Ensures no interleaving of queue operations |
| A4 | No implicit driver work | All state transitions must be explicit in the stream |

These assumptions can be cited in theorem statements: "Under A1–A4, Snapshot Safety holds."

### v1 Model (Current)

- **Single stream:** One `gdsl_exec()` per device.
- **Single queue:** All commands submit to one implicit queue.
- **Single device:** One GPU/accelerator.
- **Serialized submission:** `SUBMIT` → `FENCE_WAIT` before next stream.
- **Runtime enforcement:** `vkDeviceWaitIdle()` around every snapshot.

**Assumption:** No concurrent calls to `gdsl_exec()` from multiple host threads.

**Violation:** Undefined behavior if multiple threads call `gdsl_exec()` simultaneously.

**Workaround (v1):** Wrap `gdsl_exec()` calls in a host-level mutex.

---

### v2 Roadmap (Multi-Queue)

Extend `Γ` to track per-queue state:

```
Γ = {
  queues : QueueId → {
    phase ∈ {Build, Record, Submitted, Idle},
    fd : Multiset(FenceId),
    resources : ResourceId → resource_state_t
  },
  
  resource_owner : ResourceId → Set(QueueId)  // which queues access resource
}
```

Judgment rules updated:

```
SUBMIT(queue_q):
  Γ.queues[q].phase = Record
  ⟹ Γ.queues[q].phase := Submitted
     (other queues unchanged)

FENCE_WAIT(queue_q, fence_f):
  Γ.queues[q].phase = Submitted ∧ f ∈ Γ.queues[q].fd
  ⟹ Γ.queues[q].phase := Idle, apply pending transitions for q
     (other queues unchanged)

SNAPSHOT_BEGIN:
  ∀q ∈ active_queues : Γ.queues[q].phase = Idle
  ∀q, r : (q ∈ resource_owner[r] ∧ r.persist_flag)
          ⟹ r.domain = Host
```

**Status:** Design complete; implementation deferred to v2.

---

## 8. Diff Preconditions & Rebase Semantics

### Snapshot Metadata Structure

```c
typedef struct {
  uint64_t stream_ptr;                    // instruction index
  uint64_t heap_merkle_root;              // BLAKE3(heap pages)
  uint64_t pipeline_table_merkle_root;    // BLAKE3(pipeline blobs)
  uint64_t resource_table_merkle_root;    // BLAKE3(resource layout)
  uint64_t timestamp_ns;                  // wall-clock time
  char label[256];                        // user label (e.g., "frame_120")
} gdsl_snapshot_metadata_t;
```

### Diff Validity Criterion

**Definition (Diffability):**

```
Diff(S_a, S_b) is well-defined iff:
  S_a.pipeline_table_merkle_root = S_b.pipeline_table_merkle_root
  ∧
  S_a.resource_table_merkle_root = S_b.resource_table_merkle_root
```

**Intuition:** Both snapshots must have identical shader binaries and resource layouts to compare VRAM contents meaningfully.

### Lemma (Diff Well-Definedness)

**Statement:** If pipeline and resource tables are identical, then diffs are deterministic.

```
Lemma (Diff_Well_Definedness):
  Assume: pipeline_table(S_a) = pipeline_table(S_b)
          ∧ resource_table(S_a) = resource_table(S_b)
  
  Then:
    Diff(S_a, S_b) produces unique ΔHeaps and ΔMeta
```

**Proof:** Hash equality implies identical serialization order and alignment. Delta is defined pagewise by merkle tree. Uniqueness of page IDs guarantees determinism. QED.

### Granularity

**Page size (v1):** 256 KiB (0x40000 bytes)

This is the minimum unit of change tracked in heap diffs. Smaller changes are coalesced into page boundaries. Implementers may adjust this; changing page size requires reserialization of all snapshots.

**Case 1: Additive pipeline changes**

If new pipelines are added but old ones unchanged:

```
rebase_diff(S_a, S_b_old, S_b_new):
  if pipelines_compatible(S_b_old.pipelines, S_b_new.pipelines):
    // Safe: only additions, no breaking changes
    return compute_diff(S_a, S_b_new)
  else:
    return Error("Incompatible pipeline ABI")
```

**Case 2: ABI-breaking pipeline changes**

If kernel signatures or layouts change:

```
rebase_diff(S_a, S_b_old, S_b_new):
  if pipelines_incompatible_abi(S_b_old.pipelines, S_b_new.pipelines):
    // Cannot safely rebase; diffs may be misaligned
    warning("Pipeline ABI changed; diff may be stale")
    // Option A: fail (safe but conservative)
    // Option B: recompute from scratch (expensive)
    // Option C: emit warning + try anyway (user's risk)
    return Error("Pipeline ABI mismatch; re-record checkpoint")
```

### Diff Composition

**Law (Associativity):**

```
Diff(S_a → S_b) ⊕ Diff(S_b → S_c) = Diff(S_a → S_c)

Proof sketch:
  - Each diff is a set of (page_id → bytes) and (stream_ptr_delta)
  - Composition by page-wise union (later-wins)
  - Stream pointer sum: stream_ptr(a→c) = stream_ptr(a→b) + stream_ptr(b→c)
  - No ordering dependence on intermediate snapshots
```

### Diff Header Format

```c
typedef struct {
  uint32_t magic;                      // "GDSL"
  uint32_t version;                    // 1
  uint64_t base_heap_merkle;           // S_a.heap_merkle_root
  uint64_t target_heap_merkle;         // S_b.heap_merkle_root
  uint64_t pipeline_table_merkle;      // both must match
  uint64_t delta_page_count;           // N changed pages
  uint32_t rebase_mode;                // NONE, ADDITIVE, FAILED
  char rebase_warning[512];            // if FAILED
} gdsl_diff_header_t;
```

### Diff Query Functions

```c
// Compute sparse diff between two snapshots
int gdsl_diff(const char* snap_a_path, const char* snap_b_path,
              const char* out_diff_path);

// Apply diff to a base snapshot, producing new snapshot
int gdsl_patch(const char* base_snap_path, 
               const char* diff_path,
               const char* out_snap_path);

// List changed resources/pages between two checkpoints
int gdsl_read_changed_set(const char* snap_a, const char* snap_b,
                          gdsl_changed_set_t* out);
  // Returns { resource_id[], page_id[], ... }
  // Used for visualization (heatmaps, diffs)
```

### Usage Example

```c
// Checkpoint A
gdsl_checkpoint(exec, "A");

// Compute N rays
gdsl_exec_range(exec, 1000, 2000);

// Checkpoint B
gdsl_checkpoint(exec, "B");

// Diff A vs B
gdsl_diff("snap_A.gdsl", "snap_B.gdsl", "diff_AB.gdsl");

// Check if rebase was needed
gdsl_diff_header_t header;
gdsl_read_diff_header("diff_AB.gdsl", &header);
if (header.rebase_mode == GDSL_REBASE_FAILED) {
  fprintf(stderr, "Warning: %s\n", header.rebase_warning);
}

// Apply diff to A to regenerate B (or patch A with B's data)
gdsl_patch("snap_A.gdsl", "diff_AB.gdsl", "snap_B_reproduced.gdsl");
```

---

## 9. Change Control

| Version | Date | Changes |
|:--|:--|:--|
| 1.0 | 2025-10-31 | Initial implementation-ready release: conformance levels, determinism guarantee, invariants (I1–I5), meta-rule for unknown opcodes, fast-path verification, ASSERT_IDLE and END_PROGRAM rules, Fence Completeness lemma (L1), Reproducibility corollary, good program example, formal assumptions (A1–A4), diff well-definedness lemma, page size granularity (256 KiB) |
| 1.1 (draft) | — | Planned: multi-queue Γ, per-queue resource owner tracking, barrier inference pass (auto-insert ASSERT_IDLE/BARRIER_TO_HOST), multi-device sync, advanced residency planner (LRU eviction, predictive hints) |

---

## End of Specification

**Document version:** 1.0  
**Status:** LOCKED for v1 implementation  
**Next steps:** Implement `gdsl_verify()` in C, with full error diagnostic formatting.

**Reviewers:** Approved for coding.
