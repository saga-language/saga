// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Monomorphisation: binding a generic declaration's type parameters to
// concrete arguments and producing the specialised type. Substitution is
// memoised because a recursive generic would otherwise expand forever.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr
Analyzer::instantiate_generic_call(
    const TypePtr &callee_type, const std::vector<TypePtr> &arg_types,
    Span call_span, std::unordered_map<uint32_t, TypePtr> *out_bindings) {
  if (!callee_type || callee_type->kind != TypeKind::Func)
    return builtins.invalid_type;

  auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
  std::unordered_map<uint32_t, TypePtr> bindings;

  // Attempt to unify each parameter type with the argument type.
  size_t count = std::min(fn_info.params.size(), arg_types.size());
  for (size_t i = 0; i < count; ++i) {
    if (!unify(fn_info.params[i], arg_types[i], bindings)) {
      error(call_span,
            std::format("cannot infer type parameter from argument {}", i + 1));
      return builtins.invalid_type;
    }
  }

  if (reject_void_bindings(bindings, call_span))
    return builtins.invalid_type;

  if (!bindings_meet_bounds(callee_type, bindings, call_span))
    return poison(call_span, "constraint violation left unreported");

  if (out_bindings)
    *out_bindings = bindings;

  if (bindings.empty())
    return callee_type; // No type params to substitute.

  return substitute(callee_type, bindings);
}

// A generic's parameters, bounds and all, are the ones its signature names.
bool Analyzer::bindings_meet_bounds(
    const TypePtr &callee_type,
    const std::unordered_map<uint32_t, TypePtr> &bindings, Span span) {
  std::unordered_map<uint32_t, TypeParam> params;
  any_type_param(callee_type, [&](const TypeParam &p) {
    params.emplace(p.id, p);
    return false;
  });
  bool met = true;
  for (auto &[id, concrete] : bindings) {
    auto it = params.find(id);
    if (it != params.end() && concrete && !is_invalid_type(concrete))
      met = report_unmet_bound(concrete, it->second, span) && met;
  }
  return met;
}

TypePtr Analyzer::instantiate_generic_struct(
    const TypePtr &struct_type,
    const std::vector<std::pair<std::string, TypePtr>> &field_types,
    Span span) {
  if (!struct_type || struct_type->kind != TypeKind::Struct)
    return poison(span, "generic instantiation of a non-struct type");

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  if (info.type_params.empty())
    return struct_type; // Not generic.

  std::unordered_map<uint32_t, TypePtr> bindings;

  // Unify each provided field value type against the struct's field type. A
  // field may bind nothing — `tail Node<T> | Missing` given `Missing{}` says
  // nothing about T — which is silent here; an unbound parameter is caught
  // below by the field that was supposed to bind it.
  for (auto &[fname, ftype] : field_types) {
    for (auto &fi : info.fields) {
      if (fi.name == fname && fi.type) {
        unify(fi.type, ftype, bindings);
        break;
      }
    }
  }

  if (reject_void_bindings(bindings, span))
    return builtins.invalid_type;

  if (bindings.empty())
    return struct_type;

  std::vector<TypePtr> type_args;
  bool met = true;
  for (auto &tp : info.type_params) {
    auto it = bindings.find(tp.id);
    if (it == bindings.end())
      continue;
    type_args.push_back(it->second);
    met = report_unmet_bound(it->second, tp, span) && met;
  }
  if (!met)
    return builtins.invalid_type;

  // Publish the shell before substituting fields, and map the declaration onto
  // it, so a field naming the struct again lands on this instantiation.
  auto result = make_struct_instance(struct_type, std::move(type_args));
  auto &result_info = std::get<StructTypeInfo>(result->detail);
  SubstMemo memo{{struct_type.get(), result}};

  for (auto &f : info.fields)
    result_info.fields.push_back(
        {f.name, substitute(f.type, bindings, memo), f.is_public,
         f.default_value});
  for (auto &m : info.methods)
    result_info.methods.push_back(
        {m.name, substitute(m.signature, bindings, memo), m.is_public,
         m.origin_package});
  result_info.embeds = info.embeds;

  return result;
}

} // namespace saga
