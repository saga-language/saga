// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace saga {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

struct Type;
using TypePtr = std::shared_ptr<Type>;

// ---------------------------------------------------------------------------
// TypeKind — coarse classifier for a Type.
// ---------------------------------------------------------------------------

enum class TypeKind : uint8_t {
  Void,
  Bool,
  Int,
  Float,
  String,
  Array,
  Map,
  Range,
  Func,
  Struct,
  Enum,
  Interface,
  Union,
  TypeParam,   // unresolved generic parameter, e.g. T
  Alias,       // type alias (const MyType = Int)
  Module,      // package/module (from import)
  Error,       // sentinel for error-recovery (propagates silently)
};

// ---------------------------------------------------------------------------
// TypeParam — a single generic type variable.
//
//   |T|, |T, U|  →  each T / U is a TypeParam with a unique id.
// ---------------------------------------------------------------------------

struct TypeParam {
  uint32_t id;           // unique within one analysis pass
  std::string name;      // source name, e.g. "T"
};

// ---------------------------------------------------------------------------
// Concrete type details — stored inside Type via a variant.
// ---------------------------------------------------------------------------

struct VoidType {};
struct BoolType {};

struct IntType {
  uint8_t bits;          // 8, 16, 32, 64; 0 = platform word size (Int)
  bool is_signed;        // false for Uint variants and Byte
};

struct FloatType {
  uint8_t bits;          // 32, 64; 0 = platform word size (Float)
};

struct StringType {};
struct ErrorType {};      // error-recovery sentinel

struct ArrayTypeInfo {
  TypePtr element;
};

struct MapTypeInfo {
  TypePtr key;
  TypePtr value;
};

struct RangeTypeInfo {
  TypePtr element;
};

struct FuncTypeInfo {
  std::vector<TypePtr> params;
  std::vector<TypePtr> returns;
  bool is_variadic = false;
};

struct FieldInfo {
  std::string name;
  TypePtr type;
  bool is_public = false;
};

struct MethodInfo {
  std::string name;
  TypePtr signature;        // always a FuncTypeInfo inside
  bool is_public = false;
  std::string origin_package;
};

struct StructTypeInfo {
  std::string name;
  std::string origin_package;
  std::vector<FieldInfo> fields;
  std::vector<MethodInfo> methods;
  std::vector<TypePtr> embeds;                 // mixin / embedded types
  std::vector<TypeParam> type_params;          // generic parameters
  std::vector<TypePtr> type_args;              // concrete arguments (instantiated)
};

struct EnumVariant {
  std::string name;
  std::vector<FieldInfo> fields;               // associated data
  int64_t index = -1;                          // discriminant value (-1 = auto)
};

struct EnumTypeInfo {
  std::string name;
  std::string origin_package;
  std::vector<EnumVariant> variants;
};

struct InterfaceTypeInfo {
  std::string name;
  std::string origin_package;
  std::vector<MethodInfo> methods;
  std::vector<TypeParam> type_params;
  std::vector<TypePtr> type_args;
};

struct UnionTypeInfo {
  std::vector<TypePtr> alternatives;
};

struct TypeParamInfo {
  TypeParam param;
  std::optional<TypePtr> bound;               // upper-bound constraint, if any
};

struct AliasTypeInfo {
  std::string name;
  std::string origin_package;
  TypePtr underlying;                      // the aliased type
  std::vector<MethodInfo> methods;         // user-bound methods on this alias
};

struct ModuleExport {
  std::string name;
  TypePtr type;
};

struct ModuleTypeInfo {
  std::string name;             // package name (last segment of import path)
  std::string import_path;      // full import path as written
  std::vector<ModuleExport> exports;
};

// ---------------------------------------------------------------------------
// Type — the central type representation.
//
// Each Type is heap-allocated and shared via TypePtr (shared_ptr<Type>).
// Comparison is by identity (pointer equality) for nominal types and by
// structural recursion for structural types (arrays, maps, funcs, unions).
// ---------------------------------------------------------------------------

struct Type {
  // clang-format off
  using Detail = std::variant<
    VoidType,
    BoolType,
    IntType,
    FloatType,
    StringType,
    ArrayTypeInfo,
    MapTypeInfo,
    RangeTypeInfo,
    FuncTypeInfo,
    StructTypeInfo,
    EnumTypeInfo,
    InterfaceTypeInfo,
    UnionTypeInfo,
    TypeParamInfo,
    AliasTypeInfo,
    ModuleTypeInfo,
    ErrorType
  >;
  // clang-format on

  TypeKind kind;
  Detail detail;

  Type(TypeKind k, Detail d) : kind(k), detail(std::move(d)) {}
};

// ---------------------------------------------------------------------------
// Factory helpers — construct TypePtrs for every kind.
// ---------------------------------------------------------------------------

TypePtr make_void_type();
TypePtr make_bool_type();
TypePtr make_int_type(uint8_t bits = 0, bool is_signed = true);
TypePtr make_float_type(uint8_t bits = 0);
TypePtr make_string_type();
TypePtr make_error_type();

TypePtr make_array_type(TypePtr element);
TypePtr make_map_type(TypePtr key, TypePtr value);
TypePtr make_range_type(TypePtr element);
TypePtr make_func_type(std::vector<TypePtr> params,
                       std::vector<TypePtr> returns,
                       bool is_variadic = false);
TypePtr make_struct_type(const std::string &name,
                         std::vector<FieldInfo> fields = {},
                         std::vector<MethodInfo> methods = {},
                         std::vector<TypeParam> type_params = {},
                         std::string origin_package = "");
TypePtr make_enum_type(const std::string &name,
                       std::vector<EnumVariant> variants = {},
                       std::string origin_package = "");
TypePtr make_interface_type(const std::string &name,
                            std::vector<MethodInfo> methods = {},
                            std::vector<TypeParam> type_params = {},
                            std::string origin_package = "");
TypePtr make_union_type(std::vector<TypePtr> alternatives);
TypePtr make_type_param(uint32_t id, const std::string &name,
                        std::optional<TypePtr> bound = std::nullopt);
TypePtr make_alias_type(const std::string &name, TypePtr underlying,
                        std::vector<MethodInfo> methods = {},
                        std::string origin_package = "");
TypePtr make_module_type(const std::string &name,
                         const std::string &import_path,
                         std::vector<ModuleExport> exports = {});

// ---------------------------------------------------------------------------
// Type queries
// ---------------------------------------------------------------------------

/// True if `t` is the error-recovery sentinel.
bool is_error_type(const TypePtr &t);

/// True if `t` is a numeric type (Int or Float).
bool is_numeric(const TypePtr &t);

/// True if `t` is a type that supports ordering comparisons.
bool is_ordered(const TypePtr &t);

/// True if `t` is a type that supports equality comparisons.
bool is_equatable(const TypePtr &t);

/// True if `t` is a callable type (FuncTypeInfo).
bool is_callable(const TypePtr &t);

/// True if `t` is iterable (Array, Map, Range, String).
bool is_iterable(const TypePtr &t);

/// Human-readable type name for error messages.
std::string type_to_string(const TypePtr &t);

/// Unwrap alias types to get the underlying concrete type.
/// Returns the input if it is not an alias.
TypePtr unwrap_alias(const TypePtr &t);

/// Get the TypeKind of the underlying type, unwrapping aliases.
TypeKind underlying_kind(const TypePtr &t);

/// Return the origin_package of a nominal type (Struct, Enum, Interface),
/// unwrapping aliases first. Returns "" for non-nominal kinds.
inline std::string origin_of(const TypePtr &t) {
  if (!t) return "";
  auto u = unwrap_alias(t);
  if (!u) return "";
  switch (u->kind) {
  case TypeKind::Struct:
    return std::get<StructTypeInfo>(u->detail).origin_package;
  case TypeKind::Enum:
    return std::get<EnumTypeInfo>(u->detail).origin_package;
  case TypeKind::Interface:
    return std::get<InterfaceTypeInfo>(u->detail).origin_package;
  default:
    return "";
  }
}

// ---------------------------------------------------------------------------
// Type compatibility
// ---------------------------------------------------------------------------

/// Structural type equality (recursive).
bool types_equal(const TypePtr &a, const TypePtr &b);

/// True if `source` can be assigned to a location of type `target`.
/// Handles union widening, interface satisfaction, etc.
bool is_assignable_to(const TypePtr &source, const TypePtr &target);

/// Given two types, return the common type they can both be promoted to,
/// or nullptr if no common type exists.  Used for binary operators and
/// if/switch arms.
TypePtr common_type(const TypePtr &a, const TypePtr &b);

// ---------------------------------------------------------------------------
// Generics — substitution
// ---------------------------------------------------------------------------

/// Produce a concrete type by replacing every TypeParam whose id appears in
/// `bindings` with the corresponding concrete type.  Returns a new TypePtr;
/// the original is not mutated.
TypePtr substitute(const TypePtr &t,
                   const std::unordered_map<uint32_t, TypePtr> &bindings);

/// Attempt to infer type-parameter bindings by unifying `param_type` (which
/// may contain TypeParam nodes) against `arg_type` (fully concrete).
/// Returns true on success and populates `out`.  On conflict returns false.
bool unify(const TypePtr &param_type, const TypePtr &arg_type,
           std::unordered_map<uint32_t, TypePtr> &out);

/// Returns true if the type (or any nested type within it) contains at
/// least one TypeParam node.
bool has_type_params(const TypePtr &t);

} // namespace saga
