#pragma once

#include "ast/type.h"
#include "ast/expressions.h"

typedef struct Stmt Stmt;

typedef enum Visibility {
  VIS_DEFAULT,
  VIS_PUB,
  VIS_PRIVATE
} Visibility;

typedef struct FnSig {
  String8 name;
  Vector* generics;
  Vector* params;
  EffectRow* effects;
  Type* return_type;
  WhereClause* where;
} FnSig;

typedef struct FnDecl {
  FnSig sig;
  bool has_body;
  Stmt* body;
} FnDecl;

typedef struct ExternDecl {
  bool has_abi;
  String8 abi;
  FnSig sig;
} ExternDecl;

typedef struct StructField {
  Visibility visibility;
  bool is_mut;
  Type* type;
  String8 name;
} StructField;

typedef struct StructDecl {
  String8 name;
  Vector* generics;
  WhereClause* where;
  Vector* fields;
} StructDecl;

typedef enum VariantKind {
  VARIANT_UNIT,
  VARIANT_TUPLE,
  VARIANT_STRUCT,
  VARIANT_VALUE
} VariantKind;

typedef struct VariantField {
  Visibility visibility;
  Type* type;
  String8 name;
} VariantField;

typedef struct EnumVariant {
  Visibility visibility;
  String8 name;
  VariantKind kind;
  Vector* types;
  Vector* fields;
  Expr* value;
} EnumVariant;

typedef struct EnumDecl {
  String8 name;
  Vector* generics;
  WhereClause* where;
  Vector* variants;
} EnumDecl;

typedef struct EffectOp {
  Visibility visibility;
  String8 name;
  Vector* params;
  Type* return_type;
} EffectOp;

typedef struct EffectDecl {
  String8 name;
  Vector* generics;
  Vector* ops;
} EffectDecl;

typedef struct HandlerDecl {
  String8 name;
  Vector* generics;
  Path effect_path;
  EffectRow* effects;
  Vector* ops;
} HandlerDecl;

typedef struct TraitDecl {
  String8 name;
  Vector* generics;
  Bound* super;
  Vector* methods;
} TraitDecl;

typedef struct ImplDecl {
  Vector* generics;
  Path trait_path;
  Vector* trait_args;
  Type* for_type;
  WhereClause* where;
  Vector* methods;
} ImplDecl;

typedef struct TypeAliasDecl {
  String8 name;
  Vector* generics;
  Type* type;
} TypeAliasDecl;

typedef struct ImportDecl {
  Path path;
} ImportDecl;

typedef struct GlobalDecl {
  Stmt* declaration;
} GlobalDecl;

typedef enum ItemKind {
  ITEM_IMPORT,
  ITEM_FN,
  ITEM_EXTERN,
  ITEM_STRUCT,
  ITEM_ENUM,
  ITEM_EFFECT,
  ITEM_HANDLER,
  ITEM_TRAIT,
  ITEM_IMPL,
  ITEM_TYPE_ALIAS,
  ITEM_GLOBAL
} ItemKind;

typedef struct Item Item;
struct Item {
  ItemKind kind;
  Visibility visibility;
  union {
    ImportDecl import;
    FnDecl fn;
    ExternDecl extern_decl;
    StructDecl struct_decl;
    EnumDecl enum_decl;
    EffectDecl effect_decl;
    HandlerDecl handler_decl;
    TraitDecl trait_decl;
    ImplDecl impl_decl;
    TypeAliasDecl type_alias;
    GlobalDecl global;
  } as;
};
