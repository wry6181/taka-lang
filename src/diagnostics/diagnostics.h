#pragma once

#include "utils/types.h"

#include <stdarg.h>
#include <stdio.h>

typedef enum DiagSeverity {
  DIAG_ERROR,
  DIAG_WARNING,
  DIAG_NOTE
} DiagSeverity;

typedef enum DiagDomain {
  DIAG_DOMAIN_PARSER,
  DIAG_DOMAIN_SEMA,
  DIAG_DOMAIN_CODEGEN,
  DIAG_DOMAIN_LEXER,
  DIAG_DOMAIN_COUNT
} DiagDomain;

typedef enum DiagCode {
#define TAKA_DIAG(name, id, severity, domain, format) DIAG_##name,
#include "diagnostics.def"
#undef TAKA_DIAG
  DIAG_CODE_COUNT
} DiagCode;

typedef struct DiagInfo {
  DiagCode code;
  const char* id;
  DiagSeverity severity;
  DiagDomain domain;
  const char* format;
} DiagInfo;

const DiagInfo* diag_info(DiagCode code);
const DiagInfo* diag_find_format(DiagDomain domain, const char* format);
const char* diag_severity_name(DiagSeverity severity);
const char* diag_domain_name(DiagDomain domain);
void diag_set_source(const char* source);
const char* diag_source(void);
void diag_vemit_format(DiagDomain domain, const char* format, u64 line, u64 column,
                       va_list args);
void diag_emit_format(DiagDomain domain, const char* format, u64 line, u64 column, ...)
    T_PRINTF(2, 5);
void diag_list(FILE* out);

#include "diagnostics_messages.h"
