// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/scope.hpp"
#include "semantic/types.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// Built-in type registry
//
// Holds canonical TypePtrs for every primitive type and their methods.
// Constructed once per analysis pass and shared with the global scope.
// ---------------------------------------------------------------------------

struct BuiltinTypes {
  TypePtr void_type;
  TypePtr bool_type;
  TypePtr int_type;
  TypePtr float_type;
  TypePtr string_type;
  TypePtr error_type;   // error-recovery sentinel

  /// Initialise all built-in type singletons.
  void init();
};

// ---------------------------------------------------------------------------
// Method tables for built-in types
//
// Each built-in type can have methods (e.g. String.len(), Array.append()).
// These are stored as MethodInfo vectors keyed by TypeKind.
// ---------------------------------------------------------------------------

/// Return the methods available on the given built-in type kind.
std::vector<MethodInfo> builtin_methods(TypeKind kind,
                                        const BuiltinTypes &types);

// ---------------------------------------------------------------------------
// Scope population
// ---------------------------------------------------------------------------

/// Register all built-in types and constants into the given global scope.
/// Call once at the start of analysis.
void register_builtins(Scope::Ptr global_scope, BuiltinTypes &types);

} // namespace mc
