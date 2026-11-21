#include "lexer/tokenizer.h"
#include "parser/parser.h"
#include "sema/sema.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct SourceCase {
  const char* name;
  const char* source;
} SourceCase;

static int checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    checks++;                                                              \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      fprintf(stderr, "FAIL: %s [%s:%d]\n", (msg), __FILE__, __LINE__);    \
    }                                                                      \
  } while (0)


static bool sema_ok(const char* source_text) {
  MemArena* arena = arena_create(MByte(8));
  String8 source = str8(source_text);
  Tokenizer* tokenizer = tokenizer_create(arena, source.size + 1, source);
  tokenizer_tokenize(tokenizer);
  Stmt* program = parse_program(arena, tokenizer->tokens, tokenizer->token_count);
  Sema* sema = sema_create(arena, program);
  bool ok = sema_check(sema);
  sema_free(sema);
  arena_destroy(arena);
  return ok;
}

static bool sema_fails(const char* source_text) {
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, 2);
      close(fd);
    }
    _exit(sema_ok(source_text) ? 0 : 1);
  }
  if (pid < 0) {
    return FALSE;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return FALSE;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static const SourceCase valid_cases[] = {
    {"pure_calls_pure",
     "fn add(i32 a, i32 b) <pure> -> i32 { return a + b; }\n"
     "fn main() <pure> -> i32 { return add(1, 2); }\n"},
    {"reads_compose",
     "i32 g = 1;\n"
     "fn read_g() <reads> -> i32 { return g; }\n"
     "fn read_p(i32* p) <reads> -> i32 { return *p; }\n"
     "fn caller(i32* p) <reads> -> i32 { return read_g() + read_p(p); }\n"},
    {"writes_compose",
     "fn set(i32* p) <writes> { *p = 1; }\n"
     "fn set_ref(i32& r) <writes> { r = 2; }\n"
     "fn caller(i32* p, i32& r) <writes> { set(p); set_ref(r); }\n"},
    {"transacts_covers_body",
     "fn read() <reads> -> i32 { return 1; }\n"
     "fn tr() <transacts> -> i32 { transact { i32 x = read(); } return 0; }\n"},
    {"decides_in_condition",
     "extern fn may() <decides> -> bool;\n"
     "fn f() { if (may()) { } while (may()) { } }\n"},
    {"spawn_hides_suspends",
     "extern fn tick() <suspends>;\n"
     "fn f() { spawn tick(); }\n"},
    {"custom_effect_and_handler",
     "effect Ask { fn ask() -> i32, }\n"
     "handler A for Ask { ask() => resume(1), }\n"
     "fn f() <Ask> -> i32 { return perform Ask::ask(); }\n"
     "fn g() -> i32 { return handle perform Ask::ask() with A; }\n"},
    {"method_effects",
     "trait T { fn get(Self& self) <reads> -> i32; }\n"
     "struct S { i32 x }\n"
     "impl T for S { fn get(S& self) <reads> -> i32 { return self.x; } }\n"
     "fn f(S& s) <reads> -> i32 { return s.get(); }\n"},
    {"alloc_propagates",
     "fn alloc_fn() <alloc> -> i32 { return 1; }\n"
     "fn f() <alloc> -> i32 { return alloc_fn(); }\n"},
    {"global_write",
     "mut i32 g = 0;\n"
     "fn f() <writes> { g = 1; }\n"},
    {"lambda_body",
     "fn f() { auto l = [](i32 x) => x + 1; }\n"},
    {"default_effects_compose",
     "fn g() -> i32 { return 1; }\n"
     "fn f() -> i32 { return g(); }\n"},
    {"gpu_effect",
     "fn gpu_fn() <gpu> { }\n"
     "fn f() <gpu> { gpu_fn(); }\n"},
    {"payload_enum_match",
     "enum E { A(i32), B, }\n"
     "fn f(E e) <reads> -> i32 { return match (e) { E::A(x) => x, E::B => 0, }; }\n"},
    {"transact_nested",
     "fn f() <transacts> { transact { transact { } } }\n"},
    {"inline_handler_hides_effect",
     "effect Ask { fn ask() -> i32, }\n"
     "fn g() -> i32 { return handle perform Ask::ask() with { ask() => resume(1), }; }\n"},
    {"lambda_declared_effect",
     "extern fn io_fn() <io>;\n"
     "fn f() <io> { auto l = []() <io> { io_fn(); }; l(); }\n"},
};

static const SourceCase invalid_cases[] = {
    {"pure_calls_io",
     "extern fn out(i32 x) <io> -> i32;\n"
     "fn bad() -> i32 { out(1); return 0; }\n"},
    {"pure_reads_pointer",
     "fn bad(i32* p) <pure> -> i32 { return *p; }\n"},
    {"reads_writes",
     "fn bad(i32* p) <reads> { *p = 1; }\n"},
    {"transact_requires_effect",
     "fn bad() { transact { } }\n"},
    {"transact_body_io",
     "extern fn io_fn() <io>;\n"
     "fn bad() <transacts> { transact { io_fn(); } }\n"},
    {"decides_outside_condition",
     "extern fn may() <decides> -> bool;\n"
     "fn bad() { may(); }\n"},
    {"suspends_direct",
     "extern fn tick() <suspends>;\n"
     "fn bad() { tick(); }\n"},
    {"perform_undeclared",
     "effect Ask { fn ask() -> i32, }\n"
     "fn bad() -> i32 { return perform Ask::ask(); }\n"},
    {"global_read_requires_reads",
     "i32 g = 1;\n"
     "fn bad() <pure> -> i32 { return g; }\n"},
    {"immutable_assignment",
     "fn bad() { i32 x = 1; x = 2; }\n"},
    {"const_reference_write",
     "fn bad(const i32& x) { x = 1; }\n"},
    {"method_effect_mismatch",
     "trait T { fn get(Self& self) <reads> -> i32; }\n"
     "struct S { i32 x }\n"
     "impl T for S { fn get(S& self) <reads> -> i32 { return self.x; } }\n"
     "fn bad(S& s) <writes> -> i32 { return s.get(); }\n"},
    {"impl_effect_not_subset",
     "trait T { fn get(Self& self) <reads> -> i32; }\n"
     "struct S { i32 x }\n"
     "impl T for S { fn get(S& self) <io> -> i32 { return self.x; } }\n"},
    {"transacts_does_not_cover_io",
     "extern fn io_fn() <io>;\n"
     "fn bad() <transacts> { io_fn(); }\n"},
    {"writes_does_not_cover_reads",
     "fn read() <reads> -> i32 { return 1; }\n"
     "fn bad() <writes> -> i32 { return read(); }\n"},
    {"lambda_effect_leaks",
     "extern fn io_fn() <io>;\n"
     "fn bad() { auto l = []() { io_fn(); }; l(); }\n"},
    {"lambda_declared_effect_too_narrow",
     "extern fn io_fn() <io>;\n"
     "fn bad() { auto l = []() <pure> { io_fn(); }; }\n"},
    {"default_effects_do_not_include_io",
     "extern fn io_fn() <io>;\n"
     "fn bad() { io_fn(); }\n"},
    {"gpu_not_default",
     "fn gpu_fn() <gpu> { }\n"
     "fn bad() { gpu_fn(); }\n"},
};

int main(void) {
  for (u64 i = 0; i < sizeof(valid_cases) / sizeof(valid_cases[0]); ++i) {
    CHECK(sema_ok(valid_cases[i].source), valid_cases[i].name);
  }
  for (u64 i = 0; i < sizeof(invalid_cases) / sizeof(invalid_cases[0]); ++i) {
    CHECK(sema_fails(invalid_cases[i].source), invalid_cases[i].name);
  }
  CHECK(arena_debug_live_count() == 0, "leaked arenas");
  CHECK(map_debug_live_count() == 0, "leaked maps");

  if (failures != 0) {
    fprintf(stderr, "sema tests FAILED: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("sema test OK (%d checks, %llu valid, %llu invalid)\n", checks,
         (unsigned long long)(sizeof(valid_cases) / sizeof(valid_cases[0])),
         (unsigned long long)(sizeof(invalid_cases) / sizeof(invalid_cases[0])));
  return 0;
}
