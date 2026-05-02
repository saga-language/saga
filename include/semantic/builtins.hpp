// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/scope.hpp"
#include "semantic/types.hpp"

namespace saga {

// ---------------------------------------------------------------------------
// Built-in type registry
//
// Holds canonical TypePtrs for every primitive type and their methods.
// Constructed once per analysis pass and shared with the global scope.
// ---------------------------------------------------------------------------

struct BuiltinTypes {
  // Primitive types
  TypePtr void_type;
  TypePtr bool_type;
  TypePtr string_type;

  // Platform word-size aliases
  TypePtr int_type;      // Int  (alias to word-size signed integer)
  TypePtr float_type;    // Float (alias to word-size float)
  TypePtr byte_type;     // Byte (alias to Uint8)

  // Sized integers
  TypePtr int8_type;
  TypePtr int16_type;
  TypePtr int32_type;
  TypePtr int64_type;
  TypePtr uint8_type;
  TypePtr uint16_type;
  TypePtr uint32_type;
  TypePtr uint64_type;

  // Character type
  TypePtr char_type;     // Char (utf-8 character)

  // Sized floats
  TypePtr float32_type;
  TypePtr float64_type;

  // Internal interfaces
  TypePtr error_iface;     // Error { Message() String }
  TypePtr iterable_iface;  // |T| Iterable { Next() T | Error }

  // Internal structs
  TypePtr missing_type;    // Missing (implements Error)
  TypePtr task_type;       // Task (returned from spawn)
  TypePtr context_type;    // Context (available inside spawn block)
  TypePtr any_type;        // Any

  // Internal enums
  TypePtr comparison_type; // Comparison { Less, Equal, Greater }

  // Error-recovery sentinel (not a language type)
  TypePtr error_type;

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

} // namespace saga
