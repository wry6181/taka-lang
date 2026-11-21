#pragma once

#include "utils/types.h"
#include "utils/vector.h"
#include "lexer/tokens.h"
#include "ast/type.h"

typedef struct Pattern Pattern;
typedef struct Stmt Stmt;

typedef struct Condition {
  bool is_binding;
  bool is_auto;
  bool is_mut;
  Pattern* pattern;
  Expr* expr;
} Condition;

typedef struct FieldInit {
  String8 name;
  Expr* value;
} FieldInit;

typedef struct Capture {
  bool by_ref;
  bool is_mut;
  String8 name;
  bool has_init;
  Expr* init;
} Capture;

typedef struct IfBranch {
  Condition condition;
  Stmt* body;
} IfBranch;

typedef struct MatchArm {
  Pattern* pattern;
  Expr* guard;
  bool body_is_block;
  Stmt* block;
  Expr* expr;
} MatchArm;

typedef struct HandlerOp {
  bool is_return;
  String8 name;
  Vector* params;
  bool body_is_block;
  Stmt* block;
  Expr* expr;
} HandlerOp;

typedef enum ExprKind {
  EXPR_NUMBER,
  EXPR_STRING,
  EXPR_SYMBOL,
  EXPR_BINARY,
  EXPR_PREFIX,
  EXPR_ASSIGNMENT,
  EXPR_FLOAT,
  EXPR_CHAR,
  EXPR_BOOL,
  EXPR_NULL,
  EXPR_UNDEF,
  EXPR_PATH,
  EXPR_TERNARY,
  EXPR_CAST,
  EXPR_CALL,
  EXPR_INDEX,
  EXPR_FIELD,
  EXPR_POSTFIX,
  EXPR_STRUCT_LIT,
  EXPR_ARRAY_LIT,
  EXPR_LAMBDA,
  EXPR_SIZEOF,
  EXPR_OFFSETOF,
  EXPR_IF,
  EXPR_MATCH,
  EXPR_PERFORM,
  EXPR_HANDLE,
  EXPR_SPAWN,
  EXPR_RESUME,
  EXPR_TRANSACT,
  EXPR_BLOCK
} ExprKind;

typedef struct Expr Expr;
struct Expr {
  ExprKind kind;
  union {
    struct {
      i64 value;
      String8 raw;
    } number;
    struct {
      f64 value;
      String8 raw;
    } float_lit;
    struct {
      String8 value;
    } string;
    struct {
      String8 value;
    } char_lit;
    struct {
      bool value;
    } boolean;
    struct {
      b8 present;
    } null_lit;
    struct {
      b8 present;
    } undef_lit;
    struct {
      String8 value;
    } symbol;
    struct {
      Path path;
      Vector* generic_args;
      bool has_turbofish;
    } path;
    struct {
      Expr* lhs;
      Token oper;
      Expr* rhs;
    } bin;
    struct {
      Expr* cond;
      Expr* then_expr;
      Expr* else_expr;
    } ternary;
    struct {
      Expr* right_expr;
      Token oper;
    } prefix;
    struct {
      Expr* expr;
      Type* type;
    } cast;
    struct {
      Expr* callee;
      Vector* args;
    } call;
    struct {
      Expr* object;
      Expr* index;
    } index;
    struct {
      Expr* object;
      String8 field;
    } field;
    struct {
      Expr* expr;
      Token oper;
    } postfix;
    struct {
      Expr* assigne_expr;
      Token oper;
      Expr* rhs;
    } assignment;
    struct {
      Path path;
      Vector* fields;
    } struct_lit;
    struct {
      Vector* items;
      bool is_fill;
      Expr* fill;
      Expr* count;
    } array_lit;
    struct {
      Vector* captures;
      Vector* params;
      EffectRow* effects;
      Type* return_type;
      bool body_is_block;
      Stmt* block;
      Expr* expr;
    } lambda;
    struct {
      bool is_type;
      Type* type;
      Expr* expr;
    } sizeof_expr;
    struct {
      Type* type;
      String8 field;
    } offsetof_expr;
    struct {
      Vector* branches;
      Stmt* else_block;
    } if_expr;
    struct {
      Expr* subject;
      Vector* arms;
    } match;
    struct {
      Path path;
      Vector* args;
    } perform;
    struct {
      bool subject_is_block;
      Stmt* block;
      Expr* subject;
      bool handler_is_path;
      Path handler_path;
      Vector* handler_ops;
    } handle;
    struct {
      bool is_block;
      Stmt* block;
      Expr* expr;
    } spawn;
    struct {
      bool has_value;
      Expr* value;
    } resume;
    struct {
      Stmt* block;
    } transact;
    struct {
      Stmt* block;
    } block;
  } as;
};
