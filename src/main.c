#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/utils.h"
#include "lexer/tokenizer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "codegen/codegen.h"
#include "diagnostics/diagnostics.h"
#include "ast/ast_print.h"

static bool token_stream_ok(Tokenizer* tokenizer) {
  bool ok = TRUE;
  for (u64 i = 0; i < tokenizer->token_count; ++i) {
    Token token = tokenizer->tokens[i];
    if (token.kind == TOKEN_INVALID || token.kind == TOKEN_UNKNOWN) {
      diag_emit_format(DIAG_DOMAIN_LEXER, TAKA_MSG_LEX_INVALID_TOKEN, token.line,
                       token.column, str8_fmt(token.value));
      ok = FALSE;
    }
  }
  return ok;
}

static String8 load_source(MemArena* arena, const char* path, bool* ok) {
  BinFile file = read_file(arena, path);
  if (file.size == 0) {
    fprintf(stderr, "error: could not read %s\n", path);
    *ok = FALSE;
    return (String8){0};
  }
  *ok = TRUE;
  return bin_to_str8(&file);
}

static Sema* compile_frontend(MemArena* source_arena, MemArena* ast_arena, const char* path,
                              Stmt** out_program) {
  bool read_ok = FALSE;
  String8 source = load_source(source_arena, path, &read_ok);
  if (!read_ok) {
    return NULL;
  }
  Tokenizer* tokenizer = tokenizer_create(source_arena, source.size + 1, source);
  tokenizer_tokenize(tokenizer);
  if (!token_stream_ok(tokenizer)) {
    return NULL;
  }
  Stmt* program = parse_program(ast_arena, tokenizer->tokens, tokenizer->token_count);
  if (out_program) {
    *out_program = program;
  }
  Sema* sema = sema_create(ast_arena, program);
  if (!sema_check(sema)) {
    return NULL;
  }
  return sema;
}

static int run_cc(const char* c_path, const char* out_path) {
  const char* cc = getenv("CC");
  if (!cc || !cc[0]) {
    cc = "cc";
  }
  pid_t pid = fork();
  if (pid == 0) {
    char* const argv[] = {
        (char*)cc, (char*)"-std=c11", (char*)"-O2", (char*)"-Wall",
        (char*)"-Wextra", (char*)c_path, (char*)"-o", (char*)out_path, NULL,
    };
    execvp(cc, argv);
    _exit(127);
  }
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return 1;
  }
  return 0;
}

static int command_check(const char* path) {
  diag_set_source(path);
  MemArena* source_arena = arena_create(MByte(16));
  MemArena* ast_arena = arena_create(MByte(32));
  Stmt* program = NULL;
  Sema* sema = compile_frontend(source_arena, ast_arena, path, &program);
  int result = 0;
  if (sema) {
    printf("ok: %s\n", path);
  } else {
    result = 1;
  }
  sema_free(sema);
  arena_destroy(source_arena);
  arena_destroy(ast_arena);
  return result;
}

static int command_emit_or_build(const char* path, const char* output, bool compile) {
  diag_set_source(path);
  MemArena* source_arena = arena_create(MByte(16));
  MemArena* ast_arena = arena_create(MByte(64));
  Stmt* program = NULL;
  Sema* sema = compile_frontend(source_arena, ast_arena, path, &program);
  if (!sema) {
    arena_destroy(source_arena);
    arena_destroy(ast_arena);
    return 1;
  }

  const char* out_path = output && output[0] ? output : (compile ? "a.out" : "a.c");
  char c_path[2048];
  snprintf(c_path, sizeof(c_path), "%s.c", out_path);

  FILE* out = fopen(c_path, "w");
  if (!out) {
    fprintf(stderr, "error: could not write %s\n", c_path);
    sema_free(sema);
    arena_destroy(source_arena);
    arena_destroy(ast_arena);
    return 1;
  }
  Codegen* codegen = codegen_create(ast_arena, sema);
  bool generated = codegen_generate(codegen, out, path);
  fclose(out);
  if (!generated) {
    fprintf(stderr, "codegen failed with %u error(s)\n", codegen_error_count(codegen));
    codegen_free(codegen);
    sema_free(sema);
    arena_destroy(source_arena);
    arena_destroy(ast_arena);
    return 1;
  }

  int result = 0;
  if (compile) {
    result = run_cc(c_path, out_path);
    if (result == 0) {
      printf("built: %s\n", out_path);
    }
  } else {
    printf("emitted: %s\n", c_path);
  }
  codegen_free(codegen);
  sema_free(sema);
  arena_destroy(source_arena);
  arena_destroy(ast_arena);
  return result;
}

int main(int argc, char** argv) {
  log_system_init();

  if (argc >= 2 && strcmp(argv[1], "diagnostics") == 0) {
    diag_list(stdout);
    return 0;
  }
  if (argc >= 3 && strcmp(argv[1], "check") == 0) {
    return command_check(argv[2]);
  }
  if (argc >= 3 && (strcmp(argv[1], "emit") == 0 || strcmp(argv[1], "build") == 0)) {
    bool compile = strcmp(argv[1], "build") == 0;
    const char* output = NULL;
    if (argc >= 5 && strcmp(argv[3], "-o") == 0) {
      output = argv[4];
    }
    return command_emit_or_build(argv[2], output, compile);
  }

  const char* path = argc > 1 ? argv[1] : "example/04.tk";
  diag_set_source(path);

  MemArena* file_arena = arena_create(MByte(10));
  MemArena* tokenizer_arena = arena_create(MByte(10));
  BinFile bf = read_file(file_arena, path);
  if (bf.size == 0) {
    fprintf(stderr, "error: could not read %s\n", path);
    return 1;
  }
  String8 source = bin_to_str8(&bf);
  Tokenizer* tokenizer = tokenizer_create(tokenizer_arena, source.size + 1, source);
  tokenizer_tokenize(tokenizer);

  for (u64 i = 0; i < tokenizer->token_count; ++i) {
    debug_token(tokenizer->tokens[i]);
  }
  if (!token_stream_ok(tokenizer)) {
    arena_destroy(file_arena);
    arena_destroy(tokenizer_arena);
    return 1;
  }

  MemArena* parser_arena = arena_create(MByte(10));
  Stmt* program = parse_program(parser_arena, tokenizer->tokens, tokenizer->token_count);
  ast_print(program);
  arena_destroy(parser_arena);
  arena_destroy(file_arena);
  arena_destroy(tokenizer_arena);
  return 0;
}
