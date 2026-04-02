// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "semantic/sgi.hpp"
#include "semantic/types.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace mc {

// ===========================================================================
// Type serialization round-trip
// ===========================================================================

TEST(Sgi, TypeToSgiPrimitives) {
  EXPECT_EQ(type_to_sgi(make_void_type()), "Void");
  EXPECT_EQ(type_to_sgi(make_bool_type()), "Bool");
  EXPECT_EQ(type_to_sgi(make_int_type()), "Int");
  EXPECT_EQ(type_to_sgi(make_int_type(8, true)), "Int8");
  EXPECT_EQ(type_to_sgi(make_int_type(64, true)), "Int64");
  EXPECT_EQ(type_to_sgi(make_int_type(0, false)), "Byte");
  EXPECT_EQ(type_to_sgi(make_int_type(16, false)), "Uint16");
  EXPECT_EQ(type_to_sgi(make_float_type()), "Float");
  EXPECT_EQ(type_to_sgi(make_float_type(32)), "Float32");
  EXPECT_EQ(type_to_sgi(make_float_type(64)), "Float64");
  EXPECT_EQ(type_to_sgi(make_string_type()), "String");
}

TEST(Sgi, TypeToSgiArray) {
  auto t = make_array_type(make_int_type());
  EXPECT_EQ(type_to_sgi(t), "[Int]");
}

TEST(Sgi, TypeToSgiMap) {
  auto t = make_map_type(make_string_type(), make_int_type());
  EXPECT_EQ(type_to_sgi(t), "{String: Int}");
}

TEST(Sgi, TypeToSgiFunc) {
  auto t = make_func_type({make_int_type(), make_string_type()},
                           {make_bool_type()});
  EXPECT_EQ(type_to_sgi(t), "fn(Int, String) Bool");
}

TEST(Sgi, TypeToSgiFuncVoid) {
  auto t = make_func_type({make_string_type()}, {make_void_type()});
  EXPECT_EQ(type_to_sgi(t), "fn(String) Void");
}

TEST(Sgi, TypeToSgiFuncMultiReturn) {
  auto t = make_func_type({}, {make_int_type(), make_string_type()});
  EXPECT_EQ(type_to_sgi(t), "fn() (Int, String)");
}

TEST(Sgi, TypeToSgiFuncVariadic) {
  auto t = make_func_type({make_string_type()}, {make_void_type()}, true);
  EXPECT_EQ(type_to_sgi(t), "fn(...String) Void");
}

TEST(Sgi, TypeToSgiUnion) {
  auto t = make_union_type({make_int_type(), make_string_type()});
  EXPECT_EQ(type_to_sgi(t), "Int | String");
}

TEST(Sgi, TypeToSgiStruct) {
  auto t = make_struct_type("Point");
  EXPECT_EQ(type_to_sgi(t), "Point");
}

// ===========================================================================
// Type parse round-trip
// ===========================================================================

TEST(Sgi, SgiToTypePrimitives) {
  EXPECT_EQ(sgi_to_type("Void")->kind, TypeKind::Void);
  EXPECT_EQ(sgi_to_type("Bool")->kind, TypeKind::Bool);
  EXPECT_EQ(sgi_to_type("Int")->kind, TypeKind::Int);
  EXPECT_EQ(sgi_to_type("String")->kind, TypeKind::String);
  EXPECT_EQ(sgi_to_type("Float")->kind, TypeKind::Float);
  EXPECT_EQ(sgi_to_type("Byte")->kind, TypeKind::Int);

  auto byte = sgi_to_type("Byte");
  auto &bi = std::get<IntType>(byte->detail);
  EXPECT_FALSE(bi.is_signed);
}

TEST(Sgi, SgiToTypeArray) {
  auto t = sgi_to_type("[Int]");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Array);
  auto &info = std::get<ArrayTypeInfo>(t->detail);
  EXPECT_EQ(info.element->kind, TypeKind::Int);
}

TEST(Sgi, SgiToTypeMap) {
  auto t = sgi_to_type("{String: Int}");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Map);
  auto &info = std::get<MapTypeInfo>(t->detail);
  EXPECT_EQ(info.key->kind, TypeKind::String);
  EXPECT_EQ(info.value->kind, TypeKind::Int);
}

TEST(Sgi, SgiToTypeFunc) {
  auto t = sgi_to_type("fn(Int, String) Bool");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Func);
  auto &info = std::get<FuncTypeInfo>(t->detail);
  EXPECT_EQ(info.params.size(), 2u);
  EXPECT_EQ(info.params[0]->kind, TypeKind::Int);
  EXPECT_EQ(info.params[1]->kind, TypeKind::String);
  EXPECT_EQ(info.returns.size(), 1u);
  EXPECT_EQ(info.returns[0]->kind, TypeKind::Bool);
}

// ===========================================================================
// Full .sgi generation and parsing round-trip
// ===========================================================================

TEST(Sgi, GenerateAndParseFuncExport) {
  std::vector<SgiExport> exports = {
      {"Writes a greeting.",
       "Greet",
       make_func_type({make_string_type()}, {make_void_type()})},
  };

  std::string sgi = generate_sgi("hello", {}, exports);

  // Verify the generated text looks right
  EXPECT_NE(sgi.find("sgi 1"), std::string::npos);
  EXPECT_NE(sgi.find("package hello"), std::string::npos);
  EXPECT_NE(sgi.find("// Writes a greeting."), std::string::npos);
  EXPECT_NE(sgi.find("func Greet(String)"), std::string::npos);

  // Parse it back
  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->version, 1);
  EXPECT_EQ(parsed->package_name, "hello");
  ASSERT_EQ(parsed->exports.size(), 1u);
  EXPECT_EQ(parsed->exports[0].name, "Greet");
  EXPECT_EQ(parsed->exports[0].doc, "Writes a greeting.");
  ASSERT_NE(parsed->exports[0].type, nullptr);
  EXPECT_EQ(parsed->exports[0].type->kind, TypeKind::Func);
}

TEST(Sgi, GenerateAndParseStructExport) {
  auto point_type = make_struct_type(
      "Point",
      {FieldInfo{"x", make_int_type(), true},
       FieldInfo{"y", make_int_type(), true}},
      {MethodInfo{"Translate", make_func_type({make_int_type(), make_int_type()},
                                               {make_void_type()}), true}});

  std::vector<SgiExport> exports = {
      {"A 2D point.", "Point", point_type, true},
  };

  std::string sgi = generate_sgi("geo", {}, exports);

  EXPECT_NE(sgi.find("struct Point"), std::string::npos);
  EXPECT_NE(sgi.find("pub x Int"), std::string::npos);
  EXPECT_NE(sgi.find("pub y Int"), std::string::npos);
  EXPECT_NE(sgi.find("pub fn Translate(Int, Int)"), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->exports.size(), 1u);
  EXPECT_EQ(parsed->exports[0].name, "Point");

  auto &st = std::get<StructTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_EQ(st.fields.size(), 2u);
  EXPECT_EQ(st.fields[0].name, "x");
  EXPECT_EQ(st.fields[0].is_public, true);
  EXPECT_EQ(st.methods.size(), 1u);
  EXPECT_EQ(st.methods[0].name, "Translate");
}

TEST(Sgi, GenerateAndParseEnumExport) {
  auto color_type = make_enum_type("Color", {
      EnumVariant{"Red", {}},
      EnumVariant{"Green", {}},
      EnumVariant{"Blue", {}},
  });

  std::vector<SgiExport> exports = {
      {"", "Color", color_type, true},
  };

  std::string sgi = generate_sgi("draw", {}, exports);

  EXPECT_NE(sgi.find("enum Color"), std::string::npos);
  EXPECT_NE(sgi.find("Red"), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->exports.size(), 1u);
  auto &et = std::get<EnumTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_EQ(et.variants.size(), 3u);
  EXPECT_EQ(et.variants[0].name, "Red");
  EXPECT_EQ(et.variants[1].name, "Green");
  EXPECT_EQ(et.variants[2].name, "Blue");
}

TEST(Sgi, GenerateAndParseEnumWithFields) {
  auto shape_type = make_enum_type("Shape", {
      EnumVariant{"Circle", {FieldInfo{"radius", make_float_type(), false}}},
      EnumVariant{"Rect", {FieldInfo{"w", make_float_type(), false},
                           FieldInfo{"h", make_float_type(), false}}},
  });

  std::vector<SgiExport> exports = {{"", "Shape", shape_type, true}};
  std::string sgi = generate_sgi("geo", {}, exports);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  auto &et = std::get<EnumTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_EQ(et.variants[0].name, "Circle");
  EXPECT_EQ(et.variants[0].fields.size(), 1u);
  EXPECT_EQ(et.variants[1].name, "Rect");
  EXPECT_EQ(et.variants[1].fields.size(), 2u);
}

TEST(Sgi, GenerateAndParseInterfaceExport) {
  auto iface = make_interface_type("Readable", {
      MethodInfo{"Read", make_func_type({make_array_type(make_int_type(0, false))},
                                         {make_int_type()}), true},
  });

  std::vector<SgiExport> exports = {{"", "Readable", iface, true}};
  std::string sgi = generate_sgi("io", {}, exports);

  EXPECT_NE(sgi.find("interface Readable"), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->exports.size(), 1u);
  auto &it = std::get<InterfaceTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_EQ(it.methods.size(), 1u);
  EXPECT_EQ(it.methods[0].name, "Read");
}

TEST(Sgi, GenerateAndParseConstExport) {
  std::vector<SgiExport> exports = {
      {"Maximum buffer size.", "MaxBuffer", make_int_type()},
  };

  std::string sgi = generate_sgi("io", {}, exports);
  EXPECT_NE(sgi.find("const MaxBuffer Int"), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->exports.size(), 1u);
  EXPECT_EQ(parsed->exports[0].name, "MaxBuffer");
  EXPECT_EQ(parsed->exports[0].type->kind, TypeKind::Int);
}

TEST(Sgi, GenerateAndParseImports) {
  std::vector<SgiImport> imports = {
      {"os", "std/os"},
      {"sys", "std/sys"},
  };
  std::vector<SgiExport> exports = {
      {"", "Println", make_func_type({make_string_type()}, {make_void_type()})},
  };

  std::string sgi = generate_sgi("io", imports, exports);
  EXPECT_NE(sgi.find("import os \"std/os\""), std::string::npos);
  EXPECT_NE(sgi.find("import sys \"std/sys\""), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->imports.size(), 2u);
  EXPECT_EQ(parsed->imports[0].name, "os");
  EXPECT_EQ(parsed->imports[0].import_path, "std/os");
}

TEST(Sgi, GenerateAndParseMixedExports) {
  // Multiple export types in one file
  std::vector<SgiExport> exports = {
      {"A 2D point.", "Point",
       make_struct_type("Point",
                        {FieldInfo{"x", make_int_type(), true},
                         FieldInfo{"y", make_int_type(), true}}), true},
      {"Directions.", "Dir",
       make_enum_type("Dir", {EnumVariant{"Up", {}},
                              EnumVariant{"Down", {}}}), true},
      {"Create a point.", "NewPoint",
       make_func_type({make_int_type(), make_int_type()},
                       {make_struct_type("Point")})},
      {"", "Version", make_string_type()},
  };

  std::string sgi = generate_sgi("geo", {}, exports);
  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  // Sorted: struct, enum, func, const
  EXPECT_EQ(parsed->exports.size(), 4u);
  EXPECT_EQ(parsed->exports[0].name, "Point");
  EXPECT_EQ(parsed->exports[1].name, "Dir");
  EXPECT_EQ(parsed->exports[2].name, "NewPoint");
  EXPECT_EQ(parsed->exports[3].name, "Version");
}

// ===========================================================================
// sgi_to_module_type
// ===========================================================================

TEST(Sgi, SgiToModuleType) {
  SgiFile sgi;
  sgi.version = 1;
  sgi.package_name = "math";
  sgi.exports = {
      {"", "Add", make_func_type({make_int_type(), make_int_type()},
                                  {make_int_type()})},
      {"", "Pi", make_float_type()},
  };

  auto mod = sgi_to_module_type(sgi, "std/math");
  ASSERT_NE(mod, nullptr);
  EXPECT_EQ(mod->kind, TypeKind::Module);
  auto &info = std::get<ModuleTypeInfo>(mod->detail);
  EXPECT_EQ(info.name, "math");
  EXPECT_EQ(info.import_path, "std/math");
  EXPECT_EQ(info.exports.size(), 2u);
}

// ===========================================================================
// File I/O
// ===========================================================================

TEST(Sgi, WriteAndLoadFile) {
  auto tmp = std::filesystem::temp_directory_path() / "test_pkg.sgi";
  std::string path = tmp.string();

  std::vector<SgiExport> exports = {
      {"Do something.", "DoIt",
       make_func_type({make_int_type()}, {make_bool_type()})},
  };

  ASSERT_TRUE(write_sgi(path, "test_pkg", {}, exports));

  auto loaded = load_sgi(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->package_name, "test_pkg");
  EXPECT_EQ(loaded->exports.size(), 1u);
  EXPECT_EQ(loaded->exports[0].name, "DoIt");
  EXPECT_EQ(loaded->exports[0].doc, "Do something.");

  std::filesystem::remove(tmp);
}

// ===========================================================================
// Generic struct round-trip
// ===========================================================================

TEST(Sgi, GenericStructRoundTrip) {
  auto t = make_struct_type("List",
                             {FieldInfo{"items", make_array_type(make_int_type()), true}},
                             {},
                             {TypeParam{0, "T"}});

  std::vector<SgiExport> exports = {{"A generic list.", "List", t, true}};
  std::string sgi = generate_sgi("collections", {}, exports);

  EXPECT_NE(sgi.find("struct List|T|"), std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  auto &st = std::get<StructTypeInfo>(parsed->exports[0].type->detail);
  EXPECT_EQ(st.name, "List");
  EXPECT_EQ(st.type_params.size(), 1u);
  EXPECT_EQ(st.type_params[0].name, "T");
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST(Sgi, EmptyPackage) {
  std::string sgi = generate_sgi("empty", {}, {});
  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->package_name, "empty");
  EXPECT_TRUE(parsed->exports.empty());
}

TEST(Sgi, InvalidSgiMagic) {
  auto parsed = parse_sgi("not_sgi 1\npackage foo\n");
  EXPECT_FALSE(parsed.has_value());
}

TEST(Sgi, InvalidSgiVersion) {
  auto parsed = parse_sgi("sgi 99\npackage foo\n");
  EXPECT_FALSE(parsed.has_value());
}

TEST(Sgi, MultiLineDocComment) {
  std::vector<SgiExport> exports = {
      {"First line.\nSecond line.\nThird line.", "Foo",
       make_func_type({}, {make_void_type()})},
  };

  std::string sgi = generate_sgi("pkg", {}, exports);
  EXPECT_NE(sgi.find("// First line.\n// Second line.\n// Third line."),
            std::string::npos);

  auto parsed = parse_sgi(sgi);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->exports[0].doc, "First line.\nSecond line.\nThird line.");
}

// ===========================================================================
// Integration: use .sgi file for import resolution
// ===========================================================================

TEST(Sgi, ResolveImportFromSgiFile) {
  // Create a temporary .sgi file
  auto tmp_dir = std::filesystem::temp_directory_path() / "saga_sgi_import_test";
  std::filesystem::create_directories(tmp_dir);

  std::vector<SgiExport> exports = {
      {"", "Add",
       make_func_type({make_int_type(), make_int_type()}, {make_int_type()})},
  };
  std::string sgi_path = (tmp_dir / "mathlib.sgi").string();
  ASSERT_TRUE(write_sgi(sgi_path, "mathlib", {}, exports));

  // Verify we can load and convert it
  auto loaded = load_sgi(sgi_path);
  ASSERT_TRUE(loaded.has_value());
  auto mod = sgi_to_module_type(*loaded, "mathlib");
  ASSERT_NE(mod, nullptr);

  auto &info = std::get<ModuleTypeInfo>(mod->detail);
  EXPECT_EQ(info.name, "mathlib");
  EXPECT_EQ(info.exports.size(), 1u);
  EXPECT_EQ(info.exports[0].name, "Add");

  std::filesystem::remove_all(tmp_dir);
}

// ===========================================================================
// End-to-end: analyzer resolves import via .sgi (no source files)
// ===========================================================================

TEST(Sgi, AnalyzerResolvesImportViaSgi) {
  // Create a .sgi file for a package that has no source code present.
  auto tmp_dir = std::filesystem::temp_directory_path() / "saga_sgi_e2e_test";
  std::filesystem::create_directories(tmp_dir);

  std::vector<SgiExport> exports = {
      {"", "Add",
       make_func_type({make_int_type(), make_int_type()}, {make_int_type()})},
  };
  ASSERT_TRUE(write_sgi((tmp_dir / "mathlib.sgi").string(), "mathlib", {},
                        exports));

  // Now compile a source file that imports "mathlib".
  FileSet fileset;
  auto file = File::from_source("test.sg", R"(
import "mathlib"

pub fn Main() Int {
  mathlib.Add(1, 2)
}
  )");
  fileset.add_file(std::move(file));

  Parser parser(fileset);
  auto ast = parser.parse();
  ASSERT_NE(ast, nullptr);
  EXPECT_TRUE(parser.errors.errors.empty());

  Analyzer analyzer(fileset);
  // Point the resolver at the directory containing the .sgi.
  analyzer.package_resolver->sgi_search_paths.push_back(tmp_dir.string());
  analyzer.analyze(*ast);

  // Should resolve successfully — no errors.
  EXPECT_TRUE(analyzer.errors.errors.empty())
      << "Errors:";
  for (auto &e : analyzer.errors.errors)
    std::cerr << "  " << e.message << "\n";

  std::filesystem::remove_all(tmp_dir);
}

TEST(Sgi, AnalyzerSgiTakesPriorityOverMissingSource) {
  // When a .sgi file exists, the analyzer should NOT try to find source.
  // This test imports a package for which no source directory exists,
  // only a .sgi file.
  auto tmp_dir = std::filesystem::temp_directory_path() / "saga_sgi_prio_test";
  std::filesystem::create_directories(tmp_dir);

  std::vector<SgiExport> sgi_exports = {
      {"", "Greet",
       make_func_type({make_string_type()}, {make_void_type()})},
  };
  ASSERT_TRUE(write_sgi((tmp_dir / "greeter.sgi").string(), "greeter", {},
                        sgi_exports));

  FileSet fileset;
  auto file = File::from_source("test.sg", R"(
import "greeter"

pub fn Main() Void {
  greeter.Greet("world")
}
  )");
  fileset.add_file(std::move(file));

  Parser parser(fileset);
  auto ast = parser.parse();
  ASSERT_NE(ast, nullptr);

  Analyzer analyzer(fileset);
  // Only sgi_search_paths — no source search paths for this package.
  analyzer.package_resolver->sgi_search_paths.push_back(tmp_dir.string());
  analyzer.analyze(*ast);

  EXPECT_TRUE(analyzer.errors.errors.empty())
      << "Errors:";
  for (auto &e : analyzer.errors.errors)
    std::cerr << "  " << e.message << "\n";

  std::filesystem::remove_all(tmp_dir);
}

} // namespace mc
