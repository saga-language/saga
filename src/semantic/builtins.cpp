// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/builtins.hpp"

namespace mc {

// ===========================================================================
// BuiltinTypes::init
// ===========================================================================

void BuiltinTypes::init() {
  // -- Primitives ----------------------------------------------------------
  void_type = make_void_type();
  bool_type = make_bool_type();
  string_type = make_string_type();

  // -- Platform word-size aliases ------------------------------------------
  int_type = make_int_type(0, true);
  float_type = make_float_type(0);
  byte_type = make_int_type(8, false); // Byte = Uint8
  char_type = make_int_type(32, false); // Char = utf-8 codepoint (Uint32)

  // -- Sized integers ------------------------------------------------------
  int8_type = make_int_type(8, true);
  int16_type = make_int_type(16, true);
  int32_type = make_int_type(32, true);
  int64_type = make_int_type(64, true);
  uint8_type = make_int_type(8, false);
  uint16_type = make_int_type(16, false);
  uint32_type = make_int_type(32, false);
  uint64_type = make_int_type(64, false);

  // -- Sized floats --------------------------------------------------------
  float32_type = make_float_type(32);
  float64_type = make_float_type(64);

  // -- Error interface: Error { Message() String } -------------------------
  error_iface = make_interface_type(
      "Error",
      {MethodInfo{"Message", make_func_type({}, {string_type}), true}});

  // -- Iterable interface: |T| Iterable { Next() T | Error } --------------
  // Registered with a single type parameter; concrete instantiations are
  // produced by substitution during generic resolution.
  iterable_iface = make_interface_type(
      "Iterable",
      {MethodInfo{"Next",
                  make_func_type(
                      {},
                      {make_union_type(
                          {make_type_param(0, "T"), error_iface})}),
                  true}},
      {TypeParam{0, "T"}});

  // -- Missing struct (implements Error) -----------------------------------
  missing_type = make_struct_type(
      "Missing", /*fields=*/{},
      {MethodInfo{"Message", make_func_type({}, {string_type}), true}});

  // -- Comparison enum { Less, Equal, Greater } ----------------------------
  comparison_type = make_enum_type(
      "Comparison",
      {EnumVariant{"Less", {}}, EnumVariant{"Equal", {}},
       EnumVariant{"Greater", {}}});

  // -- Any -----------------------------------------------------------------
  // Any is a special fat type; represented as a struct with no user-visible
  // fields.  Its conversion methods are registered as built-in methods.
  any_type = make_struct_type("Any");

  // -- Task (returned from spawn) ------------------------------------------
  task_type = make_struct_type(
      "Task", /*fields=*/{},
      {MethodInfo{"Alive?", make_func_type({}, {bool_type}), true},
       MethodInfo{"Cancel", make_func_type({}, {void_type}), true},
       MethodInfo{"Term", make_func_type({}, {void_type}), true},
       MethodInfo{"Wait",
                  make_func_type(
                      {}, {make_union_type(
                              {make_type_param(0, "T"), error_iface})}),
                  true}},
      {TypeParam{0, "T"}});

  // -- Context (available inside spawn block) ------------------------------
  context_type = make_struct_type(
      "Context", /*fields=*/{},
      {MethodInfo{"Cancelled?", make_func_type({}, {bool_type}), true},
       MethodInfo{"Exit",
                  make_func_type({make_type_param(0, "T")}, {void_type}),
                  true},
       MethodInfo{"Send",
                  make_func_type({make_type_param(0, "T")}, {void_type}),
                  true}},
      {TypeParam{0, "T"}});

  // -- Error-recovery sentinel (compiler internal) -------------------------
  error_type = make_error_type();
}

// ===========================================================================
// Built-in methods per type kind
// ===========================================================================

std::vector<MethodInfo> builtin_methods(TypeKind kind,
                                        const BuiltinTypes &t) {
  std::vector<MethodInfo> methods;

  // All types implement: .String() String, .Compare(T) Comparison,
  //                      .Equals(T) Bool
  // These are added per-kind below alongside kind-specific methods.

  switch (kind) {
  case TypeKind::String:
    methods.push_back(
        {"Bytes", make_func_type({}, {make_array_type(t.byte_type)}), true});
    methods.push_back(
        {"Count", make_func_type({}, {t.int_type}), true});
    methods.push_back(
        {"Float",
         make_func_type({}, {make_union_type({t.float_type, t.error_iface})}),
         true});
    methods.push_back(
        {"Format", make_func_type({t.string_type}, {t.string_type}), true});
    methods.push_back(
        {"Int",
         make_func_type({}, {make_union_type({t.int_type, t.error_iface})}),
         true});
    methods.push_back(
        {"Lower", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Runes", make_func_type({}, {make_array_type(t.int32_type)}), true});
    methods.push_back(
        {"Size", make_func_type({}, {t.int_type}), true});
    methods.push_back(
        {"Upper", make_func_type({}, {t.string_type}), true});
    // Universal methods
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Compare", make_func_type({t.string_type}, {t.comparison_type}),
         true});
    methods.push_back(
        {"Equals", make_func_type({t.string_type}, {t.bool_type}), true});
    break;

  case TypeKind::Array: {
    // Array methods use a type-param placeholder for the element type.
    // The analyzer's type-checker resolves the concrete element type at
    // each call site; these signatures just need the right arity.
    auto tp = make_type_param(9990, "T");
    methods.push_back(
        {"At", make_func_type({t.int_type}, {tp}), true});
    methods.push_back(
        {"Find", make_func_type({tp}, {make_union_type({t.int_type, t.error_iface})}), true});
    methods.push_back(
        {"Insert", make_func_type({tp, t.int_type}, {t.void_type}), true});
    methods.push_back(
        {"Push", make_func_type({tp}, {tp}), true});
    methods.push_back(
        {"Pop", make_func_type({}, {tp}), true});
    methods.push_back(
        {"Set", make_func_type({t.int_type, tp}, {t.void_type}), true});
    methods.push_back(
        {"Size", make_func_type({}, {t.int_type}), true});
    // Universal methods
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    break;
  }

  case TypeKind::Map: {
    // Map methods use type-param placeholders for key (K) and value (V).
    // The analyzer's type-checker resolves concrete types at call sites.
    auto kp = make_type_param(9991, "K");
    auto vp = make_type_param(9992, "V");
    methods.push_back(
        {"At", make_func_type({kp}, {vp}), true});
    methods.push_back(
        {"Key?", make_func_type({kp}, {t.bool_type}), true});
    methods.push_back(
        {"Keys", make_func_type({}, {make_array_type(kp)}), true});
    methods.push_back(
        {"Remove", make_func_type({kp}, {t.void_type}), true});
    methods.push_back(
        {"Set", make_func_type({kp, vp}, {t.void_type}), true});
    methods.push_back(
        {"Size", make_func_type({}, {t.int_type}), true});
    // Universal methods
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    break;
  }

  case TypeKind::Int:
    // Universal methods
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Compare", make_func_type({t.int_type}, {t.comparison_type}), true});
    methods.push_back(
        {"Equals", make_func_type({t.int_type}, {t.bool_type}), true});
    // Converters — a subset; the full set is registered during init.
    methods.push_back(
        {"Float", make_func_type({}, {t.float_type}), true});
    methods.push_back(
        {"Float32", make_func_type({}, {t.float32_type}), true});
    methods.push_back(
        {"Float64", make_func_type({}, {t.float64_type}), true});
    methods.push_back(
        {"Format", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Int", make_func_type({}, {t.int_type}), true});
    methods.push_back(
        {"Int8", make_func_type({}, {t.int8_type}), true});
    methods.push_back(
        {"Int16", make_func_type({}, {t.int16_type}), true});
    methods.push_back(
        {"Int32", make_func_type({}, {t.int32_type}), true});
    methods.push_back(
        {"Int64", make_func_type({}, {t.int64_type}), true});
    methods.push_back(
        {"Uint8", make_func_type({}, {t.uint8_type}), true});
    methods.push_back(
        {"Uint16", make_func_type({}, {t.uint16_type}), true});
    methods.push_back(
        {"Uint32", make_func_type({}, {t.uint32_type}), true});
    methods.push_back(
        {"Uint64", make_func_type({}, {t.uint64_type}), true});
    methods.push_back(
        {"Char", make_func_type({}, {t.char_type}), true});
    break;

  case TypeKind::Float:
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Compare", make_func_type({t.float_type}, {t.comparison_type}),
         true});
    methods.push_back(
        {"Equals", make_func_type({t.float_type}, {t.bool_type}), true});
    methods.push_back(
        {"Format", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Float32", make_func_type({}, {t.float32_type}), true});
    methods.push_back(
        {"Float64", make_func_type({}, {t.float64_type}), true});
    break;

  case TypeKind::Bool:
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    methods.push_back(
        {"Equals", make_func_type({t.bool_type}, {t.bool_type}), true});
    break;

  case TypeKind::Enum:
    methods.push_back(
        {"Int", make_func_type({}, {t.int_type}), true});
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
    break;

  case TypeKind::Range:
    methods.push_back(
        {"Array", make_func_type({}, {}), true}); // returns [T], generic
    methods.push_back(
        {"String", make_func_type({}, {t.string_type}), true});
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

  auto reg_type = [&](const std::string &name, TypePtr type) {
    global_scope->declare(Symbol::builtin(name, SymbolKind::Type, type));
  };

  // -- Primitive types -----------------------------------------------------
  reg_type("Void", types.void_type);
  reg_type("Bool", types.bool_type);
  reg_type("String", types.string_type);

  // -- Numeric aliases (platform word-size) --------------------------------
  reg_type("Int", types.int_type);
  reg_type("Float", types.float_type);
  reg_type("Byte", types.byte_type);
  reg_type("Char", types.char_type);

  // -- Sized integers ------------------------------------------------------
  reg_type("Int8", types.int8_type);
  reg_type("Int16", types.int16_type);
  reg_type("Int32", types.int32_type);
  reg_type("Int64", types.int64_type);
  reg_type("Uint8", types.uint8_type);
  reg_type("Uint16", types.uint16_type);
  reg_type("Uint32", types.uint32_type);
  reg_type("Uint64", types.uint64_type);

  // -- Sized floats --------------------------------------------------------
  reg_type("Float32", types.float32_type);
  reg_type("Float64", types.float64_type);

  // -- Internal interfaces -------------------------------------------------
  reg_type("Error", types.error_iface);
  reg_type("Iterable", types.iterable_iface);

  // -- Internal structs ----------------------------------------------------
  reg_type("Missing", types.missing_type);
  reg_type("Any", types.any_type);
  reg_type("Task", types.task_type);
  reg_type("Context", types.context_type);

  // -- Internal enums ------------------------------------------------------
  reg_type("Comparison", types.comparison_type);

  // -- Built-in constants --------------------------------------------------
  global_scope->declare(
      Symbol::builtin("true", SymbolKind::Constant, types.bool_type));
  global_scope->declare(
      Symbol::builtin("false", SymbolKind::Constant, types.bool_type));

  // -- Built-in intrinsic functions ----------------------------------------
  global_scope->declare(Symbol::builtin(
      "intrinsic_print", SymbolKind::Function,
      make_func_type({types.string_type}, {types.void_type})));

  // intrinsic_yield() — voluntarily yield execution inside a spawn block
  global_scope->declare(Symbol::builtin(
      "intrinsic_yield", SymbolKind::Function,
      make_func_type({}, {types.void_type})));

  // intrinsic_atomic_add(ptr: Int, val: Int) -> Int
  // Atomically adds val to *ptr and returns the old value.
  global_scope->declare(Symbol::builtin(
      "intrinsic_atomic_add", SymbolKind::Function,
      make_func_type({types.int_type, types.int_type}, {types.int_type})));

  // intrinsic_trap(reason: String) — transition actor to ZOMBIE state
  global_scope->declare(Symbol::builtin(
      "intrinsic_trap", SymbolKind::Function,
      make_func_type({types.string_type}, {types.void_type})));

  // intrinsic_syscall(num: Int, args: [Int]) -> Int | Error
  // Raw syscall invocation used by std/sys.
  global_scope->declare(Symbol::builtin(
      "intrinsic_syscall", SymbolKind::Function,
      make_func_type({types.int_type, make_array_type(types.int_type)},
                     {make_union_type({types.int_type, types.error_iface})})));

  // intrinsic_ptr(value: String | [Byte]) -> Int
  // Returns the raw memory address of the backing buffer.
  global_scope->declare(Symbol::builtin(
      "intrinsic_ptr", SymbolKind::Function,
      make_func_type(
          {make_union_type({types.string_type, make_array_type(types.byte_type)})},
          {types.int_type})));
}

} // namespace mc
