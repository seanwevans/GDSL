# GDSL Verifier Specification — Visual Overview

---

## Specification Document Structure

```
┌─────────────────────────────────────────────────────────────┐
│         GDSL Formal Verifier Specification v1.0             │
│                  (44 pages, 1,373 lines)                    │
└─────────────────────────────────────────────────────────────┘
                           │
           ┌───────────────┼───────────────┐
           ▼               ▼               ▼
    ┌──────────────┐ ┌─────────────┐ ┌───────────────┐
    │ Foundation   │ │   Theory    │ │  Validation   │
    │ (0–2)        │ │   (3–5)     │ │   (6–8)       │
    └──────────────┘ └─────────────┘ └───────────────┘
           │               │               │
     [Syntax]        [Proofs]         [Tests]
     [Machine]       [Rules]          [Diffs]
     [Domains]       [Theorems]       [Rebasing]
```

### Section Breakdown

| # | Title | Content | Pages | Key Artifact |
|---|-------|---------|-------|--------------|
| 0 | Conformance & Determinism | Verification levels (L0/L1/L2), pure function guarantee | 2 | `gdsl_verify_level_t enum` |
| 1 | Abstract Syntax | Opcodes (100+), extension space (0xC0–0xFF) | 2 | `gdsl_opcode_table[]` |
| 2 | Abstract Machine | State (Γ), invariants (I1–I5), domains | 3 | `gdsl_state_t` struct |
| 3 | Judgment Rules | 13 rules covering all opcodes | 7 | Rule 1–13 + meta-rule |
| 4 | Verification Algorithm | Single-pass pseudo-code (~300 lines C) | 8 | `gdsl_verify()` implementation |
| 5 | Safety Theorem & Proof | Theorem + 4-step proof + 2 lemmas + corollary | 5 | Snapshot Safety proven |
| 6 | Counterexamples | 5 bad programs + 1 good program with diagnostics | 4 | Integration test cases |
| 7 | Concurrency Model | Formal assumptions (A1–A4), v2 roadmap | 3 | `Γ.queues` extension sketch |
| 8 | Diff Preconditions | Merkle-rooted metadata, rebase semantics, composition law | 4 | `gdsl_diff()` API |
| 9 | Change Control | Version tracking, v1.1 roadmap | 1 | Maintenance plan |

---

## Verification Pipeline

```
┌──────────────────────────────────────────────────────────┐
│  User writes .gdsl file or builds stream programmatically│
└──────────────────────────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│  Lexer + Parser (front-end, not in this spec)            │
└──────────────────────────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│  IR Builder → binary stream (Section 1: Abstract Syntax) │
└──────────────────────────────────────────────────────────┘
                           │
                           ▼
                    ┌──────────────┐
                    │ Compile time │
                    └──────────────┘
                           │
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                  ▼
   ┌─────────┐         ┌─────────┐      ┌──────────┐
   │ L0 pass │         │ L1 pass │      │ L2 pass  │
   │ (Syntax)│         │(Phase)  │      │ (Domain) │
   └─────────┘         └─────────┘      └──────────┘
        │                  │                   │
        └──────────────────┼───────────────────┘
                           ▼
            ┌────────────────────────────┐
            │  verified_hash in header   │
            │  (caches verification)     │
            └────────────────────────────┘
                           │
                           ▼
                    ┌──────────────┐
                    │  Runtime     │
                    └──────────────┘
                           │
            ┌──────────────┴──────────────┐
            ▼                             ▼
    ┌────────────────┐          ┌─────────────────┐
    │ Fast-path OK?  │──yes──→  │ Execute stream  │
    │ verified_hash  │          │ on GPU          │
    │ matches        │          └─────────────────┘
    └────────────────┘                   │
            │                            ▼
            │                  ┌──────────────────┐
            no                 │ Snapshot at      │
            │                  │ SNAPSHOT_BEGIN   │
            ▼                  │ (Rule 6 checks)  │
    ┌────────────────┐         └──────────────────┘
    │ Re-verify      │                   │
    │ (L2 default)   │                   ▼
    └────────────────┘         ┌──────────────────┐
            │                  │ Diff snapshots   │
            ▼                  │ (Section 8)      │
    ┌────────────────┐         │ Merkle-rooted    │
    │ Pass: execute  │         └──────────────────┘
    │ Fail: error    │                   │
    │ w/ diagnostics │                   ▼
    └────────────────┘         ┌──────────────────┐
                               │ Replay / replay  │
                               │ with patches     │
                               └──────────────────┘
```

---

## State Machine: Phase Transitions

```
                   ┌─────────┐
                   │  BUILD  │
                   │ (init)  │
                   └────┬────┘
                        │
                        │ BEGIN_STREAM
                        ▼
                   ┌─────────┐
         ┌────────→│ RECORD  │
         │         └────┬────┘
         │              │
    forbidden           │ SUBMIT
    snapshot,           ▼
    checkpoint      ┌─────────────┐
         │          │ SUBMITTED   │
         │          │ (GPU work)  │
         │          └────┬────────┘
         │               │
         │               │ FENCE_WAIT
         │               ▼
         │          ┌─────────┐
         └─────────→│  IDLE   │
                    │(safe 🔒)│
                    └────┬────┘
                         │
              [only legal opcodes]
              BEGIN_STREAM (→Record)
              SNAPSHOT_BEGIN (Rule 6 checks)
              CHECKPOINT
              ALLOC_*
              ... (see valid_ops(Idle) table)
```

**Key Rule:** SNAPSHOT_BEGIN valid only in Idle, with all PERSIST resources in Host domain.

---

## Judgment Rule Dependency Graph

```
              ┌──────────────────┐
              │  Rule 1 (BEGIN)  │
              └────────┬─────────┘
                       │
                       ▼
        ┌──────────────────────────┐
        │ Rules 2–9 (Record phase) │
        │ BARRIER creates pending  │
        └────────┬─────────────────┘
                 │
                 ▼
          ┌─────────────┐
          │ Rule 3 SUB  │
          │ (SUBMIT)    │
          └────┬────────┘
               │
               ▼ [GPU executes]
        ┌─────────────────┐
        │ Rule 4 (FENCE)  │
        │ applies pending │
        └────┬────────────┘
             │
             ▼
      ┌─────────────────────┐
      │ Rule 6 (SNAPSHOT)   │
      │ checks precond.     │
      └─────────────────────┘
             │
         ┌───┴────┐
         ▼        ▼
     [PASS]    [FAIL]
      Safe    Error +
    coherent  diagnostic
    snapshot  (Section 6)
```

---

## Barrier + Fence Semantics

```
                 Record Phase
                      │
    ┌──────────────────┼──────────────────┐
    ▼                  ▼                  ▼
ALLOC_BUFFER        BARRIER             DRAW
domain=Device   pending_transition=     (no state
                (Device→Host)           change)

           [Rule 5 records intent, not yet applied]

                   END_STREAM
                      │
                      ▼
                  Submitted Phase
                      │
                  SUBMIT (f0)
                  fence added to fd
                  phase→Submitted
                      │
                      ▼
                [GPU executes barrier]
                [data transferred]
                      │
                      ▼
                 Idle Phase
                      │
                FENCE_WAIT(f0)
                [Rule 4: apply pending transitions]
                domain→Host
                pending_transition=∅
                phase→Idle
                fd—f0
                      │
                      ▼
            ✓ Now safe for SNAPSHOT_BEGIN
              (all PERSIST in Host domain)
```

---

## Error Detection Matrix

```
┌─────────────────────────────────────────────────────────┐
│ Verification catches these bugs BEFORE GPU execution:   │
├─────────────────────────────────────────────────────────┤
│ ✓ Missing BARRIER                                       │
│   → "persistent resource in Device domain"              │
│                                                         │
│ ✓ SNAPSHOT during GPU work (Submitted phase)            │
│   → "phase must be Idle, got Submitted"                 │
│                                                         │
│ ✓ Outstanding fence at end                              │
│   → "outstanding fences not waited: {f0}"               │
│                                                         │
│ ✓ BARRIER after SUBMIT (wrong phase)                    │
│   → "phase must be Record, got Submitted"               │
│                                                         │
│ ✓ Domain mismatch (barrier src ≠ current)               │
│   → "expected domain Host, got Device"                  │
│                                                         │
│ ✓ Layered barriers (double-book same resource)          │
│   → "pending transition already queued"                 │
│                                                         │
│ ✓ Resource ID reuse within stream                       │
│   → "resource #N already allocated"                     │
└─────────────────────────────────────────────────────────┘
```

---

## Snapshot Safety Theorem (Proof Chain)

```
              ┌─────────────────────────┐
              │ Snapshot Safety         │
              │ Theorem (Main)          │
              └────────┬────────────────┘
                       │
          ┌────────────┼────────────────┐
          ▼            ▼                ▼
    ┌──────────┐ ┌─────────┐    ┌────────────┐
    │ Lemma L1 │ │Step 1–4 │    │ Corollary  │
    │ (Fence   │ │(Proof   │    │Reproduc.   │
    │Complete) │ │sketch)  │    │(Diffs)     │
    └──────────┘ └─────────┘    └────────────┘
         │            │                │
         ▼            ▼                ▼
    Every fence   Phase path    Identical
    introduced &  forced to     checkpoints
    consumed 1:1  Idle ⟹        imply
                  domains       identical
                  correct       traces
```

**Conclusion:** If stream verifies, snapshot is coherent and host-readable. ✓

---

## Conformance Levels (Fast Path to Full Check)

```
L0: Syntax (fastest)
├─ Opcode in [0x00–0xFF]
├─ Operand sizes correct
└─ Stream length valid
   └→ ~10–50 µs on 1 MB stream

L1: Phase (medium)
├─ All L0 checks
├─ Phase transitions valid (Rule 1–4)
├─ Fence balance (multiset ops)
└─ No barriers needed yet
   └→ ~50–100 µs on 1 MB stream

L2: Domain (full, default)
├─ All L0 + L1 checks
├─ All 13 judgment rules
├─ Barrier preconditions (Rule 5)
├─ Snapshot safety (Rule 6)
└─ Resource lifetime (Invariants I1–I5)
   └→ ~200–500 µs on 1 MB stream
      (or ~1 µs if cached via verified_hash)
```

---

## v1 → v2 Evolution

```
v1 (Current: Single-Queue, Single-Device)
│
├─ Γ = { phase, fd, resources, labels, checkpoints }
├─ 1 implicit queue
├─ Serial SUBMIT/FENCE_WAIT
├─ No barrier inference
└─ Verification: L0/L1/L2 levels

                    │
                    │ [Extends naturally]
                    ▼

v2 (Multi-Queue, Multi-Device)
│
├─ Γ = { queues[q]→{phase, fd, resources}, resource_owner }
├─ Per-queue phase tracking
├─ Concurrent SUBMIT on different queues
├─ Auto-inserted barriers (ASSERT_IDLE rule)
├─ Resource affinity tracking
├─ Multi-device sync primitives
└─ Streaming verification (for very large programs)
```

---

## Key References

| Artifact | Location | Use |
|----------|----------|-----|
| Judgment Rules | Section 3 | Implementation dispatch table |
| Proof | Section 5 | Safety guarantees |
| Counterexamples | Section 6 | Test cases + expected errors |
| Pseudo-code | Section 4 | Direct C translation |
| Assumptions | Section 7 | Theorem preconditions |
| Diff API | Section 8 | Snapshot comparison |

---

## Quality Metrics

| Metric | Target | Status |
|--------|--------|--------|
| Determinism | No RNG, no globals | ✅ |
| Completeness | All 13 rules | ✅ |
| Provability | Full proof with lemmas | ✅ |
| Testability | 5 bad + 1 good | ✅ |
| Extensibility | Reserved opcodes 0xC0–0xFF | ✅ |
| Performance | L2 < 500 µs/MB, cached < 1 µs | Target |

