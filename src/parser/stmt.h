#pragma once

#include "lookup.h"
#include "utils/utils.h"
#include "lexer/tokens.h"
#include "parser/parser.h"
#include "ast/statements.h"


Stmt* parse_var_stmt(Parser* p);

Stmt* parse_stmt(Parser* p) {
  // A brand-new identifier followed by '=' starts a const-by-default
  // declaration; known names stay assignments.
  if (parser_current_kind(p) == TOKEN_IDENTIFIER && p->pos + 1 < p->count &&
      p->tokens[p->pos + 1].kind == TOKEN_ASSIGNMENT &&
      !parser_find_symbol(p, parser_current(p).value)) {
    return parse_var_stmt(p);
  }

  StmtHandler stmt = lookup_stmt(p->lookups, parser_current_kind(p));
  if (stmt) {
    return stmt(p);
  }

  Expr* expr = parse_expr(p, defalt_bp);
  parser_expect(p, TOKEN_SEMICOLON);

  Stmt* exprstmt = PUSH_STRUCT_ZERO(p->arena, Stmt);
  exprstmt->kind = STMT_EXPRESION;
  exprstmt->as.expresion.expr = *expr;

  return exprstmt;
}

Stmt* parse_var_stmt(Parser* p) {
  bool is_const = TRUE;
  switch (parser_current_kind(p)) {
    case TOKEN_MUT: {
      parser_advance(p);
      is_const = FALSE;
      break;
    }
    case TOKEN_CONST: {
      parser_advance(p);
      break;
    }
    default:
      break;
  }
  String8 var_name = parser_expect_error(p, TOKEN_IDENTIFIER, "Inside variable declaration expected to find variable name").value;
  parser_expect(p, TOKEN_ASSIGNMENT);
  Expr* assigned_value = parse_expr(p, defalt_bp);
  parser_expect(p, TOKEN_SEMICOLON);

  parser_declare_symbol(p, var_name, !is_const);

  Stmt* var_stmt = PUSH_STRUCT_ZERO(p->arena, Stmt);
  var_stmt->as.var_expr.assigned_value = *assigned_value;
  var_stmt->as.var_expr.is_const = is_const;
  var_stmt->as.var_expr.var_name = var_name;
  var_stmt->kind = STMT_VAR;

  return var_stmt;
}

static inline void lookup_register_stmts(Lookups* lu) {
  lookup_register_stmt(lu, TOKEN_MUT, parse_var_stmt);
  lookup_register_stmt(lu, TOKEN_CONST, parse_var_stmt);
}
