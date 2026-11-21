#pragma once

#include "utils/types.h"
#include "utils/vector.h"
#include "ast/type.h"
#include "ast/expressions.h"
#include "ast/patterns.h"
#include "ast/items.h"

typedef enum {
  STMT_BLOCK,
  STMT_EXPRESION,
  STMT_VAR,
  STMT_DEFER,
  STMT_IF,
  STMT_WHILE,
  STMT_FOR,
  STMT_BREAK,
  STMT_CONTINUE,
  STMT_RETURN,
  STMT_LABEL,
  STMT_ITEM
} StmtKind;

typedef struct ForStmt {
  bool is_each;
  Pattern* pattern;
  Expr* iterable;
  Stmt* init;
  Expr* cond;
  Stmt* step;
} ForStmt;

typedef struct Stmt Stmt;
struct Stmt {
  StmtKind kind;
  union {
    struct {
      Vector* body;
      Expr* tail;
    } block;
    struct {
      Expr expr;
      bool is_tail;
    } expresion;
    struct {
      String8 var_name;
      bool is_const;
      Expr assigned_value;
      Type* type;
      bool is_auto;
      bool is_mut;
    } var_expr;
    struct {
      bool is_block;
      Stmt* block;
      Stmt* stmt;
    } defer_stmt;
    struct {
      Vector* branches;
      Stmt* else_block;
    } if_stmt;
    struct {
      Condition condition;
      Stmt* body;
    } while_stmt;
    struct {
      ForStmt for_stmt;
      Stmt* body;
    } for_stmt;
    struct {
      String8 label;
      bool has_label;
    } break_stmt;
    struct {
      String8 label;
      bool has_label;
    } continue_stmt;
    struct {
      bool has_value;
      Expr* value;
    } return_stmt;
    struct {
      String8 label;
      Stmt* stmt;
    } label_stmt;
    struct {
      Item* item;
    } item;
  } as;
};
