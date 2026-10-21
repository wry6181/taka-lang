#pragma once

#include "utils/types.h"
#include "utils/utils.h"
#include "lexer/tokenizer.h"
#include "expressions.h"

typedef enum { STMT_BLOCK, STMT_EXPRESION, STMT_VAR } StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
  StmtKind kind;
  union {
    struct { Vector* body; } block;
    struct { Expr expr; } expresion;
    struct { String8 var_name; bool is_const; Expr assigned_value; } var_expr;
  } as;
};



//void eval(Stmt* stms) {
//}
