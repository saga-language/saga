// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/types.hpp"
#include "frontend/ast.hpp"

#include <algorithm>
#include <sstream>

namespace saga {

// ===========================================================================
// Factory helpers
// ===========================================================================

TypePtr make_void_type() {
  return std::make_shared<Type>(TypeKind::Void, VoidType{});
}

TypePtr make_bool_type() {
  return std::make_shared<Type>(TypeKind::Bool, BoolType{});
}

TypePtr make_int_type(uint8_t bits, bool is_signed, bool is_untyped) {
  return std::make_shared<Type>(TypeKind::Int,
                                IntType{bits, is_signed, is_untyped});
}

TypePtr make_untyped_int_type() {
  return std::make_shared<Type>(TypeKind::Int,
                                IntType{0, true, /*is_untyped=*/true});
}

TypePtr materialize_untyped(const TypePtr &t) {
  if (!t) return t;
  if (t->kind == TypeKind::Int) {
    auto &info = std::get<IntType>(t->detail);
    if (!info.is_untyped) return t;
    return make_int_type(info.bits, info.is_signed);
  }
  if (t->kind == TypeKind::Float) {
    auto &info = std::get<FloatType>(t->detail);
    if (!info.is_untyped) return t;
    return make_float_type(info.bits);
  }
  return t;
}

TypePtr make_float_type(uint8_t bits) {
  return std::make_shared<Type>(TypeKind::Float, FloatType{bits, false});
}

TypePtr make_untyped_float_type() {
  return std::make_shared<Type>(TypeKind::Float, FloatType{0, true});
}

TypePtr make_string_type() {
  return std::make_shared<Type>(TypeKind::String, StringType{});
}

TypePtr make_invalid_type() {
  return std::make_shared<Type>(TypeKind::Invalid, InvalidType{});
}

TypePtr make_array_type(TypePtr element) {
  return std::make_shared<Type>(TypeKind::Array,
                                ArrayTypeInfo{std::move(element)});
}

TypePtr make_map_type(TypePtr key, TypePtr value) {
  return std::make_shared<Type>(
      TypeKind::Map, MapTypeInfo{std::move(key), std::move(value)});
}

TypePtr make_func_type(std::vector<TypePtr> params,
                       TypePtr return_type, bool is_variadic) {
  return std::make_shared<Type>(
      TypeKind::Func,
      FuncTypeInfo{std::move(params), std::move(return_type), is_variadic});
}

TypePtr make_struct_type(const std::string &name,
                         std::vector<FieldInfo> fields,
                         std::vector<MethodInfo> methods,
                         std::vector<TypeParam> type_params,
                         std::string origin_package) {
  return std::make_shared<Type>(
      TypeKind::Struct,
      StructTypeInfo{name, std::move(origin_package), std::move(fields),
                     std::move(methods), {}, std::move(type_params), {}});
}

TypePtr make_enum_type(const std::string &name,
                       std::vector<EnumVariant> variants,
                       std::string origin_package, bool string_backed) {
  return std::make_shared<Type>(
      TypeKind::Enum, EnumTypeInfo{name, std::move(origin_package),
                                   std::move(variants), string_backed});
}

TypePtr make_interface_type(const std::string &name,
                            std::vector<MethodInfo> methods,
                            std::vector<TypeParam> type_params,
                            std::string origin_package) {
  return std::make_shared<Type>(
      TypeKind::Interface,
      InterfaceTypeInfo{name, std::move(origin_package), std::move(methods),
                        std::move(type_params), {}});
}

static void flatten_union_into(const TypePtr &alt, std::vector<TypePtr> &out) {
  // A nominal alias is a distinct type and stays a member; only a structural
  // alias to a union has its members spliced.
  auto s = unwrap_structural_alias(alt);
  if (s && s->kind == TypeKind::Union) {
    for (auto &m : std::get<UnionTypeInfo>(s->detail).alternatives)
      flatten_union_into(m, out);
  } else {
    out.push_back(alt);
  }
}

std::vector<TypePtr>
flatten_union_alternatives(const std::vector<TypePtr> &alts) {
  std::vector<TypePtr> flat;
  for (auto &a : alts)
    flatten_union_into(a, flat);
  return flat;
}

TypePtr make_union_type(std::vector<TypePtr> alternatives) {
  // Canonicalize every union: splice nested unions and drop duplicates,
  // preserving first-seen order (the leftmost type is the zero value). User
  // duplicates are diagnosed in resolve_union_type; here dedupe is silent so
  // internal composition (e.g. `(T|error) | error` after substitution) is clean.
  std::vector<TypePtr> uniq;
  for (auto &a : flatten_union_alternatives(alternatives)) {
    bool dup = false;
    for (auto &e : uniq)
      if (types_equal(e, a)) { dup = true; break; }
    if (!dup)
      uniq.push_back(a);
  }
  return std::make_shared<Type>(TypeKind::Union,
                                UnionTypeInfo{std::move(uniq)});
}

TypePtr make_type_param(uint32_t id, const std::string &name,
                        std::optional<TypePtr> bound) {
  return std::make_shared<Type>(
      TypeKind::TypeParam,
      TypeParamInfo{TypeParam{id, name, TypeConstraint::None},
                    std::move(bound)});
}

TypePtr make_alias_type(const std::string &name, TypePtr underlying,
                        std::vector<MethodInfo> methods,
                        std::string origin_package, bool structural) {
  return std::make_shared<Type>(
      TypeKind::Alias,
      AliasTypeInfo{name, std::move(origin_package), std::move(underlying),
                    std::move(methods), structural});
}

TypePtr make_module_type(const std::string &name,
                         const std::string &import_path,
                         std::vector<ModuleExport> exports) {
  return std::make_shared<Type>(
      TypeKind::Module,
      ModuleTypeInfo{name, import_path, std::move(exports)});
}

// ===========================================================================
// Type queries
// ===========================================================================

TypePtr unwrap_alias(const TypePtr &t) {
  if (!t) return t;
  auto curr = t;
  while (curr && curr->kind == TypeKind::Alias) {
    curr = std::get<AliasTypeInfo>(curr->detail).underlying;
  }
  return curr;
}

// Unwrap only transparent (structural) aliases, which carry no methods and no
// distinct identity. A nominal alias (`type X T`) is a real type and stays.
TypePtr unwrap_structural_alias(const TypePtr &t) {
  auto curr = t;
  while (curr && curr->kind == TypeKind::Alias &&
         std::get<AliasTypeInfo>(curr->detail).structural) {
    curr = std::get<AliasTypeInfo>(curr->detail).underlying;
  }
  return curr;
}

TypeKind underlying_kind(const TypePtr &t) {
  auto unwrapped = unwrap_alias(t);
  return unwrapped ? unwrapped->kind : TypeKind::Invalid;
}

bool is_invalid_type(const TypePtr &t) {
  return t && t->kind == TypeKind::Invalid;
}

bool is_error_valued(const TypePtr &t) {
  auto u = unwrap_alias(t);
  return u && u->kind == TypeKind::Struct &&
         std::get<StructTypeInfo>(u->detail).is_error;
}

bool is_abstract_error(const TypePtr &t) {
  auto u = unwrap_alias(t);
  return u && u->kind == TypeKind::Struct &&
         std::get<StructTypeInfo>(u->detail).is_error &&
         std::get<StructTypeInfo>(u->detail).name == "error";
}

bool is_numeric(const TypePtr &t) {
  auto u = unwrap_alias(t);
  return u && (u->kind == TypeKind::Int || u->kind == TypeKind::Float);
}

std::string_view constraint_name(TypeConstraint c) {
  switch (c) {
  case TypeConstraint::Integer: return "integer";
  case TypeConstraint::Float:   return "float";
  case TypeConstraint::Numeric: return "numeric";
  case TypeConstraint::None:    return "";
  }
  return "";
}

TypeConstraint constraint_from_name(std::string_view name) {
  if (name == "integer") return TypeConstraint::Integer;
  if (name == "float")   return TypeConstraint::Float;
  if (name == "numeric") return TypeConstraint::Numeric;
  return TypeConstraint::None;
}

bool satisfies_constraint(const TypePtr &t, TypeConstraint c) {
  if (c == TypeConstraint::None) return false;
  auto u = unwrap_alias(t);
  if (!u) return false;
  switch (c) {
  case TypeConstraint::Integer: return u->kind == TypeKind::Int;
  case TypeConstraint::Float:   return u->kind == TypeKind::Float;
  case TypeConstraint::Numeric:
    return u->kind == TypeKind::Int || u->kind == TypeKind::Float;
  case TypeConstraint::None:    return false;
  }
  return false;
}

bool is_ordered(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u) return false;
  return u->kind == TypeKind::Int || u->kind == TypeKind::Float ||
         u->kind == TypeKind::String;
}

bool is_equatable(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  switch (u->kind) {
  case TypeKind::Bool:
  case TypeKind::Int:
  case TypeKind::Float:
  case TypeKind::String:
  case TypeKind::Enum:
    return true;
  default:
    return false;
  }
}

bool is_callable(const TypePtr &t) {
  auto u = unwrap_alias(t);
  return u && u->kind == TypeKind::Func;
}

bool is_iterable(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  switch (u->kind) {
  case TypeKind::Array:
  case TypeKind::Map:
  case TypeKind::String:
    return true;
  default:
    return false;
  }
}

// ===========================================================================
// type_to_string
// ===========================================================================

std::string type_to_string(const TypePtr &t) {
  if (!t)
    return "<null>";

  switch (t->kind) {
  case TypeKind::Void:
    return "void";
  case TypeKind::Bool:
    return "bool";
  case TypeKind::Int: {
    auto &info = std::get<IntType>(t->detail);
    if (info.bits == 0)
      return "int";
    if (!info.is_signed)
      return "uint" + std::to_string(info.bits);
    return "int" + std::to_string(info.bits);
  }
  case TypeKind::Float: {
    auto &info = std::get<FloatType>(t->detail);
    if (info.bits == 0)
      return "float";
    return "float" + std::to_string(info.bits);
  }
  case TypeKind::String:
    return "string";
  case TypeKind::Invalid:
    return "<error>";

  case TypeKind::Array: {
    auto &info = std::get<ArrayTypeInfo>(t->detail);
    return "array{" + type_to_string(info.element) + "}";
  }

  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(t->detail);
    return "map{" + type_to_string(info.key) + ": " +
           type_to_string(info.value) + "}";
  }

  case TypeKind::Func: {
    auto &info = std::get<FuncTypeInfo>(t->detail);
    std::ostringstream os;
    os << "fn(";
    for (size_t i = 0; i < info.params.size(); ++i) {
      if (i > 0)
        os << ", ";
      if (info.is_variadic && i == info.params.size() - 1)
        os << "...";
      os << type_to_string(info.params[i]);
    }
    os << ")";
    if (info.return_type)
      os << " " << type_to_string(info.return_type);
    return os.str();
  }

  case TypeKind::Struct: {
    auto &info = std::get<StructTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Enum: {
    auto &info = std::get<EnumTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Interface: {
    auto &info = std::get<InterfaceTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Union: {
    auto &info = std::get<UnionTypeInfo>(t->detail);
    std::ostringstream os;
    for (size_t i = 0; i < info.alternatives.size(); ++i) {
      if (i > 0)
        os << " | ";
      os << type_to_string(info.alternatives[i]);
    }
    return os.str();
  }

  case TypeKind::TypeParam: {
    auto &info = std::get<TypeParamInfo>(t->detail);
    return info.param.name;
  }

  case TypeKind::Alias: {
    auto &info = std::get<AliasTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Module: {
    auto &info = std::get<ModuleTypeInfo>(t->detail);
    return "module '" + info.name + "'";
  }
  }

  return "<unknown>";
}

// ===========================================================================
// Type equality (structural)
// ===========================================================================

bool types_equal(const TypePtr &a, const TypePtr &b) {
  if (a == b)
    return true; // pointer identity — fast path
  if (!a || !b)
    return false;
  if (a->kind != b->kind)
    return false;

  // Error types propagate silently — treat as equal to anything.
  if (a->kind == TypeKind::Invalid)
    return true;

  switch (a->kind) {
  case TypeKind::Void:
  case TypeKind::Bool:
  case TypeKind::String:
    return true; // singletons

  case TypeKind::Int: {
    auto &ai = std::get<IntType>(a->detail);
    auto &bi = std::get<IntType>(b->detail);
    return ai.bits == bi.bits && ai.is_signed == bi.is_signed;
  }

  case TypeKind::Float: {
    auto &ai = std::get<FloatType>(a->detail);
    auto &bi = std::get<FloatType>(b->detail);
    return ai.bits == bi.bits;
  }

  case TypeKind::Array: {
    auto &ai = std::get<ArrayTypeInfo>(a->detail);
    auto &bi = std::get<ArrayTypeInfo>(b->detail);
    return types_equal(ai.element, bi.element);
  }

  case TypeKind::Map: {
    auto &ai = std::get<MapTypeInfo>(a->detail);
    auto &bi = std::get<MapTypeInfo>(b->detail);
    return types_equal(ai.key, bi.key) && types_equal(ai.value, bi.value);
  }

  case TypeKind::Func: {
    auto &ai = std::get<FuncTypeInfo>(a->detail);
    auto &bi = std::get<FuncTypeInfo>(b->detail);
    if (ai.params.size() != bi.params.size())
      return false;
    if (static_cast<bool>(ai.return_type) != static_cast<bool>(bi.return_type))
      return false;
    if (ai.is_variadic != bi.is_variadic)
      return false;
    for (size_t i = 0; i < ai.params.size(); ++i) {
      if (!types_equal(ai.params[i], bi.params[i]))
        return false;
    }
    if (ai.return_type && !types_equal(ai.return_type, bi.return_type))
      return false;
    return true;
  }

  case TypeKind::Struct: {
    // Nominal — compare by (origin_package, name).
    auto &ai = std::get<StructTypeInfo>(a->detail);
    auto &bi = std::get<StructTypeInfo>(b->detail);
    return ai.origin_package == bi.origin_package && ai.name == bi.name;
  }

  case TypeKind::Enum: {
    auto &ai = std::get<EnumTypeInfo>(a->detail);
    auto &bi = std::get<EnumTypeInfo>(b->detail);
    return ai.origin_package == bi.origin_package && ai.name == bi.name;
  }

  case TypeKind::Interface: {
    auto &ai = std::get<InterfaceTypeInfo>(a->detail);
    auto &bi = std::get<InterfaceTypeInfo>(b->detail);
    return ai.origin_package == bi.origin_package && ai.name == bi.name;
  }

  case TypeKind::Union: {
    auto &ai = std::get<UnionTypeInfo>(a->detail);
    auto &bi = std::get<UnionTypeInfo>(b->detail);
    if (ai.alternatives.size() != bi.alternatives.size())
      return false;
    // Order-independent: every type in a must appear in b and vice versa.
    for (auto &at : ai.alternatives) {
      bool found = false;
      for (auto &bt : bi.alternatives) {
        if (types_equal(at, bt)) {
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }
    return true;
  }

  case TypeKind::TypeParam: {
    auto &ai = std::get<TypeParamInfo>(a->detail);
    auto &bi = std::get<TypeParamInfo>(b->detail);
    return ai.param.id == bi.param.id;
  }

  case TypeKind::Alias: {
    // Nominal — alias types are unique by (origin_package, name).
    auto &ai = std::get<AliasTypeInfo>(a->detail);
    auto &bi = std::get<AliasTypeInfo>(b->detail);
    return ai.origin_package == bi.origin_package && ai.name == bi.name;
  }

  case TypeKind::Module: {
    auto &ai = std::get<ModuleTypeInfo>(a->detail);
    auto &bi = std::get<ModuleTypeInfo>(b->detail);
    return ai.import_path == bi.import_path;
  }

  case TypeKind::Invalid:
    return true;
  }

  return false;
}

// ===========================================================================
// is_assignable_to
// ===========================================================================

bool is_assignable_to(const TypePtr &source, const TypePtr &target) {
  if (!source || !target)
    return false;

  // Error types propagate silently.
  if (is_invalid_type(source) || is_invalid_type(target))
    return true;

  // Alias assignability.  A structural alias (`type X = T`) is transparent:
  // it is the same type as its underlying, assignable in both directions.
  // A nominal alias (`type X T`) is a distinct type — a typed value of the
  // underlying is NOT implicitly assignable to it or vice versa; the user
  // converts at the boundary.  One carve-out: an untyped literal (e.g. `5`)
  // flows into a nominal slot because it hasn't committed to a type yet.
  auto is_structural_alias = [](const TypePtr &t) {
    return t && t->kind == TypeKind::Alias &&
           std::get<AliasTypeInfo>(t->detail).structural;
  };
  if (is_structural_alias(target))
    return is_assignable_to(source, unwrap_alias(target));
  if (is_structural_alias(source))
    return is_assignable_to(unwrap_alias(source), target);
  bool source_is_untyped =
      source && source->kind == TypeKind::Int &&
      std::get<IntType>(source->detail).is_untyped;
  if (target && target->kind == TypeKind::Alias) {
    if (source_is_untyped)
      return is_assignable_to(source, unwrap_alias(target));
    if (source && source->kind == TypeKind::Alias)
      return types_equal(source, target);
    return false;
  }
  if (source && source->kind == TypeKind::Alias)
    return false;

  // Exact match.
  if (types_equal(source, target))
    return true;

  // Any error value widens to the abstract base `error`.
  if (is_error_valued(source) && is_abstract_error(target))
    return true;

  // Int → Float promotion.
  if (source->kind == TypeKind::Int && target->kind == TypeKind::Float)
    return true;

  // Untyped integer literal — assignable to any integer width.
  // (Float targets are already handled by the Int→Float promotion above.)
  if (source->kind == TypeKind::Int &&
      std::get<IntType>(source->detail).is_untyped &&
      target->kind == TypeKind::Int)
    return true;

  // Untyped float literal — assignable to any float width.  Mirrors the
  // untyped-int rule so `f32 Float32 = 5.0` compiles.
  if (source->kind == TypeKind::Float && target->kind == TypeKind::Float &&
      std::get<FloatType>(source->detail).is_untyped)
    return true;

  // Platform-default ↔ explicit 64-bit aliases.  Saga's `Float` (bits=0)
  // and `Float64` (bits=64) lower to the same LLVM type on the supported
  // 64-bit targets, so a `Float` value flows into a `Float64` slot (and
  // vice versa).  Same for `Int` ↔ `Int64`.  Distinct widths (Float32,
  // Int32, etc.) remain unrelated.
  if (source->kind == TypeKind::Float && target->kind == TypeKind::Float) {
    auto &s = std::get<FloatType>(source->detail);
    auto &t = std::get<FloatType>(target->detail);
    bool s_word = s.bits == 0;
    bool t_word = t.bits == 0;
    if ((s_word && t.bits == 64) || (t_word && s.bits == 64))
      return true;
  }
  if (source->kind == TypeKind::Int && target->kind == TypeKind::Int) {
    auto &s = std::get<IntType>(source->detail);
    auto &t = std::get<IntType>(target->detail);
    bool s_word_signed = s.bits == 0 && s.is_signed;
    bool t_word_signed = t.bits == 0 && t.is_signed;
    if ((s_word_signed && t.bits == 64 && t.is_signed) ||
        (t_word_signed && s.bits == 64 && s.is_signed))
      return true;
  }

  // Source assignable to any alternative in a union target.
  if (target->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(target->detail);
    for (auto &alt : info.alternatives) {
      if (is_assignable_to(source, alt))
        return true;
    }
  }

  // Union source is disjunctive: a `A | B` value is assignable to the target
  // only if every alternative independently is. (Unions are concrete-only, so
  // there is no interface-widening case.)
  if (source->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(source->detail);
    for (auto &alt : info.alternatives)
      if (!is_assignable_to(alt, target))
        return false;
    return true;
  }

  // Interface satisfaction: concrete type can be assigned to an interface
  // if it implements all methods required by the interface.
  if (target->kind == TypeKind::Interface) {
    // Check whether the source type has all methods required by the
    // interface.  We do a structural check here — the full
    // satisfies_interface logic lives in the Analyzer, but for
    // is_assignable_to we replicate the method-matching check so it
    // works without an Analyzer instance.
    auto &iface_info = std::get<InterfaceTypeInfo>(target->detail);

    std::vector<MethodInfo> concrete_methods;
    if (source->kind == TypeKind::Struct) {
      concrete_methods = std::get<StructTypeInfo>(source->detail).methods;
    }
    // For non-struct types, builtin_methods would be needed but we don't
    // have access to builtins here.  Return false for now; the Analyzer's
    // satisfies_interface handles the full check.
    if (concrete_methods.empty() && source->kind != TypeKind::Struct)
      return false;

    bool satisfied = true;
    for (auto &im : iface_info.methods) {
      bool found = false;
      for (auto &cm : concrete_methods) {
        if (cm.name == im.name) {
          found = true;
          break;
        }
      }
      if (!found) {
        satisfied = false;
        break;
      }
    }
    if (satisfied)
      return true;
  }

  return false;
}

// ===========================================================================
// common_type
// ===========================================================================

TypePtr common_type(const TypePtr &a, const TypePtr &b) {
  if (!a || !b)
    return nullptr;
  if (is_invalid_type(a))
    return b;
  if (is_invalid_type(b))
    return a;
  if (types_equal(a, b))
    return a;

  // Untyped integer literal yields to any concrete integer width on the
  // other side, so `x Int32 + 5` keeps the Int32 typing.
  auto is_untyped_int = [](const TypePtr &t) {
    return t->kind == TypeKind::Int &&
           std::get<IntType>(t->detail).is_untyped;
  };
  if (is_untyped_int(a) && b->kind == TypeKind::Int)
    return b;
  if (is_untyped_int(b) && a->kind == TypeKind::Int)
    return a;

  // Int + Float → Float.
  if (a->kind == TypeKind::Int && b->kind == TypeKind::Float)
    return b;
  if (a->kind == TypeKind::Float && b->kind == TypeKind::Int)
    return a;

  // Fall back to a union.
  return make_union_type({a, b});
}

// ===========================================================================
// Generics — substitution
// ===========================================================================

TypePtr substitute(const TypePtr &t,
                   const std::unordered_map<uint32_t, TypePtr> &bindings) {
  if (!t || bindings.empty())
    return t;

  switch (t->kind) {
  case TypeKind::TypeParam: {
    auto &info = std::get<TypeParamInfo>(t->detail);
    auto it = bindings.find(info.param.id);
    if (it != bindings.end())
      return it->second;
    return t;
  }

  case TypeKind::Array: {
    auto &info = std::get<ArrayTypeInfo>(t->detail);
    auto elem = substitute(info.element, bindings);
    if (elem == info.element)
      return t;
    return make_array_type(std::move(elem));
  }

  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(t->detail);
    auto k = substitute(info.key, bindings);
    auto v = substitute(info.value, bindings);
    if (k == info.key && v == info.value)
      return t;
    return make_map_type(std::move(k), std::move(v));
  }

  case TypeKind::Func: {
    auto &info = std::get<FuncTypeInfo>(t->detail);
    bool changed = false;
    std::vector<TypePtr> params;
    params.reserve(info.params.size());
    for (auto &p : info.params) {
      auto sp = substitute(p, bindings);
      if (sp != p)
        changed = true;
      params.push_back(std::move(sp));
    }
    TypePtr ret;
    if (info.return_type) {
      ret = substitute(info.return_type, bindings);
      if (ret != info.return_type)
        changed = true;
    }
    if (!changed)
      return t;
    return make_func_type(std::move(params), std::move(ret), info.is_variadic);
  }

  case TypeKind::Union: {
    auto &info = std::get<UnionTypeInfo>(t->detail);
    bool changed = false;
    std::vector<TypePtr> alts;
    alts.reserve(info.alternatives.size());
    for (auto &a : info.alternatives) {
      auto sa = substitute(a, bindings);
      if (sa != a)
        changed = true;
      alts.push_back(std::move(sa));
    }
    if (!changed)
      return t;
    return make_union_type(std::move(alts));
  }

  case TypeKind::Alias: {
    auto &info = std::get<AliasTypeInfo>(t->detail);
    auto u = substitute(info.underlying, bindings);
    if (u == info.underlying)
      return t;
    return make_alias_type(info.name, std::move(u), info.methods,
                           info.origin_package, info.structural);
  }

  default:
    return t; // primitive / nominal — no type params inside
  }
}

// ===========================================================================
// Generics — unification
// ===========================================================================

bool has_type_params(const TypePtr &t) {
  if (!t)
    return false;
  if (t->kind == TypeKind::TypeParam)
    return true;

  switch (t->kind) {
  case TypeKind::Array:
    return has_type_params(std::get<ArrayTypeInfo>(t->detail).element);
  case TypeKind::Map: {
    auto &m = std::get<MapTypeInfo>(t->detail);
    return has_type_params(m.key) || has_type_params(m.value);
  }
  case TypeKind::Func: {
    auto &f = std::get<FuncTypeInfo>(t->detail);
    for (auto &p : f.params)
      if (has_type_params(p))
        return true;
    if (f.return_type && has_type_params(f.return_type))
      return true;
    return false;
  }
  case TypeKind::Alias:
    return has_type_params(std::get<AliasTypeInfo>(t->detail).underlying);
  case TypeKind::Union: {
    auto &u = std::get<UnionTypeInfo>(t->detail);
    for (auto &a : u.alternatives)
      if (has_type_params(a))
        return true;
    return false;
  }
  default:
    return false;
  }
}

bool unify(const TypePtr &param_type, const TypePtr &arg_type,
           std::unordered_map<uint32_t, TypePtr> &out) {
  if (!param_type || !arg_type)
    return false;

  // If the param side is a type variable, bind or check consistency.
  if (param_type->kind == TypeKind::TypeParam) {
    auto &info = std::get<TypeParamInfo>(param_type->detail);
    auto it = out.find(info.param.id);
    if (it == out.end()) {
      out[info.param.id] = arg_type;
      return true;
    }
    return types_equal(it->second, arg_type);
  }

  // Both must be the same kind to recurse.
  if (param_type->kind != arg_type->kind)
    return false;

  switch (param_type->kind) {
  case TypeKind::Array: {
    auto &pi = std::get<ArrayTypeInfo>(param_type->detail);
    auto &ai = std::get<ArrayTypeInfo>(arg_type->detail);
    return unify(pi.element, ai.element, out);
  }

  case TypeKind::Map: {
    auto &pi = std::get<MapTypeInfo>(param_type->detail);
    auto &ai = std::get<MapTypeInfo>(arg_type->detail);
    return unify(pi.key, ai.key, out) && unify(pi.value, ai.value, out);
  }

  case TypeKind::Func: {
    auto &pi = std::get<FuncTypeInfo>(param_type->detail);
    auto &ai = std::get<FuncTypeInfo>(arg_type->detail);
    if (pi.params.size() != ai.params.size())
      return false;
    if (static_cast<bool>(pi.return_type) != static_cast<bool>(ai.return_type))
      return false;
    for (size_t i = 0; i < pi.params.size(); ++i) {
      if (!unify(pi.params[i], ai.params[i], out))
        return false;
    }
    if (pi.return_type && !unify(pi.return_type, ai.return_type, out))
      return false;
    return true;
  }

  default:
    // For primitives / nominals, just check equality.
    return types_equal(param_type, arg_type);
  }
}

} // namespace saga
