// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/builtins.hpp"

namespace mc {

// ===========================================================================
// BuiltinTypes::init
// ===========================================================================

void BuiltinTypes::init() {
  void_type = make_void_type();
  bool_type = make_bool_type();
  int_type = make_int_type();
  float_type = make_float_type();
  string_type = make_string_type();
  error_type = make_error_type();
}

// ===========================================================================
// Built-in methods per type kind
// ===========================================================================

std::vector<MethodInfo> builtin_methods(TypeKind kind,
                                        const BuiltinTypes &t) {
  std::vector<MethodInfo> methods;

  switch (kind) {
  case TypeKind::String:
    // String.len() Int
    methods.push_back(
        {"len", make_func_type({}, {t.int_type}), true});
    // String.contains(String) Bool
    methods.push_back(
        {"contains", make_func_type({t.string_type}, {t.bool_type}), true});
    // String.slice(Int, Int) String
    methods.push_back(
        {"slice",
         make_func_type({t.int_type, t.int_type}, {t.string_type}), true});
    break;

  case TypeKind::Array:
    // Array.len() Int
    methods.push_back(
        {"len", make_func_type({}, {t.int_type}), true});
    // Array.push(T)  — generic; T is the element type.  Placeholder for now.
    // Array.pop() T   — likewise.
    break;

  case TypeKind::Map:
    // Map.len() Int
    methods.push_back(
        {"len", make_func_type({}, {t.int_type}), true});
    // Map.contains(K) Bool  — placeholder
    // Map.delete(K)         — placeholder
    break;

  default:
    break;
  }

  return methods;
}

// ===========================================================================
// register_builtins
// ===========================================================================

void register_builtins(Scope::Ptr global_scope, BuiltinTypes &types) {
  types.init();

  // Register primitive type names.
  auto reg_type = [&](const std::string &name, TypePtr type) {
    global_scope->declare(Symbol::builtin(name, SymbolKind::Type, type));
  };

  reg_type("Void", types.void_type);
  reg_type("Bool", types.bool_type);
  reg_type("Int", types.int_type);
  reg_type("Float", types.float_type);
  reg_type("String", types.string_type);

  // Register built-in constants.
  global_scope->declare(
      Symbol::builtin("true", SymbolKind::Constant, types.bool_type));
  global_scope->declare(
      Symbol::builtin("false", SymbolKind::Constant, types.bool_type));
}

} // namespace mc
