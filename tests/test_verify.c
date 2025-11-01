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
}

int main(void) {
    test_valid_program();
    test_missing_begin();
    test_unknown_opcode();
    test_snapshot_constraints();
    puts("All verify tests completed.");
    return 0;
}
