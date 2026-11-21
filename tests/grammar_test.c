#include "lexer/tokenizer.h"
#include "parser/parser.h"
#include "ast/ast.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Parsed {
  MemArena* arena;
  Tokenizer* tokens;
  Stmt* program;
} Parsed;

typedef struct SourceCase {
  const char* name;
  const char* source;
} SourceCase;

static int checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                  \
  do {                                                    \
    checks++;                                             \
    if (!(cond)) {                                        \
      failures++;                                         \
      fprintf(stderr, "FAIL: %s [%s:%d]\n", (msg), __FILE__, __LINE__); \
    }                                                     \
  } while (0)

static Tokenizer* tokenize_string(MemArena* arena, const char* source) {
  String8 text = str8(source);
  Tokenizer* tokenizer = tokenizer_create(arena, text.size + 1, text);
  tokenizer_tokenize(tokenizer);
  return tokenizer;
}

static Parsed parse_source(const char* source) {
  Parsed parsed = {0};
  parsed.arena = arena_create(MByte(8));
  parsed.tokens = tokenize_string(parsed.arena, source);
  for (u64 i = 0; i < parsed.tokens->token_count; ++i) {
    Token token = parsed.tokens->tokens[i];
    if (token.kind == TOKEN_INVALID || token.kind == TOKEN_UNKNOWN) {
      fprintf(stderr, "FAIL: lexer rejected token %.*s [%llu:%llu]\n",
              str8_fmt(token.value), (unsigned long long)token.line,
              (unsigned long long)token.column);
      failures++;
      break;
    }
  }
  parsed.program = parse_program(parsed.arena, parsed.tokens->tokens,
                                 parsed.tokens->token_count);
  return parsed;
}

static void parsed_free(Parsed* parsed) {
  if (parsed->arena) {
    arena_destroy(parsed->arena);
    parsed->arena = NULL;
  }
}

static void expect_token(MemArena* arena, const char* text, TokenKind kind) {
  checks++;
  Tokenizer* tokenizer = tokenize_string(arena, text);
  bool ok = tokenizer->token_count == 2 && tokenizer->tokens[0].kind == kind &&
            tokenizer->tokens[1].kind == TOKEN_EOF;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL: token '%.*s' -> %.*s, expected %.*s\n",
            str8_fmt(str8(text)),
            str8_fmt(tokenkind_to_string8(tokenizer->tokens[0].kind)),
            str8_fmt(tokenkind_to_string8(kind)));
  }
}

static void expect_invalid_token(MemArena* arena, const char* text) {
  checks++;
  Tokenizer* tokenizer = tokenize_string(arena, text);
  bool found = FALSE;
  for (u64 i = 0; i < tokenizer->token_count; ++i) {
    if (tokenizer->tokens[i].kind == TOKEN_INVALID) {
      found = TRUE;
      break;
    }
  }
  if (!found) {
    failures++;
    fprintf(stderr, "FAIL: expected an invalid token for '%s'\n", text);
  }
}

static Stmt* body_at(Stmt* block, u64 index) {
  if (!block || block->kind != STMT_BLOCK || !block->as.block.body) {
    return NULL;
  }
  return vec_at_ptr(block->as.block.body, index);
}

static Item* item_at(Stmt* program, u64 index) {
  Stmt* stmt = body_at(program, index);
  if (!stmt || stmt->kind != STMT_ITEM) {
    return NULL;
  }
  return stmt->as.item.item;
}

static Stmt* find_var(Stmt* block, const char* name) {
  if (!block || block->kind != STMT_BLOCK || !block->as.block.body) {
    return NULL;
  }
  String8 wanted = str8(name);
  u64 count = vec_len(block->as.block.body);
  for (u64 i = 0; i < count; ++i) {
    Stmt* stmt = vec_at_ptr(block->as.block.body, i);
    if (!stmt) {
      continue;
    }
    if (stmt->kind == STMT_VAR && str8_eq(stmt->as.var_expr.var_name, wanted)) {
      return stmt;
    }
    if (stmt->kind == STMT_BLOCK) {
      Stmt* nested = find_var(stmt, name);
      if (nested) {
        return nested;
      }
    }
  }
  return NULL;
}

static bool parse_fails(const char* source) {
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, 2);
      close(fd);
    }
    Parsed parsed = parse_source(source);
    parsed_free(&parsed);
    _exit(0);
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
    {"lex_comments",
     "// line comment\n/* block /* nested */ comment */\nfn f() { return; }\n"},
    {"lex_literals",
     "fn f() { i32 a = 42; i32 b = 1_000_000; i32 c = 0xFF_00; i32 d = 0b1010_0101; "
     "i32 e = 1i8; i32 h = 2u64; f32 x = 3.14; f32 y = 0.5; f32 z = 1e10; f32 w = 2.5f64; "
     "sview s = \"a\\n\"; char c1 = 'x'; char c2 = '\\n'; bool t = true; bool ff = false; "
     "auto p = null; [u8; 4] buf = undef; }\n"},
    {"lex_wildcard_pattern",
     "fn f(i32 x) -> i32 { return match (x) { _ => 0, }; }\n"},
    {"items_imports_visibility",
     "import foo;\nimport foo::bar::baz;\npub import a;\nprivate import b;\n"},
    {"items_type_aliases",
     "type A = i32;\npub type B<T> = List<T>;\nprivate type C = (i32, f32);\n"
     "type D = fn(i32, f32) <io> -> i32;\ntype E = dyn Shape*;\ntype F = [i32; 4];\n"
     "type G = extern fn(i32) -> i32;\ntype H = const i32*;\ntype I = i32&;\n"
     "type J = const sview&;\ntype K = (i32, f32,);\ntype L = List<i32,>;\n"
     "type M = dyn Trait<T>*;\n"},
    {"items_globals",
     "i32 a = 0;\npub mut i32 b = 1;\nprivate sview c = \"x\";\nauto d = 42;\n"
     "const i32 e = 2;\ni32* p = null;\n[u8; 8] buf = undef;\n"},
    {"items_structs",
     "struct Empty { }\nstruct Point { i32 x, i32 y, }\n"
     "pub struct P { pub i32 x, private i32 y, const i32* name, i32* ptr, i32** pp, "
     "[i32; 4] arr, fn(i32) -> i32 cb, dyn Shape* shape, i32& ref, }\n"
     "struct G<T> where T: Clone { T v, }\n"
     "struct B<T: Copy + Clone, U: Into<T>> { T a, U b, }\n"
     "struct GT2<T,> { T v, }\n"},
    {"items_enums",
     "enum Color { Red = 1, Green = 2, Blue = 4, }\n"
     "pub enum Shape { pub Circle(f32), private Rect { f32 w, f32 h, }, Empty, }\n"
     "enum G<T> where T: Clone { Yes(T), No, Other = 7, }\n"
     "enum Nested { A(i32, f32,), B { i32 x, bool on, }, C = 3, }\n"},
    {"items_effects",
     "effect Log { fn log(sview msg), fn level() -> i32, }\n"
     "pub effect E<T> { pub fn put(T v) -> T, private fn get() -> T, }\n"
     "effect Empty { }\n"},
    {"items_handlers",
     "effect Log { fn log(sview msg), fn level() -> i32, }\n"
     "handler ConsoleLog for Log { log(sview msg) => { print(msg); resume(); }, "
     "level() => resume(2), }\n"
     "handler H<T> for Log <Log> { return (i32 x) => x, }\n"
     "handler EmptyH for Log { }\n"},
    {"items_fn_sigs",
     "fn plain() { return; }\npub fn ret(i32 x) -> i32 { return x; }\n"
     "private fn eff() <io, alloc> -> i32 { return 0; }\nfn proto(i32 x) -> i32;\n"
     "extern fn c_fn(i32 x) -> i32;\nextern \"C\" fn c_void();\n"
     "extern \"C\" fn c_eff() <io>;\nfn trailing(i32 a, f32 b,) -> i32 { return a; }\n"
     "fn ret_ref(i32& x) -> i32& { return x; }\n"
     "fn ret_ptr(i32* x) -> i32* { return x; }\n"
     "fn proto_where<T>(T x) where T: Clone;\n"},
    {"items_generics_effects",
     "fn a<T>(T x) -> T { return x; }\n"
     "fn b<T: Clone>(T x) -> T { return x; }\n"
     "fn c<T, U: Clone + Copy, effect E>(T x, U y) <alloc, E> -> T { return x; }\n"
     "fn d<T>(T x) where T: Clone { return x; }\n"
     "fn e<T, U>(T x) where T: Clone + Copy, U: Into<T> { return; }\n"
     "fn f() <pure> { return; }\nfn g() <reads, writes, alloc,> { return; }\n"
     "fn h() <transacts> { return; }\nfn i() <decides> { return; }\n"
     "fn j() <suspends> { return; }\nfn k() <ownership> { return; }\n"
     "fn l() <Log> { return; }\nfn m() <alloc | e> { return; }\n"
     "fn n() <| e> { return; }\n"},
    {"items_type_showcase",
     "fn showcase(i8 a, i16 b, i32 c, i64 d, u8 e, u16 f, u32 g, u64 h, f32 i, f64 j, "
     "char k, bool l, sview m, never n, const i32 o, mut i32 p, i32* q, i32** r, "
     "const char* s, mut u8* t, List<i32> u, Map<i32, sview> v, (i32, f32) w, "
     "fn(i32) -> i32 x, fn(i32) <io> -> i32 y, fn(T) <E> -> U z, [i32; 4] aa, "
     "dyn Shape* ab, i32& ac, const i32& ad, i32*& ae) <pure> { return; }\n"},
    {"items_traits_impls",
     "trait Shape { fn area(Self& self) <reads> -> f32; fn name() -> sview; }\n"
     "trait Sub: Shape + Clone { fn x(Self* self); }\n"
     "trait GT<T> { fn get(Self& self) -> T; }\n"
     "trait TG { fn id<T>(T x) -> T; }\ntrait EmptyT { }\n"
     "struct Circle { f32 r }\n"
     "impl Shape for Circle { fn area(Circle& self) <reads> -> f32 { return self.r; } }\n"
     "struct W<T> { T v }\n"
     "impl<T> Wrapper<T> for W<T> where T: Clone { "
     "fn area(W<T>& self) <reads> -> f32 { return 0.0; } }\n"
     "enum EmptyEnum { }\nimpl Shape for EmptyEnum { }\n"},
    {"stmt_declarations",
     "fn f() { i32 x = 1; mut i32 y = 2, z = 3; auto w = 4; const i32 c = 5; "
     "i32* p = null; auto& r = x; const auto& cr = x; [u8; 4096] buf = undef; "
     "auto pt = Point { x: 1, y: 2, }; auto empty = Point { }; auto arr = [1, 2, 3]; "
     "auto arr2 = []; auto arr3 = [1, 2, 3,]; auto fill = [0; 256]; "
     "auto brace_arr = { 1, 2, 3 }; auto brace_empty = { }; "
     "auto blockv = { auto t = 1; t + 1 }; }\n"},
    {"stmt_assignments",
     "fn f() { mut i32 x = 0; i32* p = null; x = 1; x += 2; x -= 3; x *= 4; x /= 5; "
     "x %= 6; x <<= 1; x >>= 1; x &= 255; x |= 15; x ^= 7; xs[0] = 1; obj.field = 2; "
     "*p = 3; *p += 4; a.b.c = 5; }\n"},
    {"expr_calls_unary_postfix",
     "fn f() { f(); f(1, 2,); obj.method(3); xs[0] = f()[1].field; i++; i--; ++i; --i; "
     "auto a = +1; auto b = -2; auto c = !flag; auto d = ~mask; auto e = *ptr; "
     "auto f2 = &val; auto g = &&val; auto h = sizeof(i32); auto i2 = sizeof x; "
     "auto j = offsetof(Point, x); auto k = offsetof y; auto l = a as u64 as i32; }\n"},
    {"expr_binary_ternary",
     "fn f(i32 a, i32 b, i32 c) { auto r1 = a * b / c % 2; auto r2 = a + b - c; "
     "auto r3 = a << 2 >> 1; auto r4 = (a & b) ^ c | 255; auto r5 = (a == b); "
     "auto r6 = (a != b); auto r7 = (a <= b); auto r8 = (a >= b); auto r9 = (a > b); "
     "auto r10 = (a && b) || c; auto r11 = a ? 1 : b ? 2 : 3; "
     "auto r12 = (a + b) * (c - 1) > 10 && flag || done; }\n"},
    {"stmt_if_while",
     "fn f(i32 x) { if (x > 0) { print(x); } if (x > 0) { print(1); } else { print(0); } "
     "if (x == 1) { print(1); } else if (x == 2) { print(2); } else { print(3); } "
     "if (auto idx = find(&list, 7)) { print(idx); } else { print(0); } "
     "if (mut y = maybe()) { print(y); } while (x < 10) { x++; } "
     "while (auto v = next()) { print(v); } }\n"},
    {"stmt_for",
     "fn f() { for (mut u64 i = 0; i < 10; i++) { print(i); } "
     "for (i = 0; i < 5; i += 1) { print(i); } for (;;) { break; } "
     "for (; cond;) { continue; } for (v in list) { print(v); } "
     "for (Shape::Circle(r) in shapes) { print(r); } for (&x in xs) { x += 1; } "
     "for (auto y in ys) { print(y); } "
     "outer: for (;;) { inner: while (cond) { break outer; continue inner; } } }\n"},
    {"stmt_labels_defer_transact",
     "fn f() { outer: for (auto x in xs) { inner: while (flag) { if (done) { break; } "
     "continue; } } l2: { defer cleanup(); defer { unlock(); } "
     "transact { a.balance -= 1; b.balance += 1; } } return; }\n"},
    {"expr_match_patterns",
     "fn f(Shape s) -> i32 { return match (s) { Shape::Circle(r,) if r > 10.0 => 2, "
     "Shape::Circle(_) | Shape::Empty => 1, Shape::Rect { w, h } => 2, "
     "Shape::Rect { w, .. } => 0, Shape::Rect { .. } => 0, "
     "Shape::Rect { w: width, h } => 1, _ => 99, 42 => 100, -1 => 101, mut x => x, "
     "&r => 102, null => 103, true => 104, false => 105, undef => 106, 'c' => 107, "
     "\"s\" => 108, 1.5 => 109, Shape::Empty() => 110, }; }\n"},
    {"expr_if_match",
     "fn f(i32 x) -> i32 { auto y = if (x > 0) { 1 } else if (x < 0) { -1 } else { 0 }; "
     "auto z = match (x) { 0 => 10, _ => 20, }; return y + z; }\n"},
    {"expr_effects",
     "effect Ask { fn ask() -> i32, }\n"
     "handler Answer for Ask { ask() => resume(42), }\n"
     "fn uses() <Ask, alloc> -> i32 { perform Ask::ask(); auto v = perform Ask::ask(); "
     "if (perform Ask::level() > 1) { perform Ask::log(\"x\"); } return v; }\n"
     "fn handles() <io> { i32 r = handle work(21) with ConsoleLog; "
     "i32 q = handle work(1) with { log(sview m) => resume(), level() => resume(0), }; "
     "i32 t = handle { perform Ask::ask(); } with Answer; "
     "handle work(2) with { ask() => resume(1), }; spawn background_job(); "
     "spawn { do_work(); } return; }\n"
     "fn tr(i64 n) <transacts> { transact { a.balance -= n; b.balance += n; } }\n"},
    {"expr_lambdas",
     "fn f() <io> { auto a = [](i32 x) => x + 1; i32 k = 10; "
     "auto b = [k](i32 x) => x + k; auto c = [&k](i32 x) <writes> { k += x; }; "
     "auto d = [k = 32, mut y = 4,](auto z) -> i32 { return k + y + z; }; "
     "auto e = [](auto x, mut i32 y) { return x + y; }; auto g = []() <pure> => 1; }\n"},
    {"expr_dyn_refs",
     "fn f(Circle c) <alloc> { dyn Shape* s = &c as dyn Shape*; "
     "dyn Shape& r = c as dyn Shape&; auto a = s.area(); i32& ref = c.r; "
     "i32*& pref = ptr; auto p = Point { x: 1, y: 2 }; auto q = Point { }; "
     "auto arr = [1, 2, 3]; auto fill = [0; 16]; return; }\n"},
    {"expr_sizes_offsets",
     "fn f() { auto a = sizeof(i32); auto b = sizeof(i32*); auto c = sizeof(x + 1); "
     "auto d = offsetof(Point, x); }\n"},
    {"expr_block_tails",
     "fn f(i32 x) -> i32 { if (x > 0) { 1 } else { 2 } }\n"
     "fn g() { { auto a = 1; a + 1 } }\n"
     "fn h() { match (1) { _ => 2 } }\n"
     "fn i() { transact { x += 1; } }\n"},
    {"types_nested_turbofish",
     "fn f() { List<List<List<i32>>> xs = f::<List<List<i32>>>(g::<i32>()); "
     "Map<i32, fn(i32) <io> -> List<u8>> m = x; }\n"},
};

static const SourceCase invalid_cases[] = {
    {"empty_effect_row", "fn f() < > { return; }\n"},
    {"pure_mixed", "fn f() <pure, io> { return; }\n"},
    {"transacts_mixed", "fn f() <transacts, reads> { return; }\n"},
    {"suspends_decides", "fn f() <suspends, decides> { return; }\n"},
    {"rvalue_reference", "fn f(T&& x) { return; }\n"},
    {"pointer_to_reference", "fn f(T&* x) { return; }\n"},
    {"comparison_chain", "fn f() { auto x = a < b < c; }\n"},
    {"assignment_condition", "fn f() { if (x = 5) { return; } }\n"},
    {"missing_initializer", "fn f() { i32 x; }\n"},
    {"missing_semicolon", "fn f() { i32 x = 1 }\n"},
    {"auto_pointer", "fn f() { auto* p = null; }\n"},
    {"auto_fn_param", "fn f(auto x) { return; }\n"},
    {"auto_alias", "type A = auto;\n"},
    {"empty_tuple_type", "type A = ();\n"},
    {"empty_generic_args", "type A = List<>;\n"},
    {"empty_generics", "struct S<> { }\n"},
    {"struct_missing_comma", "struct S { i32 x i32 y }\n"},
    {"enum_missing_comma", "enum E { A B }\n"},
    {"param_missing_comma", "fn f(i32 a i32 b) { return; }\n"},
    {"effect_missing_comma", "effect E { fn a() fn b() }\n"},
    {"handler_missing_comma", "handler H for E { a() => 1 b() => 2 }\n"},
    {"spawn_non_call", "fn f() { spawn f; }\n"},
    {"resume_outside_handler", "fn f() { resume(1); }\n"},
    {"defer_declaration", "fn f() { defer i32 x = 1; }\n"},
    {"for_missing_header_semicolons", "fn f() { for (i32 x = 1) { } }\n"},
    {"if_missing_block", "fn f() { if (x) y; }\n"},
    {"wildcard_expression", "fn f() { _ = 1; }\n"},
    {"match_missing_comma", "fn f() { match (x) { A => 1 B => 2 } }\n"},
    {"float_missing_fraction", "fn f() { auto x = 1. ; }\n"},
    {"hex_missing_digits", "fn f() { auto x = 0x; }\n"},
    {"binary_missing_digits", "fn f() { auto x = 0b2; }\n"},
    {"bad_exponent", "fn f() { auto x = 1e; }\n"},
    {"break_missing_semicolon", "fn f() { break label }\n"},
    {"return_missing_semicolon", "fn f() { return 1 }\n"},
    {"transact_missing_block", "fn f() { transact }\n"},
    {"handle_missing_handler", "fn f() { handle x with }\n"},
    {"perform_missing_path", "fn f() { perform; }\n"},
    {"sizeof_empty", "fn f() { sizeof(); }\n"},
    {"offsetof_missing_field", "fn f() { offsetof(i32); }\n"},
    {"auto_struct_field", "struct S { auto x }\n"},
    {"missing_return_type", "fn f() -> { }\n"},
    {"mixed_comparison", "fn f() { 1 < 2 == true; }\n"},
    {"mod_bodies_reserved", "mod foo;\n"},
};

static void test_token_tables(void) {
  MemArena* arena = arena_create(MByte(4));

  static const struct {
    const char* text;
    TokenKind kind;
  } keywords[] = {
      {"fn", TOKEN_FN},         {"struct", TOKEN_STRUCT},   {"enum", TOKEN_ENUM},
      {"effect", TOKEN_EFFECT}, {"handler", TOKEN_HANDLER}, {"trait", TOKEN_TRAIT},
      {"impl", TOKEN_IMPL},     {"dyn", TOKEN_DYN},         {"type", TOKEN_TYPE},
      {"extern", TOKEN_EXTERN}, {"import", TOKEN_IMPORT},   {"mod", TOKEN_MOD},
      {"pub", TOKEN_PUB},       {"private", TOKEN_PRIVATE}, {"const", TOKEN_CONST},
      {"mut", TOKEN_MUT},       {"auto", TOKEN_AUTO},       {"where", TOKEN_WHERE},
      {"for", TOKEN_FOR},       {"in", TOKEN_IN},           {"if", TOKEN_IF},
      {"else", TOKEN_ELSE},     {"while", TOKEN_WHILE},     {"match", TOKEN_MATCH},
      {"break", TOKEN_BREAK},   {"continue", TOKEN_CONTINUE},
      {"return", TOKEN_RETURN}, {"defer", TOKEN_DEFER},     {"perform", TOKEN_PERFORM},
      {"handle", TOKEN_HANDLE}, {"with", TOKEN_WITH},       {"resume", TOKEN_RESUME},
      {"transact", TOKEN_TRANSACT}, {"spawn", TOKEN_SPAWN}, {"as", TOKEN_AS},
      {"sizeof", TOKEN_SIZEOF}, {"offsetof", TOKEN_OFFSETOF},
      {"true", TOKEN_TRUE},     {"false", TOKEN_FALSE},     {"null", TOKEN_NULL},
      {"undef", TOKEN_UNDEF},   {"Self", TOKEN_SELF},       {"_", TOKEN_WILDCARD},
      {"i8", TOKEN_I8},         {"i16", TOKEN_I16},         {"i32", TOKEN_I32},
      {"i64", TOKEN_I64},       {"u8", TOKEN_U8},           {"u16", TOKEN_U16},
      {"u32", TOKEN_U32},       {"u64", TOKEN_U64},         {"f32", TOKEN_F32},
      {"f64", TOKEN_F64},       {"char", TOKEN_CHAR_TYPE},  {"bool", TOKEN_BOOL},
      {"sview", TOKEN_SVIEW},   {"never", TOKEN_NEVER},
  };
  for (u64 i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
    expect_token(arena, keywords[i].text, keywords[i].kind);
  }

  static const struct {
    const char* text;
    TokenKind kind;
  } operators[] = {
      {"[", TOKEN_OPEN_BRACKET}, {"]", TOKEN_CLOSE_BRACKET},
      {"{", TOKEN_OPEN_CURLY},   {"}", TOKEN_CLOSE_CURLY},
      {"(", TOKEN_OPEN_PAREN},   {")", TOKEN_CLOSE_PAREN},
      {"=", TOKEN_ASSIGNMENT},   {"==", TOKEN_EQUALS},
      {"!=", TOKEN_NOT_EQUALS},  {"!", TOKEN_NOT},
      {"<", TOKEN_LESS},         {"<=", TOKEN_LESS_EQUALS},
      {">", TOKEN_GREATER},      {">=", TOKEN_GREATER_EQUALS},
      {"+", TOKEN_PLUS},         {"++", TOKEN_PLUS_PLUS},
      {"-", TOKEN_MINUS},        {"--", TOKEN_MINUS_MINUS},
      {"*", TOKEN_STAR},         {"/", TOKEN_SLASH},
      {"%", TOKEN_PERCENT},      {"+=", TOKEN_PLUS_EQUALS},
      {"-=", TOKEN_MINUS_EQUALS}, {"*=", TOKEN_STAR_EQUALS},
      {"/=", TOKEN_SLASH_EQUALS}, {"%=", TOKEN_PERCENT_EQUALS},
      {"&", TOKEN_AMP},          {"&=", TOKEN_AMP_EQUALS},
      {"|", TOKEN_PIPE},         {"|=", TOKEN_PIPE_EQUALS},
      {"^", TOKEN_CARET},        {"^=", TOKEN_CARET_EQUALS},
      {"~", TOKEN_TILDE},        {"<<", TOKEN_LESS_LESS},
      {">>", TOKEN_GREATER_GREATER},
      {"<<=", TOKEN_LESS_LESS_EQUALS},
      {">>=", TOKEN_GREATER_GREATER_EQUALS},
      {"&&", TOKEN_AMP_AMP},     {"||", TOKEN_PIPE_PIPE},
      {"?", TOKEN_QUESTION},     {",", TOKEN_COMMA},
      {";", TOKEN_SEMICOLON},    {":", TOKEN_COLON},
      {".", TOKEN_DOT},          {"..", TOKEN_DOT_DOT},
      {"::", TOKEN_COLON_COLON}, {"->", TOKEN_ARROW},
      {"=>", TOKEN_FAT_ARROW},
  };
  for (u64 i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i) {
    expect_token(arena, operators[i].text, operators[i].kind);
  }

  expect_invalid_token(arena, "\"unterminated");
  expect_invalid_token(arena, "'x");
  expect_invalid_token(arena, "/* unterminated");
  expect_invalid_token(arena, "/* nested /* still open */");
  expect_invalid_token(arena, "@");
  expect_invalid_token(arena, "0x");
  expect_invalid_token(arena, "0b");
  expect_invalid_token(arena, "0x_");
  expect_invalid_token(arena, "0b_");

  arena_destroy(arena);
}

static void test_lexer_robustness(void) {
  u64 rng = 0x9e3779b97f4a7c15ULL;
  for (u32 iteration = 0; iteration < 256; ++iteration) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    u64 size = rng % 512;
    u8 data[512];
    for (u64 i = 0; i < size; ++i) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      data[i] = (u8)(rng & 0xff);
    }
    MemArena* arena = arena_create(MByte(1));
    Tokenizer* tokenizer = tokenizer_create(arena, size + 1, (String8){data, size});
    tokenizer_tokenize(tokenizer);
    CHECK(tokenizer->token_count > 0, "fuzz lexer produced no tokens");
    CHECK(tokenizer->tokens[tokenizer->token_count - 1].kind == TOKEN_EOF,
          "fuzz lexer did not terminate with EOF");
    arena_destroy(arena);
  }
}

static void test_ast_shapes(void) {
  {
    Parsed p = parse_source(
        "fn f() { auto x = 1 + 2 * 3; mut i32 y = 4; i32& r = y; "
        "const i32* q = null; }\n");
    Item* fn = item_at(p.program, 0);
    CHECK(fn && fn->kind == ITEM_FN, "var test function item");
    Stmt* body = fn ? fn->as.fn.body : NULL;
    Stmt* x = find_var(body, "x");
    CHECK(x && x->kind == STMT_VAR, "var x found");
    CHECK(x && x->as.var_expr.is_auto, "auto var flag");
    CHECK(x && x->as.var_expr.type && x->as.var_expr.type->kind == TYPE_AUTO,
          "auto var type");
    CHECK(x && x->as.var_expr.assigned_value.kind == EXPR_BINARY, "binary initializer");
    CHECK(x && x->as.var_expr.assigned_value.as.bin.oper.kind == TOKEN_PLUS,
          "addition root");
    CHECK(x && x->as.var_expr.assigned_value.as.bin.rhs->kind == EXPR_BINARY &&
              x->as.var_expr.assigned_value.as.bin.rhs->as.bin.oper.kind == TOKEN_STAR,
          "multiplication binds tighter");
    Stmt* y = find_var(body, "y");
    CHECK(y && y->as.var_expr.is_mut && !y->as.var_expr.is_const, "mut var flags");
    CHECK(y && y->as.var_expr.type && y->as.var_expr.type->kind == TYPE_PRIMITIVE &&
              str8_eq(y->as.var_expr.type->as.primitive.name, str8_lit("i32")),
          "mut var type");
    Stmt* r = find_var(body, "r");
    CHECK(r && r->as.var_expr.type && r->as.var_expr.type->is_reference,
          "reference type");
    Stmt* q = find_var(body, "q");
    CHECK(q && q->as.var_expr.type && q->as.var_expr.type->is_const &&
              q->as.var_expr.type->pointer_depth == 1,
          "const pointer type");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { i32 a = 0xFF_00; i32 b = 0b1010_0101; i32 c = 1_000_000; "
        "i32 d = 12i8; f32 e = 1e10; f32 g = 2.5f64; f32 h = 3.0f32; "
        "sview s = \"hi\"; char ch = 'x'; bool bt = true; auto n = null; "
        "[u8; 2] u = undef; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* body = fn ? fn->as.fn.body : NULL;
    Stmt* a = find_var(body, "a");
    Stmt* b = find_var(body, "b");
    Stmt* c = find_var(body, "c");
    Stmt* d = find_var(body, "d");
    Stmt* e = find_var(body, "e");
    Stmt* g = find_var(body, "g");
    Stmt* h = find_var(body, "h");
    Stmt* s = find_var(body, "s");
    Stmt* ch = find_var(body, "ch");
    Stmt* bt = find_var(body, "bt");
    Stmt* n = find_var(body, "n");
    Stmt* u = find_var(body, "u");
    CHECK(a && a->as.var_expr.assigned_value.kind == EXPR_NUMBER &&
              a->as.var_expr.assigned_value.as.number.value == 0xFF00,
          "hex literal value");
    CHECK(b && b->as.var_expr.assigned_value.kind == EXPR_NUMBER &&
              b->as.var_expr.assigned_value.as.number.value == 0b10100101,
          "binary literal value");
    CHECK(c && c->as.var_expr.assigned_value.kind == EXPR_NUMBER &&
              c->as.var_expr.assigned_value.as.number.value == 1000000,
          "underscored literal value");
    CHECK(d && d->as.var_expr.assigned_value.kind == EXPR_NUMBER &&
              d->as.var_expr.assigned_value.as.number.value == 12,
          "suffixed integer value");
    CHECK(e && e->as.var_expr.assigned_value.kind == EXPR_FLOAT &&
              e->as.var_expr.assigned_value.as.float_lit.value == 1e10,
          "exponent float value");
    CHECK(g && g->as.var_expr.assigned_value.kind == EXPR_FLOAT &&
              g->as.var_expr.assigned_value.as.float_lit.value == 2.5,
          "suffixed float value");
    CHECK(h && h->as.var_expr.assigned_value.kind == EXPR_FLOAT &&
              h->as.var_expr.assigned_value.as.float_lit.value == 3.0,
          "f32 float value");
    CHECK(s && s->as.var_expr.assigned_value.kind == EXPR_STRING, "string literal");
    CHECK(ch && ch->as.var_expr.assigned_value.kind == EXPR_CHAR, "char literal");
    CHECK(bt && bt->as.var_expr.assigned_value.kind == EXPR_BOOL &&
              bt->as.var_expr.assigned_value.as.boolean.value,
          "bool literal");
    CHECK(n && n->as.var_expr.assigned_value.kind == EXPR_NULL, "null literal");
    CHECK(u && u->as.var_expr.assigned_value.kind == EXPR_UNDEF, "undef literal");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "pub fn f<T: Clone, effect E>(i32 a, mut f32& b) <alloc, E> -> List<T> "
        "where T: Clone { return; }\n");
    Item* fn = item_at(p.program, 0);
    CHECK(fn && fn->kind == ITEM_FN && fn->visibility == VIS_PUB, "fn visibility");
    CHECK(fn && fn->as.fn.sig.generics && vec_len(fn->as.fn.sig.generics) == 2,
          "fn generics");
    GenericParam* g0 =
        fn && fn->as.fn.sig.generics ? vec_at_ptr(fn->as.fn.sig.generics, 0) : NULL;
    GenericParam* g1 =
        fn && fn->as.fn.sig.generics ? vec_at_ptr(fn->as.fn.sig.generics, 1) : NULL;
    CHECK(g0 && !g0->is_effect && g0->bound != NULL, "type generic bound");
    CHECK(g1 && g1->is_effect, "effect generic");
    CHECK(fn && vec_len(fn->as.fn.sig.params) == 2, "fn param count");
    Param* p1 = fn && vec_len(fn->as.fn.sig.params) == 2
                    ? vec_at_ptr(fn->as.fn.sig.params, 1)
                    : NULL;
    CHECK(p1 && p1->is_mut && p1->type->is_reference, "mut reference param");
    CHECK(fn && fn->as.fn.sig.effects && vec_len(fn->as.fn.sig.effects->items) == 2,
          "fn effect row");
    CHECK(fn && fn->as.fn.sig.return_type &&
              fn->as.fn.sig.return_type->kind == TYPE_PATH &&
              fn->as.fn.sig.return_type->as.path.generic_args &&
              vec_len(fn->as.fn.sig.return_type->as.path.generic_args) == 1,
          "generic return type");
    CHECK(fn && fn->as.fn.sig.where &&
              vec_len(fn->as.fn.sig.where->constraints) == 1,
          "where clause");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "pub struct S<T> where T: Clone { pub i32 x, private T y, const i32* z, }\n");
    Item* item = item_at(p.program, 0);
    CHECK(item && item->kind == ITEM_STRUCT && item->visibility == VIS_PUB,
          "struct item");
    CHECK(item && item->as.struct_decl.generics &&
              vec_len(item->as.struct_decl.generics) == 1,
          "struct generics");
    CHECK(item && item->as.struct_decl.where &&
              vec_len(item->as.struct_decl.where->constraints) == 1,
          "struct where");
    CHECK(item && vec_len(item->as.struct_decl.fields) == 3, "struct field count");
    StructField* f0 = item && vec_len(item->as.struct_decl.fields) > 0
                          ? vec_at_ptr(item->as.struct_decl.fields, 0)
                          : NULL;
    StructField* f1 = item && vec_len(item->as.struct_decl.fields) > 1
                          ? vec_at_ptr(item->as.struct_decl.fields, 1)
                          : NULL;
    StructField* f2 = item && vec_len(item->as.struct_decl.fields) > 2
                          ? vec_at_ptr(item->as.struct_decl.fields, 2)
                          : NULL;
    CHECK(f0 && f0->visibility == VIS_PUB && f0->type->kind == TYPE_PRIMITIVE,
          "public primitive field");
    CHECK(f1 && f1->visibility == VIS_PRIVATE && f1->type->kind == TYPE_PATH,
          "private path field");
    CHECK(f2 && f2->type->is_const && f2->type->pointer_depth == 1,
          "const pointer field");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "enum E { A, B(i32, f32), C { i32 x, bool on }, D = 4, }\n");
    Item* item = item_at(p.program, 0);
    CHECK(item && item->kind == ITEM_ENUM, "enum item");
    CHECK(item && vec_len(item->as.enum_decl.variants) == 4, "enum variant count");
    EnumVariant* a = item ? vec_at_ptr(item->as.enum_decl.variants, 0) : NULL;
    EnumVariant* b = item ? vec_at_ptr(item->as.enum_decl.variants, 1) : NULL;
    EnumVariant* c = item ? vec_at_ptr(item->as.enum_decl.variants, 2) : NULL;
    EnumVariant* d = item ? vec_at_ptr(item->as.enum_decl.variants, 3) : NULL;
    CHECK(a && a->kind == VARIANT_UNIT, "unit variant");
    CHECK(b && b->kind == VARIANT_TUPLE && vec_len(b->types) == 2, "tuple variant");
    CHECK(c && c->kind == VARIANT_STRUCT && vec_len(c->fields) == 2, "struct variant");
    CHECK(d && d->kind == VARIANT_VALUE && d->value &&
              d->value->kind == EXPR_NUMBER && d->value->as.number.value == 4,
          "value variant");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "effect Log { fn log(sview m), fn level() -> i32, }\n"
        "handler H for Log <Log> { log(sview m) => resume(), "
        "return (i32 x) => x, }\n");
    Item* effect = item_at(p.program, 0);
    Item* handler = item_at(p.program, 1);
    CHECK(effect && effect->kind == ITEM_EFFECT, "effect item");
    CHECK(effect && vec_len(effect->as.effect_decl.ops) == 2, "effect op count");
    EffectOp* level =
        effect && vec_len(effect->as.effect_decl.ops) == 2
            ? vec_at_ptr(effect->as.effect_decl.ops, 1)
            : NULL;
    CHECK(level && level->return_type && level->return_type->kind == TYPE_PRIMITIVE,
          "effect op return type");
    CHECK(handler && handler->kind == ITEM_HANDLER, "handler item");
    CHECK(handler && handler->as.handler_decl.effects &&
              vec_len(handler->as.handler_decl.effects->items) == 1,
          "handler effect row");
    CHECK(handler && vec_len(handler->as.handler_decl.ops) == 2, "handler op count");
    HandlerOp* ret = handler && vec_len(handler->as.handler_decl.ops) == 2
                         ? vec_at_ptr(handler->as.handler_decl.ops, 1)
                         : NULL;
    CHECK(ret && ret->is_return && ret->expr != NULL, "handler return op");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "trait Shape { fn area(Self& self) <reads> -> f32; }\n"
        "struct C { f32 r }\n"
        "impl Shape for C { fn area(C& self) <reads> -> f32 { return self.r; } }\n");
    Item* trait = item_at(p.program, 0);
    Item* impl = item_at(p.program, 2);
    CHECK(trait && trait->kind == ITEM_TRAIT && vec_len(trait->as.trait_decl.methods) == 1,
          "trait methods");
    CHECK(impl && impl->kind == ITEM_IMPL && vec_len(impl->as.impl_decl.methods) == 1,
          "impl methods");
    CHECK(impl && impl->as.impl_decl.for_type &&
              impl->as.impl_decl.for_type->kind == TYPE_PATH,
          "impl target type");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { auto a = [&k, v = 3, mut w = 4](auto x, mut i32 y) <writes> -> i32 "
        "{ return x; }; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* a = fn ? find_var(fn->as.fn.body, "a") : NULL;
    CHECK(a && a->as.var_expr.assigned_value.kind == EXPR_LAMBDA, "lambda expression");
    Expr* lambda = a ? &a->as.var_expr.assigned_value : NULL;
    CHECK(lambda && vec_len(lambda->as.lambda.captures) == 3, "lambda captures");
    Capture* c0 = lambda ? vec_at_ptr(lambda->as.lambda.captures, 0) : NULL;
    Capture* c1 = lambda ? vec_at_ptr(lambda->as.lambda.captures, 1) : NULL;
    Capture* c2 = lambda ? vec_at_ptr(lambda->as.lambda.captures, 2) : NULL;
    CHECK(c0 && c0->by_ref && str8_eq(c0->name, str8_lit("k")), "reference capture");
    CHECK(c1 && c1->has_init && str8_eq(c1->name, str8_lit("v")), "init capture");
    CHECK(c2 && c2->is_mut && c2->has_init, "mut init capture");
    CHECK(lambda && vec_len(lambda->as.lambda.params) == 2, "lambda params");
    Param* lp0 = lambda ? vec_at_ptr(lambda->as.lambda.params, 0) : NULL;
    Param* lp1 = lambda ? vec_at_ptr(lambda->as.lambda.params, 1) : NULL;
    CHECK(lp0 && lp0->type->kind == TYPE_AUTO, "lambda auto param");
    CHECK(lp1 && lp1->is_mut && lp1->type->kind == TYPE_PRIMITIVE, "lambda mut param");
    CHECK(lambda && lambda->as.lambda.effects &&
              vec_len(lambda->as.lambda.effects->items) == 1,
          "lambda effect row");
    CHECK(lambda && lambda->as.lambda.return_type &&
              lambda->as.lambda.return_type->kind == TYPE_PRIMITIVE,
          "lambda return type");
    CHECK(lambda && lambda->as.lambda.body_is_block, "lambda block body");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { auto t = a ? 1 : b ? 2 : 3; auto c = x as u64 as i32; "
        "auto arr = [0; 4]; auto l = [&x, y = 1, mut z = 2,]() => 1; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* t = fn ? find_var(fn->as.fn.body, "t") : NULL;
    Stmt* c = fn ? find_var(fn->as.fn.body, "c") : NULL;
    Stmt* arr = fn ? find_var(fn->as.fn.body, "arr") : NULL;
    Stmt* l = fn ? find_var(fn->as.fn.body, "l") : NULL;
    Expr* ternary = t ? &t->as.var_expr.assigned_value : NULL;
    CHECK(ternary && ternary->kind == EXPR_TERNARY &&
              ternary->as.ternary.else_expr->kind == EXPR_TERNARY,
          "right-associative ternary");
    Expr* cast = c ? &c->as.var_expr.assigned_value : NULL;
    CHECK(cast && cast->kind == EXPR_CAST && cast->as.cast.expr &&
              cast->as.cast.expr->kind == EXPR_CAST,
          "chained cast");
    Expr* fill = arr ? &arr->as.var_expr.assigned_value : NULL;
    CHECK(fill && fill->kind == EXPR_ARRAY_LIT && fill->as.array_lit.is_fill,
          "array fill literal");
    Expr* lambda = l ? &l->as.var_expr.assigned_value : NULL;
    CHECK(lambda && lambda->kind == EXPR_LAMBDA &&
              vec_len(lambda->as.lambda.captures) == 3,
          "lambda captured trailing comma");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f(S s) -> i32 { return match (s) { S::A(x) | S::B => 1, "
        "S::C { f, .. } => 2, _ => 3, mut y => 4, &r => 5, 42 => 6, }; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* ret = fn ? body_at(fn->as.fn.body, 0) : NULL;
    CHECK(ret && ret->kind == STMT_RETURN && ret->as.return_stmt.has_value,
          "return match");
    Expr* match = ret ? ret->as.return_stmt.value : NULL;
    CHECK(match && match->kind == EXPR_MATCH, "match expression");
    CHECK(match && vec_len(match->as.match.arms) == 6, "match arm count");
    MatchArm* arm0 = match ? vec_at_ptr(match->as.match.arms, 0) : NULL;
    MatchArm* arm1 = match ? vec_at_ptr(match->as.match.arms, 1) : NULL;
    MatchArm* arm2 = match ? vec_at_ptr(match->as.match.arms, 2) : NULL;
    MatchArm* arm3 = match ? vec_at_ptr(match->as.match.arms, 3) : NULL;
    MatchArm* arm4 = match ? vec_at_ptr(match->as.match.arms, 4) : NULL;
    MatchArm* arm5 = match ? vec_at_ptr(match->as.match.arms, 5) : NULL;
    CHECK(arm0 && arm0->pattern->kind == PAT_ALT &&
              vec_len(arm0->pattern->as.alt.alternatives) == 2,
          "pattern alternatives");
    Pattern* tuple = arm0 ? vec_at_ptr(arm0->pattern->as.alt.alternatives, 0) : NULL;
    CHECK(tuple && tuple->kind == PAT_PATH && tuple->as.path.has_tuple,
          "tuple pattern");
    CHECK(arm1 && arm1->pattern->kind == PAT_PATH && arm1->pattern->as.path.has_struct,
          "struct pattern");
    CHECK(arm2 && arm2->pattern->kind == PAT_WILDCARD, "wildcard pattern");
    CHECK(arm3 && arm3->pattern->kind == PAT_BIND && arm3->pattern->as.bind.is_mut,
          "mut binding pattern");
    CHECK(arm4 && arm4->pattern->kind == PAT_REF_BIND, "reference pattern");
    CHECK(arm5 && arm5->pattern->kind == PAT_LITERAL, "literal pattern");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { for (mut i32 i = 0; i < 10; i++) { } for (x in xs) { } }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* for_c = fn ? body_at(fn->as.fn.body, 0) : NULL;
    Stmt* for_each = fn ? body_at(fn->as.fn.body, 1) : NULL;
    CHECK(for_c && for_c->kind == STMT_FOR && !for_c->as.for_stmt.for_stmt.is_each,
          "c-style for");
    CHECK(for_c && for_c->as.for_stmt.for_stmt.init &&
              for_c->as.for_stmt.for_stmt.init->kind == STMT_VAR,
          "for init declaration");
    CHECK(for_c && for_c->as.for_stmt.for_stmt.cond &&
              for_c->as.for_stmt.for_stmt.cond->kind == EXPR_BINARY,
          "for condition");
    CHECK(for_c && for_c->as.for_stmt.for_stmt.step != NULL, "for step");
    CHECK(for_each && for_each->kind == STMT_FOR &&
              for_each->as.for_stmt.for_stmt.is_each,
          "for each");
    CHECK(for_each && for_each->as.for_stmt.for_stmt.pattern &&
              for_each->as.for_stmt.for_stmt.pattern->kind == PAT_BIND,
          "for each pattern");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { defer g(); defer { h(); } transact { x += 1; } }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* d0 = fn ? body_at(fn->as.fn.body, 0) : NULL;
    Stmt* d1 = fn ? body_at(fn->as.fn.body, 1) : NULL;
    CHECK(d0 && d0->kind == STMT_DEFER && !d0->as.defer_stmt.is_block, "simple defer");
    CHECK(d1 && d1->kind == STMT_DEFER && d1->as.defer_stmt.is_block, "block defer");
    Expr* tail = fn ? fn->as.fn.body->as.block.tail : NULL;
    CHECK(tail && tail->kind == EXPR_TRANSACT, "transact tail expression");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source(
        "fn f() { i32 a = handle work() with H; "
        "i32 b = handle w() with { op() => resume(), }; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* a = fn ? find_var(fn->as.fn.body, "a") : NULL;
    Stmt* b = fn ? find_var(fn->as.fn.body, "b") : NULL;
    CHECK(a && a->as.var_expr.assigned_value.kind == EXPR_HANDLE &&
              a->as.var_expr.assigned_value.as.handle.handler_is_path,
          "handle with named handler");
    CHECK(b && b->as.var_expr.assigned_value.kind == EXPR_HANDLE &&
              !b->as.var_expr.assigned_value.as.handle.handler_is_path &&
              vec_len(b->as.var_expr.assigned_value.as.handle.handler_ops) == 1,
          "handle with inline handler");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source("fn f() { List<List<i32>> xs = null; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* xs = fn ? find_var(fn->as.fn.body, "xs") : NULL;
    Type* outer = xs ? xs->as.var_expr.type : NULL;
    Type* inner = NULL;
    if (outer && outer->kind == TYPE_PATH && outer->as.path.generic_args &&
        vec_len(outer->as.path.generic_args) > 0) {
      inner = vec_at_ptr(outer->as.path.generic_args, 0);
    }
    CHECK(outer && outer->kind == TYPE_PATH && inner && inner->kind == TYPE_PATH &&
              inner->as.path.generic_args &&
              vec_len(inner->as.path.generic_args) == 1,
          "nested generic type");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source("fn f() { auto x = f::<List<i32>>(1); }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* x = fn ? find_var(fn->as.fn.body, "x") : NULL;
    Expr* call = x ? &x->as.var_expr.assigned_value : NULL;
    CHECK(call && call->kind == EXPR_CALL && call->as.call.callee &&
              call->as.call.callee->kind == EXPR_PATH &&
              call->as.call.callee->as.path.has_turbofish &&
              vec_len(call->as.call.callee->as.path.generic_args) == 1,
          "turbofish call");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source("fn f() { i32 a = 1, b = 2; }\n");
    Item* fn = item_at(p.program, 0);
    Stmt* a = fn ? find_var(fn->as.fn.body, "a") : NULL;
    Stmt* b = fn ? find_var(fn->as.fn.body, "b") : NULL;
    CHECK(a && b, "comma declaration variables");
    parsed_free(&p);
  }

  {
    Parsed p = parse_source("fn f() <alloc | e> { return; }\n");
    Item* fn = item_at(p.program, 0);
    CHECK(fn && fn->as.fn.sig.effects && fn->as.fn.sig.effects->has_tail &&
              str8_eq(fn->as.fn.sig.effects->tail, str8_lit("e")),
          "effect row tail");
    parsed_free(&p);
  }
}

static void test_stress_program(void) {
  const u64 capacity = 256 * 1024;
  char* source = malloc(capacity);
  CHECK(source != NULL, "stress source allocation");
  if (!source) {
    return;
  }
  u64 used = 0;
  int written = snprintf(source + used, capacity - used, "fn stress() {\n");
  CHECK(written > 0, "stress header");
  used += (u64)written;
  for (u64 i = 0; i < 500; ++i) {
    written = snprintf(source + used, capacity - used, "i32 v%llu = %llu;\n",
                       (unsigned long long)i, (unsigned long long)i);
    CHECK(written > 0 && used + (u64)written < capacity, "stress declaration");
    used += (u64)written;
  }
  for (u64 i = 0; i < 200; ++i) {
    written = snprintf(source + used, capacity - used,
                       "if (v%llu > 0) { v%llu += 1; }\n",
                       (unsigned long long)(i % 500), (unsigned long long)(i % 500));
    CHECK(written > 0 && used + (u64)written < capacity, "stress statement");
    used += (u64)written;
  }
  written = snprintf(source + used, capacity - used, "return;\n}\n");
  CHECK(written > 0 && used + (u64)written < capacity, "stress footer");

  Parsed p = parse_source(source);
  Item* fn = item_at(p.program, 0);
  CHECK(fn && fn->kind == ITEM_FN && fn->as.fn.has_body, "stress function item");
  if (fn) {
    CHECK(vec_len(fn->as.fn.body->as.block.body) == 701, "stress statement count");
    Stmt* last = find_var(fn->as.fn.body, "v499");
    CHECK(last && last->as.var_expr.assigned_value.as.number.value == 499,
          "stress last declaration");
  }
  parsed_free(&p);
  free(source);
}

static void test_deep_generics(void) {
  char type[1024];
  u64 used = 0;
  for (u32 i = 0; i < 32; ++i) {
    int written = snprintf(type + used, sizeof(type) - used, "List<");
    CHECK(written > 0, "deep generic open");
    used += (u64)written;
  }
  int written = snprintf(type + used, sizeof(type) - used, "i32");
  CHECK(written > 0, "deep generic base");
  used += (u64)written;
  for (u32 i = 0; i < 32; ++i) {
    written = snprintf(type + used, sizeof(type) - used, ">");
    CHECK(written > 0, "deep generic close");
    used += (u64)written;
  }
  char source[2048];
  snprintf(source, sizeof(source), "fn deep() { %s xs = null; }\n", type);
  Parsed p = parse_source(source);
  Item* fn = item_at(p.program, 0);
  Stmt* xs = fn ? find_var(fn->as.fn.body, "xs") : NULL;
  Type* type_node = xs ? xs->as.var_expr.type : NULL;
  u32 depth = 0;
  while (type_node && type_node->kind == TYPE_PATH &&
         type_node->as.path.generic_args &&
         vec_len(type_node->as.path.generic_args) == 1) {
    depth++;
    type_node = vec_at_ptr(type_node->as.path.generic_args, 0);
  }
  CHECK(depth == 32 && type_node && type_node->kind == TYPE_PRIMITIVE,
        "deep nested generic type");
  parsed_free(&p);
}

static void test_depth_guard(void) {
  const u64 depth = 2000;
  char* expr_source = malloc(depth * 2 + 8);
  CHECK(expr_source != NULL, "depth expression allocation");
  if (expr_source) {
    u64 used = 0;
    for (u64 i = 0; i < depth; ++i) expr_source[used++] = '(';
    expr_source[used++] = '1';
    for (u64 i = 0; i < depth; ++i) expr_source[used++] = ')';
    expr_source[used++] = ';';
    expr_source[used] = '\0';
    CHECK(parse_fails(expr_source), "deep expression rejected");
    free(expr_source);
  }

  char* unary_source = malloc(depth + 8);
  CHECK(unary_source != NULL, "depth unary allocation");
  if (unary_source) {
    u64 used = 0;
    for (u64 i = 0; i < depth; ++i) unary_source[used++] = '!';
    unary_source[used++] = '1';
    unary_source[used++] = ';';
    unary_source[used] = '\0';
    CHECK(parse_fails(unary_source), "deep unary rejected");
    free(unary_source);
  }

  char* block_source = malloc(depth * 2 + 32);
  CHECK(block_source != NULL, "depth block allocation");
  if (block_source) {
    u64 used = 0;
    const char* header = "fn f() ";
    while (*header) block_source[used++] = *header++;
    for (u64 i = 0; i < depth; ++i) block_source[used++] = '{';
    for (u64 i = 0; i < depth; ++i) block_source[used++] = '}';
    block_source[used] = '\0';
    CHECK(parse_fails(block_source), "deep block rejected");
    free(block_source);
  }
}

int main(void) {
  test_token_tables();
  test_lexer_robustness();
  test_stress_program();
  test_deep_generics();
  test_depth_guard();

  for (u64 i = 0; i < sizeof(valid_cases) / sizeof(valid_cases[0]); ++i) {
    Parsed p = parse_source(valid_cases[i].source);
    CHECK(p.program && p.program->kind == STMT_BLOCK, valid_cases[i].name);
    parsed_free(&p);
  }

  for (u64 i = 0; i < sizeof(invalid_cases) / sizeof(invalid_cases[0]); ++i) {
    CHECK(parse_fails(invalid_cases[i].source), invalid_cases[i].name);
  }

  test_ast_shapes();

  CHECK(arena_debug_live_count() == 0, "leaked arenas");
  CHECK(map_debug_live_count() == 0, "leaked maps");

  if (failures != 0) {
    fprintf(stderr, "grammar tests FAILED: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("grammar test OK (%d checks, %llu valid programs, %llu invalid programs)\n", checks,
         (unsigned long long)(sizeof(valid_cases) / sizeof(valid_cases[0])),
         (unsigned long long)(sizeof(invalid_cases) / sizeof(invalid_cases[0])));
  return 0;
}
