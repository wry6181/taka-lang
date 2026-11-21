#pragma once

#include "utils/types.h"
#include "utils/vector.h"

typedef struct Expr Expr;
typedef struct Type Type;

typedef struct Path {
  Vector* segments;
} Path;

typedef struct BoundTrait {
  Path path;
  Vector* generic_args;
} BoundTrait;

typedef struct Bound {
  Vector* traits;
} Bound;

typedef struct GenericParam {
  String8 name;
  bool is_effect;
  Bound* bound;
} GenericParam;

typedef struct WhereConstraint {
  String8 name;
  Bound* bound;
} WhereConstraint;

typedef struct WhereClause {
  Vector* constraints;
} WhereClause;

typedef struct EffectItem {
  bool is_builtin;
  Path path;
  Vector* generic_args;
} EffectItem;

typedef struct EffectRow {
  Vector* items;
  bool has_tail;
  String8 tail;
} EffectRow;

typedef struct Param {
  bool is_mut;
  Type* type;
  String8 name;
} Param;

typedef enum TypeKind {
  TYPE_PRIMITIVE,
  TYPE_PATH,
  TYPE_FN,
  TYPE_DYN,
  TYPE_TUPLE,
  TYPE_ARRAY,
  TYPE_AUTO
} TypeKind;

struct Type {
  TypeKind kind;
  bool is_const;
  u32 pointer_depth;
  bool is_reference;
  union {
    struct {
      String8 name;
    } primitive;
    struct {
      Path path;
      Vector* generic_args;
    } path;
    struct {
      bool is_extern;
      Vector* params;
      EffectRow* effects;
      Type* return_type;
    } fn;
    struct {
      Path path;
      Vector* generic_args;
    } dyn;
    struct {
      Vector* types;
    } tuple;
    struct {
      Type* element;
      Expr* length;
    } array;
    struct {
      bool is_auto;
    } auto_;
  } as;
};
