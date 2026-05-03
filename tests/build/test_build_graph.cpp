// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "build/build_graph.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using saga::BuildGraph;

// ---------------------------------------------------------------------------
// Temp directory fixture — creates a clean dir per test, removes on teardown.
// ---------------------------------------------------------------------------

class BuildGraphTest : public ::testing::Test {
protected:
  fs::path tmp_dir;

  void SetUp() override {
    tmp_dir = fs::temp_directory_path() /
              ("saga_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                  ->current_test_info()
                                                  ->line()));
    fs::create_directories(tmp_dir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
  }

  // Create a directory and write a .sg source file inside it.
  fs::path make_pkg(const std::string &rel_path,
                    const std::string &filename,
                    const std::string &content) {
    fs::path dir = tmp_dir / rel_path;
    fs::create_directories(dir);
    std::ofstream f(dir / filename);
    f << content;
    return dir;
  }

  // Shorthand: make a package with one file named <last_segment>.sg.
  fs::path make_simple_pkg(const std::string &rel_path,
                            const std::string &content) {
    auto last = fs::path(rel_path).filename().string();
    return make_pkg(rel_path, last + ".sg", content);
  }
};

// ===========================================================================
// scan_imports — unit tests for the import-line scanner
// ===========================================================================

TEST(BuildGraphScanImports, EmptyFile) {
  // Create a temp file.
  auto tmp = fs::temp_directory_path() / "saga_scan_empty.sg";
  { std::ofstream f(tmp); }
  auto imports = BuildGraph::scan_imports(tmp.string());
  EXPECT_TRUE(imports.empty());
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, NoImports) {
  auto tmp = fs::temp_directory_path() / "saga_scan_no_import.sg";
  { std::ofstream f(tmp); f << "pub fn Main() Void {}\n"; }
  auto imports = BuildGraph::scan_imports(tmp.string());
  EXPECT_TRUE(imports.empty());
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, SingleImport) {
  auto tmp = fs::temp_directory_path() / "saga_scan_single.sg";
  { std::ofstream f(tmp); f << "import \"io\"\n\npub fn Main() Void {}\n"; }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0], "io");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, AliasedImport) {
  auto tmp = fs::temp_directory_path() / "saga_scan_alias.sg";
  { std::ofstream f(tmp); f << "import stdos \"std/os\"\n\npub fn Main() Void {}\n"; }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0], "std/os");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, MultipleImports) {
  auto tmp = fs::temp_directory_path() / "saga_scan_multi.sg";
  {
    std::ofstream f(tmp);
    f << "import \"io\"\nimport \"os\"\nimport \"sys\"\n\nfn Foo() Void {}\n";
  }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 3u);
  EXPECT_EQ(imports[0], "io");
  EXPECT_EQ(imports[1], "os");
  EXPECT_EQ(imports[2], "sys");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, StopsAtFirstNonImport) {
  auto tmp = fs::temp_directory_path() / "saga_scan_stop.sg";
  {
    std::ofstream f(tmp);
    // "struct" is a non-import declaration — scanning should stop here.
    f << "import \"io\"\nstruct Foo {}\nimport \"os\"\n";
  }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0], "io");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, IgnoresComments) {
  auto tmp = fs::temp_directory_path() / "saga_scan_comments.sg";
  {
    std::ofstream f(tmp);
    f << "// This is a comment\n"
         "import \"io\"\n"
         "// Another comment\n"
         "import \"os\"\n";
  }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 2u);
  EXPECT_EQ(imports[0], "io");
  EXPECT_EQ(imports[1], "os");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, PackageDeclarationSkipped) {
  auto tmp = fs::temp_directory_path() / "saga_scan_pkg.sg";
  {
    std::ofstream f(tmp);
    f << "package myapp\n\nimport \"io\"\n\npub fn Main() Void {}\n";
  }
  auto imports = BuildGraph::scan_imports(tmp.string());
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0], "io");
  fs::remove(tmp);
}

TEST(BuildGraphScanImports, MissingFile) {
  auto imports = BuildGraph::scan_imports("/nonexistent/path/file.sg");
  EXPECT_TRUE(imports.empty());
}

// ===========================================================================
// compute_hash
// ===========================================================================

TEST(BuildGraphComputeHash, EmptyFileList) {
  auto h = BuildGraph::compute_hash({});
  EXPECT_FALSE(h.empty());
  EXPECT_EQ(h.size(), 16u); // 16 hex chars = 64-bit hash
}

TEST(BuildGraphComputeHash, StableAcrossRuns) {
  auto tmp = fs::temp_directory_path() / "saga_hash_stable.sg";
  { std::ofstream f(tmp); f << "pub fn Add(a, b Int) Int { a + b }\n"; }
  auto h1 = BuildGraph::compute_hash({tmp.string()});
  auto h2 = BuildGraph::compute_hash({tmp.string()});
  EXPECT_EQ(h1, h2);
  fs::remove(tmp);
}

TEST(BuildGraphComputeHash, DifferentContentDifferentHash) {
  auto tmp1 = fs::temp_directory_path() / "saga_hash_a.sg";
  auto tmp2 = fs::temp_directory_path() / "saga_hash_b.sg";
  { std::ofstream f(tmp1); f << "fn Foo() Void {}\n"; }
  { std::ofstream f(tmp2); f << "fn Bar() Void {}\n"; }
  auto h1 = BuildGraph::compute_hash({tmp1.string()});
  auto h2 = BuildGraph::compute_hash({tmp2.string()});
  EXPECT_NE(h1, h2);
  fs::remove(tmp1);
  fs::remove(tmp2);
}

TEST(BuildGraphComputeHash, OrderIndependent) {
  // Hash({a,b}) == Hash({b,a})
  auto tmp1 = fs::temp_directory_path() / "saga_hash_ord_a.sg";
  auto tmp2 = fs::temp_directory_path() / "saga_hash_ord_b.sg";
  { std::ofstream f(tmp1); f << "fn A() Void {}\n"; }
  { std::ofstream f(tmp2); f << "fn B() Void {}\n"; }
  auto h_ab = BuildGraph::compute_hash({tmp1.string(), tmp2.string()});
  auto h_ba = BuildGraph::compute_hash({tmp2.string(), tmp1.string()});
  EXPECT_EQ(h_ab, h_ba);
  fs::remove(tmp1);
  fs::remove(tmp2);
}

// ===========================================================================
// needs_rebuild / save_hash
// ===========================================================================

TEST_F(BuildGraphTest, NeedsRebuildMissingObj) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "abc123";

  // Neither .o nor .sgi exist yet.
  EXPECT_TRUE(BuildGraph::needs_rebuild(node));
}

TEST_F(BuildGraphTest, NeedsRebuildMissingSgi) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "abc123";

  // Create .o but not .sgi or .hash.
  { std::ofstream f(tmp_dir / "mypkg.o"); f << "obj\n"; }
  EXPECT_TRUE(BuildGraph::needs_rebuild(node));
}

TEST_F(BuildGraphTest, NeedsRebuildMissingHash) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "abc123";

  { std::ofstream f(tmp_dir / "mypkg.o"); f << "obj\n"; }
  { std::ofstream f(tmp_dir / "mypkg.sgi"); f << "sgi 1\n"; }
  // No .hash file.
  EXPECT_TRUE(BuildGraph::needs_rebuild(node));
}

TEST_F(BuildGraphTest, NeedsRebuildStaleHash) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "newerhash";

  { std::ofstream f(tmp_dir / "mypkg.o"); f << "obj\n"; }
  { std::ofstream f(tmp_dir / "mypkg.sgi"); f << "sgi 1\n"; }
  { std::ofstream f(tmp_dir / "mypkg.hash"); f << "oldhash\n"; }
  EXPECT_TRUE(BuildGraph::needs_rebuild(node));
}

TEST_F(BuildGraphTest, NeedsRebuildUpToDate) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "matching_hash";

  { std::ofstream f(tmp_dir / "mypkg.o"); f << "obj\n"; }
  { std::ofstream f(tmp_dir / "mypkg.sgi"); f << "sgi 1\n"; }
  { std::ofstream f(tmp_dir / "mypkg.hash"); f << "matching_hash\n"; }
  EXPECT_FALSE(BuildGraph::needs_rebuild(node));
}

TEST_F(BuildGraphTest, SaveHashCreatesFile) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "deadbeef12345678";

  EXPECT_TRUE(BuildGraph::save_hash(node));

  std::ifstream f(tmp_dir / "mypkg.hash");
  ASSERT_TRUE(f.good());
  std::string stored;
  std::getline(f, stored);
  EXPECT_EQ(stored, "deadbeef12345678");
}

TEST_F(BuildGraphTest, SaveHashCreatesOutputDir) {
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = (tmp_dir / "nested" / "dir").string();
  node.content_hash = "cafebabe";

  EXPECT_TRUE(BuildGraph::save_hash(node));
  EXPECT_TRUE(fs::is_regular_file(fs::path(node.output_dir) / "mypkg.hash"));
}

TEST_F(BuildGraphTest, NeedsRebuildRoundTrip) {
  // After save_hash with matching content_hash, needs_rebuild → false.
  BuildGraph::Node node;
  node.name = "mypkg";
  node.output_dir = tmp_dir.string();
  node.content_hash = "roundtrip_hash";

  // Create the artifact stubs.
  { std::ofstream f(tmp_dir / "mypkg.o"); f << "obj\n"; }
  { std::ofstream f(tmp_dir / "mypkg.sgi"); f << "sgi 1\n"; }

  ASSERT_TRUE(BuildGraph::save_hash(node));
  EXPECT_FALSE(BuildGraph::needs_rebuild(node));
}

// ===========================================================================
// BuildGraph::scan — graph construction
// ===========================================================================

TEST_F(BuildGraphTest, ScanSingleNoDeps) {
  auto pkg_dir = make_simple_pkg("myapp", "pub fn Main() Void {}\n");

  BuildGraph g;
  bool ok = g.scan(pkg_dir.string(), "myapp", (tmp_dir / "out").string(), {});
  ASSERT_TRUE(ok) << g.error;
  ASSERT_EQ(g.nodes.size(), 1u);
  EXPECT_EQ(g.nodes[0].import_path, "myapp");
  EXPECT_EQ(g.nodes[0].name, "myapp");
  EXPECT_TRUE(g.nodes[0].deps.empty());
  EXPECT_FALSE(g.nodes[0].content_hash.empty());
}

TEST_F(BuildGraphTest, ScanDerivedImportPath) {
  // When import_path is "", it should be derived from the source dir name.
  auto pkg_dir = make_simple_pkg("util", "pub fn Double(x Int) Int { x + x }\n");

  BuildGraph g;
  bool ok = g.scan(pkg_dir.string(), "", (tmp_dir / "out").string(), {});
  ASSERT_TRUE(ok) << g.error;
  ASSERT_EQ(g.nodes.size(), 1u);
  EXPECT_EQ(g.nodes[0].import_path, "util");
}

TEST_F(BuildGraphTest, ScanWithOneDep) {
  // lib has no deps; app imports lib.
  make_simple_pkg("lib", "pub fn Add(a, b Int) Int { a + b }\n");
  make_simple_pkg("app",
      "import \"lib\"\n"
      "pub fn Main() Void { lib.Add(1, 2) }\n");

  BuildGraph g;
  bool ok = g.scan((tmp_dir / "app").string(), "app",
                   (tmp_dir / "out").string(),
                   {tmp_dir.string()}); // search_paths includes tmp_dir so "lib" resolves
  ASSERT_TRUE(ok) << g.error;
  ASSERT_EQ(g.nodes.size(), 2u);

  // lib should appear before app (topo order not guaranteed from scan, but
  // sorted() ensures correct order).
  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 2u);
  EXPECT_EQ(sorted[0]->import_path, "lib");
  EXPECT_EQ(sorted[1]->import_path, "app");
}

TEST_F(BuildGraphTest, ScanTransitiveDeps) {
  // chain: app → mid → base
  make_simple_pkg("base", "pub fn One() Int { 1 }\n");
  make_simple_pkg("mid",
      "import \"base\"\n"
      "pub fn Two() Int { base.One() + base.One() }\n");
  make_simple_pkg("app",
      "import \"mid\"\n"
      "pub fn Main() Void { mid.Two() }\n");

  BuildGraph g;
  bool ok = g.scan((tmp_dir / "app").string(), "app",
                   (tmp_dir / "out").string(),
                   {tmp_dir.string()});
  ASSERT_TRUE(ok) << g.error;
  EXPECT_EQ(g.nodes.size(), 3u);

  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 3u);
  // base must come first.
  EXPECT_EQ(sorted[0]->import_path, "base");
  // mid before app.
  bool mid_before_app = false;
  size_t mid_pos = 0, app_pos = 0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (sorted[i]->import_path == "mid") mid_pos = i;
    if (sorted[i]->import_path == "app") app_pos = i;
  }
  mid_before_app = mid_pos < app_pos;
  EXPECT_TRUE(mid_before_app);
}

TEST_F(BuildGraphTest, ScanDiamondDeps) {
  // Diamond: app → {left, right} → base
  make_simple_pkg("base", "pub fn Val() Int { 42 }\n");
  make_simple_pkg("left",
      "import \"base\"\n"
      "pub fn LeftVal() Int { base.Val() }\n");
  make_simple_pkg("right",
      "import \"base\"\n"
      "pub fn RightVal() Int { base.Val() }\n");
  make_simple_pkg("app",
      "import \"left\"\n"
      "import \"right\"\n"
      "pub fn Main() Void { left.LeftVal() }\n");

  BuildGraph g;
  bool ok = g.scan((tmp_dir / "app").string(), "app",
                   (tmp_dir / "out").string(),
                   {tmp_dir.string()});
  ASSERT_TRUE(ok) << g.error;
  // base, left, right, app — but base deduped.
  EXPECT_EQ(g.nodes.size(), 4u);

  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 4u);
  // base must be first.
  EXPECT_EQ(sorted[0]->import_path, "base");
  // app must be last.
  EXPECT_EQ(sorted[3]->import_path, "app");
}

TEST_F(BuildGraphTest, ScanMissingPackageError) {
  make_simple_pkg("app",
      "import \"nonexistent\"\n"
      "pub fn Main() Void {}\n");

  BuildGraph g;
  bool ok = g.scan((tmp_dir / "app").string(), "app",
                   (tmp_dir / "out").string(),
                   {tmp_dir.string()});
  EXPECT_FALSE(ok);
  EXPECT_FALSE(g.error.empty());
  EXPECT_NE(g.error.find("nonexistent"), std::string::npos);
}

TEST_F(BuildGraphTest, ScanEmptyDirectoryError) {
  fs::create_directories(tmp_dir / "empty");

  BuildGraph g;
  bool ok = g.scan((tmp_dir / "empty").string(), "empty",
                   (tmp_dir / "out").string(), {});
  EXPECT_FALSE(ok);
  EXPECT_FALSE(g.error.empty());
}

TEST_F(BuildGraphTest, ScanNonexistentDirectoryError) {
  BuildGraph g;
  bool ok = g.scan((tmp_dir / "does_not_exist").string(), "ghost",
                   (tmp_dir / "out").string(), {});
  EXPECT_FALSE(ok);
  EXPECT_FALSE(g.error.empty());
}

TEST_F(BuildGraphTest, ContentHashChangesWithSource) {
  auto pkg_dir = make_simple_pkg("mypkg", "pub fn Foo() Void {}\n");

  BuildGraph g1;
  g1.scan(pkg_dir.string(), "mypkg", (tmp_dir / "out").string(), {});
  ASSERT_EQ(g1.nodes.size(), 1u);
  std::string hash1 = g1.nodes[0].content_hash;

  // Modify the source file.
  std::ofstream f(pkg_dir / "mypkg.sg", std::ios::app);
  f << "pub fn Bar() Void {}\n";
  f.close();

  BuildGraph g2;
  g2.scan(pkg_dir.string(), "mypkg", (tmp_dir / "out").string(), {});
  ASSERT_EQ(g2.nodes.size(), 1u);
  std::string hash2 = g2.nodes[0].content_hash;

  EXPECT_NE(hash1, hash2);
}

TEST_F(BuildGraphTest, ContentHashMixesDependencyHashes) {
  // base and app — changing base should change app's hash too.
  make_simple_pkg("base_v1", "pub fn Val() Int { 1 }\n");
  make_simple_pkg("app_v1",
      "import \"base_v1\"\npub fn Main() Void { base_v1.Val() }\n");

  BuildGraph g1;
  g1.scan((tmp_dir / "app_v1").string(), "app_v1",
          (tmp_dir / "out").string(), {tmp_dir.string()});
  ASSERT_EQ(g1.nodes.size(), 2u);
  std::string app_hash1;
  for (auto &n : g1.nodes)
    if (n.import_path == "app_v1") app_hash1 = n.content_hash;

  // Now change the base source and rescan.
  { std::ofstream f(tmp_dir / "base_v1" / "base_v1.sg"); f << "pub fn Val() Int { 99 }\n"; }

  BuildGraph g2;
  g2.scan((tmp_dir / "app_v1").string(), "app_v1",
          (tmp_dir / "out").string(), {tmp_dir.string()});
  std::string app_hash2;
  for (auto &n : g2.nodes)
    if (n.import_path == "app_v1") app_hash2 = n.content_hash;

  EXPECT_NE(app_hash1, app_hash2)
      << "Changing a dependency's source should change the dependent's hash";
}

// ===========================================================================
// sorted() — topological sort
// ===========================================================================

TEST_F(BuildGraphTest, SortedEmptyGraph) {
  BuildGraph g;
  auto sorted = g.sorted();
  EXPECT_TRUE(sorted.empty());
}

TEST_F(BuildGraphTest, SortedSingleNode) {
  BuildGraph g;
  g.nodes.push_back({"myapp", "myapp", "/src", "/out", {}, "abc"});
  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 1u);
  EXPECT_EQ(sorted[0]->import_path, "myapp");
}

TEST_F(BuildGraphTest, SortedRespectsDependencyOrder) {
  BuildGraph g;
  // Add in reverse order: app (depends on lib), lib (no deps).
  g.nodes.push_back({"app", "app", "/src/app", "/out/app", {"lib"}, "h1"});
  g.nodes.push_back({"lib", "lib", "/src/lib", "/out/lib", {}, "h2"});

  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 2u);
  EXPECT_EQ(sorted[0]->import_path, "lib");
  EXPECT_EQ(sorted[1]->import_path, "app");
}

TEST_F(BuildGraphTest, SortedCycleDetected) {
  BuildGraph g;
  // a → b → a  (cycle)
  g.nodes.push_back({"a", "a", "/src/a", "/out/a", {"b"}, "h1"});
  g.nodes.push_back({"b", "b", "/src/b", "/out/b", {"a"}, "h2"});

  auto sorted = g.sorted();
  EXPECT_TRUE(sorted.empty());
  EXPECT_FALSE(g.error.empty());
  EXPECT_NE(g.error.find("cycle"), std::string::npos);
}

TEST_F(BuildGraphTest, SortedMultipleRoots) {
  BuildGraph g;
  // Two independent packages — both are roots (no dependents).
  g.nodes.push_back({"foo", "foo", "/src/foo", "/out/foo", {}, "h1"});
  g.nodes.push_back({"bar", "bar", "/src/bar", "/out/bar", {}, "h2"});

  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 2u);
  // Order between independent roots is arbitrary but both must be present.
  bool has_foo = false, has_bar = false;
  for (auto *n : sorted) {
    if (n->import_path == "foo") has_foo = true;
    if (n->import_path == "bar") has_bar = true;
  }
  EXPECT_TRUE(has_foo);
  EXPECT_TRUE(has_bar);
}

// ===========================================================================
// Node fields after scan
// ===========================================================================

TEST_F(BuildGraphTest, NodeHasCorrectSourceDir) {
  auto pkg_dir = make_simple_pkg("mypkg", "pub fn F() Void {}\n");

  BuildGraph g;
  g.scan(pkg_dir.string(), "mypkg", (tmp_dir / "out").string(), {});
  ASSERT_EQ(g.nodes.size(), 1u);
  EXPECT_EQ(g.nodes[0].source_dir, pkg_dir.string());
}

TEST_F(BuildGraphTest, NodeHasCorrectOutputDir) {
  auto pkg_dir = make_simple_pkg("mypkg", "pub fn F() Void {}\n");
  std::string out = (tmp_dir / "build").string();

  BuildGraph g;
  g.scan(pkg_dir.string(), "mypkg", out, {});
  ASSERT_EQ(g.nodes.size(), 1u);
  // Root node's output_dir is the given output_dir directly.
  EXPECT_EQ(g.nodes[0].output_dir, out);
}

TEST_F(BuildGraphTest, DepNodeOutputDirIsSubdir) {
  make_simple_pkg("base", "pub fn B() Void {}\n");
  make_simple_pkg("app",
      "import \"base\"\npub fn Main() Void { base.B() }\n");

  std::string out = (tmp_dir / "build").string();
  BuildGraph g;
  g.scan((tmp_dir / "app").string(), "app", out, {tmp_dir.string()});
  ASSERT_EQ(g.nodes.size(), 2u);

  // Dependency "base" should have output_dir = <out>/base.
  for (auto &n : g.nodes) {
    if (n.import_path == "base") {
      EXPECT_EQ(n.output_dir, out + "/base");
    }
  }
}

// ===========================================================================
// Incremental build: full round-trip with save_hash + needs_rebuild
// ===========================================================================

TEST_F(BuildGraphTest, IncrementalSkipsUpToDate) {
  auto pkg_dir = make_simple_pkg("inc_pkg", "pub fn Val() Int { 7 }\n");
  std::string out = (tmp_dir / "out").string();

  // First scan: node should need a build.
  BuildGraph g1;
  ASSERT_TRUE(g1.scan(pkg_dir.string(), "inc_pkg", out, {}));
  ASSERT_EQ(g1.nodes.size(), 1u);
  EXPECT_TRUE(BuildGraph::needs_rebuild(g1.nodes[0]));

  // Simulate a successful build: create artifact stubs + save hash.
  fs::create_directories(out);
  { std::ofstream f(out + "/inc_pkg.o"); f << "obj\n"; }
  { std::ofstream f(out + "/inc_pkg.sgi"); f << "sgi 1\npackage inc_pkg\n"; }
  ASSERT_TRUE(BuildGraph::save_hash(g1.nodes[0]));

  // Second scan: same source → same hash → up-to-date.
  BuildGraph g2;
  ASSERT_TRUE(g2.scan(pkg_dir.string(), "inc_pkg", out, {}));
  ASSERT_EQ(g2.nodes.size(), 1u);
  EXPECT_FALSE(BuildGraph::needs_rebuild(g2.nodes[0]))
      << "Should be up-to-date after save_hash with matching content";
}

TEST_F(BuildGraphTest, IncrementalRebuildAfterSourceChange) {
  auto pkg_dir = make_simple_pkg("chg_pkg", "pub fn Val() Int { 1 }\n");
  std::string out = (tmp_dir / "out").string();

  // First build.
  BuildGraph g1;
  ASSERT_TRUE(g1.scan(pkg_dir.string(), "chg_pkg", out, {}));
  fs::create_directories(out);
  { std::ofstream f(out + "/chg_pkg.o"); f << "obj\n"; }
  { std::ofstream f(out + "/chg_pkg.sgi"); f << "sgi 1\npackage chg_pkg\n"; }
  ASSERT_TRUE(BuildGraph::save_hash(g1.nodes[0]));

  // Modify source.
  { std::ofstream f(pkg_dir / "chg_pkg.sg"); f << "pub fn Val() Int { 99 }\n"; }

  // Re-scan: hash should differ.
  BuildGraph g2;
  ASSERT_TRUE(g2.scan(pkg_dir.string(), "chg_pkg", out, {}));
  ASSERT_EQ(g2.nodes.size(), 1u);
  EXPECT_TRUE(BuildGraph::needs_rebuild(g2.nodes[0]))
      << "Should need rebuild after source change";
  // Hashes must differ.
  EXPECT_NE(g1.nodes[0].content_hash, g2.nodes[0].content_hash);
}

TEST_F(BuildGraphTest, IncrementalCascadesFromDep) {
  // base → app.  Change base → app's hash changes too.
  make_simple_pkg("dep_base", "pub fn X() Int { 1 }\n");
  make_simple_pkg("dep_app",
      "import \"dep_base\"\npub fn Main() Void { dep_base.X() }\n");

  std::string out = (tmp_dir / "out").string();
  fs::create_directories(out + "/dep_base");

  // First scan.
  BuildGraph g1;
  ASSERT_TRUE(g1.scan((tmp_dir / "dep_app").string(), "dep_app", out,
                       {tmp_dir.string()}));
  ASSERT_EQ(g1.nodes.size(), 2u);
  std::string app_hash_before;
  for (auto &n : g1.nodes)
    if (n.import_path == "dep_app") app_hash_before = n.content_hash;

  // Simulate dep_base compiled.
  { std::ofstream f(out + "/dep_base/dep_base.o"); f << "obj\n"; }
  { std::ofstream f(out + "/dep_base/dep_base.sgi"); f << "sgi 1\npackage dep_base\n"; }
  for (auto &n : g1.nodes) BuildGraph::save_hash(n);

  // Change dep_base source.
  { std::ofstream f(tmp_dir / "dep_base" / "dep_base.sg");
    f << "pub fn X() Int { 99 }\n"; }

  // Re-scan.
  BuildGraph g2;
  ASSERT_TRUE(g2.scan((tmp_dir / "dep_app").string(), "dep_app", out,
                       {tmp_dir.string()}));
  std::string app_hash_after;
  for (auto &n : g2.nodes)
    if (n.import_path == "dep_app") app_hash_after = n.content_hash;

  EXPECT_NE(app_hash_before, app_hash_after)
      << "Changing dep_base should change dep_app's hash";

  // dep_base needs rebuild.
  for (auto &n : g2.nodes)
    if (n.import_path == "dep_base")
      EXPECT_TRUE(BuildGraph::needs_rebuild(n));
}

TEST_F(BuildGraphTest, SortedPreservesAllNodesForDiamond) {
  // Diamond: no node is lost in sort even when shared dep appears twice.
  BuildGraph g;
  g.nodes.push_back({"base", "base", "/src/base", "/out/base", {}, "h0"});
  g.nodes.push_back({"left", "left", "/src/left", "/out/left", {"base"}, "h1"});
  g.nodes.push_back({"right", "right", "/src/right", "/out/right", {"base"}, "h2"});
  g.nodes.push_back({"app", "app", "/src/app", "/out/app", {"left", "right"}, "h3"});

  auto sorted = g.sorted();
  ASSERT_EQ(sorted.size(), 4u) << "All 4 nodes should survive the sort";
  EXPECT_EQ(sorted[0]->import_path, "base");
  EXPECT_EQ(sorted[3]->import_path, "app");
}

TEST_F(BuildGraphTest, MultipleSourceFilesInPackage) {
  // A package with two .sg files — both should be included in the hash.
  fs::path pkg = tmp_dir / "multi";
  fs::create_directories(pkg);
  { std::ofstream f(pkg / "a.sg"); f << "pub fn A() Int { 1 }\n"; }
  { std::ofstream f(pkg / "b.sg"); f << "pub fn B() Int { 2 }\n"; }

  BuildGraph g;
  ASSERT_TRUE(g.scan(pkg.string(), "multi", (tmp_dir / "out").string(), {}));
  ASSERT_EQ(g.nodes.size(), 1u);
  EXPECT_FALSE(g.nodes[0].content_hash.empty());

  // Changing one file should change the hash.
  std::string hash_before = g.nodes[0].content_hash;
  { std::ofstream f(pkg / "b.sg"); f << "pub fn B() Int { 999 }\n"; }

  BuildGraph g2;
  ASSERT_TRUE(g2.scan(pkg.string(), "multi", (tmp_dir / "out").string(), {}));
  EXPECT_NE(g2.nodes[0].content_hash, hash_before)
      << "Changing one of two source files should change the package hash";
}
