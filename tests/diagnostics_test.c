#include "diagnostics/diagnostics.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  u64 count = (u64)DIAG_CODE_COUNT;
  assert(count > 0);
  const char* ids[256];
  assert(count <= 256);
  for (u64 i = 0; i < count; ++i) {
    const DiagInfo* info = diag_info((DiagCode)i);
    assert(info != NULL);
    assert(info->id != NULL && info->id[0] != '\0');
    assert(info->format != NULL && info->format[0] != '\0');
    assert(info->domain >= 0 && info->domain < DIAG_DOMAIN_COUNT);
    for (u64 j = 0; j < i; ++j) {
      assert(strcmp(ids[j], info->id) != 0);
    }
    ids[i] = info->id;
    const DiagInfo* found = diag_find_format(info->domain, info->format);
    assert(found != NULL);
    assert(strcmp(found->id, info->id) == 0);
  }

  const DiagInfo* lexer = diag_find_format(DIAG_DOMAIN_LEXER, TAKA_MSG_LEX_INVALID_TOKEN);
  assert(lexer != NULL && strcmp(lexer->id, "L0001") == 0);

  FILE* list = tmpfile();
  assert(list != NULL);
  diag_list(list);
  long size = ftell(list);
  assert(size > 0);
  fclose(list);

  diag_set_source("diag_test.tk");
  assert(strcmp(diag_source(), "diag_test.tk") == 0);

  printf("diagnostics test OK (%llu codes)\n", (unsigned long long)count);
  return 0;
}
