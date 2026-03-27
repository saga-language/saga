// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/types.hpp"
#include "frontend/span.hpp"

#include <cstdint>
#include <string>

namespace mc {

// ---------------------------------------------------------------------------
// SymbolKind — what a name refers to.
// ---------------------------------------------------------------------------

enum class SymbolKind : uint8_t {
  Variable,
  Constant,
  Function,
  Parameter,
  Type,          // struct, enum, interface, or built-in type name
  TypeParam,     // generic type parameter (T, U, …)
  EnumVariant,
  Field,
  Method,
  Module,
};

// ---------------------------------------------------------------------------
// Symbol — a named entity in a scope.
//
// Every declaration and every built-in is represented as a Symbol.
// The `type` field may be nullptr for forward declarations that haven't
// been resolved yet.
// ---------------------------------------------------------------------------

struct Symbol {
  std::string name;
  SymbolKind kind;
  TypePtr type;              // resolved type (nullptr before type-checking)
  Span decl_span = {0, 0};  // source location of the declaration
  bool is_public = false;
  bool is_mutable = true;   // false for constants and parameters
  bool is_builtin = false;  // true for compiler-provided symbols

  // Convenience constructors
  static Symbol variable(const std::string &name, TypePtr type, Span span,
                          bool is_public = false);
  static Symbol constant(const std::string &name, TypePtr type, Span span,
                          bool is_public = false);
  static Symbol function(const std::string &name, TypePtr type, Span span,
                          bool is_public = false);
  static Symbol parameter(const std::string &name, TypePtr type, Span span);
  static Symbol type_sym(const std::string &name, TypePtr type, Span span,
                          bool is_public = false);
  static Symbol type_param(const std::string &name, TypePtr type, Span span);
  static Symbol enum_variant(const std::string &name, TypePtr type, Span span);
  static Symbol builtin(const std::string &name, SymbolKind kind, TypePtr type);
};

} // namespace mc
