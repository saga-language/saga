// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/symbol.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace saga {

// ---------------------------------------------------------------------------
// ScopeKind — distinguishes scope levels for semantic rules.
//
// For example, `break` is only valid inside a Loop scope; `return` must
// find an enclosing Function scope to determine the expected return type.
// ---------------------------------------------------------------------------

enum class ScopeKind : uint8_t {
  Global,      // built-ins and top-level declarations
  Module,      // one source file's top-level scope
  Function,    // function or method body
  Loop,        // for-loop body
  Block,       // plain { } block, if/else/switch arms
  Struct,      // struct declaration body (fields + methods)
  Enum,        // enum declaration body
  Interface,   // interface declaration body
  Spawn,       // spawn block
};

// ---------------------------------------------------------------------------
// Scope — a lexical scope that holds symbol declarations.
//
// Scopes form a tree rooted at the global scope. Each scope has a pointer
// to its parent; lookup walks the chain. A scope can also carry generic
// type-parameter bindings for the current context.
// ---------------------------------------------------------------------------

struct Scope : public std::enable_shared_from_this<Scope> {
  using Ptr = std::shared_ptr<Scope>;

  ScopeKind kind;
  std::shared_ptr<Scope> parent;               // nullptr for global scope
  std::unordered_map<std::string, Symbol> symbols;

  // Type-parameter bindings active in this scope (populated when entering
  // a generic function or type).
  std::unordered_map<uint32_t, TypePtr> type_bindings;

  // For Function scopes: the declared return types.
  std::vector<TypePtr> return_types;

  // For Loop scopes: the expected break-value type (if any).
  std::optional<TypePtr> break_type;

  // True if this Function scope belongs to a closure (FuncExprNode).
  bool is_closure = false;

  // ── Construction ─────────────────────────────────────────────────────

  explicit Scope(ScopeKind k, Ptr parent = nullptr);

  /// Create a child scope of the given kind.
  Ptr child(ScopeKind k);

  // ── Symbol operations ────────────────────────────────────────────────

  /// Declare a symbol in this scope. Returns false if the name is already
  /// declared in *this* scope (does not check parents).
  bool declare(const Symbol &sym);

  /// Look up a name in this scope and all ancestors. Returns std::nullopt
  /// if not found anywhere.
  std::optional<Symbol> lookup(const std::string &name) const;

  /// Look up a name in *only* this scope (no parent walk).
  std::optional<Symbol> lookup_local(const std::string &name) const;

  // ── Scope queries ────────────────────────────────────────────────────

  /// Walk the parent chain and return the nearest scope of the given kind,
  /// or nullptr.
  std::shared_ptr<const Scope> nearest(ScopeKind k) const;

  /// Convenience: is there an enclosing scope of this kind?
  bool is_inside(ScopeKind k) const;

  /// Collect all type-parameter bindings visible from this scope
  /// (merges this scope's bindings with ancestors').
  std::unordered_map<uint32_t, TypePtr> all_type_bindings() const;
};

} // namespace saga
