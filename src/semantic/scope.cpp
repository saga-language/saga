// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/scope.hpp"

namespace mc {

// ===========================================================================
// Construction
// ===========================================================================

Scope::Scope(ScopeKind k, Ptr parent_scope)
    : kind(k), parent(std::move(parent_scope)) {}

Scope::Ptr Scope::child(ScopeKind k) {
  return std::make_shared<Scope>(k, shared_from_this());
}

// ===========================================================================
// Symbol operations
// ===========================================================================

bool Scope::declare(const Symbol &sym) {
  auto [_, inserted] = symbols.emplace(sym.name, sym);
  return inserted;
}

std::optional<Symbol> Scope::lookup(const std::string &name) const {
  auto it = symbols.find(name);
  if (it != symbols.end())
    return it->second;
  if (parent)
    return parent->lookup(name);
  return std::nullopt;
}

std::optional<Symbol> Scope::lookup_local(const std::string &name) const {
  auto it = symbols.find(name);
  if (it != symbols.end())
    return it->second;
  return std::nullopt;
}

// ===========================================================================
// Scope queries
// ===========================================================================

std::shared_ptr<const Scope> Scope::nearest(ScopeKind k) const {
  if (kind == k)
    return shared_from_this();
  if (parent)
    return parent->nearest(k);
  return nullptr;
}

bool Scope::is_inside(ScopeKind k) const { return nearest(k) != nullptr; }

std::unordered_map<uint32_t, TypePtr> Scope::all_type_bindings() const {
  std::unordered_map<uint32_t, TypePtr> result;

  // Walk from outermost to innermost so inner bindings override.
  if (parent) {
    result = parent->all_type_bindings();
  }
  for (auto &[id, type] : type_bindings) {
    result[id] = type;
  }
  return result;
}

} // namespace mc
