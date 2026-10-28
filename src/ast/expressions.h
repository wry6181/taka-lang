#pragma once

#include "utils/types.h"
#include "utils/utils.h"
#include "lexer/tokens.h"

typedef enum {
  EXPR_NUMBER,
  EXPR_STRING,
  EXPR_SYMBOL,
  EXPR_BINARY,
  EXPR_PREFIX,
  EXPR_ASSIGNMENT
} ExprKind;

typedef struct Expr Expr;
struct Expr {
  ExprKind kind;
  union {
    // --------
    // LITERAL
    // --------
    struct { i64 value; } number;
    struct { String8 value; } string;
    struct { String8 value; } symbol;
    // --------
    // COMPLEX
    // --------

    struct { Expr* lhs; Token oper; Expr* rhs; } bin;
    struct { Expr* right_expr; Token oper; } prefix;
    struct { Expr* assigne_expr; Token oper; Expr* rhs; } assignment;
  } as;
};

//void eval(Expr* expr) {
//}
