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

  // Sized floats
  TypePtr float32_type;
  TypePtr float64_type;

  // Base error: struct-backed abstract error, boxed at runtime as a pointer
  // to { i64 type_id, String message, ...fields }. Every concrete error
  // widens to it; `.message` is the common prefix.
  TypePtr error_base;      // error { type_id Int64, message String }

  // Internal interfaces
  TypePtr iterable_iface;  // |T| Iterable { Next() T | error }

  // Named protocols loaded from std/proto.  Populated by load_prelude
  // from proto.sgi; null when the package is unavailable (during the
  // first compilation of std/proto itself or when no resolver is set).
  // Used by the analyzer to resolve method calls on TypeParam values
  // against a named protocol shape.
  TypePtr hashable_iface;    // Hashable   { Hash() Int64; Equals(Self) Bool }
  TypePtr stringable_iface;  // Stringable { String() String }

  // Internal structs
  // Null is the value channel's "there is a value here, and it carries no
  // information" — what JSON and SQL mean by null. Missing is the error
  // channel's "there was nothing here". Both are empty; the channel is the
  // whole distinction, which is why `or` catches one and walks past the other.
  TypePtr null_type;       // Null (value; a value that carries nothing)
  TypePtr missing_type;    // Missing (error; index/map miss, parse failure)
  TypePtr trapped_type;    // Trapped (error; Task.Wait on a killed actor)
  TypePtr task_type;       // Task (returned from spawn)
  TypePtr context_type;    // Context (available inside spawn block)

  // Internal enums
  TypePtr comparison_type; // Comparison { Less, Equal, Greater }

  // Error-recovery sentinel (not a language type)
  TypePtr invalid_type;
  TypePtr unknown_type;

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
