#pragma once

#include "ast/ast.h"
#include "utils/utils.h"

typedef enum SymbolKind {
  SYM_LOCAL,
  SYM_PARAM,
  SYM_GLOBAL,
  SYM_FN,
  SYM_EXTERN_FN,
  SYM_STRUCT,
  SYM_ENUM,
  SYM_EFFECT,
  SYM_TRAIT,
  SYM_HANDLER,
  SYM_TYPE_ALIAS,
  SYM_TYPE_PARAM,
  SYM_CAPTURE
} SymbolKind;

typedef struct Symbol Symbol;
struct Symbol {
  String8 name;
  SymbolKind kind;
  bool is_mut;
  bool is_global;
  Type* type;
  Item* item;
  Symbol* capture_outer;
  union {
    Stmt* var;
    Param* param;
    FnDecl* fn;
    FnSig* fn_sig;
    ExternDecl* extern_decl;
    StructDecl* struct_decl;
    EnumDecl* enum_decl;
    EffectDecl* effect_decl;
    TraitDecl* trait_decl;
    HandlerDecl* handler_decl;
    TypeAliasDecl* type_alias;
  } as;
};

typedef enum EffectBit {
  EFF_IO = 1u << 0,
  EFF_ALLOC = 1u << 1,
  EFF_READS = 1u << 2,
  EFF_WRITES = 1u << 3,
  EFF_DECIDES = 1u << 4,
  EFF_SUSPENDS = 1u << 5,
  EFF_TRANSACTS = 1u << 6,
  EFF_OWNERSHIP = 1u << 7,
  EFF_GPU = 1u << 8
} EffectBit;

typedef struct EffectSet EffectSet;
struct EffectSet {
  u32 builtins;
  Vector* customs;
  bool tail;
};

typedef struct Sema Sema;

Sema* sema_create(MemArena* arena, Stmt* program);
void sema_free(Sema* sema);
bool sema_check(Sema* sema);

Type* sema_expr_type(Sema* sema, Expr* expr);
Symbol* sema_expr_symbol(Sema* sema, Expr* expr);
Symbol* sema_pattern_symbol(Sema* sema, Pattern* pattern);
Symbol* sema_capture_symbol(Sema* sema, Capture* capture);
EffectSet* sema_expr_effects(Sema* sema, Expr* expr);
EffectSet* sema_item_effects(Sema* sema, Item* item);
Symbol* sema_lookup_global(Sema* sema, String8 name);
Symbol* sema_lookup(Sema* sema, String8 name);
Symbol* sema_lookup_enum_variant(Sema* sema, Path path, EnumVariant** out_variant);
Symbol* sema_lookup_method(Sema* sema, Type* object_type, String8 method_name, FnDecl** out_method);
bool sema_effect_has(const EffectSet* set, u32 bit);
bool sema_effect_has_custom(const EffectSet* set, String8 name);
u32 sema_error_count(Sema* sema);
MemArena* sema_arena(Sema* sema);
Stmt* sema_program(Sema* sema);
