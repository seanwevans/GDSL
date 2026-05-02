#include "gdsl/verify.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void print_report(const char *label, const gdsl_verify_report_t *report) {
    printf("%s: success=%d errors=%zu warnings=%zu infos=%zu\n",
           label,
           report->success,
           report->error_count,
           report->warning_count,
           report->info_count);
    for (size_t i = 0; i < report->diagnostic_count; ++i) {
        const gdsl_verify_diagnostic_t *diag = &report->diagnostics[i];
        printf("  [%zu] severity=%d %s\n",
               diag->instruction_index,
               (int)diag->severity,
               diag->message);
    }
}

static void test_valid_program(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x02, /* BARRIER */
        0x03, /* SUBMIT */
        0x04, /* FENCE_WAIT */
        0x05, /* END_STREAM */
        0x06  /* END_PROGRAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_DOMAIN, &report);
    assert(rc == 0);
    print_report("valid", &report);
    assert(report.success);
    assert(report.error_count == 0);
}

static void test_missing_begin(void) {
    const uint8_t stream[] = {
        0x03, /* SUBMIT */
        0x04, /* FENCE_WAIT */
        0x05, /* END_STREAM */
        0x06  /* END_PROGRAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_PHASE, &report);
    assert(rc == 0);
    print_report("missing_begin", &report);
    assert(!report.success);
    assert(report.error_count > 0);
    assert(strstr(report.diagnostics[0].message,
                  "SUBMIT requires Record, got Build") != NULL);
}

static void test_unknown_opcode(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0xFF, /* unknown */
        0x05, /* END_STREAM */
        0x06  /* END_PROGRAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_SYNTAX, &report);
    assert(rc == 0);
    print_report("unknown_opcode", &report);
    assert(!report.success);
    assert(report.error_count > 0);
}

static void test_snapshot_constraints(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x03, /* SUBMIT */
        0x07, /* SNAPSHOT_BEGIN */
        0x04, /* FENCE_WAIT */
        0x07, /* SNAPSHOT_BEGIN */
        0x08, /* SNAPSHOT_END */
        0x04, /* FENCE_WAIT */
        0x05, /* END_STREAM */
        0x06  /* END_PROGRAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_DOMAIN, &report);
    assert(rc == 0);
    print_report("snapshot", &report);
    assert(!report.success);
    assert(report.error_count >= 1);
    assert(strstr(report.diagnostics[0].message,
                  "SNAPSHOT_BEGIN requires Idle, got Submitted") != NULL);
}

static void test_invalid_level(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x05, /* END_STREAM */
        0x06  /* END_PROGRAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), (gdsl_verify_level_t)99, &report);
    assert(rc == -1);
    print_report("invalid_level", &report);
    assert(!report.success);
    assert(report.error_count == 1);
    assert(report.diagnostic_count == 1);
    assert(strstr(report.diagnostics[0].message, "invalid verification level") != NULL);
}

static void test_trailing_opcode_after_end_program(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x05, /* END_STREAM */
        0x06, /* END_PROGRAM */
        0x00  /* NOP (invalid trailing instruction) */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_PHASE, &report);
    assert(rc == 0);
    print_report("trailing_after_end_program", &report);
    assert(!report.success);
    assert(report.instruction_count == 3);

    int found_terminal_error = 0;
    for (size_t i = 0; i < report.diagnostic_count; ++i) {
        if (strstr(report.diagnostics[i].message, "END_PROGRAM must be terminal") != NULL) {
            found_terminal_error = 1;
            break;
        }
    }
    assert(found_terminal_error);
}

static void test_end_stream_without_end_program(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x05  /* END_STREAM */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_PHASE, &report);
    assert(rc == 0);
    print_report("end_stream_without_end_program", &report);
    assert(!report.success);

    int found_missing_program = 0;
    for (size_t i = 0; i < report.diagnostic_count; ++i) {
        if (strstr(report.diagnostics[i].message, "missing END_PROGRAM") != NULL) {
            found_missing_program = 1;
            break;
        }
    }
    assert(found_missing_program);
}

static void test_end_program_wrong_phase_with_extra_bytes(void) {
    const uint8_t stream[] = {
        0x01, /* BEGIN_STREAM */
        0x06, /* END_PROGRAM (wrong phase) */
        0x02  /* BARRIER (trailing byte) */
    };

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_PHASE, &report);
    assert(rc == 0);
    print_report("end_program_wrong_phase_with_extra", &report);
    assert(!report.success);
    assert(report.instruction_count == 2);

    int found_phase_error = 0;
    int found_terminal_error = 0;
    int found_unterminated_state = 0;
    for (size_t i = 0; i < report.diagnostic_count; ++i) {
        if (strstr(report.diagnostics[i].message, "END_PROGRAM requires Finished, got Record") != NULL) {
            found_phase_error = 1;
        }
        if (strstr(report.diagnostics[i].message, "END_PROGRAM must be terminal") != NULL) {
            found_terminal_error = 1;
        }
        if (strstr(report.diagnostics[i].message, "unterminated program state") != NULL) {
            found_unterminated_state = 1;
        }
    }
    assert(found_phase_error);
    assert(found_terminal_error);
    assert(found_unterminated_state);
}

static void test_diagnostic_overflow_preserves_existing_entries(void) {
    uint8_t stream[GDSL_VERIFY_MAX_DIAGNOSTICS + 5];
    memset(stream, 0xFF, sizeof(stream)); /* unknown opcodes => one error each */
    stream[GDSL_VERIFY_MAX_DIAGNOSTICS + 3] = 0x01; /* BEGIN_STREAM */
    stream[GDSL_VERIFY_MAX_DIAGNOSTICS + 4] = 0x06; /* END_PROGRAM */

    gdsl_verify_report_t report;
    int rc = gdsl_verify(stream, sizeof(stream), GDSL_VERIFY_LEVEL_SYNTAX, &report);
    assert(rc == 0);
    print_report("overflow_preserve", &report);
    assert(!report.success);
    assert(report.diagnostics_overflow);
    assert(report.diagnostic_count == GDSL_VERIFY_MAX_DIAGNOSTICS);
    assert(report.dropped_diagnostic_count == 4);
    assert(report.error_count == GDSL_VERIFY_MAX_DIAGNOSTICS);
    assert(strstr(report.diagnostics[0].message, "unknown opcode 0xff") != NULL);
    assert(strstr(report.diagnostics[GDSL_VERIFY_MAX_DIAGNOSTICS - 1].message,
                  "unknown opcode 0xff") != NULL);
}

int main(void) {
    test_valid_program();
    test_missing_begin();
    test_unknown_opcode();
    test_snapshot_constraints();
    test_invalid_level();
    test_trailing_opcode_after_end_program();
    test_end_stream_without_end_program();
    test_end_program_wrong_phase_with_extra_bytes();
    test_diagnostic_overflow_preserves_existing_entries();
    puts("All verify tests completed.");
    return 0;
}
