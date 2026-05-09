// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/types.hpp"

#include <gtest/gtest.h>

namespace saga {

// ===========================================================================
// Factory / singleton tests
// ===========================================================================

TEST(Types, PrimitiveFactories) {
  auto v = make_void_type();
  auto b = make_bool_type();
  auto i = make_int_type();
  auto f = make_float_type();
  auto s = make_string_type();
  auto e = make_error_type();

  EXPECT_EQ(v->kind, TypeKind::Void);
  EXPECT_EQ(b->kind, TypeKind::Bool);
  EXPECT_EQ(i->kind, TypeKind::Int);
  EXPECT_EQ(f->kind, TypeKind::Float);
  EXPECT_EQ(s->kind, TypeKind::String);
  EXPECT_EQ(e->kind, TypeKind::Error);
}

TEST(Types, ArrayFactory) {
  auto arr = make_array_type(make_int_type());
  EXPECT_EQ(arr->kind, TypeKind::Array);
  auto &info = std::get<ArrayTypeInfo>(arr->detail);
  EXPECT_EQ(info.element->kind, TypeKind::Int);
}

TEST(Types, MapFactory) {
  auto m = make_map_type(make_string_type(), make_int_type());
  EXPECT_EQ(m->kind, TypeKind::Map);
  auto &info = std::get<MapTypeInfo>(m->detail);
  EXPECT_EQ(info.key->kind, TypeKind::String);
  EXPECT_EQ(info.value->kind, TypeKind::Int);
}

TEST(Types, FuncFactory) {
  auto fn = make_func_type({make_int_type(), make_string_type()},
                           {make_bool_type()});
  EXPECT_EQ(fn->kind, TypeKind::Func);
  auto &info = std::get<FuncTypeInfo>(fn->detail);
  EXPECT_EQ(info.params.size(), 2u);
  EXPECT_EQ(info.returns.size(), 1u);
  EXPECT_FALSE(info.is_variadic);
}

TEST(Types, UnionFactory) {
  auto u = make_union_type({make_int_type(), make_string_type()});
  EXPECT_EQ(u->kind, TypeKind::Union);
  auto &info = std::get<UnionTypeInfo>(u->detail);
  EXPECT_EQ(info.alternatives.size(), 2u);
}

TEST(Types, TypeParamFactory) {
  auto tp = make_type_param(0, "T");
  EXPECT_EQ(tp->kind, TypeKind::TypeParam);
  auto &info = std::get<TypeParamInfo>(tp->detail);
  EXPECT_EQ(info.param.id, 0u);
  EXPECT_EQ(info.param.name, "T");
}

// ===========================================================================
// Query tests
// ===========================================================================

TEST(Types, IsErrorType) {
  EXPECT_TRUE(is_error_type(make_error_type()));
  EXPECT_FALSE(is_error_type(make_int_type()));
}

TEST(Types, IsNumeric) {
  EXPECT_TRUE(is_numeric(make_int_type()));
  EXPECT_TRUE(is_numeric(make_float_type()));
  EXPECT_FALSE(is_numeric(make_string_type()));
}

TEST(Types, IsOrdered) {
  EXPECT_TRUE(is_ordered(make_int_type()));
  EXPECT_TRUE(is_ordered(make_float_type()));
  EXPECT_TRUE(is_ordered(make_string_type()));
  EXPECT_FALSE(is_ordered(make_bool_type()));
}

TEST(Types, IsCallable) {
  auto fn = make_func_type({}, {});
  EXPECT_TRUE(is_callable(fn));
  EXPECT_FALSE(is_callable(make_int_type()));
}

TEST(Types, IsIterable) {
  EXPECT_TRUE(is_iterable(make_array_type(make_int_type())));
  EXPECT_TRUE(is_iterable(make_map_type(make_string_type(), make_int_type())));
  EXPECT_TRUE(is_iterable(make_range_type(make_int_type())));
  EXPECT_TRUE(is_iterable(make_string_type()));
  EXPECT_FALSE(is_iterable(make_bool_type()));
}

// ===========================================================================
// type_to_string
// ===========================================================================

TEST(Types, TypeToString) {
  EXPECT_EQ(type_to_string(make_void_type()), "Void");
  EXPECT_EQ(type_to_string(make_int_type()), "Int");
  EXPECT_EQ(type_to_string(make_array_type(make_string_type())), "String[]");
  EXPECT_EQ(type_to_string(make_type_param(0, "T")), "T");
}

TEST(Types, SizedIntTypeToString) {
  EXPECT_EQ(type_to_string(make_int_type(8, true)), "Int8");
  EXPECT_EQ(type_to_string(make_int_type(16, true)), "Int16");
  EXPECT_EQ(type_to_string(make_int_type(32, true)), "Int32");
  EXPECT_EQ(type_to_string(make_int_type(64, true)), "Int64");
  EXPECT_EQ(type_to_string(make_int_type(8, false)), "Uint8");
  EXPECT_EQ(type_to_string(make_int_type(16, false)), "Uint16");
  EXPECT_EQ(type_to_string(make_int_type(32, false)), "Uint32");
  EXPECT_EQ(type_to_string(make_int_type(64, false)), "Uint64");
}

TEST(Types, SizedFloatTypeToString) {
  EXPECT_EQ(type_to_string(make_float_type(32)), "Float32");
  EXPECT_EQ(type_to_string(make_float_type(64)), "Float64");
}

TEST(Types, SizedIntEquality) {
  // Same size and signedness are equal.
  EXPECT_TRUE(types_equal(make_int_type(32, true), make_int_type(32, true)));
  // Different size are not.
  EXPECT_FALSE(types_equal(make_int_type(32, true), make_int_type(64, true)));
  // Different signedness are not.
  EXPECT_FALSE(types_equal(make_int_type(32, true), make_int_type(32, false)));
  // Platform Int is distinct from Int32/Int64.
  EXPECT_FALSE(types_equal(make_int_type(), make_int_type(64, true)));
}

TEST(Types, SizedFloatEquality) {
  EXPECT_TRUE(types_equal(make_float_type(64), make_float_type(64)));
  EXPECT_FALSE(types_equal(make_float_type(32), make_float_type(64)));
  EXPECT_FALSE(types_equal(make_float_type(), make_float_type(64)));
}

// ===========================================================================
// Equality
// ===========================================================================

TEST(Types, EqualityPrimitives) {
  EXPECT_TRUE(types_equal(make_int_type(), make_int_type()));
  EXPECT_FALSE(types_equal(make_int_type(), make_float_type()));
}

TEST(Types, EqualityPointerIdentity) {
  auto t = make_int_type();
  EXPECT_TRUE(types_equal(t, t));
}

TEST(Types, EqualityArrays) {
  auto a = make_array_type(make_int_type());
  auto b = make_array_type(make_int_type());
  auto c = make_array_type(make_string_type());
  EXPECT_TRUE(types_equal(a, b));
  EXPECT_FALSE(types_equal(a, c));
}

TEST(Types, EqualityErrorPropagates) {
  EXPECT_TRUE(types_equal(make_error_type(), make_error_type()));
}

TEST(Types, EqualityUnionOrderIndependent) {
  auto u1 = make_union_type({make_int_type(), make_string_type()});
  auto u2 = make_union_type({make_string_type(), make_int_type()});
  EXPECT_TRUE(types_equal(u1, u2));
}

// ===========================================================================
// Assignability
// ===========================================================================

TEST(Types, AssignableExact) {
  EXPECT_TRUE(is_assignable_to(make_int_type(), make_int_type()));
}

TEST(Types, AssignableIntToFloat) {
  EXPECT_TRUE(is_assignable_to(make_int_type(), make_float_type()));
  EXPECT_FALSE(is_assignable_to(make_float_type(), make_int_type()));
}

TEST(Types, AssignableToUnion) {
  auto u = make_union_type({make_int_type(), make_string_type()});
  EXPECT_TRUE(is_assignable_to(make_int_type(), u));
  EXPECT_TRUE(is_assignable_to(make_string_type(), u));
  EXPECT_FALSE(is_assignable_to(make_bool_type(), u));
}

TEST(Types, AssignableErrorPropagates) {
  EXPECT_TRUE(is_assignable_to(make_error_type(), make_int_type()));
  EXPECT_TRUE(is_assignable_to(make_int_type(), make_error_type()));
}

// ===========================================================================
// common_type
// ===========================================================================

TEST(Types, CommonTypeSame) {
  auto t = common_type(make_int_type(), make_int_type());
  EXPECT_TRUE(types_equal(t, make_int_type()));
}

TEST(Types, CommonTypeIntFloat) {
  auto t = common_type(make_int_type(), make_float_type());
  EXPECT_EQ(t->kind, TypeKind::Float);
}

TEST(Types, CommonTypeDifferentBecomesUnion) {
  auto t = common_type(make_int_type(), make_string_type());
  EXPECT_EQ(t->kind, TypeKind::Union);
}

// ===========================================================================
// Substitution
// ===========================================================================

TEST(Types, SubstituteTypeParam) {
  auto tp = make_type_param(42, "T");
  std::unordered_map<uint32_t, TypePtr> bindings{{42, make_int_type()}};
  auto result = substitute(tp, bindings);
  EXPECT_EQ(result->kind, TypeKind::Int);
}

TEST(Types, SubstituteArray) {
  auto tp = make_type_param(1, "T");
  auto arr = make_array_type(tp);
  std::unordered_map<uint32_t, TypePtr> bindings{{1, make_string_type()}};
  auto result = substitute(arr, bindings);
  EXPECT_EQ(result->kind, TypeKind::Array);
  auto &info = std::get<ArrayTypeInfo>(result->detail);
  EXPECT_EQ(info.element->kind, TypeKind::String);
}

TEST(Types, SubstituteNoChange) {
  auto arr = make_array_type(make_int_type());
  std::unordered_map<uint32_t, TypePtr> bindings{{99, make_string_type()}};
  auto result = substitute(arr, bindings);
  EXPECT_EQ(result.get(), arr.get()); // same pointer — no copy
}

// ===========================================================================
// Unification
// ===========================================================================

TEST(Types, UnifySimple) {
  auto tp = make_type_param(0, "T");
  auto concrete = make_int_type();
  std::unordered_map<uint32_t, TypePtr> out;
  EXPECT_TRUE(unify(tp, concrete, out));
  EXPECT_EQ(out.size(), 1u);
  EXPECT_TRUE(types_equal(out[0], make_int_type()));
}

TEST(Types, UnifyArray) {
  auto tp = make_type_param(0, "T");
  auto param_type = make_array_type(tp);
  auto arg_type = make_array_type(make_string_type());
  std::unordered_map<uint32_t, TypePtr> out;
  EXPECT_TRUE(unify(param_type, arg_type, out));
  EXPECT_TRUE(types_equal(out[0], make_string_type()));
}

TEST(Types, UnifyConflict) {
  auto tp = make_type_param(0, "T");
  auto param_type = make_func_type({tp, tp}, {});
  auto arg_type = make_func_type({make_int_type(), make_string_type()}, {});
  std::unordered_map<uint32_t, TypePtr> out;
  EXPECT_FALSE(unify(param_type, arg_type, out));
}

} // namespace saga
