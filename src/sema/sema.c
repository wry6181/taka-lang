#include "sema/sema.h"
#include "diagnostics/diagnostics.h"

#include "lexer/tokens.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Scope {
  Vector* symbols;
  struct Scope* parent;
} Scope;

struct Sema {
  MemArena* arena;
  Stmt* program;
  Scope* global;
  Scope* current;
  EffectSet* current_fn_effects;
  Type* current_self_type;
  Map* expr_types;
  Map* expr_symbols;
  Map* pattern_symbols;
  Map* capture_symbols;
  Map* expr_effects;
  Map* item_effects;
  Map* sig_effects;
  Vector* impls;
  u32 errors;
};

static void sema_error(Sema* sema, const char* fmt, ...) T_PRINTF(2, 3);

static void sema_error(Sema* sema, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  diag_vemit_format(DIAG_DOMAIN_SEMA, fmt, 0, 0, args);
  va_end(args);
  sema->errors++;
}

static EffectSet* effect_new(Sema* sema) {
  EffectSet* set = PUSH_STRUCT_ZERO(sema->arena, EffectSet);
  set->customs = vec_new(sema->arena, sizeof(String8), 0);
  return set;
}

static bool effect_has_builtin_value(const EffectSet* set, u32 bit) {
  return set && (set->builtins & bit) != 0;
}

static bool effect_has_custom_value(const EffectSet* set, String8 name) {
  if (!set || !set->customs) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(set->customs); ++i) {
    String8* current = vec_at(set->customs, i);
    if (current && str8_eq(*current, name)) {
      return TRUE;
    }
  }
  return FALSE;
}

static void effect_add_builtin(EffectSet* set, u32 bit) {
  set->builtins |= bit;
}

static void effect_add_custom(EffectSet* set, String8 name) {
  if (!set->customs) {
    return;
  }
  if (!effect_has_custom_value(set, name)) {
    vec_push(set->customs, &name);
  }
}

static void effect_remove_custom(EffectSet* set, String8 name) {
  if (!set || !set->customs) {
    return;
  }
  Vector* kept = set->customs;
  u64 write = 0;
  for (u64 i = 0; i < vec_len(kept); ++i) {
    String8* current = vec_at(kept, i);
    if (!current || str8_eq(*current, name)) {
      continue;
    }
    if (write != i) {
      String8* slot = vec_at(kept, write);
      if (slot) *slot = *current;
    }
    write++;
  }
  while (vec_len(kept) > write) {
    vec_pop(kept);
  }
}

static void effect_merge(EffectSet* target, const EffectSet* source) {
  if (!target || !source) {
    return;
  }
  target->builtins |= source->builtins;
  if (source->customs) {
    for (u64 i = 0; i < vec_len(source->customs); ++i) {
      String8* name = vec_at(source->customs, i);
      if (name) {
        effect_add_custom(target, *name);
      }
    }
  }
  target->tail = target->tail || source->tail;
}

static EffectSet* effect_clone(Sema* sema, const EffectSet* source) {
  EffectSet* copy = effect_new(sema);
  if (source) {
    effect_merge(copy, source);
  }
  return copy;
}

static void effect_expand_transacts(EffectSet* set) {
  if (set && (set->builtins & EFF_TRANSACTS)) {
    set->builtins |= EFF_READS | EFF_WRITES | EFF_ALLOC;
  }
}

static bool effect_is_subset(Sema* sema, const EffectSet* sub, const EffectSet* super,
                             const char* context) {
  if (!sub) {
    return TRUE;
  }
  EffectSet* expanded = effect_clone(sema, super);
  effect_expand_transacts(expanded);
  u32 missing = sub->builtins & ~expanded->builtins;
  if (missing != 0) {
    sema_error(sema, TAKA_MSG_S_REQUIRES_EFFECTS_NOT_DECLARED_BY_THE_ENCLOSING_FUNCTION, context);
    return FALSE;
  }
  if (sub->customs) {
    for (u64 i = 0; i < vec_len(sub->customs); ++i) {
      String8* name = vec_at(sub->customs, i);
      if (!name) {
        continue;
      }
      if (!effect_has_custom_value(expanded, *name) && !expanded->tail) {
        sema_error(sema, TAKA_MSG_S_REQUIRES_UNDECLARED_EFFECT_S, context, str8_fmt(*name));
        return FALSE;
      }
    }
  }
  if (sub->tail && !expanded->tail) {
    sema_error(sema, TAKA_MSG_S_REQUIRES_AN_UNDECLARED_OPEN_EFFECT_ROW, context);
    return FALSE;
  }
  return TRUE;
}

static u32 builtin_effect_bit(String8 name) {
  if (str8_eq(name, str8_lit("io"))) return EFF_IO;
  if (str8_eq(name, str8_lit("alloc"))) return EFF_ALLOC;
  if (str8_eq(name, str8_lit("reads"))) return EFF_READS;
  if (str8_eq(name, str8_lit("writes"))) return EFF_WRITES;
  if (str8_eq(name, str8_lit("decides"))) return EFF_DECIDES;
  if (str8_eq(name, str8_lit("suspends"))) return EFF_SUSPENDS;
  if (str8_eq(name, str8_lit("transacts"))) return EFF_TRANSACTS;
  if (str8_eq(name, str8_lit("ownership"))) return EFF_OWNERSHIP;
  if (str8_eq(name, str8_lit("gpu"))) return EFF_GPU;
  return 0;
}

static String8 path_first(Path path) {
  if (!path.segments || vec_len(path.segments) == 0) {
    return (String8){0};
  }
  String8* first = vec_at(path.segments, 0);
  return first ? *first : (String8){0};
}

static String8 path_last(Path path) {
  if (!path.segments || vec_len(path.segments) == 0) {
    return (String8){0};
  }
  String8* last = vec_at(path.segments, vec_len(path.segments) - 1);
  return last ? *last : (String8){0};
}

static Symbol* symbol_new(Sema* sema, String8 name, SymbolKind kind) {
  Symbol* symbol = PUSH_STRUCT_ZERO(sema->arena, Symbol);
  symbol->name = name;
  symbol->kind = kind;
  return symbol;
}

static Scope* scope_new(Sema* sema, Scope* parent) {
  Scope* scope = PUSH_STRUCT_ZERO(sema->arena, Scope);
  scope->symbols = vec_new(sema->arena, sizeof(Symbol*), 0);
  scope->parent = parent;
  return scope;
}

static void scope_push(Sema* sema, Scope* scope) {
  sema->current = scope;
}

static void scope_pop(Sema* sema) {
  if (sema->current && sema->current->parent) {
    sema->current = sema->current->parent;
  }
}

static Symbol* scope_lookup(Scope* scope, String8 name) {
  for (Scope* current = scope; current; current = current->parent) {
    for (u64 i = 0; i < vec_len(current->symbols); ++i) {
      Symbol* symbol = vec_at_ptr(current->symbols, i);
      if (symbol && str8_eq(symbol->name, name)) {
        return symbol;
      }
    }
  }
  return NULL;
}

static void scope_add(Sema* sema, Scope* scope, Symbol* symbol) {
  for (u64 i = 0; i < vec_len(scope->symbols); ++i) {
    Symbol* existing = vec_at_ptr(scope->symbols, i);
    if (existing && str8_eq(existing->name, symbol->name)) {
      sema_error(sema, TAKA_MSG_S_DUPLICATE_DECLARATION_OF_S, str8_fmt(symbol->name));
      return;
    }
  }
  vec_push_ptr(scope->symbols, symbol);
}

static void add_generics(Sema* sema, Scope* scope, Vector* generics) {
  if (!generics) {
    return;
  }
  for (u64 i = 0; i < vec_len(generics); ++i) {
    GenericParam* param = vec_at_ptr(generics, i);
    if (!param || param->is_effect) {
      continue;
    }
    Symbol* symbol = symbol_new(sema, param->name, SYM_TYPE_PARAM);
    Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
    type->kind = TYPE_PATH;
    type->as.path.path.segments = vec_new(sema->arena, sizeof(String8), 0);
    vec_push(type->as.path.path.segments, &param->name);
    symbol->type = type;
    vec_push_ptr(scope->symbols, symbol);
  }
}

static Type* resolve_type(Sema* sema, Type* type, Scope* scope, u32 depth);

static Type* resolve_effect_row(Sema* sema, EffectRow* row, Scope* scope) {
  if (!row || !row->items) {
    return NULL;
  }
  for (u64 i = 0; i < vec_len(row->items); ++i) {
    EffectItem* item = vec_at_ptr(row->items, i);
    if (!item || !item->generic_args) {
      continue;
    }
    for (u64 j = 0; j < vec_len(item->generic_args); ++j) {
      Type* arg = vec_at_ptr(item->generic_args, j);
      (void)resolve_type(sema, arg, scope, 0);
    }
  }
  return NULL;
}

static Type* type_clone_shallow(Sema* sema, Type* source, TypeKind kind) {
  Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
  type->kind = kind;
  type->is_const = source ? source->is_const : FALSE;
  type->pointer_depth = source ? source->pointer_depth : 0;
  type->is_reference = source ? source->is_reference : FALSE;
  return type;
}

static Type* resolve_type(Sema* sema, Type* type, Scope* scope, u32 depth) {
  if (!type) {
    return NULL;
  }
  if (depth > 64) {
    sema_error(sema, TAKA_MSG_S_TYPE_ALIAS_CYCLE_DETECTED);
    return type;
  }
  switch (type->kind) {
    case TYPE_PRIMITIVE:
    case TYPE_AUTO:
      return type;
    case TYPE_PATH: {
      String8 name = path_first(type->as.path.path);
      if (str8_eq(name, str8_lit("Self"))) {
        if (sema->current_self_type) {
          return sema->current_self_type;
        }
        Type* self = type_clone_shallow(sema, type, TYPE_PATH);
        self->as.path.path.segments = vec_new(sema->arena, sizeof(String8), 0);
        String8 self_name = str8_lit("Self");
        vec_push(self->as.path.path.segments, &self_name);
        self->as.path.generic_args = NULL;
        return self;
      }
      Symbol* symbol = scope_lookup(scope, name);
      if (!symbol) {
        symbol = sema_lookup_global(sema, path_last(type->as.path.path));
      }
      if (!symbol) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_TYPE_S, str8_fmt(name));
        return type;
      }
      if (symbol->kind == SYM_TYPE_PARAM) {
        return symbol->type;
      }
      if (symbol->kind == SYM_TYPE_ALIAS) {
        TypeAliasDecl* alias = symbol->as.type_alias;
        Type* target = resolve_type(sema, alias->type, scope, depth + 1);
        if (!target) {
          return type;
        }
        Type* merged = type_clone_shallow(sema, target, target->kind);
        merged->pointer_depth += type->pointer_depth;
        merged->is_reference = merged->is_reference || type->is_reference;
        merged->is_const = merged->is_const || type->is_const;
        merged->as = target->as;
        return merged;
      }
      Type* resolved = type_clone_shallow(sema, type, TYPE_PATH);
      resolved->as.path.path = type->as.path.path;
      resolved->as.path.generic_args = NULL;
      if (type->as.path.generic_args) {
        resolved->as.path.generic_args = vec_new(sema->arena, sizeof(Type*), 0);
        for (u64 i = 0; i < vec_len(type->as.path.generic_args); ++i) {
          Type* arg = vec_at_ptr(type->as.path.generic_args, i);
          Type* fixed = resolve_type(sema, arg, scope, depth + 1);
          vec_push_ptr(resolved->as.path.generic_args, fixed);
        }
      }
      return resolved;
    }
    case TYPE_FN: {
      Type* resolved = type_clone_shallow(sema, type, TYPE_FN);
      resolved->as.fn.is_extern = type->as.fn.is_extern;
      resolved->as.fn.params = vec_new(sema->arena, sizeof(Type*), 0);
      for (u64 i = 0; i < vec_len(type->as.fn.params); ++i) {
        Type* param = vec_at_ptr(type->as.fn.params, i);
        vec_push_ptr(resolved->as.fn.params, resolve_type(sema, param, scope, depth + 1));
      }
      resolved->as.fn.effects = type->as.fn.effects;
      resolve_effect_row(sema, resolved->as.fn.effects, scope);
      resolved->as.fn.return_type =
          resolve_type(sema, type->as.fn.return_type, scope, depth + 1);
      return resolved;
    }
    case TYPE_DYN: {
      Type* resolved = type_clone_shallow(sema, type, TYPE_DYN);
      resolved->as.dyn.path = type->as.dyn.path;
      resolved->as.dyn.generic_args = NULL;
      if (type->as.dyn.generic_args) {
        resolved->as.dyn.generic_args = vec_new(sema->arena, sizeof(Type*), 0);
        for (u64 i = 0; i < vec_len(type->as.dyn.generic_args); ++i) {
          Type* arg = vec_at_ptr(type->as.dyn.generic_args, i);
          vec_push_ptr(resolved->as.dyn.generic_args,
                       resolve_type(sema, arg, scope, depth + 1));
        }
      }
      return resolved;
    }
    case TYPE_TUPLE: {
      Type* resolved = type_clone_shallow(sema, type, TYPE_TUPLE);
      resolved->as.tuple.types = vec_new(sema->arena, sizeof(Type*), 0);
      for (u64 i = 0; i < vec_len(type->as.tuple.types); ++i) {
        Type* elem = vec_at_ptr(type->as.tuple.types, i);
        vec_push_ptr(resolved->as.tuple.types, resolve_type(sema, elem, scope, depth + 1));
      }
      return resolved;
    }
    case TYPE_ARRAY: {
      Type* resolved = type_clone_shallow(sema, type, TYPE_ARRAY);
      resolved->as.array.element =
          resolve_type(sema, type->as.array.element, scope, depth + 1);
      resolved->as.array.length = type->as.array.length;
      return resolved;
    }
  }
  return type;
}

static Type* make_primitive(Sema* sema, const char* name) {
  Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
  type->kind = TYPE_PRIMITIVE;
  type->as.primitive.name = str8(name);
  return type;
}

static Type* make_bool_type(Sema* sema) {
  return make_primitive(sema, "bool");
}

static Type* make_u64_type(Sema* sema) {
  return make_primitive(sema, "u64");
}

static Type* make_ptr_like(Sema* sema, Type* inner, bool reference) {
  if (!inner) {
    return NULL;
  }
  Type* type = type_clone_shallow(sema, inner, inner->kind);
  type->as = inner->as;
  if (reference) {
    type->is_reference = TRUE;
  } else {
    type->pointer_depth++;
  }
  return type;
}

static Type* strip_pointer_one(Sema* sema, Type* type) {
  if (!type) {
    return NULL;
  }
  if (type->is_reference) {
    Type* result = type_clone_shallow(sema, type, type->kind);
    result->as = type->as;
    result->is_reference = FALSE;
    return result;
  }
  if (type->pointer_depth > 0) {
    Type* result = type_clone_shallow(sema, type, type->kind);
    result->as = type->as;
    result->pointer_depth--;
    return result;
  }
  return type;
}

static bool is_pointer_like(Type* type) {
  return type && (type->is_reference || type->pointer_depth > 0);
}

static Symbol* find_struct_symbol_from_type(Sema* sema, Type* type) {
  if (!type) {
    return NULL;
  }
  if (type->kind == TYPE_PATH) {
    String8 name = path_first(type->as.path.path);
    Symbol* symbol = sema_lookup_global(sema, name);
    if (symbol && symbol->kind == SYM_STRUCT) {
      return symbol;
    }
  }
  if (type->kind == TYPE_DYN) {
    String8 name = path_first(type->as.dyn.path);
    Symbol* symbol = sema_lookup_global(sema, name);
    if (symbol && symbol->kind == SYM_TRAIT) {
      return symbol;
    }
  }
  return NULL;
}

static StructField* find_field(Symbol* struct_symbol, String8 name) {
  if (!struct_symbol || struct_symbol->kind != SYM_STRUCT || !struct_symbol->as.struct_decl) {
    return NULL;
  }
  Vector* fields = struct_symbol->as.struct_decl->fields;
  if (!fields) {
    return NULL;
  }
  for (u64 i = 0; i < vec_len(fields); ++i) {
    StructField* field = vec_at_ptr(fields, i);
    if (field && str8_eq(field->name, name)) {
      return field;
    }
  }
  return NULL;
}

static Type* find_variant_field_type(EnumVariant* variant, String8 name) {
  if (!variant || !variant->fields) {
    return NULL;
  }
  for (u64 i = 0; i < vec_len(variant->fields); ++i) {
    VariantField* field = vec_at_ptr(variant->fields, i);
    if (field && str8_eq(field->name, name)) {
      return field->type;
    }
  }
  return NULL;
}

static EffectSet* effect_from_row(Sema* sema, EffectRow* row) {
  EffectSet* set = effect_new(sema);
  if (!row || !row->items) {
    return set;
  }
  for (u64 i = 0; i < vec_len(row->items); ++i) {
    EffectItem* item = vec_at_ptr(row->items, i);
    if (!item) {
      continue;
    }
    String8 name = path_first(item->path);
    u32 bit = builtin_effect_bit(name);
    if (bit != 0) {
      effect_add_builtin(set, bit);
    } else if (!str8_eq(name, str8_lit("pure"))) {
      effect_add_custom(set, name);
    }
  }
  set->tail = row->has_tail;
  return set;
}

static EffectSet* effects_for_sig(Sema* sema, FnSig* sig) {
  if (!sig) {
    return effect_new(sema);
  }
  EffectSet* cached = map_get_ptr(sema->sig_effects, (u64)(uintptr_t)sig);
  if (cached) {
    return cached;
  }
  EffectSet* set = NULL;
  if (!sig->effects) {
    set = effect_new(sema);
    effect_add_builtin(set, EFF_READS | EFF_WRITES | EFF_ALLOC);
  } else {
    set = effect_from_row(sema, sig->effects);
  }
  map_set_ptr(sema->sig_effects, (u64)(uintptr_t)sig, set);
  return set;
}

static EffectRow* effect_set_to_row(Sema* sema, EffectSet* set) {
  EffectRow* row = PUSH_STRUCT_ZERO(sema->arena, EffectRow);
  row->items = vec_new(sema->arena, sizeof(EffectItem*), 0);
  row->has_tail = set ? set->tail : FALSE;
  struct {
    u32 bit;
    const char* name;
  } builtins[] = {
      {EFF_IO, "io"},       {EFF_ALLOC, "alloc"},       {EFF_READS, "reads"},
      {EFF_WRITES, "writes"}, {EFF_DECIDES, "decides"}, {EFF_SUSPENDS, "suspends"},
      {EFF_TRANSACTS, "transacts"}, {EFF_OWNERSHIP, "ownership"}, {EFF_GPU, "gpu"},
  };
  if (set) {
    for (u64 i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
      if ((set->builtins & builtins[i].bit) == 0) {
        continue;
      }
      EffectItem* item = PUSH_STRUCT_ZERO(sema->arena, EffectItem);
      item->is_builtin = TRUE;
      item->path.segments = vec_new(sema->arena, sizeof(String8), 0);
      String8 name = str8(builtins[i].name);
      vec_push(item->path.segments, &name);
      vec_push_ptr(row->items, item);
    }
    if (set->customs) {
      for (u64 i = 0; i < vec_len(set->customs); ++i) {
        String8* name = vec_at(set->customs, i);
        if (!name) {
          continue;
        }
        EffectItem* item = PUSH_STRUCT_ZERO(sema->arena, EffectItem);
        item->path.segments = vec_new(sema->arena, sizeof(String8), 0);
        vec_push(item->path.segments, name);
        vec_push_ptr(row->items, item);
      }
    }
  }
  return row;
}

static bool find_effect_for_op(Sema* sema, String8 op_name, String8* out_effect) {
  for (u64 i = 0; i < vec_len(sema->global->symbols); ++i) {
    Symbol* symbol = vec_at_ptr(sema->global->symbols, i);
    if (!symbol || symbol->kind != SYM_EFFECT || !symbol->as.effect_decl->ops) {
      continue;
    }
    for (u64 j = 0; j < vec_len(symbol->as.effect_decl->ops); ++j) {
      EffectOp* op = vec_at_ptr(symbol->as.effect_decl->ops, j);
      if (op && str8_eq(op->name, op_name)) {
        if (out_effect) {
          *out_effect = symbol->name;
        }
        return TRUE;
      }
    }
  }
  return FALSE;
}

static Type* make_fn_type(Sema* sema, FnSig* sig) {
  Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
  type->kind = TYPE_FN;
  type->as.fn.params = vec_new(sema->arena, sizeof(Type*), 0);
  if (sig->params) {
    for (u64 i = 0; i < vec_len(sig->params); ++i) {
      Param* param = vec_at_ptr(sig->params, i);
      if (param) {
        vec_push_ptr(type->as.fn.params, param->type);
      }
    }
  }
  type->as.fn.effects = sig->effects;
  type->as.fn.return_type = sig->return_type;
  return type;
}

static void collect_global_declaration(Sema* sema, Stmt* stmt);

static void collect_item(Sema* sema, Item* item) {
  if (!item) {
    return;
  }
  switch (item->kind) {
    case ITEM_IMPORT:
      return;
    case ITEM_FN: {
      Symbol* symbol = symbol_new(sema, item->as.fn.sig.name, SYM_FN);
      symbol->item = item;
      symbol->as.fn = &item->as.fn;
      symbol->type = make_fn_type(sema, &item->as.fn.sig);
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_EXTERN: {
      Symbol* symbol = symbol_new(sema, item->as.extern_decl.sig.name, SYM_EXTERN_FN);
      symbol->item = item;
      symbol->as.extern_decl = &item->as.extern_decl;
      symbol->type = make_fn_type(sema, &item->as.extern_decl.sig);
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_STRUCT: {
      Symbol* symbol = symbol_new(sema, item->as.struct_decl.name, SYM_STRUCT);
      symbol->item = item;
      symbol->as.struct_decl = &item->as.struct_decl;
      symbol->type = PUSH_STRUCT_ZERO(sema->arena, Type);
      symbol->type->kind = TYPE_PATH;
      symbol->type->as.path.path.segments = vec_new(sema->arena, sizeof(String8), 0);
      vec_push(symbol->type->as.path.path.segments, &item->as.struct_decl.name);
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_ENUM: {
      Symbol* symbol = symbol_new(sema, item->as.enum_decl.name, SYM_ENUM);
      symbol->item = item;
      symbol->as.enum_decl = &item->as.enum_decl;
      symbol->type = PUSH_STRUCT_ZERO(sema->arena, Type);
      symbol->type->kind = TYPE_PATH;
      symbol->type->as.path.path.segments = vec_new(sema->arena, sizeof(String8), 0);
      vec_push(symbol->type->as.path.path.segments, &item->as.enum_decl.name);
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_EFFECT: {
      Symbol* symbol = symbol_new(sema, item->as.effect_decl.name, SYM_EFFECT);
      symbol->item = item;
      symbol->as.effect_decl = &item->as.effect_decl;
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_HANDLER: {
      Symbol* symbol = symbol_new(sema, item->as.handler_decl.name, SYM_HANDLER);
      symbol->item = item;
      symbol->as.handler_decl = &item->as.handler_decl;
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_TRAIT: {
      Symbol* symbol = symbol_new(sema, item->as.trait_decl.name, SYM_TRAIT);
      symbol->item = item;
      symbol->as.trait_decl = &item->as.trait_decl;
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_IMPL:
      vec_push_ptr(sema->impls, item);
      return;
    case ITEM_TYPE_ALIAS: {
      Symbol* symbol = symbol_new(sema, item->as.type_alias.name, SYM_TYPE_ALIAS);
      symbol->item = item;
      symbol->as.type_alias = &item->as.type_alias;
      scope_add(sema, sema->global, symbol);
      return;
    }
    case ITEM_GLOBAL:
      collect_global_declaration(sema, item->as.global.declaration);
      return;
  }
}

static void collect_global_declaration(Sema* sema, Stmt* stmt) {
  if (!stmt) {
    return;
  }
  if (stmt->kind == STMT_VAR) {
    Symbol* symbol = symbol_new(sema, stmt->as.var_expr.var_name, SYM_GLOBAL);
    symbol->as.var = stmt;
    symbol->is_mut = stmt->as.var_expr.is_mut;
    symbol->is_global = TRUE;
    symbol->type = stmt->as.var_expr.type;
    scope_add(sema, sema->global, symbol);
    return;
  }
  if (stmt->kind == STMT_BLOCK) {
    for (u64 i = 0; i < vec_len(stmt->as.block.body); ++i) {
      collect_global_declaration(sema, vec_at_ptr(stmt->as.block.body, i));
    }
    return;
  }
  sema_error(sema, TAKA_MSG_S_TOP_LEVEL_STATEMENTS_ARE_NOT_ALLOWED);
}

static void resolve_sig_types(Sema* sema, FnSig* sig, Scope* scope) {
  if (!sig) {
    return;
  }
  if (sig->params) {
    for (u64 i = 0; i < vec_len(sig->params); ++i) {
      Param* param = vec_at_ptr(sig->params, i);
      if (param) {
        param->type = resolve_type(sema, param->type, scope, 0);
      }
    }
  }
  sig->return_type = resolve_type(sema, sig->return_type, scope, 0);
  resolve_effect_row(sema, sig->effects, scope);
  effects_for_sig(sema, sig);
}

static void resolve_declarations(Sema* sema) {
  for (u64 i = 0; i < vec_len(sema->global->symbols); ++i) {
    Symbol* symbol = vec_at_ptr(sema->global->symbols, i);
    if (!symbol) {
      continue;
    }
    Scope* scope = scope_new(sema, sema->global);
    switch (symbol->kind) {
      case SYM_STRUCT: {
        StructDecl* decl = symbol->as.struct_decl;
        add_generics(sema, scope, decl->generics);
        if (decl->fields) {
          for (u64 j = 0; j < vec_len(decl->fields); ++j) {
            StructField* field = vec_at_ptr(decl->fields, j);
            if (field) {
              field->type = resolve_type(sema, field->type, scope, 0);
            }
          }
        }
        break;
      }
      case SYM_ENUM: {
        EnumDecl* decl = symbol->as.enum_decl;
        add_generics(sema, scope, decl->generics);
        if (decl->variants) {
          for (u64 j = 0; j < vec_len(decl->variants); ++j) {
            EnumVariant* variant = vec_at_ptr(decl->variants, j);
            if (!variant) {
              continue;
            }
            if (variant->types) {
              for (u64 k = 0; k < vec_len(variant->types); ++k) {
                Type** slot = (Type**)vec_at(variant->types, k);
                if (slot) {
                  *slot = resolve_type(sema, *slot, scope, 0);
                }
              }
            }
            if (variant->fields) {
              for (u64 k = 0; k < vec_len(variant->fields); ++k) {
                VariantField* field = vec_at_ptr(variant->fields, k);
                if (field) {
                  field->type = resolve_type(sema, field->type, scope, 0);
                }
              }
            }
          }
        }
        break;
      }
      case SYM_FN: {
        FnDecl* fn = symbol->as.fn;
        add_generics(sema, scope, fn->sig.generics);
        resolve_sig_types(sema, &fn->sig, scope);
        symbol->type = make_fn_type(sema, &fn->sig);
        map_set_ptr(sema->item_effects, (u64)(uintptr_t)symbol->item,
                    effects_for_sig(sema, &fn->sig));
        break;
      }
      case SYM_EXTERN_FN: {
        ExternDecl* fn = symbol->as.extern_decl;
        add_generics(sema, scope, fn->sig.generics);
        resolve_sig_types(sema, &fn->sig, scope);
        symbol->type = make_fn_type(sema, &fn->sig);
        map_set_ptr(sema->item_effects, (u64)(uintptr_t)symbol->item,
                    effects_for_sig(sema, &fn->sig));
        break;
      }
      case SYM_EFFECT: {
        EffectDecl* decl = symbol->as.effect_decl;
        add_generics(sema, scope, decl->generics);
        if (decl->ops) {
          for (u64 j = 0; j < vec_len(decl->ops); ++j) {
            EffectOp* op = vec_at_ptr(decl->ops, j);
            if (!op) {
              continue;
            }
            if (op->params) {
              for (u64 k = 0; k < vec_len(op->params); ++k) {
                Param* param = vec_at_ptr(op->params, k);
                if (param) {
                  param->type = resolve_type(sema, param->type, scope, 0);
                }
              }
            }
            op->return_type = resolve_type(sema, op->return_type, scope, 0);
          }
        }
        break;
      }
      case SYM_TRAIT: {
        TraitDecl* decl = symbol->as.trait_decl;
        add_generics(sema, scope, decl->generics);
        if (decl->methods) {
          for (u64 j = 0; j < vec_len(decl->methods); ++j) {
            FnDecl* method = vec_at_ptr(decl->methods, j);
            if (!method) {
              continue;
            }
            add_generics(sema, scope, method->sig.generics);
            resolve_sig_types(sema, &method->sig, scope);
          }
        }
        break;
      }
      case SYM_HANDLER: {
        HandlerDecl* decl = symbol->as.handler_decl;
        add_generics(sema, scope, decl->generics);
        resolve_effect_row(sema, decl->effects, scope);
        if (decl->ops) {
          for (u64 j = 0; j < vec_len(decl->ops); ++j) {
            HandlerOp* op = vec_at_ptr(decl->ops, j);
            if (!op) {
              continue;
            }
            if (op->params) {
              for (u64 k = 0; k < vec_len(op->params); ++k) {
                Param* param = vec_at_ptr(op->params, k);
                if (param) {
                  param->type = resolve_type(sema, param->type, scope, 0);
                }
              }
            }
          }
        }
        break;
      }
      case SYM_TYPE_ALIAS: {
        TypeAliasDecl* alias = symbol->as.type_alias;
        add_generics(sema, scope, alias->generics);
        alias->type = resolve_type(sema, alias->type, scope, 0);
        break;
      }
      case SYM_GLOBAL: {
        Stmt* var = symbol->as.var;
        if (var->as.var_expr.type && var->as.var_expr.type->kind != TYPE_AUTO) {
          var->as.var_expr.type = resolve_type(sema, var->as.var_expr.type, scope, 0);
          symbol->type = var->as.var_expr.type;
        }
        break;
      }
      default:
        break;
    }
  }

  for (u64 i = 0; i < vec_len(sema->impls); ++i) {
    Item* item = vec_at_ptr(sema->impls, i);
    if (!item) {
      continue;
    }
    Scope* scope = scope_new(sema, sema->global);
    add_generics(sema, scope, item->as.impl_decl.generics);
    item->as.impl_decl.for_type =
        resolve_type(sema, item->as.impl_decl.for_type, scope, 0);
    sema->current_self_type = item->as.impl_decl.for_type;
    if (item->as.impl_decl.methods) {
      for (u64 j = 0; j < vec_len(item->as.impl_decl.methods); ++j) {
        FnDecl* method = vec_at_ptr(item->as.impl_decl.methods, j);
        if (!method) {
          continue;
        }
        add_generics(sema, scope, method->sig.generics);
        resolve_sig_types(sema, &method->sig, scope);
      }
    }
    sema->current_self_type = NULL;
  }
}

Symbol* sema_lookup_global(Sema* sema, String8 name) {
  if (!sema || !sema->global) {
    return NULL;
  }
  return scope_lookup(sema->global, name);
}

Symbol* sema_lookup(Sema* sema, String8 name) {
  if (!sema) {
    return NULL;
  }
  return scope_lookup(sema->current ? sema->current : sema->global, name);
}

Type* sema_expr_type(Sema* sema, Expr* expr) {
  if (!sema || !expr) {
    return NULL;
  }
  return map_get_ptr(sema->expr_types, (u64)(uintptr_t)expr);
}

Symbol* sema_expr_symbol(Sema* sema, Expr* expr) {
  if (!sema || !expr) {
    return NULL;
  }
  return map_get_ptr(sema->expr_symbols, (u64)(uintptr_t)expr);
}

Symbol* sema_pattern_symbol(Sema* sema, Pattern* pattern) {
  if (!sema || !pattern) {
    return NULL;
  }
  return map_get_ptr(sema->pattern_symbols, (u64)(uintptr_t)pattern);
}

Symbol* sema_capture_symbol(Sema* sema, Capture* capture) {
  if (!sema || !capture) {
    return NULL;
  }
  return map_get_ptr(sema->capture_symbols, (u64)(uintptr_t)capture);
}

EffectSet* sema_expr_effects(Sema* sema, Expr* expr) {
  if (!sema || !expr) {
    return NULL;
  }
  return map_get_ptr(sema->expr_effects, (u64)(uintptr_t)expr);
}

EffectSet* sema_item_effects(Sema* sema, Item* item) {
  if (!sema || !item) {
    return NULL;
  }
  return map_get_ptr(sema->item_effects, (u64)(uintptr_t)item);
}

u32 sema_error_count(Sema* sema) {
  return sema ? sema->errors : 0;
}

MemArena* sema_arena(Sema* sema) {
  return sema ? sema->arena : NULL;
}

Stmt* sema_program(Sema* sema) {
  return sema ? sema->program : NULL;
}

static void store_expr_type(Sema* sema, Expr* expr, Type* type) {
  if (expr) {
    map_set_ptr(sema->expr_types, (u64)(uintptr_t)expr, type);
  }
}

static void store_expr_symbol(Sema* sema, Expr* expr, Symbol* symbol) {
  if (expr) {
    map_set_ptr(sema->expr_symbols, (u64)(uintptr_t)expr, symbol);
  }
}

static void store_expr_effects(Sema* sema, Expr* expr, EffectSet* effects) {
  if (expr) {
    map_set_ptr(sema->expr_effects, (u64)(uintptr_t)expr, effects);
  }
}

static Type* analyze_expr(Sema* sema, Expr* expr, EffectSet* effects);
static Type* analyze_block(Sema* sema, Stmt* block, EffectSet* effects);
static void analyze_stmt(Sema* sema, Stmt* stmt, EffectSet* effects);
static void analyze_handler_ops(Sema* sema, Vector* ops, EffectSet* effects);

static void bind_pattern(Sema* sema, Pattern* pattern, Type* subject) {
  if (!pattern) {
    return;
  }
  switch (pattern->kind) {
    case PAT_BIND: {
      Symbol* symbol = symbol_new(sema, pattern->as.bind.name, SYM_LOCAL);
      symbol->is_mut = pattern->as.bind.is_mut;
      symbol->type = subject;
      scope_add(sema, sema->current, symbol);
      map_set_ptr(sema->pattern_symbols, (u64)(uintptr_t)pattern, symbol);
      return;
    }
    case PAT_REF_BIND: {
      Symbol* symbol = symbol_new(sema, pattern->as.bind.name, SYM_LOCAL);
      symbol->is_mut = TRUE;
      symbol->type = make_ptr_like(sema, subject, TRUE);
      scope_add(sema, sema->current, symbol);
      map_set_ptr(sema->pattern_symbols, (u64)(uintptr_t)pattern, symbol);
      return;
    }
    case PAT_WILDCARD:
    case PAT_LITERAL:
      return;
    case PAT_ALT:
      if (pattern->as.alt.alternatives) {
        for (u64 i = 0; i < vec_len(pattern->as.alt.alternatives); ++i) {
          bind_pattern(sema, vec_at_ptr(pattern->as.alt.alternatives, i), subject);
        }
      }
      return;
    case PAT_PATH: {
      String8 first = path_first(pattern->as.path.path);
      Symbol* symbol = sema_lookup_global(sema, first);
      EnumVariant* variant = NULL;
      if (symbol && symbol->kind == SYM_ENUM) {
        String8 variant_name = path_last(pattern->as.path.path);
        if (symbol->as.enum_decl->variants) {
          for (u64 i = 0; i < vec_len(symbol->as.enum_decl->variants); ++i) {
            EnumVariant* candidate = vec_at_ptr(symbol->as.enum_decl->variants, i);
            if (candidate && str8_eq(candidate->name, variant_name)) {
              variant = candidate;
              break;
            }
          }
        }
      }
      if (pattern->as.path.has_tuple && pattern->as.path.tuple) {
        for (u64 i = 0; i < vec_len(pattern->as.path.tuple); ++i) {
          Pattern* child = vec_at_ptr(pattern->as.path.tuple, i);
          Type* child_type = NULL;
          if (variant && variant->types && i < vec_len(variant->types)) {
            child_type = vec_at_ptr(variant->types, i);
          }
          bind_pattern(sema, child, child_type);
        }
      }
      if (pattern->as.path.has_struct && pattern->as.path.fields) {
        for (u64 i = 0; i < vec_len(pattern->as.path.fields); ++i) {
          FieldPat* field = vec_at_ptr(pattern->as.path.fields, i);
          if (!field || str8_eq(field->name, str8_lit(".."))) {
            continue;
          }
          Type* child_type = NULL;
          if (variant && variant->fields) {
            for (u64 j = 0; j < vec_len(variant->fields); ++j) {
              VariantField* candidate = vec_at_ptr(variant->fields, j);
              if (candidate && str8_eq(candidate->name, field->name)) {
                child_type = candidate->type;
                break;
              }
            }
          }
          if (field->has_pattern) {
            bind_pattern(sema, field->pattern, child_type);
          } else {
            Symbol* field_symbol = symbol_new(sema, field->name, SYM_LOCAL);
            field_symbol->type = child_type;
            scope_add(sema, sema->current, field_symbol);
          }
        }
      }
      return;
    }
  }
}

static Type* analyze_condition(Sema* sema, Condition* condition, EffectSet* effects) {
  if (!condition) {
    return NULL;
  }
  Type* type = analyze_expr(sema, condition->expr, effects);
  effects->builtins &= ~(u32)EFF_DECIDES;
  if (condition->is_binding) {
    bind_pattern(sema, condition->pattern, type);
  }
  return type;
}

static Type* analyze_lvalue(Sema* sema, Expr* expr, EffectSet* effects) {
  if (!expr) {
    return NULL;
  }
  switch (expr->kind) {
    case EXPR_SYMBOL: {
      Symbol* symbol = sema_lookup(sema, expr->as.symbol.value);
      if (!symbol) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_VARIABLE_S, str8_fmt(expr->as.symbol.value));
        return NULL;
      }
      store_expr_symbol(sema, expr, symbol);
      store_expr_type(sema, expr, symbol->type);
      if (symbol->type && symbol->type->is_reference) {
        if (symbol->type->is_const) {
          sema_error(sema, TAKA_MSG_S_CANNOT_WRITE_THROUGH_CONST_REFERENCE_S,
                     str8_fmt(expr->as.symbol.value));
        }
        effect_add_builtin(effects, EFF_WRITES);
        return symbol->type;
      }
      if (symbol->is_global) {
        effect_add_builtin(effects, EFF_WRITES);
      }
      if (!symbol->is_mut) {
        sema_error(sema, TAKA_MSG_S_CANNOT_ASSIGN_TO_IMMUTABLE_S,
                   str8_fmt(expr->as.symbol.value));
      }
      return symbol->type;
    }
    case EXPR_FIELD: {
      Type* object_type = analyze_expr(sema, expr->as.field.object, effects);
      if (is_pointer_like(object_type)) {
        effect_add_builtin(effects, EFF_WRITES);
      }
      Type* base = strip_pointer_one(sema, object_type);
      Symbol* symbol = find_struct_symbol_from_type(sema, base);
      StructField* field = find_field(symbol, expr->as.field.field);
      if (!field) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_FIELD_S, str8_fmt(expr->as.field.field));
        return NULL;
      }
      if (field->type && field->type->is_const) {
        sema_error(sema, TAKA_MSG_S_CANNOT_WRITE_TO_CONST_FIELD_S,
                   str8_fmt(expr->as.field.field));
      }
      store_expr_type(sema, expr, field->type);
      return field->type;
    }
    case EXPR_INDEX: {
      Type* object_type = analyze_expr(sema, expr->as.index.object, effects);
      analyze_expr(sema, expr->as.index.index, effects);
      Type* element = NULL;
      if (object_type && object_type->kind == TYPE_ARRAY) {
        element = object_type->as.array.element;
      } else if (object_type &&
                 (object_type->pointer_depth > 0 || object_type->is_reference)) {
        element = strip_pointer_one(sema, object_type);
        effect_add_builtin(effects, EFF_WRITES);
      } else {
        sema_error(sema, TAKA_MSG_S_INDEX_ASSIGNMENT_REQUIRES_AN_ARRAY_OR_POINTER);
      }
      store_expr_type(sema, expr, element);
      return element;
    }
    case EXPR_PREFIX: {
      if (expr->as.prefix.oper.kind == TOKEN_STAR) {
        Type* inner = analyze_expr(sema, expr->as.prefix.right_expr, effects);
        effect_add_builtin(effects, EFF_WRITES);
        Type* type = strip_pointer_one(sema, inner);
        store_expr_type(sema, expr, type);
        return type;
      }
      sema_error(sema, TAKA_MSG_S_INVALID_ASSIGNMENT_TARGET);
      return NULL;
    }
    default:
      sema_error(sema, TAKA_MSG_S_INVALID_ASSIGNMENT_TARGET);
      return NULL;
  }
}

static bool resolve_enum_variant(Sema* sema, Path path, Symbol** out_symbol,
                                 EnumVariant** out_variant) {
  if (out_symbol) *out_symbol = NULL;
  if (out_variant) *out_variant = NULL;
  String8 first = path_first(path);
  String8 last = path_last(path);
  Symbol* symbol = sema_lookup_global(sema, first);
  if (!symbol || symbol->kind != SYM_ENUM || !symbol->as.enum_decl->variants) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(symbol->as.enum_decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(symbol->as.enum_decl->variants, i);
    if (variant && str8_eq(variant->name, last)) {
      if (out_symbol) *out_symbol = symbol;
      if (out_variant) *out_variant = variant;
      return TRUE;
    }
  }
  return FALSE;
}

Symbol* sema_lookup_enum_variant(Sema* sema, Path path, EnumVariant** out_variant) {
  Symbol* symbol = NULL;
  resolve_enum_variant(sema, path, &symbol, out_variant);
  return symbol;
}

static Symbol* find_trait_method(Sema* sema, Symbol* trait_symbol, String8 name,
                                 FnDecl** out_method) {
  (void)sema;
  if (!trait_symbol || trait_symbol->kind != SYM_TRAIT || !trait_symbol->as.trait_decl) {
    return NULL;
  }
  TraitDecl* trait = trait_symbol->as.trait_decl;
  if (!trait->methods) {
    return NULL;
  }
  for (u64 i = 0; i < vec_len(trait->methods); ++i) {
    FnDecl* method = vec_at_ptr(trait->methods, i);
    if (method && str8_eq(method->sig.name, name)) {
      if (out_method) *out_method = method;
      return trait_symbol;
    }
  }
  return NULL;
}

static Symbol* find_impl_method(Sema* sema, Type* object_type, String8 name,
                                FnDecl** out_method) {
  if (!object_type || object_type->kind != TYPE_PATH) {
    return NULL;
  }
  String8 object_name = path_first(object_type->as.path.path);
  for (u64 i = 0; i < vec_len(sema->impls); ++i) {
    Item* item = vec_at_ptr(sema->impls, i);
    if (!item) {
      continue;
    }
    ImplDecl* impl = &item->as.impl_decl;
    if (!impl->for_type || impl->for_type->kind != TYPE_PATH) {
      continue;
    }
    if (!str8_eq(path_first(impl->for_type->as.path.path), object_name)) {
      continue;
    }
    if (!impl->methods) {
      continue;
    }
    for (u64 j = 0; j < vec_len(impl->methods); ++j) {
      FnDecl* method = vec_at_ptr(impl->methods, j);
      if (method && str8_eq(method->sig.name, name)) {
        if (out_method) *out_method = method;
        return sema_lookup_global(sema, path_first(impl->trait_path));
      }
    }
  }
  return NULL;
}

Symbol* sema_lookup_method(Sema* sema, Type* object_type, String8 method_name,
                           FnDecl** out_method) {
  Type* base = strip_pointer_one(sema, object_type);
  Symbol* result = find_impl_method(sema, base, method_name, out_method);
  if (result) {
    return result;
  }
  Symbol* symbol = find_struct_symbol_from_type(sema, base);
  if (symbol && symbol->kind == SYM_TRAIT) {
    return find_trait_method(sema, symbol, method_name, out_method);
  }
  if (symbol && symbol->kind == SYM_STRUCT) {
    StructField* field = find_field(symbol, method_name);
    if (field && field->type && field->type->kind == TYPE_FN) {
      return symbol;
    }
  }
  return NULL;
}

static Type* analyze_call(Sema* sema, Expr* expr, EffectSet* effects) {
  Expr* callee = expr->as.call.callee;
  Type* result = NULL;
  Vector* call_params = NULL;
  u64 call_param_offset = 0;

  if (callee && callee->kind == EXPR_PATH) {
    Symbol* enum_symbol = NULL;
    EnumVariant* variant = NULL;
    if (resolve_enum_variant(sema, callee->as.path.path, &enum_symbol, &variant)) {
      store_expr_symbol(sema, callee, enum_symbol);
      Type* enum_type = enum_symbol->type;
      store_expr_type(sema, callee, enum_type);
      result = enum_type;
      for (u64 i = 0; i < vec_len(expr->as.call.args); ++i) {
        analyze_expr(sema, vec_at_ptr(expr->as.call.args, i), effects);
      }
      store_expr_type(sema, expr, result);
      return result;
    }
  }

  if (callee && callee->kind == EXPR_FIELD) {
    Type* object_type = analyze_expr(sema, callee->as.field.object, effects);
    Symbol* method_symbol = NULL;
    FnDecl* method = NULL;
    method_symbol = sema_lookup_method(sema, object_type, callee->as.field.field, &method);
    if (method) {
      EffectSet* method_effects = effects_for_sig(sema, &method->sig);
      effect_merge(effects, method_effects);
      call_params = method->sig.params;
      call_param_offset = 1;
      result = method->sig.return_type;
      store_expr_type(sema, callee, make_fn_type(sema, &method->sig));
      store_expr_symbol(sema, callee, method_symbol);
    } else {
      Type* callee_type = sema_expr_type(sema, callee);
      if (callee_type && callee_type->kind == TYPE_FN) {
        EffectSet* fn_effects = effect_from_row(sema, callee_type->as.fn.effects);
        effect_merge(effects, fn_effects);
        result = callee_type->as.fn.return_type;
      } else {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_METHOD_OR_CALLABLE_FIELD_S,
                   str8_fmt(callee->as.field.field));
      }
    }
  } else {
    Type* callee_type = analyze_expr(sema, callee, effects);
    Symbol* symbol = sema_expr_symbol(sema, callee);
    if (symbol) {
      if (symbol->kind == SYM_FN || symbol->kind == SYM_EXTERN_FN) {
        Item* item = symbol->item;
        FnSig* fn_sig = item->kind == ITEM_FN ? &item->as.fn.sig : &item->as.extern_decl.sig;
        call_params = fn_sig->params;
        call_param_offset = 0;
        EffectSet* fn_effects = sema_item_effects(sema, item);
        if (fn_effects) {
          effect_merge(effects, fn_effects);
        }
        if (item->kind == ITEM_FN) {
          result = item->as.fn.sig.return_type;
        } else if (item->kind == ITEM_EXTERN) {
          result = item->as.extern_decl.sig.return_type;
        }
      } else if (callee_type && callee_type->kind == TYPE_FN) {
        EffectSet* fn_effects = effect_from_row(sema, callee_type->as.fn.effects);
        effect_merge(effects, fn_effects);
        result = callee_type->as.fn.return_type;
      } else {
        sema_error(sema, TAKA_MSG_S_SYMBOL_S_IS_NOT_CALLABLE, str8_fmt(symbol->name));
      }
    } else if (!(callee_type && callee_type->kind == TYPE_FN)) {
      sema_error(sema, TAKA_MSG_S_EXPRESSION_IS_NOT_CALLABLE);
    } else {
      EffectSet* fn_effects = effect_from_row(sema, callee_type->as.fn.effects);
      effect_merge(effects, fn_effects);
      result = callee_type->as.fn.return_type;
    }
  }

  for (u64 i = 0; i < vec_len(expr->as.call.args); ++i) {
    Expr* arg = vec_at_ptr(expr->as.call.args, i);
    Param* param = NULL;
    if (call_params && call_param_offset + i < vec_len(call_params)) {
      param = vec_at_ptr(call_params, call_param_offset + i);
    }
    if (param && param->type && param->type->is_reference) {
      EffectSet* arg_effects = effect_new(sema);
      analyze_expr(sema, arg, arg_effects);
      arg_effects->builtins &= ~(u32)EFF_READS;
      effect_merge(effects, arg_effects);
    } else {
      analyze_expr(sema, arg, effects);
    }
  }
  store_expr_type(sema, expr, result);
  return result;
}

static Type* analyze_expr(Sema* sema, Expr* expr, EffectSet* effects) {
  if (!expr) {
    return NULL;
  }
  switch (expr->kind) {
    case EXPR_NUMBER: {
      Type* type = make_primitive(sema, "i32");
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_FLOAT: {
      Type* type = make_primitive(sema, "f64");
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_STRING: {
      Type* type = make_primitive(sema, "sview");
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_CHAR: {
      Type* type = make_primitive(sema, "char");
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_BOOL: {
      Type* type = make_bool_type(sema);
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_NULL:
    case EXPR_UNDEF:
      store_expr_type(sema, expr, NULL);
      return NULL;
    case EXPR_SYMBOL: {
      Symbol* symbol = sema_lookup(sema, expr->as.symbol.value);
      if (!symbol) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_SYMBOL_S, str8_fmt(expr->as.symbol.value));
        return NULL;
      }
      store_expr_symbol(sema, expr, symbol);
      if (symbol->is_global && symbol->kind == SYM_GLOBAL) {
        effect_add_builtin(effects, EFF_READS);
      }
      if (symbol->type && symbol->type->is_reference) {
        effect_add_builtin(effects, EFF_READS);
      }
      store_expr_type(sema, expr, symbol->type);
      return symbol->type;
    }
    case EXPR_PATH: {
      Symbol* symbol = sema_lookup_global(sema, path_first(expr->as.path.path));
      if (!symbol) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_PATH_S, str8_fmt(path_first(expr->as.path.path)));
        return NULL;
      }
      store_expr_symbol(sema, expr, symbol);
      if (symbol->kind == SYM_ENUM) {
        store_expr_type(sema, expr, symbol->type);
        return symbol->type;
      }
      if (symbol->kind == SYM_FN || symbol->kind == SYM_EXTERN_FN) {
        store_expr_type(sema, expr, symbol->type);
        return symbol->type;
      }
      if (symbol->kind == SYM_EFFECT) {
        return NULL;
      }
      sema_error(sema, TAKA_MSG_S_PATH_S_IS_NOT_AN_EXPRESSION,
                 str8_fmt(path_first(expr->as.path.path)));
      return NULL;
    }
    case EXPR_BINARY: {
      Type* left = analyze_expr(sema, expr->as.bin.lhs, effects);
      Type* right = analyze_expr(sema, expr->as.bin.rhs, effects);
      TokenKind kind = expr->as.bin.oper.kind;
      Type* result = left ? left : right;
      if (kind == TOKEN_EQUALS || kind == TOKEN_NOT_EQUALS || kind == TOKEN_LESS ||
          kind == TOKEN_GREATER || kind == TOKEN_LESS_EQUALS ||
          kind == TOKEN_GREATER_EQUALS || kind == TOKEN_AMP_AMP ||
          kind == TOKEN_PIPE_PIPE) {
        result = make_bool_type(sema);
      }
      store_expr_type(sema, expr, result);
      return result;
    }
    case EXPR_TERNARY: {
      analyze_expr(sema, expr->as.ternary.cond, effects);
      Type* then_type = analyze_expr(sema, expr->as.ternary.then_expr, effects);
      Type* else_type = analyze_expr(sema, expr->as.ternary.else_expr, effects);
      Type* result = then_type ? then_type : else_type;
      store_expr_type(sema, expr, result);
      return result;
    }
    case EXPR_PREFIX: {
      TokenKind kind = expr->as.prefix.oper.kind;
      Type* inner = analyze_expr(sema, expr->as.prefix.right_expr, effects);
      Type* result = inner;
      if (kind == TOKEN_AMP) {
        result = make_ptr_like(sema, inner, FALSE);
      } else if (kind == TOKEN_STAR) {
        effect_add_builtin(effects, EFF_READS);
        result = strip_pointer_one(sema, inner);
      } else if (kind == TOKEN_NOT) {
        result = make_bool_type(sema);
      } else if (kind == TOKEN_PLUS_PLUS || kind == TOKEN_MINUS_MINUS) {
        result = analyze_lvalue(sema, expr->as.prefix.right_expr, effects);
      }
      store_expr_type(sema, expr, result);
      return result;
    }
    case EXPR_POSTFIX: {
      Type* result = analyze_lvalue(sema, expr->as.postfix.expr, effects);
      store_expr_type(sema, expr, result);
      return result;
    }
    case EXPR_ASSIGNMENT: {
      Type* target = analyze_lvalue(sema, expr->as.assignment.assigne_expr, effects);
      analyze_expr(sema, expr->as.assignment.rhs, effects);
      store_expr_type(sema, expr, target);
      return target;
    }
    case EXPR_CAST: {
      analyze_expr(sema, expr->as.cast.expr, effects);
      Type* type = resolve_type(sema, expr->as.cast.type, sema->current, 0);
      expr->as.cast.type = type;
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_CALL:
      return analyze_call(sema, expr, effects);
    case EXPR_INDEX: {
      Type* object_type = analyze_expr(sema, expr->as.index.object, effects);
      analyze_expr(sema, expr->as.index.index, effects);
      Type* element = NULL;
      if (object_type && object_type->kind == TYPE_ARRAY) {
        element = object_type->as.array.element;
      } else if (object_type && is_pointer_like(object_type)) {
        effect_add_builtin(effects, EFF_READS);
        element = strip_pointer_one(sema, object_type);
      } else {
        sema_error(sema, TAKA_MSG_S_INDEXING_REQUIRES_AN_ARRAY_OR_POINTER);
      }
      store_expr_type(sema, expr, element);
      return element;
    }
    case EXPR_FIELD: {
      Type* object_type = analyze_expr(sema, expr->as.field.object, effects);
      Type* base = strip_pointer_one(sema, object_type);
      Symbol* symbol = find_struct_symbol_from_type(sema, base);
      StructField* field = find_field(symbol, expr->as.field.field);
      if (field) {
        if (is_pointer_like(object_type)) {
          effect_add_builtin(effects, EFF_READS);
        }
        store_expr_type(sema, expr, field->type);
        return field->type;
      }
      if (symbol && symbol->kind == SYM_STRUCT) {
        FnDecl* method = NULL;
        if (sema_lookup_method(sema, object_type, expr->as.field.field, &method) && method) {
          Type* method_type = make_fn_type(sema, &method->sig);
          store_expr_type(sema, expr, method_type);
          return method_type;
        }
      }
      store_expr_type(sema, expr, NULL);
      return NULL;
    }
    case EXPR_STRUCT_LIT: {
      String8 name = path_first(expr->as.struct_lit.path);
      Symbol* symbol = sema_lookup_global(sema, name);
      EnumVariant* enum_variant = NULL;
      Symbol* enum_symbol = NULL;
      if (!symbol || symbol->kind != SYM_STRUCT) {
        enum_symbol = sema_lookup_enum_variant(sema, expr->as.struct_lit.path, &enum_variant);
        if (enum_symbol && enum_variant && enum_variant->kind == VARIANT_STRUCT) {
          if (expr->as.struct_lit.fields) {
            for (u64 i = 0; i < vec_len(expr->as.struct_lit.fields); ++i) {
              FieldInit* field = vec_at_ptr(expr->as.struct_lit.fields, i);
              if (!field) {
                continue;
              }
              analyze_expr(sema, field->value, effects);
              if (!find_variant_field_type(enum_variant, field->name)) {
                sema_error(sema, TAKA_MSG_S_UNKNOWN_ENUM_VARIANT_FIELD_S,
                           str8_fmt(field->name));
              }
            }
          }
          store_expr_type(sema, expr, enum_symbol->type);
          return enum_symbol->type;
        }
        sema_error(sema, TAKA_MSG_S_UNKNOWN_STRUCT_S, str8_fmt(name));
        return NULL;
      }
      if (expr->as.struct_lit.fields) {
        for (u64 i = 0; i < vec_len(expr->as.struct_lit.fields); ++i) {
          FieldInit* field = vec_at_ptr(expr->as.struct_lit.fields, i);
          if (!field) {
            continue;
          }
          analyze_expr(sema, field->value, effects);
          if (!find_field(symbol, field->name)) {
            sema_error(sema, TAKA_MSG_S_UNKNOWN_FIELD_S, str8_fmt(field->name));
          }
        }
      }
      store_expr_type(sema, expr, symbol->type);
      return symbol->type;
    }
    case EXPR_ARRAY_LIT: {
      Type* element = NULL;
      if (expr->as.array_lit.is_fill) {
        element = analyze_expr(sema, expr->as.array_lit.fill, effects);
        analyze_expr(sema, expr->as.array_lit.count, effects);
      } else {
        for (u64 i = 0; i < vec_len(expr->as.array_lit.items); ++i) {
          Type* item = analyze_expr(sema, vec_at_ptr(expr->as.array_lit.items, i), effects);
          if (!element) {
            element = item;
          }
        }
      }
      Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
      type->kind = TYPE_ARRAY;
      type->as.array.element = element;
      type->as.array.length = expr->as.array_lit.is_fill ? expr->as.array_lit.count : NULL;
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_LAMBDA: {
      Scope* scope = scope_new(sema, sema->current);
      scope_push(sema, scope);
      if (expr->as.lambda.captures) {
        for (u64 i = 0; i < vec_len(expr->as.lambda.captures); ++i) {
          Capture* capture = vec_at_ptr(expr->as.lambda.captures, i);
          if (!capture) {
            continue;
          }
          Symbol* outer = scope_lookup(scope->parent, capture->name);
          Symbol* symbol = symbol_new(sema, capture->name, SYM_CAPTURE);
          symbol->is_mut = capture->is_mut;
          symbol->capture_outer = outer;
          if (outer && outer->type) {
            if (capture->by_ref) {
              symbol->type = outer->type->is_reference
                                 ? outer->type
                                 : make_ptr_like(sema, outer->type, TRUE);
              symbol->is_mut = TRUE;
            } else {
              symbol->type = outer->type->is_reference
                                 ? strip_pointer_one(sema, outer->type)
                                 : outer->type;
            }
          }
          scope_add(sema, scope, symbol);
          map_set_ptr(sema->capture_symbols, (u64)(uintptr_t)capture, symbol);
          if (capture->has_init) {
            Type* init_type = analyze_expr(sema, capture->init, effects);
            if (init_type) {
              symbol->type = init_type;
            }
          } else if (!capture->by_ref && outer) {
            if (outer->is_global || (outer->type && outer->type->is_reference)) {
              effect_add_builtin(effects, EFF_READS);
            }
          }
        }
      }
      if (expr->as.lambda.params) {
        for (u64 i = 0; i < vec_len(expr->as.lambda.params); ++i) {
          Param* param = vec_at_ptr(expr->as.lambda.params, i);
          if (!param) {
            continue;
          }
          Symbol* symbol = symbol_new(sema, param->name, SYM_PARAM);
          symbol->is_mut = param->is_mut;
          param->type = resolve_type(sema, param->type, scope, 0);
          symbol->type = param->type;
          scope_add(sema, scope, symbol);
        }
      }
      expr->as.lambda.return_type =
          resolve_type(sema, expr->as.lambda.return_type, scope, 0);
      EffectSet* lambda_effects = effect_new(sema);
      Type* body_type = NULL;
      if (expr->as.lambda.body_is_block) {
        body_type = analyze_block(sema, expr->as.lambda.block, lambda_effects);
      } else {
        body_type = analyze_expr(sema, expr->as.lambda.expr, lambda_effects);
      }
      scope_pop(sema);
      store_expr_effects(sema, expr, lambda_effects);
      Type* type = PUSH_STRUCT_ZERO(sema->arena, Type);
      type->kind = TYPE_FN;
      type->as.fn.params = vec_new(sema->arena, sizeof(Type*), 0);
      if (expr->as.lambda.params) {
        for (u64 i = 0; i < vec_len(expr->as.lambda.params); ++i) {
          Param* param = vec_at_ptr(expr->as.lambda.params, i);
          if (param) {
            vec_push_ptr(type->as.fn.params, param->type);
          }
        }
      }
      if (expr->as.lambda.effects) {
        EffectSet* declared = effect_from_row(sema, expr->as.lambda.effects);
        effect_is_subset(sema, lambda_effects, declared, "lambda body");
        type->as.fn.effects = expr->as.lambda.effects;
      } else {
        type->as.fn.effects = effect_set_to_row(sema, lambda_effects);
      }
      type->as.fn.return_type = expr->as.lambda.return_type ? expr->as.lambda.return_type
                                                           : body_type;
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_SIZEOF: {
      if (expr->as.sizeof_expr.is_type) {
        expr->as.sizeof_expr.type =
            resolve_type(sema, expr->as.sizeof_expr.type, sema->current, 0);
      } else {
        EffectSet* ignored = effect_new(sema);
        analyze_expr(sema, expr->as.sizeof_expr.expr, ignored);
      }
      Type* type = make_u64_type(sema);
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_OFFSETOF: {
      expr->as.offsetof_expr.type =
          resolve_type(sema, expr->as.offsetof_expr.type, sema->current, 0);
      Type* type = make_u64_type(sema);
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_IF: {
      EffectSet* branch_effects = effect_new(sema);
      Type* type = NULL;
      if (expr->as.if_expr.branches) {
        for (u64 i = 0; i < vec_len(expr->as.if_expr.branches); ++i) {
          IfBranch* branch = vec_at_ptr(expr->as.if_expr.branches, i);
          if (!branch) {
            continue;
          }
          Scope* scope = scope_new(sema, sema->current);
          scope_push(sema, scope);
          analyze_condition(sema, &branch->condition, branch_effects);
          Type* body_type = analyze_block(sema, branch->body, branch_effects);
          if (!type) {
            type = body_type;
          }
          scope_pop(sema);
        }
      }
      Type* else_type = NULL;
      if (expr->as.if_expr.else_block) {
        Scope* scope = scope_new(sema, sema->current);
        scope_push(sema, scope);
        else_type = analyze_block(sema, expr->as.if_expr.else_block, branch_effects);
        scope_pop(sema);
      }
      if (!type) {
        type = else_type;
      }
      effect_merge(effects, branch_effects);
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_MATCH: {
      Type* subject_type = analyze_expr(sema, expr->as.match.subject, effects);
      Type* type = NULL;
      if (expr->as.match.arms) {
        for (u64 i = 0; i < vec_len(expr->as.match.arms); ++i) {
          MatchArm* arm = vec_at_ptr(expr->as.match.arms, i);
          if (!arm) {
            continue;
          }
          Scope* scope = scope_new(sema, sema->current);
          scope_push(sema, scope);
          bind_pattern(sema, arm->pattern, subject_type);
          if (arm->guard) {
            analyze_expr(sema, arm->guard, effects);
          }
          Type* arm_type = NULL;
          if (arm->body_is_block) {
            arm_type = analyze_block(sema, arm->block, effects);
          } else {
            arm_type = analyze_expr(sema, arm->expr, effects);
          }
          if (!type) {
            type = arm_type;
          }
          scope_pop(sema);
        }
      }
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_PERFORM: {
      String8 effect_name = path_first(expr->as.perform.path);
      String8 op_name = path_last(expr->as.perform.path);
      Symbol* symbol = sema_lookup_global(sema, effect_name);
      Type* result = NULL;
      bool found = FALSE;
      if (symbol && symbol->kind == SYM_EFFECT && symbol->as.effect_decl->ops) {
        for (u64 i = 0; i < vec_len(symbol->as.effect_decl->ops); ++i) {
          EffectOp* op = vec_at_ptr(symbol->as.effect_decl->ops, i);
          if (op && str8_eq(op->name, op_name)) {
            result = op->return_type;
            found = TRUE;
            break;
          }
        }
      }
      if (!found) {
        sema_error(sema, TAKA_MSG_S_UNKNOWN_EFFECT_OPERATION_S, str8_fmt(op_name));
      } else {
        effect_add_custom(effects, effect_name);
      }
      for (u64 i = 0; i < vec_len(expr->as.perform.args); ++i) {
        analyze_expr(sema, vec_at_ptr(expr->as.perform.args, i), effects);
      }
      store_expr_type(sema, expr, result);
      return result;
    }
    case EXPR_HANDLE: {
      EffectSet* subject_effects = effect_new(sema);
      Type* subject_type = NULL;
      if (expr->as.handle.subject_is_block) {
        subject_type = analyze_block(sema, expr->as.handle.block, subject_effects);
      } else {
        subject_type = analyze_expr(sema, expr->as.handle.subject, subject_effects);
      }
      if (expr->as.handle.handler_is_path) {
        Symbol* handler =
            sema_lookup_global(sema, path_first(expr->as.handle.handler_path));
        if (!handler || handler->kind != SYM_HANDLER) {
          sema_error(sema, TAKA_MSG_S_UNKNOWN_HANDLER_S,
                     str8_fmt(path_first(expr->as.handle.handler_path)));
        } else {
          String8 handled = path_first(handler->as.handler_decl->effect_path);
          analyze_handler_ops(sema, handler->as.handler_decl->ops, effects);
          EffectSet* filtered = effect_new(sema);
          filtered->builtins = subject_effects->builtins;
          filtered->tail = subject_effects->tail;
          if (subject_effects->customs) {
            for (u64 i = 0; i < vec_len(subject_effects->customs); ++i) {
              String8* name = vec_at(subject_effects->customs, i);
              if (name && !str8_eq(*name, handled)) {
                effect_add_custom(filtered, *name);
              }
            }
          }
          effect_merge(effects, filtered);
        }
      } else {
        EffectSet* filtered = effect_clone(sema, subject_effects);
        if (expr->as.handle.handler_ops) {
          for (u64 i = 0; i < vec_len(expr->as.handle.handler_ops); ++i) {
            HandlerOp* op = vec_at_ptr(expr->as.handle.handler_ops, i);
            if (!op || op->is_return) {
              continue;
            }
            String8 effect_name = {0};
            if (find_effect_for_op(sema, op->name, &effect_name)) {
              effect_remove_custom(filtered, effect_name);
            }
          }
        }
        effect_merge(effects, filtered);
        if (expr->as.handle.handler_ops) {
          for (u64 i = 0; i < vec_len(expr->as.handle.handler_ops); ++i) {
            HandlerOp* op = vec_at_ptr(expr->as.handle.handler_ops, i);
            if (!op) {
              continue;
            }
            Scope* scope = scope_new(sema, sema->current);
            scope_push(sema, scope);
            if (op->params) {
              for (u64 j = 0; j < vec_len(op->params); ++j) {
                Param* param = vec_at_ptr(op->params, j);
                if (!param) {
                  continue;
                }
                Symbol* symbol = symbol_new(sema, param->name, SYM_PARAM);
                symbol->is_mut = param->is_mut;
                symbol->type = param->type;
                scope_add(sema, scope, symbol);
              }
            }
            EffectSet* op_effects = effect_new(sema);
            if (op->body_is_block) {
              analyze_block(sema, op->block, op_effects);
            } else {
              analyze_expr(sema, op->expr, op_effects);
            }
            effect_merge(effects, op_effects);
            scope_pop(sema);
          }
        }
      }
      store_expr_type(sema, expr, subject_type);
      return subject_type;
    }
    case EXPR_SPAWN: {
      EffectSet* spawned = effect_new(sema);
      if (expr->as.spawn.is_block) {
        analyze_block(sema, expr->as.spawn.block, spawned);
      } else {
        analyze_expr(sema, expr->as.spawn.expr, spawned);
      }
      spawned->builtins &= ~(u32)EFF_SUSPENDS;
      effect_merge(effects, spawned);
      Type* type = make_primitive(sema, "void");
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_RESUME: {
      if (expr->as.resume.has_value) {
        analyze_expr(sema, expr->as.resume.value, effects);
      }
      store_expr_type(sema, expr, NULL);
      return NULL;
    }
    case EXPR_TRANSACT: {
      EffectSet* body_effects = effect_new(sema);
      analyze_block(sema, expr->as.transact.block, body_effects);
      EffectSet* allowed = effect_new(sema);
      effect_add_builtin(allowed, EFF_READS);
      effect_add_builtin(allowed, EFF_WRITES);
      effect_add_builtin(allowed, EFF_ALLOC);
      effect_add_builtin(allowed, EFF_TRANSACTS);
      effect_is_subset(sema, body_effects, allowed, "transact body");
      if (!sema->current_fn_effects ||
          !effect_has_builtin_value(sema->current_fn_effects, EFF_TRANSACTS)) {
        sema_error(sema, TAKA_MSG_S_TRANSACT_REQUIRES_THE_ENCLOSING_FUNCTION_TO_DECLARE_TRANSACTS);
      }
      effect_merge(effects, body_effects);
      effect_add_builtin(effects, EFF_TRANSACTS);
      Type* type = make_bool_type(sema);
      store_expr_type(sema, expr, type);
      return type;
    }
    case EXPR_BLOCK: {
      Type* type = analyze_block(sema, expr->as.block.block, effects);
      store_expr_type(sema, expr, type);
      return type;
    }
  }
  return NULL;
}

static Type* analyze_block(Sema* sema, Stmt* block, EffectSet* effects) {
  if (!block || block->kind != STMT_BLOCK) {
    return NULL;
  }
  Scope* scope = scope_new(sema, sema->current);
  scope_push(sema, scope);
  Type* tail_type = NULL;
  if (block->as.block.body) {
    for (u64 i = 0; i < vec_len(block->as.block.body); ++i) {
      Stmt* stmt = vec_at_ptr(block->as.block.body, i);
      analyze_stmt(sema, stmt, effects);
    }
  }
  if (block->as.block.tail) {
    tail_type = analyze_expr(sema, block->as.block.tail, effects);
  }
  scope_pop(sema);
  return tail_type;
}

static void analyze_var_stmt(Sema* sema, Stmt* stmt, EffectSet* effects) {
  Type* declared = stmt->as.var_expr.type;
  if (declared && declared->kind != TYPE_AUTO) {
    declared = resolve_type(sema, declared, sema->current, 0);
    stmt->as.var_expr.type = declared;
  }
  Type* value_type = analyze_expr(sema, &stmt->as.var_expr.assigned_value, effects);
  if (!declared || declared->kind == TYPE_AUTO) {
    bool wants_reference = declared && declared->is_reference;
    bool wants_const = declared && declared->is_const;
    declared = wants_reference ? make_ptr_like(sema, value_type, TRUE) : value_type;
    if (declared && wants_const) {
      declared->is_const = TRUE;
    }
    stmt->as.var_expr.type = declared;
    stmt->as.var_expr.is_auto = TRUE;
  }
  if (!declared) {
    sema_error(sema, TAKA_MSG_S_CANNOT_INFER_THE_TYPE_OF_S,
               str8_fmt(stmt->as.var_expr.var_name));
  }
  Symbol* symbol = symbol_new(sema, stmt->as.var_expr.var_name, SYM_LOCAL);
  symbol->is_mut = stmt->as.var_expr.is_mut;
  symbol->as.var = stmt;
  symbol->type = declared;
  scope_add(sema, sema->current, symbol);
}

static void analyze_handler_ops(Sema* sema, Vector* ops, EffectSet* effects) {
  if (!ops) {
    return;
  }
  for (u64 i = 0; i < vec_len(ops); ++i) {
    HandlerOp* op = vec_at_ptr(ops, i);
    if (!op) {
      continue;
    }
    Scope* scope = scope_new(sema, sema->current);
    scope_push(sema, scope);
    if (op->params) {
      for (u64 j = 0; j < vec_len(op->params); ++j) {
        Param* param = vec_at_ptr(op->params, j);
        if (!param) {
          continue;
        }
        Symbol* symbol = symbol_new(sema, param->name, SYM_PARAM);
        symbol->is_mut = param->is_mut;
        symbol->type = param->type;
        scope_add(sema, scope, symbol);
      }
    }
    if (op->body_is_block) {
      analyze_block(sema, op->block, effects);
    } else {
      analyze_expr(sema, op->expr, effects);
    }
    scope_pop(sema);
  }
}

static void analyze_stmt(Sema* sema, Stmt* stmt, EffectSet* effects) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind) {
    case STMT_BLOCK:
      analyze_block(sema, stmt, effects);
      return;
    case STMT_EXPRESION:
      analyze_expr(sema, &stmt->as.expresion.expr, effects);
      return;
    case STMT_VAR:
      analyze_var_stmt(sema, stmt, effects);
      return;
    case STMT_DEFER:
      if (stmt->as.defer_stmt.is_block) {
        analyze_block(sema, stmt->as.defer_stmt.block, effects);
      } else {
        analyze_stmt(sema, stmt->as.defer_stmt.stmt, effects);
      }
      return;
    case STMT_IF:
      if (stmt->as.if_stmt.branches) {
        for (u64 i = 0; i < vec_len(stmt->as.if_stmt.branches); ++i) {
          IfBranch* branch = vec_at_ptr(stmt->as.if_stmt.branches, i);
          if (!branch) {
            continue;
          }
          Scope* scope = scope_new(sema, sema->current);
          scope_push(sema, scope);
          analyze_condition(sema, &branch->condition, effects);
          analyze_block(sema, branch->body, effects);
          scope_pop(sema);
        }
      }
      if (stmt->as.if_stmt.else_block) {
        analyze_block(sema, stmt->as.if_stmt.else_block, effects);
      }
      return;
    case STMT_WHILE: {
      Scope* scope = scope_new(sema, sema->current);
      scope_push(sema, scope);
      analyze_condition(sema, &stmt->as.while_stmt.condition, effects);
      analyze_block(sema, stmt->as.while_stmt.body, effects);
      scope_pop(sema);
      return;
    }
    case STMT_FOR: {
      Scope* scope = scope_new(sema, sema->current);
      scope_push(sema, scope);
      ForStmt* loop = &stmt->as.for_stmt.for_stmt;
      if (loop->is_each) {
        Type* iterable = analyze_expr(sema, loop->iterable, effects);
        Type* element = NULL;
        if (iterable && iterable->kind == TYPE_ARRAY) {
          element = iterable->as.array.element;
        }
        bind_pattern(sema, loop->pattern, element);
      } else {
        analyze_stmt(sema, loop->init, effects);
        if (loop->cond) {
          analyze_expr(sema, loop->cond, effects);
          effects->builtins &= ~(u32)EFF_DECIDES;
        }
        analyze_stmt(sema, loop->step, effects);
      }
      analyze_block(sema, stmt->as.for_stmt.body, effects);
      scope_pop(sema);
      return;
    }
    case STMT_BREAK:
    case STMT_CONTINUE:
      return;
    case STMT_RETURN:
      if (stmt->as.return_stmt.has_value) {
        analyze_expr(sema, stmt->as.return_stmt.value, effects);
      }
      return;
    case STMT_LABEL:
      analyze_stmt(sema, stmt->as.label_stmt.stmt, effects);
      return;
    case STMT_ITEM:
      sema_error(sema, TAKA_MSG_S_NESTED_ITEM_DECLARATIONS_ARE_NOT_SUPPORTED);
      return;
  }
}

static void analyze_function(Sema* sema, FnDecl* fn, Vector* impl_generics,
                             Type* self_type) {
  if (!fn || !fn->has_body) {
    return;
  }
  Scope* scope = scope_new(sema, sema->global);
  scope_push(sema, scope);
  if (self_type) {
    sema->current_self_type = self_type;
  }
  add_generics(sema, scope, impl_generics);
  add_generics(sema, scope, fn->sig.generics);
  if (fn->sig.params) {
    for (u64 i = 0; i < vec_len(fn->sig.params); ++i) {
      Param* param = vec_at_ptr(fn->sig.params, i);
      if (!param) {
        continue;
      }
      Symbol* symbol = symbol_new(sema, param->name, SYM_PARAM);
      symbol->is_mut = param->is_mut;
      symbol->as.param = param;
      symbol->type = param->type;
      scope_add(sema, scope, symbol);
    }
  }
  EffectSet* declared = effects_for_sig(sema, &fn->sig);
  EffectSet* body_effects = effect_new(sema);
  EffectSet* saved = sema->current_fn_effects;
  sema->current_fn_effects = declared;
  analyze_block(sema, fn->body, body_effects);
  effect_is_subset(sema, body_effects, declared, "function body");
  sema->current_fn_effects = saved;
  sema->current_self_type = NULL;
  scope_pop(sema);
}

static void analyze_impls(Sema* sema) {
  for (u64 i = 0; i < vec_len(sema->impls); ++i) {
    Item* item = vec_at_ptr(sema->impls, i);
    if (!item) {
      continue;
    }
    ImplDecl* impl = &item->as.impl_decl;
    Symbol* trait_symbol = sema_lookup_global(sema, path_first(impl->trait_path));
    if (!trait_symbol || trait_symbol->kind != SYM_TRAIT) {
      sema_error(sema, TAKA_MSG_S_UNKNOWN_TRAIT_S, str8_fmt(path_first(impl->trait_path)));
    }
    if (!impl->methods) {
      continue;
    }
    for (u64 j = 0; j < vec_len(impl->methods); ++j) {
      FnDecl* method = vec_at_ptr(impl->methods, j);
      if (!method) {
        continue;
      }
      analyze_function(sema, method, impl->generics, impl->for_type);
      FnDecl* trait_method = NULL;
      if (trait_symbol) {
        find_trait_method(sema, trait_symbol, method->sig.name, &trait_method);
      }
      if (!trait_method) {
        sema_error(sema, TAKA_MSG_S_TRAIT_S_HAS_NO_METHOD_S,
                   str8_fmt(path_first(impl->trait_path)), str8_fmt(method->sig.name));
      } else {
        EffectSet* impl_effects = effects_for_sig(sema, &method->sig);
        EffectSet* trait_effects = effects_for_sig(sema, &trait_method->sig);
        effect_is_subset(sema, impl_effects, trait_effects, "impl method");
      }
    }
  }
}

static void analyze_handlers(Sema* sema) {
  for (u64 i = 0; i < vec_len(sema->global->symbols); ++i) {
    Symbol* symbol = vec_at_ptr(sema->global->symbols, i);
    if (!symbol || symbol->kind != SYM_HANDLER) {
      continue;
    }
    HandlerDecl* handler = symbol->as.handler_decl;
    EffectSet* effects = effect_new(sema);
    analyze_handler_ops(sema, handler->ops, effects);
  }
}

static void analyze_globals(Sema* sema) {
  for (u64 i = 0; i < vec_len(sema->global->symbols); ++i) {
    Symbol* symbol = vec_at_ptr(sema->global->symbols, i);
    if (!symbol || symbol->kind != SYM_GLOBAL) {
      continue;
    }
    Stmt* var = symbol->as.var;
    EffectSet* body_effects = effect_new(sema);
    EffectSet* saved = sema->current_fn_effects;
    Scope* scope = scope_new(sema, sema->global);
    scope_push(sema, scope);
    sema->current_fn_effects = effect_new(sema);
    analyze_var_stmt(sema, var, body_effects);
    sema->current_fn_effects = saved;
    scope_pop(sema);
    effect_is_subset(sema, body_effects, NULL, "global initializer");
    symbol->type = var->as.var_expr.type;
  }
}

static void analyze_functions(Sema* sema) {
  for (u64 i = 0; i < vec_len(sema->global->symbols); ++i) {
    Symbol* symbol = vec_at_ptr(sema->global->symbols, i);
    if (!symbol) {
      continue;
    }
    if (symbol->kind == SYM_FN) {
      Item* item = symbol->item;
      map_set_ptr(sema->item_effects, (u64)(uintptr_t)item,
                  effects_for_sig(sema, &item->as.fn.sig));
      analyze_function(sema, &item->as.fn, NULL, NULL);
    } else if (symbol->kind == SYM_EXTERN_FN) {
      Item* item = symbol->item;
      map_set_ptr(sema->item_effects, (u64)(uintptr_t)item,
                  effects_for_sig(sema, &item->as.extern_decl.sig));
    }
  }
}

Sema* sema_create(MemArena* arena, Stmt* program) {
  Sema* sema = PUSH_STRUCT_ZERO(arena, Sema);
  sema->arena = arena;
  sema->program = program;
  sema->global = scope_new(sema, NULL);
  sema->current = sema->global;
  sema->expr_types = map_new(arena);
  sema->expr_symbols = map_new(arena);
  sema->pattern_symbols = map_new(arena);
  sema->capture_symbols = map_new(arena);
  sema->expr_effects = map_new(arena);
  sema->item_effects = map_new(arena);
  sema->sig_effects = map_new(arena);
  sema->impls = vec_new(arena, sizeof(Item*), 0);
  return sema;
}

void sema_free(Sema* sema) {
  if (!sema) {
    return;
  }
  map_free(sema->expr_types);
  map_free(sema->expr_symbols);
  map_free(sema->pattern_symbols);
  map_free(sema->capture_symbols);
  map_free(sema->expr_effects);
  map_free(sema->item_effects);
  map_free(sema->sig_effects);
}

bool sema_check(Sema* sema) {
  if (!sema || !sema->program || sema->program->kind != STMT_BLOCK) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(sema->program->as.block.body); ++i) {
    Stmt* stmt = vec_at_ptr(sema->program->as.block.body, i);
    if (!stmt) {
      continue;
    }
    if (stmt->kind == STMT_ITEM) {
      collect_item(sema, stmt->as.item.item);
    } else if (stmt->kind == STMT_VAR) {
      collect_global_declaration(sema, stmt);
    } else {
      sema_error(sema, TAKA_MSG_S_TOP_LEVEL_STATEMENTS_ARE_NOT_ALLOWED);
    }
  }
  resolve_declarations(sema);
  analyze_globals(sema);
  analyze_functions(sema);
  analyze_impls(sema);
  analyze_handlers(sema);
  return sema->errors == 0;
}
