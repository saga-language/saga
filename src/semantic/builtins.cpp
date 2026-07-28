// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/builtins.hpp"

namespace saga {

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

  // -- Errors: struct-backed, marked, boxed at runtime ---------------------
  // Every error carries the { type_id, message } prefix; concrete errors
  // append their own fields. `error` is the abstract base every error widens
  // to; `Missing`/`Trapped` are the built-in errors.
  auto make_error_struct = [&](const std::string &name) {
    auto t = make_struct_type(
        name,
        {FieldInfo{"type_id", int64_type, /*is_public=*/false, nullptr},
         FieldInfo{"message", string_type, /*is_public=*/true, nullptr}});
    std::get<StructTypeInfo>(t->detail).is_error = true;
    return t;
  };
  error_base = make_error_struct("error");
  missing_type = make_error_struct("Missing");
  trapped_type = make_error_struct("Trapped");

  // -- Iterable interface: |T| Iterable { Next() T | error } --------------
  // Registered with a single type parameter; concrete instantiations are
  // produced by substitution during generic resolution.
  iterable_iface = make_interface_type(
      "Iterable",
      {MethodInfo{"Next",
                  make_func_type(
                      {},
                      {make_union_type(
                          {make_type_param(0, "T"), error_base})}),
                  true}},
      {TypeParam{0, "T"}});

  // -- Comparison enum { Less, Equal, Greater } ----------------------------
  comparison_type = make_enum_type(
      "Comparison",
      {EnumVariant{"Less"}, EnumVariant{"Equal"}, EnumVariant{"Greater"}});

  // -- Task (returned from spawn) ------------------------------------------
  task_type = make_struct_type(
      "Task", /*fields=*/{},
      {MethodInfo{"Alive?", make_func_type({}, {bool_type}), true},
       MethodInfo{"Cancel", make_func_type({}, {void_type}), true},
       MethodInfo{"Term", make_func_type({}, {void_type}), true},
       MethodInfo{"Wait",
                  make_func_type(
                      {}, {make_union_type(
                              {make_type_param(0, "T"), error_base})}),
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
  invalid_type = make_invalid_type();
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
  // Bool, Int, Float, String, Array, Map methods are fully migrated to
  // stdlib packages (std/bool, std/int, std/float, std/string, std/array,
  // std/map).

  case TypeKind::Enum:
    methods.push_back(
        {"Int", make_func_type({}, {t.int_type}), true});
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
  reg_type("void", types.void_type);
  reg_type("bool", types.bool_type);
  reg_type("string", types.string_type);

  // -- Numeric aliases (platform word-size) --------------------------------
  reg_type("int", types.int_type);
  reg_type("float", types.float_type);
  reg_type("byte", types.byte_type);

  // -- Sized integers ------------------------------------------------------
  reg_type("int8", types.int8_type);
  reg_type("int16", types.int16_type);
  reg_type("int32", types.int32_type);
  reg_type("int64", types.int64_type);
  reg_type("uint8", types.uint8_type);
  reg_type("uint16", types.uint16_type);
  reg_type("uint32", types.uint32_type);
  reg_type("uint64", types.uint64_type);

  // -- Sized floats --------------------------------------------------------
  reg_type("float32", types.float32_type);
  reg_type("float64", types.float64_type);

  // -- Internal interfaces -------------------------------------------------
  reg_type("error", types.error_base);
  reg_type("Iterable", types.iterable_iface);

  // -- Internal structs ----------------------------------------------------
  reg_type("Missing", types.missing_type);
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

  // intrinsic_syscall(num: Int, args: [Int]) -> Int | error
  // Raw syscall invocation used by std/sys.
  global_scope->declare(Symbol::builtin(
      "intrinsic_syscall", SymbolKind::Function,
      make_func_type({types.int_type, make_array_type(types.int_type)},
                     {make_union_type({types.int_type, types.error_base})})));

  // intrinsic_ptr(value: String | [Byte]) -> Int
  // Returns the raw memory address of the backing buffer.
  global_scope->declare(Symbol::builtin(
      "intrinsic_ptr", SymbolKind::Function,
      make_func_type(
          {make_union_type({types.string_type, make_array_type(types.byte_type)})},
          {types.int_type})));

  // Helper: build a TypeParam carrying a Phase-2 constraint.
  auto bounded_tp = [](TypeConstraint c) {
    auto tp = make_type_param(0, "T");
    std::get<TypeParamInfo>(tp->detail).param.constraint = c;
    return tp;
  };

  // intrinsic_sitofp |T Integer| (value: T) -> Float
  // LLVM sitofp T → f64.  Constraint accepts any integer-kind input
  // (Int, Int8/16/32/64, Uint*, Byte).
  global_scope->declare(Symbol::builtin(
      "intrinsic_sitofp", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.float_type})));

  // intrinsic_fptosi |T Float| (value: T) -> Int
  // LLVM fptosi T → i64.  Constraint accepts Float, Float32, Float64.
  global_scope->declare(Symbol::builtin(
      "intrinsic_fptosi", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Float)},
                     {types.int_type})));

  // One symbol per destination width keeps the return type concrete —
  // a generic-over-output-type signature would need return-context
  // inference which Saga does not have.
  global_scope->declare(Symbol::builtin(
      "intrinsic_sext_i8", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.int8_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_sext_i16", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.int16_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_sext_i32", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.int32_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_sext_i64", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.int64_type})));

  global_scope->declare(Symbol::builtin(
      "intrinsic_zext_u8", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.uint8_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_zext_u16", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.uint16_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_zext_u32", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.uint32_type})));
  global_scope->declare(Symbol::builtin(
      "intrinsic_zext_u64", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.uint64_type})));

  // intrinsic_sitofp32 |T Integer| (value: T) -> Float32
  // LLVM sitofp T → f32.
  global_scope->declare(Symbol::builtin(
      "intrinsic_sitofp32", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Integer)},
                     {types.float32_type})));

  // intrinsic_fptrunc |T Float| (value: T) -> Float32
  // LLVM fptrunc T → f32.  Float/Float64 narrowed to f32; an f32 input
  // is passed through unchanged at codegen time.
  global_scope->declare(Symbol::builtin(
      "intrinsic_fptrunc", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Float)},
                     {types.float32_type})));

  // intrinsic_fpext |T Float| (value: T) -> Float
  // LLVM fpext T → f64.  Float32 widened to f64; f64 input passes through.
  global_scope->declare(Symbol::builtin(
      "intrinsic_fpext", SymbolKind::Function,
      make_func_type({bounded_tp(TypeConstraint::Float)},
                     {types.float_type})));

  // intrinsic_is_string |T| (value T) -> Bool
  // Compile-time predicate folded by codegen against the argument's static
  // type after monomorphisation: i1 1 when the type is String, i1 0
  // otherwise.  Used by stdlib formatters (Map/Array String) to add
  // surrounding quotes only when rendering a String value.
  {
    auto t = make_type_param(0, "T");
    global_scope->declare(Symbol::builtin(
        "intrinsic_is_string", SymbolKind::Function,
        make_func_type({t}, {types.bool_type})));
  }
}

} // namespace saga
