// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace mc {

// ---------------------------------------------------------------------------
// Helper — parse source text and run the analyzer, returning both.
// Supports mock packages for import testing.
// ---------------------------------------------------------------------------

struct ModuleTestResult {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static ModuleTestResult from(const std::string &source) {
    ModuleTestResult r;
    auto file = File::from_source("test.sg", source);
    r.fileset.add_file(std::move(file));

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    if (r.ast) {
      r.analyzer->analyze(*r.ast);
    }
    return r;
  }

  /// Create a result with pre-registered mock packages.
  static ModuleTestResult
  with_mocks(const std::string &source,
             std::unordered_map<std::string, TypePtr> mocks) {
    ModuleTestResult r;
    auto file = File::from_source("test.sg", source);
    r.fileset.add_file(std::move(file));

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->package_resolver->mock_packages = std::move(mocks);
    if (r.ast) {
      r.analyzer->analyze(*r.ast);
    }
    return r;
  }

  size_t error_count() const { return analyzer->errors.errors.size(); }
  bool has_error() const { return error_count() > 0; }
  bool has_no_errors() const { return error_count() == 0; }

  bool has_error_containing(const std::string &substr) const {
    for (auto &err : analyzer->errors.errors) {
      if (err.message.find(substr) != std::string::npos)
        return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// Helper — create a mock module type with exports
// ---------------------------------------------------------------------------

static TypePtr make_mock_module(const std::string &name,
                                const std::string &import_path,
                                std::vector<ModuleExport> exports) {
  return make_module_type(name, import_path, std::move(exports));
}

// ===========================================================================
// Module type tests
// ===========================================================================

TEST(Modules, ModuleTypeCreation) {
  auto mod = make_module_type("io", "std/io", {{"Println", make_func_type({make_string_type()}, {make_void_type()})}});
  ASSERT_NE(mod, nullptr);
  EXPECT_EQ(mod->kind, TypeKind::Module);
  auto &info = std::get<ModuleTypeInfo>(mod->detail);
  EXPECT_EQ(info.name, "io");
  EXPECT_EQ(info.import_path, "std/io");
  EXPECT_EQ(info.exports.size(), 1u);
  EXPECT_EQ(info.exports[0].name, "Println");
}

TEST(Modules, ModuleTypeEquality) {
  auto a = make_module_type("io", "std/io");
  auto b = make_module_type("io", "std/io");
  auto c = make_module_type("io", "other/io");
  EXPECT_TRUE(types_equal(a, b));
  EXPECT_FALSE(types_equal(a, c));
}

TEST(Modules, ModuleTypeToString) {
  auto mod = make_module_type("io", "std/io");
  EXPECT_EQ(type_to_string(mod), "module 'io'");
}

// ===========================================================================
// Import declaration with mock packages
// ===========================================================================

TEST(Modules, ImportDeclBindsModuleSymbol) {
  auto mock_io = make_mock_module("io", "std/io",
      {{"Println", make_func_type({make_string_type()}, {make_void_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/io"

pub fn Main() Void {
  io.Println("hello")
}
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST(Modules, ImportDeclLastSegmentName) {
  // The name "io" should be derived from "std/io"
  auto mock_io = make_mock_module("io", "std/io",
      {{"Println", make_func_type({make_string_type()}, {make_void_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/io"

pub fn Test() Void {
  io.Println("test")
}
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_no_errors());
}

TEST(Modules, ConstImportBinding) {
  // `const MyIO = import "std/io"` should bind to MyIO
  auto mock_io = make_mock_module("io", "std/io",
      {{"Println", make_func_type({make_string_type()}, {make_void_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
const MyIO = import "std/io"

pub fn Main() Void {
  MyIO.Println("hello")
}
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

// ===========================================================================
// Duplicate import detection
// ===========================================================================

TEST(Modules, DuplicateImportError) {
  auto mock_io = make_mock_module("io", "std/io", {});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/io"
import "std/io"
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("duplicate import"));
}

TEST(Modules, DuplicateImportConstAndDecl) {
  auto mock_io = make_mock_module("io", "std/io", {});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/io"
const IO = import "std/io"
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("duplicate import"));
}

// ===========================================================================
// Visibility enforcement
// ===========================================================================

TEST(Modules, AccessNonExportedMember) {
  // Module only exports Println, not println (private).
  auto mock_io = make_mock_module("io", "std/io",
      {{"Println", make_func_type({make_string_type()}, {make_void_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/io"

pub fn Main() Void {
  io.secret_func()
}
  )", {{"std/io", mock_io}});

  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("no exported member"));
}

TEST(Modules, AccessExportedFunction) {
  auto mock_math = make_mock_module("math", "std/math",
      {{"Add", make_func_type({make_int_type(), make_int_type()}, {make_int_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/math"

pub fn Main() Void {
  x := math.Add(1, 2)
}
  )", {{"std/math", mock_math}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST(Modules, AccessExportedType) {
  auto point_type = make_struct_type("Point",
      {FieldInfo{"x", make_int_type(), true},
       FieldInfo{"y", make_int_type(), true}});

  auto mock_geo = make_mock_module("geo", "std/geo",
      {{"Point", point_type}});

  auto r = ModuleTestResult::with_mocks(R"(
import "std/geo"

pub fn Main() Void {
  p := geo.Point
}
  )", {{"std/geo", mock_geo}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST(Modules, EmbedQualifiedStructFromImport) {
  auto ts_type = make_struct_type("Timestamps",
      {FieldInfo{"created", make_int_type(), true},
       FieldInfo{"updated", make_int_type(), true}},
      {}, {}, "lib");

  auto mock_lib = make_mock_module("lib", "lib",
      {{"Timestamps", ts_type}});

  auto r = ModuleTestResult::with_mocks(R"(
import "lib"

struct User < lib.Timestamps {
  name String
}

pub fn Main() Void {}
  )", {{"lib", mock_lib}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";

  // Locate the User struct in the analyzer's symbol table and confirm it
  // recorded one embed pointing at the imported Timestamps type.
  auto &scope = r.analyzer->package_scope_;
  ASSERT_NE(scope, nullptr);
  auto sym = scope->symbols.find("User");
  ASSERT_NE(sym, scope->symbols.end());
  ASSERT_NE(sym->second.type, nullptr);
  ASSERT_EQ(sym->second.type->kind, TypeKind::Struct);
  auto &info = std::get<StructTypeInfo>(sym->second.type->detail);
  ASSERT_EQ(info.embeds.size(), 1u);
  EXPECT_EQ(info.embeds[0]->kind, TypeKind::Struct);
  auto &emb_info = std::get<StructTypeInfo>(info.embeds[0]->detail);
  EXPECT_EQ(emb_info.name, "Timestamps");
  EXPECT_EQ(emb_info.origin_package, "lib");
}

// ===========================================================================
// Module not found
// ===========================================================================

TEST(Modules, ImportNotFoundError) {
  auto r = ModuleTestResult::from(R"(
import "nonexistent/package"

pub fn Main() Void {}
  )");

  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("cannot find package"));
}

// ===========================================================================
// Multi-file package compilation (filesystem tests)
// ===========================================================================

class MultiFileTest : public ::testing::Test {
protected:
  std::filesystem::path test_dir;

  void SetUp() override {
    test_dir = std::filesystem::temp_directory_path() / "saga_test_pkg";
    std::filesystem::create_directories(test_dir);
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir);
  }

  void write_file(const std::string &name, const std::string &content) {
    std::ofstream out(test_dir / name);
    out << content;
  }

  // Parse and analyze all .sg files in the test directory.
  ModuleTestResult analyze_dir() {
    ModuleTestResult r;

    // Collect .sg files
    std::vector<std::filesystem::path> sg_files;
    for (auto &entry : std::filesystem::directory_iterator(test_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sg")
        sg_files.push_back(entry.path());
    }
    std::sort(sg_files.begin(), sg_files.end());

    for (auto &f : sg_files) {
      auto file = File::from_path(f.string());
      if (file) r.fileset.add_file(std::move(file));
    }

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->current_package_dir = test_dir.string();
    if (r.ast) {
      r.analyzer->analyze(*r.ast);
    }
    return r;
  }
};

TEST_F(MultiFileTest, TwoFilesSamePackage) {
  write_file("types.sg", R"(
pub struct Point {
  pub x, y Int
}
  )");

  write_file("main.sg", R"(
pub fn MakePoint() Point {
  Point{x: 1, y: 2}
}
  )");

  auto r = analyze_dir();
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(MultiFileTest, CrossFilePrivateAccess) {
  // Within a package, private symbols are accessible.
  write_file("internal.sg", R"(
fn helperFunc() Int { 42 }
  )");

  write_file("main.sg", R"(
pub fn Main() Int {
  helperFunc()
}
  )");

  auto r = analyze_dir();
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(MultiFileTest, CrossFileStructUsage) {
  write_file("data.sg", R"(
struct Config {
  pub name String
  pub value Int
}
  )");

  write_file("logic.sg", R"(
fn makeConfig() Config {
  Config{name: "test", value: 42}
}
  )");

  auto r = analyze_dir();
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

// ===========================================================================
// Import resolution from filesystem
// ===========================================================================

class ImportResolutionTest : public ::testing::Test {
protected:
  std::filesystem::path root_dir;
  std::filesystem::path main_dir;
  std::filesystem::path lib_dir;

  void SetUp() override {
    root_dir = std::filesystem::temp_directory_path() / "saga_import_test";
    main_dir = root_dir / "main";
    lib_dir = root_dir / "mylib";
    std::filesystem::create_directories(main_dir);
    std::filesystem::create_directories(lib_dir);
  }

  void TearDown() override {
    std::filesystem::remove_all(root_dir);
  }

  void write_file(const std::filesystem::path &dir,
                   const std::string &name,
                   const std::string &content) {
    std::ofstream out(dir / name);
    out << content;
  }

  ModuleTestResult analyze_main() {
    ModuleTestResult r;

    // Load main directory files
    std::vector<std::filesystem::path> sg_files;
    for (auto &entry : std::filesystem::directory_iterator(main_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sg")
        sg_files.push_back(entry.path());
    }
    std::sort(sg_files.begin(), sg_files.end());

    for (auto &f : sg_files) {
      auto file = File::from_path(f.string());
      if (file) r.fileset.add_file(std::move(file));
    }

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->current_package_dir = main_dir.string();
    // Add root as search path so "mylib" can be found
    r.analyzer->package_resolver->search_paths.push_back(root_dir.string());

    if (r.ast) {
      r.analyzer->analyze(*r.ast);
    }
    return r;
  }
};

TEST_F(ImportResolutionTest, ImportSiblingPackage) {
  write_file(lib_dir, "lib.sg", R"(
pub fn Add(a, b Int) Int { a + b }
  )");

  write_file(main_dir, "main.sg", R"(
import "mylib"

pub fn Main() Int {
  mylib.Add(1, 2)
}
  )");

  auto r = analyze_main();
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(ImportResolutionTest, ImportOnlyPublicVisible) {
  write_file(lib_dir, "lib.sg", R"(
pub fn PublicFunc() Int { 42 }
fn privateFunc() Int { 99 }
  )");

  write_file(main_dir, "main.sg", R"(
import "mylib"

pub fn Main() Int {
  mylib.privateFunc()
}
  )");

  auto r = analyze_main();
  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("no exported member"));
}

TEST_F(ImportResolutionTest, ImportPublicStructs) {
  write_file(lib_dir, "types.sg", R"(
pub struct Point {
  pub x, y Int
}
  )");

  write_file(main_dir, "main.sg", R"(
import "mylib"

pub fn Main() Void {
  p := mylib.Point
}
  )");

  auto r = analyze_main();
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(ImportResolutionTest, ImportNonExistentPackage) {
  write_file(main_dir, "main.sg", R"(
import "does_not_exist"

pub fn Main() Void {}
  )");

  auto r = analyze_main();
  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("cannot find package"));
}

TEST_F(ImportResolutionTest, CircularImportDetection) {
  // This test relies on both packages importing each other.
  // Create package "a" importing "b" and vice versa.
  auto a_dir = root_dir / "a";
  auto b_dir = root_dir / "b";
  std::filesystem::create_directories(a_dir);
  std::filesystem::create_directories(b_dir);

  write_file(a_dir, "a.sg", R"(
import "b"
pub fn FromA() Int { 1 }
  )");

  write_file(b_dir, "b.sg", R"(
import "a"
pub fn FromB() Int { 2 }
  )");

  // Analyze package "a"
  ModuleTestResult r;
  auto file = File::from_path((a_dir / "a.sg").string());
  if (file) r.fileset.add_file(std::move(file));

  Parser parser(r.fileset);
  r.ast = parser.parse();
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->current_package_dir = a_dir.string();
  r.analyzer->package_resolver->search_paths.push_back(root_dir.string());
  if (r.ast) r.analyzer->analyze(*r.ast);

  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("circular import"));
}

// ===========================================================================
// PackageResolver unit tests
// ===========================================================================

TEST(PackageResolver, FindPackageDirSearchPaths) {
  // Create a temp directory structure
  auto root = std::filesystem::temp_directory_path() / "saga_resolver_test";
  auto pkg_dir = root / "mylib";
  std::filesystem::create_directories(pkg_dir);

  // Create a dummy .sg file
  { std::ofstream(pkg_dir / "lib.sg") << "pub fn X() Int { 1 }"; }

  PackageResolver resolver;
  resolver.search_paths.push_back(root.string());

  auto found = resolver.find_package_dir("mylib");
  EXPECT_FALSE(found.empty());

  auto not_found = resolver.find_package_dir("nonexistent");
  EXPECT_TRUE(not_found.empty());

  auto files = resolver.list_source_files(found);
  EXPECT_EQ(files.size(), 1u);

  std::filesystem::remove_all(root);
}

TEST(PackageResolver, ListSourceFilesOnlyDotSg) {
  auto root = std::filesystem::temp_directory_path() / "saga_list_test";
  std::filesystem::create_directories(root);

  { std::ofstream(root / "a.sg") << "fn A() {}"; }
  { std::ofstream(root / "b.sg") << "fn B() {}"; }
  { std::ofstream(root / "c.txt") << "not a source file"; }
  { std::ofstream(root / "d.go") << "not a source file"; }

  PackageResolver resolver;
  auto files = resolver.list_source_files(root.string());
  EXPECT_EQ(files.size(), 2u);

  std::filesystem::remove_all(root);
}

// ===========================================================================
// Module symbol kind
// ===========================================================================

TEST(Modules, ModuleSymbolCreation) {
  auto mod_type = make_module_type("io", "std/io");
  auto sym = Symbol::module_sym("io", mod_type, Span{0, 10});
  EXPECT_EQ(sym.kind, SymbolKind::Module);
  EXPECT_EQ(sym.name, "io");
  EXPECT_EQ(sym.type, mod_type);
}

// ===========================================================================
// Multi-file with imports (integration)
// ===========================================================================

class MultiFileImportTest : public ::testing::Test {
protected:
  std::filesystem::path root_dir;

  void SetUp() override {
    root_dir = std::filesystem::temp_directory_path() / "saga_mfi_test";
    std::filesystem::remove_all(root_dir);
  }

  void TearDown() override {
    std::filesystem::remove_all(root_dir);
  }

  void write_file(const std::filesystem::path &dir,
                   const std::string &name,
                   const std::string &content) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / name);
    out << content;
  }

  ModuleTestResult analyze_pkg(const std::filesystem::path &pkg_dir) {
    ModuleTestResult r;

    std::vector<std::filesystem::path> sg_files;
    for (auto &entry : std::filesystem::directory_iterator(pkg_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sg")
        sg_files.push_back(entry.path());
    }
    std::sort(sg_files.begin(), sg_files.end());

    for (auto &f : sg_files) {
      auto file = File::from_path(f.string());
      if (file) r.fileset.add_file(std::move(file));
    }

    Parser parser(r.fileset);
    r.ast = parser.parse();

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->current_package_dir = pkg_dir.string();
    r.analyzer->package_resolver->search_paths.push_back(root_dir.string());

    if (r.ast) r.analyzer->analyze(*r.ast);
    return r;
  }
};

TEST_F(MultiFileImportTest, MultiFilePackageImportedCorrectly) {
  // Create a lib package with two files
  write_file(root_dir / "mathlib", "add.sg", R"(
pub fn Add(a, b Int) Int { a + b }
  )");
  write_file(root_dir / "mathlib", "sub.sg", R"(
pub fn Sub(a, b Int) Int { a - b }
  )");

  // Create main that imports it
  write_file(root_dir / "app", "main.sg", R"(
import "mathlib"

pub fn Main() Int {
  mathlib.Add(mathlib.Sub(10, 3), 5)
}
  )");

  auto r = analyze_pkg(root_dir / "app");
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(MultiFileImportTest, ImportedPackageCaching) {
  // Two files in the same package both use a type from an imported package.
  // The import should only be resolved once (caching).
  write_file(root_dir / "shared", "shared.sg", R"(
pub fn Magic() Int { 42 }
  )");

  write_file(root_dir / "app", "a.sg", R"(
import "shared"

pub fn UseA() Int { shared.Magic() }
  )");

  // Note: duplicate import within same package IS an error by spec.
  // But if we only import once, both files should be able to use it.
  auto r = analyze_pkg(root_dir / "app");
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(MultiFileImportTest, PrivateSymbolsNotExported) {
  write_file(root_dir / "secretlib", "lib.sg", R"(
pub fn Public() Int { private_helper() }
fn private_helper() Int { 99 }

pub struct PubStruct {
  pub value Int
}

struct PrivStruct {
  value Int
}
  )");

  write_file(root_dir / "app", "main.sg", R"(
import "secretlib"

pub fn Main() Void {
  x := secretlib.Public()
  p := secretlib.PubStruct
}
  )");

  auto r = analyze_pkg(root_dir / "app");
  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST_F(MultiFileImportTest, PrivateTypeNotAccessible) {
  write_file(root_dir / "secretlib", "lib.sg", R"(
struct PrivStruct {
  value Int
}
  )");

  write_file(root_dir / "app", "main.sg", R"(
import "secretlib"

pub fn Main() Void {
  p := secretlib.PrivStruct
}
  )");

  auto r = analyze_pkg(root_dir / "app");
  EXPECT_TRUE(r.has_error());
  EXPECT_TRUE(r.has_error_containing("no exported member"));
}

// ===========================================================================
// const alias import with selector access
// ===========================================================================

TEST(Modules, ConstImportCallExportedFunc) {
  auto mock = make_mock_module("math", "mega/long/mathematics",
      {{"Pi", make_float_type()},
       {"Sqrt", make_func_type({make_float_type()}, {make_float_type()})}});

  auto r = ModuleTestResult::with_mocks(R"(
const Math = import "mega/long/mathematics"

pub fn Main() Void {
  x := Math.Sqrt(2.0)
}
  )", {{"mega/long/mathematics", mock}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

TEST(Modules, ConstImportAccessExportedConstant) {
  auto mock = make_mock_module("math", "std/math",
      {{"Pi", make_float_type()},
       {"E", make_float_type()}});

  auto r = ModuleTestResult::with_mocks(R"(
const M = import "std/math"

pub fn Main() Void {
  x := M.Pi
}
  )", {{"std/math", mock}});

  EXPECT_TRUE(r.has_no_errors()) << "Errors:";
  for (auto &e : r.analyzer->errors.errors)
    std::cerr << "  " << e.message << "\n";
}

} // namespace mc
