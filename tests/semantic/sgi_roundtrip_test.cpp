// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// SGI v2 round-trip tests. Covers the behaviors introduced in P2:
//   - Header bump to `sgi 2` (hard break on v1 is tested in test_sgi.cpp).
//   - `@origin "pkg"` annotations populate `origin_package` on nominal types.
//   - Generic type parameters serialize with stable `|T#id|` IDs and
//     reconstruct as proper TypeParam nodes (not Struct "T" stubs).
//   - Method-local generics (on non-generic structs, on interfaces, and on
//     free functions) round-trip.
//   - Every TypeInfo shape survives write → parse → module-type construction.

#include "semantic/sgi.hpp"
#include "semantic/types.hpp"

#include <gtest/gtest.h>

namespace saga {

namespace {

// Write then parse in one shot; asserts parsing succeeded.
SgiFile roundtrip(const std::string &pkg,
                  const std::vector<SgiExport> &exports,
                  const std::vector<SgiImport> &imports = {},
                  const std::vector<SgiReceiverMethod> &rm = {}) {
  auto text = generate_sgi(pkg, imports, exports, rm);
  auto parsed = parse_sgi(text);
  EXPECT_TRUE(parsed.has_value()) << "failed to parse:\n" << text;
  return parsed.value_or(SgiFile{});
}

} // namespace

// ---------------------------------------------------------------------------
// Header / version
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, HeaderIsV2) {
  auto text = generate_sgi("pkg", {}, {});
  EXPECT_NE(text.find("sgi 2"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Origin round-trip
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, StructOriginDefaultsToFilePackage) {
  auto s = make_struct_type("Point", {FieldInfo{"x", make_int_type(), true}});
  auto parsed = roundtrip("geo", {{"", "Point", s, true}});
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &info = std::get<StructTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(info.origin_package, "geo");
}

TEST(SgiRoundtrip, StructOriginExplicitOverridesFilePackage) {
  auto s = make_struct_type("Token", {}, {}, {}, "lexer");
  std::vector<SgiExport> exports;
  exports.push_back({"", "Token", s, true, "lexer"});
  auto parsed = roundtrip("parser", exports);
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &info = std::get<StructTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(info.origin_package, "lexer");
}

TEST(SgiRoundtrip, EnumOriginDefaultsToFilePackage) {
  auto e = make_enum_type("Color", {EnumVariant{"Red", {}},
                                      EnumVariant{"Blue", {}}});
  auto parsed = roundtrip("palette", {{"", "Color", e, true}});
  auto &info = std::get<EnumTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(info.origin_package, "palette");
}

TEST(SgiRoundtrip, InterfaceOriginDefaultsToFilePackage) {
  auto i = make_interface_type(
      "Readable",
      {MethodInfo{"Read", make_func_type({}, {make_int_type()}), true, ""}});
  auto parsed = roundtrip("io", {{"", "Readable", i, true}});
  auto &info = std::get<InterfaceTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(info.origin_package, "io");
}

// ---------------------------------------------------------------------------
// Generic type-param round-trip with stable IDs
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, GenericStructParamIdsSurvive) {
  auto s = make_struct_type(
      "Box",
      {FieldInfo{"value", make_type_param(42, "T"), true}},
      {},
      {TypeParam{42, "T"}});
  auto text = generate_sgi("pkg", {}, {{"", "Box", s, true}});
  EXPECT_NE(text.find("|T#42|"), std::string::npos);

  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  auto &st = std::get<StructTypeInfo>(parsed->exports[0].type->detail);
  ASSERT_EQ(st.type_params.size(), 1u);
  EXPECT_EQ(st.type_params[0].id, 42u);
  ASSERT_EQ(st.fields.size(), 1u);
  ASSERT_EQ(st.fields[0].type->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(st.fields[0].type->detail).param.id, 42u);
}

TEST(SgiRoundtrip, GenericInterfaceParamIdsSurvive) {
  auto i = make_interface_type(
      "Seq",
      {MethodInfo{"At", make_func_type({make_int_type()},
                                         {make_type_param(7, "T")}),
                   true, ""}},
      {TypeParam{7, "T"}});
  auto parsed = roundtrip("col", {{"", "Seq", i, true}});
  auto &info = std::get<InterfaceTypeInfo>(parsed.exports[0].type->detail);
  ASSERT_EQ(info.type_params.size(), 1u);
  EXPECT_EQ(info.type_params[0].id, 7u);
  ASSERT_EQ(info.methods.size(), 1u);
  auto &sig = std::get<FuncTypeInfo>(info.methods[0].signature->detail);
  ASSERT_EQ(sig.returns.size(), 1u);
  ASSERT_EQ(sig.returns[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(sig.returns[0]->detail).param.id, 7u);
}

// ---------------------------------------------------------------------------
// Method-local generics on a non-generic struct (the testing.AssertEqual
// shape: pub fn |T| AssertEqual(T, T, String))
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, NonGenericStructWithGenericMethod) {
  auto tparam = make_type_param(17, "T");
  auto sig = make_func_type({tparam, tparam, make_string_type()},
                             {make_void_type()});
  auto s = make_struct_type(
      "Case", {}, {MethodInfo{"AssertEqual", sig, true, ""}});
  auto text = generate_sgi("testing", {}, {{"", "Case", s, true}});
  EXPECT_NE(text.find("pub fn |T#17| AssertEqual(T, T, String)"),
            std::string::npos);

  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  auto &st = std::get<StructTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_TRUE(st.type_params.empty());
  ASSERT_EQ(st.methods.size(), 1u);
  auto &msig = std::get<FuncTypeInfo>(st.methods[0].signature->detail);
  ASSERT_EQ(msig.params.size(), 3u);
  EXPECT_EQ(msig.params[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(msig.params[1]->kind, TypeKind::TypeParam);
  EXPECT_EQ(msig.params[2]->kind, TypeKind::String);
  EXPECT_EQ(std::get<TypeParamInfo>(msig.params[0]->detail).param.id, 17u);
  EXPECT_EQ(std::get<TypeParamInfo>(msig.params[1]->detail).param.id, 17u);
}

TEST(SgiRoundtrip, GenericStructNonGenericMethodOnlyDeclaresOuterParam) {
  // `struct |T#3| Box` + `pub fn Get() T` — the method should NOT declare T
  // itself; T is inherited from the struct's params.
  auto tparam = make_type_param(3, "T");
  auto get_sig = make_func_type({}, {tparam});
  auto s = make_struct_type(
      "Box", {FieldInfo{"value", tparam, true}},
      {MethodInfo{"Get", get_sig, true, ""}}, {TypeParam{3, "T"}});
  auto text = generate_sgi("con", {}, {{"", "Box", s, true}});
  EXPECT_NE(text.find("struct |T#3| Box"), std::string::npos);
  // The method line should not re-declare the struct's T.
  EXPECT_EQ(text.find("pub fn |T"), std::string::npos);

  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  auto &st = std::get<StructTypeInfo>(parsed->exports[0].type->detail);
  ASSERT_EQ(st.type_params.size(), 1u);
  EXPECT_EQ(st.type_params[0].id, 3u);
  ASSERT_EQ(st.methods.size(), 1u);
  auto &msig = std::get<FuncTypeInfo>(st.methods[0].signature->detail);
  ASSERT_EQ(msig.returns.size(), 1u);
  ASSERT_EQ(msig.returns[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(msig.returns[0]->detail).param.id, 3u);
}

// ---------------------------------------------------------------------------
// Generic free functions
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, GenericFreeFunction) {
  auto tparam = make_type_param(5, "T");
  auto fn = make_func_type({tparam, tparam}, {tparam});
  auto text = generate_sgi("util", {}, {{"", "Max", fn}});
  EXPECT_NE(text.find("func |T#5| Max(T, T) T"), std::string::npos);

  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->exports.size(), 1u);
  auto &fi = std::get<FuncTypeInfo>(parsed->exports[0].type->detail);
  ASSERT_EQ(fi.params.size(), 2u);
  EXPECT_EQ(fi.params[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(fi.params[0]->detail).param.id, 5u);
  // Both T references bind to the same TypeParam id, so they unify.
  EXPECT_EQ(std::get<TypeParamInfo>(fi.params[0]->detail).param.id,
            std::get<TypeParamInfo>(fi.params[1]->detail).param.id);
}

// ---------------------------------------------------------------------------
// Remaining TypeInfo shapes
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, FuncVariadic) {
  auto fn = make_func_type({make_string_type()}, {make_void_type()}, true);
  auto parsed = roundtrip("pkg", {{"", "Log", fn}});
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &fi = std::get<FuncTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_TRUE(fi.is_variadic);
}

TEST(SgiRoundtrip, EnumWithFields) {
  auto e = make_enum_type(
      "Shape",
      {EnumVariant{"Circle", {FieldInfo{"r", make_float_type(), false}}},
       EnumVariant{"Rect",
                   {FieldInfo{"w", make_float_type(), false},
                    FieldInfo{"h", make_float_type(), false}}}});
  auto parsed = roundtrip("geo", {{"", "Shape", e, true}});
  auto &info = std::get<EnumTypeInfo>(parsed.exports[0].type->detail);
  ASSERT_EQ(info.variants.size(), 2u);
  EXPECT_EQ(info.variants[0].fields.size(), 1u);
  EXPECT_EQ(info.variants[1].fields.size(), 2u);
}

TEST(SgiRoundtrip, ConstScalarAndComposite) {
  auto map_t = make_map_type(make_string_type(), make_int_type());
  auto arr_t = make_array_type(make_float_type());
  std::vector<SgiExport> exports = {
      {"", "Pi", make_float_type()},
      {"", "Primes", arr_t},
      {"", "Ports", map_t},
      {"", "Greeting", make_string_type()},
  };
  auto parsed = roundtrip("consts", exports);
  ASSERT_EQ(parsed.exports.size(), 4u);
  EXPECT_EQ(parsed.exports[0].type->kind, TypeKind::Float);
  EXPECT_EQ(parsed.exports[1].type->kind, TypeKind::Array);
  EXPECT_EQ(parsed.exports[2].type->kind, TypeKind::Map);
  EXPECT_EQ(parsed.exports[3].type->kind, TypeKind::String);
}

// ---------------------------------------------------------------------------
// sgi_to_module_type carries origin through
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, ModuleTypeExposesStructWithOrigin) {
  auto point = make_struct_type(
      "Point", {FieldInfo{"x", make_int_type(), true}});
  auto parsed = roundtrip("geo", {{"", "Point", point, true}});
  auto mod = sgi_to_module_type(parsed, "geo");
  ASSERT_NE(mod, nullptr);
  auto &mi = std::get<ModuleTypeInfo>(mod->detail);
  ASSERT_EQ(mi.exports.size(), 1u);
  auto &si = std::get<StructTypeInfo>(mi.exports[0].type->detail);
  EXPECT_EQ(si.origin_package, "geo");
}

// ---------------------------------------------------------------------------
// MethodInfo.origin_package survives SGI round-trip
// ---------------------------------------------------------------------------
// P5 required that selector resolution preserve the method's origin_package
// all the way to codegen so cross-package method calls mangle against the
// defining package's symbol table. Verify that SGI parsing populates the
// field for every reconstructed method, both on direct struct exports and
// on interfaces.

TEST(SgiRoundtrip, StructMethodOriginMatchesType) {
  auto sig = make_func_type({}, {make_string_type()});
  auto s = make_struct_type(
      "Counter",
      {FieldInfo{"value", make_int_type(), true}},
      {MethodInfo{"Name", sig, true, ""}});
  auto parsed = roundtrip("lib", {{"", "Counter", s, true}});
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &si = std::get<StructTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(si.origin_package, "lib");
  ASSERT_EQ(si.methods.size(), 1u);
  EXPECT_EQ(si.methods[0].origin_package, "lib")
      << "method origin must mirror the enclosing struct so that "
         "cross-package method calls mangle to lib__Counter__Name";
}

TEST(SgiRoundtrip, InterfaceMethodOriginMatchesType) {
  auto sig = make_func_type({}, {make_string_type()});
  auto iface = make_interface_type(
      "Drawable", {MethodInfo{"Draw", sig, true, ""}});
  auto parsed = roundtrip("ui", {{"", "Drawable", iface, true}});
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &ii = std::get<InterfaceTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(ii.origin_package, "ui");
  ASSERT_EQ(ii.methods.size(), 1u);
  EXPECT_EQ(ii.methods[0].origin_package, "ui");
}

TEST(SgiRoundtrip, ExplicitOriginOverridesFilePackageForMethods) {
  auto sig = make_func_type({}, {make_int_type()});
  auto s = make_struct_type(
      "Box",
      {FieldInfo{"value", make_int_type(), true}},
      {MethodInfo{"Get", sig, true, ""}});
  // SgiExport fields: { doc, name, type, is_type, origin_path } — the
  // origin_path is the trailing field, not the first.
  SgiExport exp;
  exp.name = "Box";
  exp.type = s;
  exp.is_type = true;
  exp.origin_path = "orig";
  auto parsed = roundtrip("re_export", {exp});
  ASSERT_EQ(parsed.exports.size(), 1u);
  auto &si = std::get<StructTypeInfo>(parsed.exports[0].type->detail);
  EXPECT_EQ(si.origin_package, "orig");
  ASSERT_EQ(si.methods.size(), 1u);
  EXPECT_EQ(si.methods[0].origin_package, "orig")
      << "@origin override must apply to methods so re-exports still mangle "
         "to the defining package";
}

// ---------------------------------------------------------------------------
// Receiver methods preserve T→9990, K/V→9991/9992 for generic intrinsics
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, ArrayReceiverMethodsUseSentinelT) {
  auto push_sig = make_func_type({make_type_param(9990, "T")},
                                   {make_type_param(9990, "T")});
  SgiReceiverMethod rm{"Array", {{"Push", push_sig, true, ""}}};
  auto text = generate_sgi("array", {}, {}, {rm});
  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->receiver_methods.size(), 1u);
  auto &methods = parsed->receiver_methods[0].methods;
  ASSERT_EQ(methods.size(), 1u);
  auto &sig = std::get<FuncTypeInfo>(methods[0].signature->detail);
  ASSERT_EQ(sig.params[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(sig.params[0]->detail).param.id, 9990u);
  ASSERT_EQ(sig.returns[0]->kind, TypeKind::TypeParam);
  EXPECT_EQ(std::get<TypeParamInfo>(sig.returns[0]->detail).param.id, 9990u);
}

// ---------------------------------------------------------------------------
// P5: SGI parser rejects type-param shadowing in nested scopes (mirror of the
// analyzer check)
// ---------------------------------------------------------------------------

TEST(SgiRoundtrip, RejectsMethodTypeParamShadowingStructTypeParam) {
  // A method declared inside a generic struct must not redeclare a name
  // that's already bound in the enclosing struct's type-param list.
  std::string text =
      "sgi 2\n"
      "package box\n"
      "\n"
      "struct |T#0| Box {\n"
      "  pub fn |T#1| Bad() T\n"
      "}\n";
  auto parsed = parse_sgi(text);
  EXPECT_FALSE(parsed.has_value());
}

TEST(SgiRoundtrip, AllowsMethodTypeParamDistinctFromStructTypeParam) {
  std::string text =
      "sgi 2\n"
      "package box\n"
      "\n"
      "struct |T#0| Box {\n"
      "  pub fn |U#1| Convert() U\n"
      "}\n";
  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
}

TEST(SgiRoundtrip, MapReceiverMethodsUseSentinelKV) {
  auto set_sig = make_func_type({make_type_param(9991, "K"),
                                   make_type_param(9992, "V")},
                                  {make_void_type()});
  SgiReceiverMethod rm{"Map", {{"Set", set_sig, true, ""}}};
  auto text = generate_sgi("map", {}, {}, {rm});
  auto parsed = parse_sgi(text);
  ASSERT_TRUE(parsed.has_value());
  auto &methods = parsed->receiver_methods[0].methods;
  auto &sig = std::get<FuncTypeInfo>(methods[0].signature->detail);
  EXPECT_EQ(std::get<TypeParamInfo>(sig.params[0]->detail).param.id, 9991u);
  EXPECT_EQ(std::get<TypeParamInfo>(sig.params[1]->detail).param.id, 9992u);
}

} // namespace saga
