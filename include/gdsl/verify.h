#ifndef GDSL_VERIFY_H
#define GDSL_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDSL_VERIFY_MAX_DIAGNOSTICS 64
#define GDSL_VERIFY_MAX_MESSAGE 256

typedef enum {
    GDSL_VERIFY_SEVERITY_INFO = 0,
    GDSL_VERIFY_SEVERITY_WARNING = 1,
    GDSL_VERIFY_SEVERITY_ERROR = 2
} gdsl_verify_severity_t;

typedef enum {
    GDSL_VERIFY_LEVEL_SYNTAX = 0,
    GDSL_VERIFY_LEVEL_PHASE = 1,
    GDSL_VERIFY_LEVEL_DOMAIN = 2
} gdsl_verify_level_t;

typedef struct {
    size_t instruction_index;
    gdsl_verify_severity_t severity;
    char message[GDSL_VERIFY_MAX_MESSAGE];
} gdsl_verify_diagnostic_t;

typedef struct {
    int success;
    size_t instruction_count;
    size_t error_count;
    size_t warning_count;
    size_t info_count;
    size_t diagnostic_count;
    gdsl_verify_diagnostic_t diagnostics[GDSL_VERIFY_MAX_DIAGNOSTICS];
} gdsl_verify_report_t;

int gdsl_verify(const uint8_t *stream,
                size_t length,
                gdsl_verify_level_t level,
                gdsl_verify_report_t *report);

#ifdef __cplusplus
}
#endif

#endif // GDSL_VERIFY_H
