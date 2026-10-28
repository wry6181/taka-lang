#include "parser.h"
#include "lookup.h"
#include "stmt.h"
#include "expr.h"

#include <stdlib.h>

Parser create_parser(MemArena* arena, Token* tokens, u64 count) {
  Lookups* lookups = PUSH_STRUCT(arena, Lookups);
  lookup_init(lookups, arena);
  lookup_register_exprs(lookups);
  lookup_register_stmts(lookups);
  return (Parser){
      .arena = arena,
      .tokens = tokens,
      .count = count,
      .pos = 0,
      .lookups = lookups,
  };
}

void parser_free(Parser* p) {
  if (p) {
    lookup_free(p->lookups);
  }
}

void parser_die(void) {
  exit(1);
}

Token parser_current(const Parser* p) {
  if (!p || p->pos >= p->count) {
    T_FATAL("parser: unexpected end of token stream");
    parser_die();
  }
  return p->tokens[p->pos];
}

TokenKind parser_current_kind(const Parser* p) {
  return parser_current(p).kind;
}

Token parser_advance(Parser* p) {
  Token tk = parser_current(p);
  p->pos++;
  return tk;
}

bool parser_has_tokens(const Parser* p) {
  return p && p->pos < p->count && p->tokens[p->pos].kind != TOKEN_EOF;
}

Token parser_expect_error(Parser* p, TokenKind expected, const char* err) {
  Token token = parser_current(p);
  if (token.kind != expected) {
    if (err) {
      T_FATAL("parser: %s [%lu:%lu]", err, token.line, token.column);
      parser_die();
    }
    String8 want = tokenkind_to_string8(expected);
    String8 got = tokenkind_to_string8(token.kind);
    T_FATAL("parser: expected %.*s but received %.*s instead [%lu:%lu]", str8_fmt(want),
            str8_fmt(got), token.line, token.column);
    parser_die();
  }
  return parser_advance(p);
}

Token parser_expect(Parser* p, TokenKind expected) {
  return parser_expect_error(p, expected, NULL);
}

Stmt* parse_program(MemArena* arena, Token* tokens, u64 count) {
  Parser parser = create_parser(arena, tokens, count);

  // Go: Body := make([]ast.Stmt, 0) — []ast.Stmt is a Vector of Stmt* for us
  Vector* body = vec_new(arena, sizeof(Stmt*), 0);

  // Go: for p.hasTokens() { Body = append(Body, parse_stmt(p)) }
  while (parser_has_tokens(&parser)) {
    Stmt* stmt = parse_stmt(&parser);
    vec_push_ptr(body, stmt);
  }

  parser_free(&parser);

  // Go: return ast.BlockStmt{ Body: Body }
  Stmt* block = PUSH_STRUCT_ZERO(arena, Stmt);
  block->kind = STMT_BLOCK;
  block->as.block.body = body;
  return block;
}
