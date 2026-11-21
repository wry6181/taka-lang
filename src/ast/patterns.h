#pragma once

#include "ast/type.h"
#include "ast/expressions.h"

typedef struct Pattern Pattern;
typedef struct FieldPat FieldPat;

struct FieldPat {
  String8 name;
  bool has_pattern;
  Pattern* pattern;
};

typedef enum PatternKind {
  PAT_WILDCARD,
  PAT_REF_BIND,
  PAT_LITERAL,
  PAT_BIND,
  PAT_PATH,
  PAT_ALT
} PatternKind;

struct Pattern {
  PatternKind kind;
  union {
    struct {
      bool is_mut;
      String8 name;
    } bind;
    struct {
      Expr* literal;
    } literal;
    struct {
      Path path;
      bool has_tuple;
      Vector* tuple;
      bool has_struct;
      Vector* fields;
      bool has_ellipsis;
    } path;
    struct {
      Vector* alternatives;
    } alt;
  } as;
};
