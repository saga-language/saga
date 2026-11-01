// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A method on a generic struct may bound its receiver's parameters
// (`fn (b Box<T numeric>) Twice() T`). It is then a method of only the
// instances whose type arguments meet those bounds: elsewhere it is neither
// callable nor counted toward an interface.

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
  auto *methods = declared_methods(instance);
  if (!methods || instance->kind != TypeKind::Struct)
    return std::nullopt;
  auto &args = std::get<StructTypeInfo>(instance->detail).type_args;
  for (auto &m : *methods)
    if (m.name == name)
      return unmet_receiver_params(args, m.receiver_params);
  return std::nullopt;
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
