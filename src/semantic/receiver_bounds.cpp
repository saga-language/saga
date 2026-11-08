// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A method on a generic struct, array or map may bound its receiver's
// parameters (`fn (b Box<T numeric>) Twice() T`). It is then a method of only
// the instances whose type arguments meet those bounds: elsewhere it is
// neither callable nor counted toward an interface.

#include "semantic/analyzer.hpp"

#include <format>

namespace saga {

namespace {

const TypeParam *param_of(const TypePtr &t) {
  if (!t || t->kind != TypeKind::TypeParam)
    return nullptr;
  return &std::get<TypeParamInfo>(t->detail).param;
}

} // namespace

std::optional<std::string>
Analyzer::unmet_receiver_bound(const TypePtr &instance,
                               std::string_view name) {
  auto [methods, args] = instance_methods(instance);
  if (!methods)
    return std::nullopt;
  for (auto &m : *methods)
    if (m.name == name)
      return unmet_receiver_params(args, m.receiver_params);
  return std::nullopt;
}

// The methods every instance of the type declares, and the arguments this
// instance binds their receiver's parameters to: a struct's type arguments,
// an array's element, a map's key and value.
std::pair<const std::vector<MethodInfo> *, std::vector<TypePtr>>
Analyzer::instance_methods(const TypePtr &instance) {
  if (instance->kind == TypeKind::Struct)
    return {declared_methods(instance),
            std::get<StructTypeInfo>(instance->detail).type_args};
  auto t = unwrap_alias(instance);
  auto it = t ? kind_methods_.find(t->kind) : kind_methods_.end();
  if (it == kind_methods_.end())
    return {nullptr, {}};
  if (t->kind == TypeKind::Array)
    return {&it->second, {std::get<ArrayTypeInfo>(t->detail).element}};
  auto &map = std::get<MapTypeInfo>(t->detail);
  return {&it->second, {map.key, map.value}};
}

std::optional<std::string>
Analyzer::unmet_receiver_params(const std::vector<TypePtr> &args,
                                const std::vector<TypePtr> &params) {
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    auto *param = param_of(params[i]);
    if (!param)
      continue;
    if (auto unmet = unmet_bound(args[i], *param))
      return std::format("it needs {}, and {} is {}", *unmet, param->name,
                         type_to_string(args[i]));
  }
  return std::nullopt;
}

} // namespace saga
