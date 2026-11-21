#include "lexer/tokenizer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "codegen/codegen.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct CodegenCase {
  const char* name;
  const char* source;
  int expected_exit;
} CodegenCase;

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

static const char* compiler_name(void) {
  const char* cc = getenv("CC");
  return (cc && cc[0]) ? cc : "cc";
}

static bool run_process(char* const argv[], int* out_status) {
  pid_t pid = fork();
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  if (pid < 0) {
    return FALSE;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return FALSE;
  }
  if (out_status) {
    *out_status = status;
  }
  return TRUE;
}

static bool compile_c(const char* c_path, const char* bin_path) {
  char sanitize_flag[128] = {0};
  const char* sanitize = getenv("TAKA_TEST_SANITIZE");
  bool with_sanitize = sanitize && sanitize[0] && strcmp(sanitize, "0") != 0;
  if (with_sanitize) {
    snprintf(sanitize_flag, sizeof(sanitize_flag), "-fsanitize=%s", sanitize);
  }
  char* argv[16];
  int index = 0;
  argv[index++] = (char*)compiler_name();
  const char* opt = getenv("TAKA_TEST_OPT");
  if (!opt || !opt[0]) {
    opt = "-O2";
  }
  argv[index++] = (char*)"-std=c11";
  argv[index++] = (char*)opt;
  argv[index++] = (char*)"-Wall";
  argv[index++] = (char*)"-Wextra";
  argv[index++] = (char*)"-Werror";
  if (with_sanitize) {
    argv[index++] = sanitize_flag;
    argv[index++] = (char*)"-fno-sanitize-recover=all";
  }
  argv[index++] = (char*)c_path;
  argv[index++] = (char*)"-o";
  argv[index++] = (char*)bin_path;
  argv[index] = NULL;
  int status = 0;
  if (!run_process(argv, &status)) {
    return FALSE;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_binary(const char* bin_path, int* exit_code) {
  char* argv[] = {(char*)bin_path, NULL};
  int status = 0;
  if (!run_process(argv, &status)) {
    return FALSE;
  }
  if (!WIFEXITED(status)) {
    return FALSE;
  }
  *exit_code = WEXITSTATUS(status);
  return TRUE;
}

static bool write_codegen(const char* source_text, const char* c_path) {
  MemArena* arena = arena_create(MByte(16));
  String8 source = str8(source_text);
  Tokenizer* tokenizer = tokenizer_create(arena, source.size + 1, source);
  tokenizer_tokenize(tokenizer);
  Stmt* program = parse_program(arena, tokenizer->tokens, tokenizer->token_count);
  Sema* sema = sema_create(arena, program);
  if (!sema_check(sema)) {
    sema_free(sema);
    arena_destroy(arena);
    return FALSE;
  }
  FILE* out = fopen(c_path, "w");
  if (!out) {
    sema_free(sema);
    arena_destroy(arena);
    return FALSE;
  }
  Codegen* codegen = codegen_create(arena, sema);
  bool ok = codegen_generate(codegen, out, "test.tk");
  fclose(out);
  codegen_free(codegen);
  sema_free(sema);
  arena_destroy(arena);
  return ok;
}

static bool codegen_fails(const char* source_text) {
  MemArena* arena = arena_create(MByte(16));
  String8 source = str8(source_text);
  Tokenizer* tokenizer = tokenizer_create(arena, source.size + 1, source);
  tokenizer_tokenize(tokenizer);
  Stmt* program = parse_program(arena, tokenizer->tokens, tokenizer->token_count);
  Sema* sema = sema_create(arena, program);
  if (!sema_check(sema)) {
    sema_free(sema);
    arena_destroy(arena);
    return FALSE;
  }
  FILE* out = tmpfile();
  if (!out) {
    sema_free(sema);
    arena_destroy(arena);
    return FALSE;
  }
  Codegen* codegen = codegen_create(arena, sema);
  int saved_stderr = dup(STDERR_FILENO);
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
  bool ok = codegen_generate(codegen, out, "test.tk");
  codegen_free(codegen);
  if (saved_stderr >= 0) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
  }
  fclose(out);
  sema_free(sema);
  arena_destroy(arena);
  return !ok;
}

static bool c_file_contains(const char* path, const char* needle) {
  FILE* file = fopen(path, "rb");
  if (!file) {
    return FALSE;
  }
  char buffer[131072];
  size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  buffer[size] = '\0';
  return strstr(buffer, needle) != NULL;
}

static const CodegenCase codegen_cases[] = {
    {"arithmetic_control",
     "fn add(i32 a, i32 b) <pure> -> i32 { return a + b; }\n"
     "fn fact(i32 n) <pure> -> i32 { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
     "struct Point { i32 x, i32 y, }\n"
     "fn sum(Point& p) <reads> -> i32 { return p.x + p.y; }\n"
     "fn main() <reads> -> i32 {\n"
     "  mut i32 a = add(2, 3);\n"
     "  mut i32 i = 0;\n"
     "  while (i < 3) { i += 1; }\n"
     "  for (mut i32 j = 0; j < 2; j += 1) { a += j; }\n"
     "  Point p = Point { x: 4, y: 5 };\n"
     "  i32 s = sum(p);\n"
     "  i32 arr = sizeof(i32);\n"
     "  return a + s + fact(4) + arr;\n"
     "}\n",
     43},
    {"hidden_arena",
     "fn helper() <alloc> -> i32 { return 7; }\n"
     "fn main() <alloc> -> i32 { return helper(); }\n",
     7},
    {"structs_refs_methods",
     "trait T { fn get(Self& self) <reads> -> i32; }\n"
     "struct P { i32 x, }\n"
     "impl T for P { fn get(P& self) <reads> -> i32 { return self.x; } }\n"
     "fn inc(i32& x) <writes> { x += 1; }\n"
     "fn main() <reads, writes> -> i32 {\n"
     "  mut i32 a = 4;\n"
     "  inc(a);\n"
     "  P p = P { x: 5, };\n"
     "  return a + p.get() + sizeof(P) + offsetof(P, x);\n"
     "}\n",
     14},
    {"reference_field",
     "struct R { i32& r, }\n"
     "fn main() <reads, writes> -> i32 {\n"
     "  mut i32 x = 3;\n"
     "  R holder = R { r: &x, };\n"
     "  holder.r += 2;\n"
     "  return holder.r;\n"
     "}\n",
     5},
    {"arrays",
     "fn main() <reads> -> i32 {\n"
     "  [i32; 4] a = [1, 2, 3, 4];\n"
     "  [i32; 3] b = [2; 3];\n"
     "  return a[1] + a[3] + b[2];\n"
     "}\n",
     8},
    {"globals_enums_casts",
     "mut i32 g = 1;\n"
     "enum E { A = 2, B = 3, }\n"
     "fn main() <reads, writes> -> i32 {\n"
     "  g += 1;\n"
     "  i32 x = g > 0 ? 4 : 5;\n"
     "  return x + E::B + (g as i32);\n"
     "}\n",
     9},
    {"defer",
     "mut i32 g = 0;\n"
     "fn f() <writes> { defer g = 7; }\n"
     "fn main() <reads, writes> -> i32 { f(); return g; }\n",
     7},
    {"empty_struct",
     "struct Empty { }\n"
     "fn main() <pure> -> i32 { Empty e = Empty { }; return sizeof(Empty); }\n",
     1},
    {"for_each_and_labels",
     "fn main() <reads, writes> -> i32 {\n"
     "  [i32; 4] a = [1, 2, 3, 4];\n"
     "  mut i32 sum = 0;\n"
     "  for (v in a) { sum += v; }\n"
     "  for (&r in a) { r += 1; }\n"
     "  outer: for (v in a) {\n"
     "    if (v == 3) { continue outer; }\n"
     "    sum += v;\n"
     "  }\n"
     "  return sum;\n"
     "}\n",
     21},
    {"labeled_break",
     "fn main() <pure> -> i32 {\n"
     "  [i32; 3] a = [1, 2, 3];\n"
     "  mut i32 r = 0;\n"
     "  outer: for (v in a) {\n"
     "    for (w in a) {\n"
     "      if (w == 2) { break outer; }\n"
     "      r += 1;\n"
     "    }\n"
     "  }\n"
     "  return r;\n"
     "}\n",
     1},
    {"payload_enum_match",
     "enum Shape { Circle(f32), Rect { f32 w, f32 h, }, Empty, }\n"
     "fn area(Shape& s) <reads> -> f32 {\n"
     "  return match (s) {\n"
     "    Shape::Circle(r) => 3.0 * r * r,\n"
     "    Shape::Rect { w, h } => w * h,\n"
     "    Shape::Empty => 0.0,\n"
     "  };\n"
     "}\n"
     "fn main() <reads> -> i32 {\n"
     "  Shape s = Shape::Circle(2.0);\n"
     "  Shape r = Shape::Rect { w: 3.0, h: 4.0 };\n"
     "  Shape e = Shape::Empty;\n"
     "  return (area(s) + area(r) + area(e)) as i32;\n"
     "}\n",
     24},
    {"match_guards_alternatives",
     "enum E { A(i32), B, C(i32, i32), }\n"
     "fn f(E e) <reads> -> i32 {\n"
     "  return match (e) {\n"
     "    E::A(x) if x > 10 => x,\n"
     "    E::A(x) | E::B => 1,\n"
     "    E::C(a, b) => a + b,\n"
     "    mut x => 0,\n"
     "  };\n"
     "}\n"
     "fn main() <reads> -> i32 { return f(E::A(20)) + f(E::A(1)) + f(E::C(2, 3)); }\n",
     26},
    {"match_as_initializer",
     "enum E { A(i32), B, }\n"
     "fn f(E e) <reads> -> i32 {\n"
     "  auto x = match (e) { E::A(v) => v, E::B => 4, };\n"
     "  return x + 1;\n"
     "}\n"
     "fn main() <reads> -> i32 { return f(E::B); }\n",
     5},
    {"closures",
     "fn inc1(i32 x) <pure> -> i32 { return x + 1; }\n"
     "fn apply(fn(i32) -> i32 f, i32 x) <pure> -> i32 { return f(x); }\n"
     "fn main() <writes> -> i32 {\n"
     "  auto f = [](i32 x) => x + 1;\n"
     "  i32 k = 10;\n"
     "  auto g = [k](i32 x) => x + k;\n"
     "  mut i32 n = 1;\n"
     "  auto h = [&n](i32 x) <writes> { n += x; };\n"
     "  h(2);\n"
     "  fn(i32) -> i32 fp = inc1;\n"
     "  return f(2) + g(5) + n + apply([](i32 y) => y * 2, 4) + fp(10);\n"
     "}\n",
     40},
};

static const char* unsupported_cases[] = {
    "effect Ask { fn ask() -> i32, }\nfn main() <Ask> -> i32 { return perform Ask::ask(); }\n",
};

int main(void) {
  for (u64 i = 0; i < sizeof(codegen_cases) / sizeof(codegen_cases[0]); ++i) {
    char c_path[512];
    char bin_path[512];
    snprintf(c_path, sizeof(c_path), "build/tests/codegen_%llu.c",
             (unsigned long long)i);
    snprintf(bin_path, sizeof(bin_path), "build/tests/codegen_%llu",
             (unsigned long long)i);
    if (!write_codegen(codegen_cases[i].source, c_path)) {
      failures++;
      fprintf(stderr, "FAIL: %s codegen\n", codegen_cases[i].name);
      continue;
    }
    checks++;
    if (!compile_c(c_path, bin_path)) {
      failures++;
      fprintf(stderr, "FAIL: %s compile\n", codegen_cases[i].name);
      continue;
    }
    int exit_code = -1;
    if (!run_binary(bin_path, &exit_code) || exit_code != codegen_cases[i].expected_exit) {
      failures++;
      fprintf(stderr, "FAIL: %s run (got %d, expected %d)\n", codegen_cases[i].name,
              exit_code, codegen_cases[i].expected_exit);
    }
  }

  CHECK(c_file_contains("build/tests/codegen_0.c", "TAKA_CONST"), "pure function attr");
  CHECK(c_file_contains("build/tests/codegen_0.c", "TAKA_PURE"), "reads function attr");
  CHECK(c_file_contains("build/tests/codegen_1.c", "__taka_arena"), "hidden arena param");
  CHECK(c_file_contains("build/tests/codegen_1.c", "taka_arena_new"), "arena runtime");

  for (u64 i = 0; i < sizeof(unsupported_cases) / sizeof(unsupported_cases[0]); ++i) {
    CHECK(codegen_fails(unsupported_cases[i]), "unsupported construct rejected");
  }

  CHECK(arena_debug_live_count() == 0, "leaked arenas");
  CHECK(map_debug_live_count() == 0, "leaked maps");

  if (failures != 0) {
    fprintf(stderr, "codegen tests FAILED: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("codegen test OK (%d checks, %llu compiled programs)\n", checks,
         (unsigned long long)(sizeof(codegen_cases) / sizeof(codegen_cases[0])));
  return 0;
}
