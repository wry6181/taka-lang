#include "diagnostics.h"

#include <string.h>

static const char* g_source = "<input>";

static const DiagInfo k_diags[] = {
#define TAKA_DIAG(name, id, severity, domain, format) \
  {DIAG_##name, id, severity, domain, format},
#include "diagnostics.def"
#undef TAKA_DIAG
};

const DiagInfo* diag_info(DiagCode code) {
  if (code < 0 || (u64)code >= (u64)(sizeof(k_diags) / sizeof(k_diags[0]))) {
    return NULL;
  }
  return &k_diags[code];
}

const char* diag_severity_name(DiagSeverity severity) {
  switch (severity) {
    case DIAG_ERROR: return "error";
    case DIAG_WARNING: return "warning";
    case DIAG_NOTE: return "note";
  }
  return "error";
}

const char* diag_domain_name(DiagDomain domain) {
  switch (domain) {
    case DIAG_DOMAIN_PARSER: return "parser";
    case DIAG_DOMAIN_SEMA: return "sema";
    case DIAG_DOMAIN_CODEGEN: return "codegen";
    case DIAG_DOMAIN_LEXER: return "lexer";
    case DIAG_DOMAIN_COUNT: break;
  }
  return "general";
}

const DiagInfo* diag_find_format(DiagDomain domain, const char* format) {
  if (!format) {
    return NULL;
  }
  for (u64 i = 0; i < (u64)(sizeof(k_diags) / sizeof(k_diags[0])); ++i) {
    if (k_diags[i].domain == domain && strcmp(k_diags[i].format, format) == 0) {
      return &k_diags[i];
    }
  }
  return NULL;
}

void diag_set_source(const char* source) {
  g_source = (source && source[0]) ? source : "<input>";
}

const char* diag_source(void) {
  return g_source;
}

void diag_vemit_format(DiagDomain domain, const char* format, u64 line, u64 column,
                       va_list args) {
  const DiagInfo* info = diag_find_format(domain, format);
  char message[2048];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(message, sizeof(message), format, copy);
  va_end(copy);

  const char* id = info ? info->id : "GEN0000";
  const char* severity = info ? diag_severity_name(info->severity) : "error";
  if (line > 0) {
    fprintf(stderr, "%s:%llu:%llu: %s[%s]: %s\n", g_source,
            (unsigned long long)line, (unsigned long long)column, severity, id, message);
  } else {
    fprintf(stderr, "%s: %s[%s]: %s\n", g_source, severity, id, message);
  }
}

void diag_emit_format(DiagDomain domain, const char* format, u64 line, u64 column, ...) {
  va_list args;
  va_start(args, column);
  diag_vemit_format(domain, format, line, column, args);
  va_end(args);
}

void diag_list(FILE* out) {
  if (!out) {
    return;
  }
  fprintf(out, "%-9s %-8s %-8s %s\n", "CODE", "SEVERITY", "DOMAIN", "MESSAGE");
  for (u64 i = 0; i < (u64)(sizeof(k_diags) / sizeof(k_diags[0])); ++i) {
    const DiagInfo* info = &k_diags[i];
    fprintf(out, "%-9s %-8s %-8s %s\n", info->id,
            diag_severity_name(info->severity), diag_domain_name(info->domain),
            info->format);
  }
}
