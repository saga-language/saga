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
  if (!t || t->kind != TypeKind::Int)
    return t;
  auto &info = std::get<IntType>(t->detail);
  if (!info.is_untyped)
    return t;
  return make_int_type(info.bits, info.is_signed);
}

TypePtr make_float_type(uint8_t bits) {
  return std::make_shared<Type>(TypeKind::Float, FloatType{bits});
}

TypePtr make_string_type() {
  return std::make_shared<Type>(TypeKind::String, StringType{});
}

TypePtr make_error_type() {
  return std::make_shared<Type>(TypeKind::Error, ErrorType{});
}

TypePtr make_array_type(TypePtr element) {
  return std::make_shared<Type>(TypeKind::Array,
                                ArrayTypeInfo{std::move(element)});
}

TypePtr make_map_type(TypePtr key, TypePtr value) {
  return std::make_shared<Type>(
      TypeKind::Map, MapTypeInfo{std::move(key), std::move(value)});
}

TypePtr make_range_type(TypePtr element) {
  return std::make_shared<Type>(TypeKind::Range,
                                RangeTypeInfo{std::move(element)});
}

TypePtr make_func_type(std::vector<TypePtr> params,
                       std::vector<TypePtr> returns, bool is_variadic) {
  return std::make_shared<Type>(
      TypeKind::Func,
      FuncTypeInfo{std::move(params), std::move(returns), is_variadic});
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
                       std::string origin_package) {
  return std::make_shared<Type>(
      TypeKind::Enum,
      EnumTypeInfo{name, std::move(origin_package), std::move(variants)});
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

TypePtr make_union_type(std::vector<TypePtr> alternatives) {
  return std::make_shared<Type>(TypeKind::Union,
                                UnionTypeInfo{std::move(alternatives)});
}

TypePtr make_type_param(uint32_t id, const std::string &name,
                        std::optional<TypePtr> bound) {
  return std::make_shared<Type>(
      TypeKind::TypeParam,
      TypeParamInfo{TypeParam{id, name}, std::move(bound)});
}

TypePtr make_alias_type(const std::string &name, TypePtr underlying,
                        std::vector<MethodInfo> methods,
                        std::string origin_package) {
  return std::make_shared<Type>(
      TypeKind::Alias,
      AliasTypeInfo{name, std::move(origin_package), std::move(underlying),
                    std::move(methods)});
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

TypeKind underlying_kind(const TypePtr &t) {
  auto unwrapped = unwrap_alias(t);
  return unwrapped ? unwrapped->kind : TypeKind::Error;
}

bool is_error_type(const TypePtr &t) {
  return t && t->kind == TypeKind::Error;
}

bool is_numeric(const TypePtr &t) {
  auto u = unwrap_alias(t);
  return u && (u->kind == TypeKind::Int || u->kind == TypeKind::Float);
}

bool is_ordered(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u) return false;
  // Any is the top/bottom type used by intrinsics; treat as orderable.
  if (u->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(u->detail);
    if (info.name == "Any") return true;
  }
  return u->kind == TypeKind::Int || u->kind == TypeKind::Float ||
         u->kind == TypeKind::String;
}

bool is_equatable(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  // Any is the top/bottom type used by intrinsics; treat as equatable.
  if (u->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(u->detail);
    if (info.name == "Any") return true;
  }
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
  case TypeKind::Range:
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
    return "Void";
  case TypeKind::Bool:
    return "Bool";
  case TypeKind::Int: {
    auto &info = std::get<IntType>(t->detail);
    if (info.bits == 0)
      return "Int";
    if (!info.is_signed)
      return "Uint" + std::to_string(info.bits);
    return "Int" + std::to_string(info.bits);
  }
  case TypeKind::Float: {
    auto &info = std::get<FloatType>(t->detail);
    if (info.bits == 0)
      return "Float";
    return "Float" + std::to_string(info.bits);
  }
  case TypeKind::String:
    return "String";
  case TypeKind::Error:
    return "<error>";

  case TypeKind::Array: {
    auto &info = std::get<ArrayTypeInfo>(t->detail);
    return "[" + type_to_string(info.element) + "]";
  }

  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(t->detail);
    return "{" + type_to_string(info.key) + ": " +
           type_to_string(info.value) + "}";
  }

  case TypeKind::Range: {
    auto &info = std::get<RangeTypeInfo>(t->detail);
    return "(" + type_to_string(info.element) + ")";
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
    if (!info.returns.empty()) {
      os << " ";
      if (info.returns.size() == 1) {
        os << type_to_string(info.returns[0]);
      } else {
        os << "(";
        for (size_t i = 0; i < info.returns.size(); ++i) {
          if (i > 0)
            os << ", ";
          os << type_to_string(info.returns[i]);
        }
        os << ")";
      }
    }
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
  if (a->kind == TypeKind::Error)
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

  case TypeKind::Range: {
    auto &ai = std::get<RangeTypeInfo>(a->detail);
    auto &bi = std::get<RangeTypeInfo>(b->detail);
    return types_equal(ai.element, bi.element);
  }

  case TypeKind::Func: {
    auto &ai = std::get<FuncTypeInfo>(a->detail);
    auto &bi = std::get<FuncTypeInfo>(b->detail);
    if (ai.params.size() != bi.params.size())
      return false;
    if (ai.returns.size() != bi.returns.size())
      return false;
    if (ai.is_variadic != bi.is_variadic)
      return false;
    for (size_t i = 0; i < ai.params.size(); ++i) {
      if (!types_equal(ai.params[i], bi.params[i]))
        return false;
    }
    for (size_t i = 0; i < ai.returns.size(); ++i) {
      if (!types_equal(ai.returns[i], bi.returns[i]))
        return false;
    }
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

  case TypeKind::Error:
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
  if (is_error_type(source) || is_error_type(target))
    return true;

  // Any is a top type — any value is assignable to Any, and Any is assignable
  // to any type.  Used by intrinsic_runtime / intrinsic_field signatures.
  auto is_any = [](const TypePtr &t) {
    if (t->kind != TypeKind::Struct) return false;
    return std::get<StructTypeInfo>(t->detail).name == "Any";
  };
  if (is_any(source) || is_any(target))
    return true;

  // Exact match.
  if (types_equal(source, target))
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

  // Source assignable to any alternative in a union target.
  if (target->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(target->detail);
    for (auto &alt : info.alternatives) {
      if (is_assignable_to(source, alt))
        return true;
    }
  }

  // Union source: every alternative must be assignable to target.
  if (source->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(source->detail);
    for (auto &alt : info.alternatives) {
      if (!is_assignable_to(alt, target))
        return false;
    }
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
  if (is_error_type(a))
    return b;
  if (is_error_type(b))
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

  case TypeKind::Range: {
    auto &info = std::get<RangeTypeInfo>(t->detail);
    auto elem = substitute(info.element, bindings);
    if (elem == info.element)
      return t;
    return make_range_type(std::move(elem));
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
    std::vector<TypePtr> rets;
    rets.reserve(info.returns.size());
    for (auto &r : info.returns) {
      auto sr = substitute(r, bindings);
      if (sr != r)
        changed = true;
      rets.push_back(std::move(sr));
    }
    if (!changed)
      return t;
    return make_func_type(std::move(params), std::move(rets), info.is_variadic);
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
                           info.origin_package);
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
  case TypeKind::Range:
    return has_type_params(std::get<RangeTypeInfo>(t->detail).element);
  case TypeKind::Func: {
    auto &f = std::get<FuncTypeInfo>(t->detail);
    for (auto &p : f.params)
      if (has_type_params(p))
        return true;
    for (auto &r : f.returns)
      if (has_type_params(r))
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

  case TypeKind::Range: {
    auto &pi = std::get<RangeTypeInfo>(param_type->detail);
    auto &ai = std::get<RangeTypeInfo>(arg_type->detail);
    return unify(pi.element, ai.element, out);
  }

  case TypeKind::Func: {
    auto &pi = std::get<FuncTypeInfo>(param_type->detail);
    auto &ai = std::get<FuncTypeInfo>(arg_type->detail);
    if (pi.params.size() != ai.params.size())
      return false;
    if (pi.returns.size() != ai.returns.size())
      return false;
    for (size_t i = 0; i < pi.params.size(); ++i) {
      if (!unify(pi.params[i], ai.params[i], out))
        return false;
    }
    for (size_t i = 0; i < pi.returns.size(); ++i) {
      if (!unify(pi.returns[i], ai.returns[i], out))
        return false;
    }
    return true;
  }

  default:
    // For primitives / nominals, just check equality.
    return types_equal(param_type, arg_type);
  }
}

} // namespace saga
