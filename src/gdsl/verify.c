#include "gdsl/verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef enum {
    GDSL_PHASE_BUILD = 0,
    GDSL_PHASE_RECORD,
    GDSL_PHASE_SUBMITTED,
    GDSL_PHASE_IDLE,
    GDSL_PHASE_FINISHED
} gdsl_phase_t;

typedef enum {
    GDSL_DOMAIN_HOST = 0,
    GDSL_DOMAIN_DEVICE = 1
} gdsl_domain_t;

typedef enum {
    GDSL_OPCODE_NOP = 0x00,
    GDSL_OPCODE_BEGIN_STREAM = 0x01,
    GDSL_OPCODE_BARRIER = 0x02,
    GDSL_OPCODE_SUBMIT = 0x03,
    GDSL_OPCODE_FENCE_WAIT = 0x04,
    GDSL_OPCODE_END_STREAM = 0x05,
    GDSL_OPCODE_END_PROGRAM = 0x06,
    GDSL_OPCODE_SNAPSHOT_BEGIN = 0x07,
    GDSL_OPCODE_SNAPSHOT_END = 0x08,
    GDSL_OPCODE_CHECKPOINT = 0x09
} gdsl_opcode_t;

typedef struct {
    const char *name;
    uint8_t size;
} gdsl_opcode_metadata_t;

static const gdsl_opcode_metadata_t gdsl_opcode_table[256] = {
    [GDSL_OPCODE_NOP] = {"NOP", 1},
    [GDSL_OPCODE_BEGIN_STREAM] = {"BEGIN_STREAM", 1},
    [GDSL_OPCODE_BARRIER] = {"BARRIER", 1},
    [GDSL_OPCODE_SUBMIT] = {"SUBMIT", 1},
    [GDSL_OPCODE_FENCE_WAIT] = {"FENCE_WAIT", 1},
    [GDSL_OPCODE_END_STREAM] = {"END_STREAM", 1},
    [GDSL_OPCODE_END_PROGRAM] = {"END_PROGRAM", 1},
    [GDSL_OPCODE_SNAPSHOT_BEGIN] = {"SNAPSHOT_BEGIN", 1},
    [GDSL_OPCODE_SNAPSHOT_END] = {"SNAPSHOT_END", 1},
    [GDSL_OPCODE_CHECKPOINT] = {"CHECKPOINT", 1},
};

typedef struct {
    gdsl_phase_t phase;
    gdsl_domain_t domain;
    int snapshot_active;
} gdsl_state_t;

static void gdsl_state_reset(gdsl_state_t *state) {
    state->phase = GDSL_PHASE_BUILD;
    state->domain = GDSL_DOMAIN_HOST;
    state->snapshot_active = 0;
}

static const char *phase_name(gdsl_phase_t phase) {
    switch (phase) {
    case GDSL_PHASE_BUILD:
        return "Build";
    case GDSL_PHASE_RECORD:
        return "Record";
    case GDSL_PHASE_SUBMITTED:
        return "Submitted";
    case GDSL_PHASE_IDLE:
        return "Idle";
    case GDSL_PHASE_FINISHED:
        return "Finished";
    default:
        return "Unknown";
    }
}

static void add_diagnostic(gdsl_verify_report_t *report,
                           size_t instruction_index,
                           gdsl_verify_severity_t severity,
                           const char *fmt,
                           ...) {
    if (!report) {
        return;
    }

    if (report->diagnostic_count >= GDSL_VERIFY_MAX_DIAGNOSTICS) {
        report->diagnostics_overflow = 1;
        report->dropped_diagnostic_count++;
        report->success = 0;
        return;
    }

    gdsl_verify_diagnostic_t *diag =
        &report->diagnostics[report->diagnostic_count++];
    diag->instruction_index = instruction_index;
    diag->severity = severity;

    va_list args;
    va_start(args, fmt);
    vsnprintf(diag->message, GDSL_VERIFY_MAX_MESSAGE, fmt, args);
    va_end(args);

    if (severity == GDSL_VERIFY_SEVERITY_ERROR) {
        report->error_count++;
    } else if (severity == GDSL_VERIFY_SEVERITY_WARNING) {
        report->warning_count++;
    } else {
        report->info_count++;
    }
}

static void report_transition_error(gdsl_verify_report_t *report,
                                    size_t index,
                                    const char *op,
                                    gdsl_phase_t actual_phase,
                                    const char *expected) {
    add_diagnostic(report, index, GDSL_VERIFY_SEVERITY_ERROR,
                   "%s requires %s, got %s",
                   op,
                   expected ? expected : "a valid phase",
                   phase_name(actual_phase));
}

int gdsl_verify(const uint8_t *stream,
                size_t length,
                gdsl_verify_level_t level,
                gdsl_verify_report_t *report) {
    if (!report) {
        return -1;
    }

    memset(report, 0, sizeof(*report));
    report->success = 0;

    if (level < GDSL_VERIFY_LEVEL_SYNTAX || level > GDSL_VERIFY_LEVEL_DOMAIN) {
        add_diagnostic(report,
                       0,
                       GDSL_VERIFY_SEVERITY_ERROR,
                       "invalid verification level %d",
                       (int)level);
        return -1;
    }

    if (!stream && length > 0) {
        add_diagnostic(report, 0, GDSL_VERIFY_SEVERITY_ERROR,
                       "null stream pointer with non-zero length");
        return 0;
    }

    gdsl_state_t state;
    gdsl_state_reset(&state);

    size_t offset = 0;
    size_t instruction_index = 0;
    int seen_end_stream = 0;
    int seen_end_program = 0;
    int stop_processing = 0;

    while (offset < length && !stop_processing) {
        uint8_t opcode = stream[offset];
        const gdsl_opcode_metadata_t *meta = &gdsl_opcode_table[opcode];

        if (!meta->name) {
            add_diagnostic(report, instruction_index,
                           GDSL_VERIFY_SEVERITY_ERROR,
                           "unknown opcode 0x%02x", opcode);
            offset += 1;
            instruction_index++;
            continue;
        }

        if (meta->size == 0 || offset + meta->size > length) {
            add_diagnostic(report, instruction_index,
                           GDSL_VERIFY_SEVERITY_ERROR,
                           "truncated instruction for %s", meta->name);
            break;
        }

        report->instruction_count++;

        switch (opcode) {
        case GDSL_OPCODE_BEGIN_STREAM:
            if (level >= GDSL_VERIFY_LEVEL_PHASE) {
                if (state.snapshot_active) {
                    add_diagnostic(report, instruction_index,
                                   GDSL_VERIFY_SEVERITY_ERROR,
                                   "cannot BEGIN_STREAM while snapshot is active");
                }
                if (state.phase != GDSL_PHASE_BUILD &&
                    state.phase != GDSL_PHASE_IDLE) {
                    report_transition_error(report, instruction_index,
                                            meta->name,
                                            state.phase,
                                            "Build or Idle");
                }
            }
            state.phase = GDSL_PHASE_RECORD;
            break;
        case GDSL_OPCODE_BARRIER:
            if (level >= GDSL_VERIFY_LEVEL_PHASE &&
                state.phase != GDSL_PHASE_RECORD) {
                report_transition_error(report, instruction_index,
                                        meta->name, state.phase, "Record");
            }
            if (level >= GDSL_VERIFY_LEVEL_DOMAIN &&
                state.domain != GDSL_DOMAIN_DEVICE) {
                add_diagnostic(report, instruction_index,
                               GDSL_VERIFY_SEVERITY_WARNING,
                               "BARRIER issued outside device domain; assuming implicit promotion");
                state.domain = GDSL_DOMAIN_DEVICE;
            }
            break;
        case GDSL_OPCODE_SUBMIT:
            if (level >= GDSL_VERIFY_LEVEL_PHASE) {
                if (state.phase != GDSL_PHASE_RECORD) {
                    report_transition_error(report, instruction_index,
                                            meta->name, state.phase, "Record");
                }
                if (state.snapshot_active) {
                    add_diagnostic(report, instruction_index,
                                   GDSL_VERIFY_SEVERITY_ERROR,
                                   "cannot SUBMIT inside a snapshot");
                }
            }
            state.phase = GDSL_PHASE_SUBMITTED;
            state.domain = GDSL_DOMAIN_DEVICE;
            break;
        case GDSL_OPCODE_FENCE_WAIT:
            if (level >= GDSL_VERIFY_LEVEL_PHASE &&
                state.phase != GDSL_PHASE_SUBMITTED) {
                report_transition_error(report, instruction_index,
                                        meta->name, state.phase, "Submitted");
            }
            state.phase = GDSL_PHASE_IDLE;
            state.domain = GDSL_DOMAIN_HOST;
            break;
        case GDSL_OPCODE_END_STREAM:
            if (level >= GDSL_VERIFY_LEVEL_PHASE &&
                state.phase != GDSL_PHASE_IDLE &&
                state.phase != GDSL_PHASE_RECORD) {
                report_transition_error(report, instruction_index,
                                        meta->name, state.phase, "Record or Idle");
            }
            if (state.phase == GDSL_PHASE_RECORD && level >= GDSL_VERIFY_LEVEL_PHASE) {
                add_diagnostic(report, instruction_index,
                               GDSL_VERIFY_SEVERITY_WARNING,
                               "END_STREAM while GPU work still pending; assuming idle transition");
            }
            state.phase = GDSL_PHASE_FINISHED;
            seen_end_stream = 1;
            break;
        case GDSL_OPCODE_END_PROGRAM:
            seen_end_program = 1;
            if (level >= GDSL_VERIFY_LEVEL_PHASE &&
                state.phase != GDSL_PHASE_FINISHED) {
                report_transition_error(report, instruction_index,
                                        meta->name, state.phase, "Finished");
            }
            if (offset + meta->size < length) {
                add_diagnostic(report,
                               instruction_index,
                               GDSL_VERIFY_SEVERITY_ERROR,
                               "END_PROGRAM must be terminal; trailing bytes detected");
                stop_processing = 1;
            }
            break;
        case GDSL_OPCODE_SNAPSHOT_BEGIN:
            if (level >= GDSL_VERIFY_LEVEL_DOMAIN) {
                if (state.snapshot_active) {
                    add_diagnostic(report, instruction_index,
                                   GDSL_VERIFY_SEVERITY_ERROR,
                                   "nested SNAPSHOT_BEGIN not allowed");
                }
                if (state.phase != GDSL_PHASE_IDLE) {
                    report_transition_error(report, instruction_index,
                                            meta->name, state.phase, "Idle");
                }
                if (state.domain != GDSL_DOMAIN_HOST) {
                    add_diagnostic(report, instruction_index,
                                   GDSL_VERIFY_SEVERITY_ERROR,
                                   "snapshots require host domain but current domain is device");
                }
            }
            state.snapshot_active = 1;
            break;
        case GDSL_OPCODE_SNAPSHOT_END:
            if (level >= GDSL_VERIFY_LEVEL_DOMAIN && !state.snapshot_active) {
                add_diagnostic(report, instruction_index,
                               GDSL_VERIFY_SEVERITY_ERROR,
                               "SNAPSHOT_END without SNAPSHOT_BEGIN");
            }
            state.snapshot_active = 0;
            break;
        case GDSL_OPCODE_CHECKPOINT:
            if (level >= GDSL_VERIFY_LEVEL_DOMAIN &&
                state.phase != GDSL_PHASE_IDLE) {
                report_transition_error(report, instruction_index,
                                        meta->name, state.phase, "Idle");
            }
            break;
        default:
            break;
        }

        offset += meta->size;
        instruction_index++;
    }

    if (state.snapshot_active) {
        add_diagnostic(report, instruction_index, GDSL_VERIFY_SEVERITY_ERROR,
                       "unterminated snapshot region");
    }

    if (!seen_end_program) {
        if (!seen_end_stream) {
            add_diagnostic(report, instruction_index, GDSL_VERIFY_SEVERITY_ERROR,
                           "missing END_STREAM");
        } else {
            add_diagnostic(report, instruction_index, GDSL_VERIFY_SEVERITY_ERROR,
                           "missing END_PROGRAM");
        }
    } else if (state.phase != GDSL_PHASE_FINISHED) {
        add_diagnostic(report, instruction_index, GDSL_VERIFY_SEVERITY_ERROR,
                       "unterminated program state");
    }

    report->success = (report->error_count == 0 &&
                       !report->diagnostics_overflow);
    return 0;
}
