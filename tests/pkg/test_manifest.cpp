// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "pkg/manifest.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using saga::Manifest;
using saga::ManifestDep;

// ---------------------------------------------------------------------------
// Temp-directory fixture
// ---------------------------------------------------------------------------

class ManifestTest : public ::testing::Test {
protected:
  fs::path tmp;

  void SetUp() override {
    tmp = fs::temp_directory_path() /
          ("saga_manifest_" +
           std::to_string(
               ::testing::UnitTest::GetInstance()->current_test_info()->line()));
    fs::create_directories(tmp);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp, ec);
  }

  std::string write(const std::string &name, const std::string &content) {
    auto path = (tmp / name).string();
    std::ofstream f(path);
    f << content;
    return path;
  }
};

// ===========================================================================
// Manifest::load — happy-path parsing
// ===========================================================================

TEST_F(ManifestTest, LoadMinimal) {
  auto path = write("project.saga", R"(
[package]
name = "myapp"
kind = "binary"
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->name, "myapp");
  EXPECT_EQ(m->kind, "binary");
  EXPECT_TRUE(m->dependencies.empty());
}

TEST_F(ManifestTest, LoadAllPackageFields) {
  auto path = write("project.saga", R"(
[package]
name = "mylib"
kind = "library"
description = "A useful library"
license = "mit"
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->name, "mylib");
  EXPECT_EQ(m->kind, "library");
  EXPECT_EQ(m->description, "A useful library");
  EXPECT_EQ(m->license, "mit");
}

TEST_F(ManifestTest, LoadRemoteDependency) {
  auto path = write("project.saga", R"(
[package]
name = "app"
kind = "binary"

[dependencies]
mathlib = { url = "github.com/user/mathlib", commit = "abc123def456", hash = "sha256-XYZ", branch = "main" }
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->dependencies.size(), 1u);
  auto &dep = m->dependencies[0];
  EXPECT_EQ(dep.name,   "mathlib");
  EXPECT_EQ(dep.url,    "github.com/user/mathlib");
  EXPECT_EQ(dep.commit, "abc123def456");
  EXPECT_EQ(dep.hash,   "sha256-XYZ");
  EXPECT_EQ(dep.branch, "main");
  EXPECT_TRUE(dep.path.empty());
  EXPECT_TRUE(dep.is_remote());
  EXPECT_FALSE(dep.is_local());
}

TEST_F(ManifestTest, LoadLocalDependency) {
  auto path = write("project.saga", R"(
[package]
name = "app"
kind = "binary"

[dependencies]
util = { path = "../util" }
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->dependencies.size(), 1u);
  auto &dep = m->dependencies[0];
  EXPECT_EQ(dep.name, "util");
  EXPECT_EQ(dep.path, "../util");
  EXPECT_TRUE(dep.url.empty());
  EXPECT_TRUE(dep.is_local());
  EXPECT_FALSE(dep.is_remote());
}

TEST_F(ManifestTest, LoadMultipleDependencies) {
  auto path = write("project.saga", R"(
[package]
name = "app"
kind = "binary"

[dependencies]
alpha = { path = "../alpha" }
beta  = { url = "github.com/user/beta", commit = "deadbeef", hash = "sha256-AAA", branch = "main" }
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->dependencies.size(), 2u);
  // Order preserved from file.
  EXPECT_EQ(m->dependencies[0].name, "alpha");
  EXPECT_EQ(m->dependencies[1].name, "beta");
}

TEST_F(ManifestTest, LoadToolsSection) {
  auto path = write("project.saga", R"(
[package]
name = "app"
kind = "binary"

[tools]
linter = { url = "github.com/user/linter", commit = "cafe0123", hash = "sha256-TTT", branch = "main" }
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  EXPECT_TRUE(m->dependencies.empty());
  ASSERT_EQ(m->tools.size(), 1u);
  EXPECT_EQ(m->tools[0].name, "linter");
  EXPECT_EQ(m->tools[0].url,  "github.com/user/linter");
}

TEST_F(ManifestTest, LoadIgnoresComments) {
  auto path = write("project.saga", R"(
# This is a top-level comment
[package]
# comment inside section
name = "commented"
kind = "binary"

# Another comment
[dependencies]
# dep comment
util = { path = "../util" }
)");
  auto m = Manifest::load(path);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->name, "commented");
  ASSERT_EQ(m->dependencies.size(), 1u);
  EXPECT_EQ(m->dependencies[0].name, "util");
}

TEST_F(ManifestTest, LoadNonExistentReturnsNullopt) {
  auto m = Manifest::load("/nonexistent/path/project.saga");
  EXPECT_FALSE(m.has_value());
}

TEST_F(ManifestTest, LoadEmptyFileReturnsManifest) {
  auto path = write("project.saga", "");
  auto m = Manifest::load(path);
  // Empty file parses as empty manifest (no fields set), not nullopt.
  ASSERT_TRUE(m.has_value());
  EXPECT_TRUE(m->name.empty());
}

// ===========================================================================
// Manifest::save — round-trip
// ===========================================================================

TEST_F(ManifestTest, SaveAndReloadPackageFields) {
  Manifest m;
  m.name = "roundtrip";
  m.kind = "library";
  m.description = "Test library";
  m.license = "apache-2.0";

  auto path = (tmp / "project.saga").string();
  ASSERT_TRUE(m.save(path));

  auto m2 = Manifest::load(path);
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2->name,        "roundtrip");
  EXPECT_EQ(m2->kind,        "library");
  EXPECT_EQ(m2->description, "Test library");
  EXPECT_EQ(m2->license,     "apache-2.0");
}

TEST_F(ManifestTest, SaveAndReloadRemoteDep) {
  Manifest m;
  m.name = "app";
  m.kind = "binary";
  ManifestDep dep;
  dep.name   = "mathlib";
  dep.url    = "github.com/user/mathlib";
  dep.commit = "abc123def456789012345678";
  dep.hash   = "sha256-ABCDEF";
  dep.branch = "main";
  m.dependencies.push_back(dep);

  auto path = (tmp / "project.saga").string();
  ASSERT_TRUE(m.save(path));

  auto m2 = Manifest::load(path);
  ASSERT_TRUE(m2.has_value());
  ASSERT_EQ(m2->dependencies.size(), 1u);
  EXPECT_EQ(m2->dependencies[0].url,    "github.com/user/mathlib");
  EXPECT_EQ(m2->dependencies[0].commit, "abc123def456789012345678");
  EXPECT_EQ(m2->dependencies[0].hash,   "sha256-ABCDEF");
  EXPECT_EQ(m2->dependencies[0].branch, "main");
}

TEST_F(ManifestTest, SaveAndReloadLocalDep) {
  Manifest m;
  m.name = "app";
  m.kind = "binary";
  ManifestDep dep;
  dep.name = "util";
  dep.path = "../util";
  m.dependencies.push_back(dep);

  auto path = (tmp / "project.saga").string();
  ASSERT_TRUE(m.save(path));

  auto m2 = Manifest::load(path);
  ASSERT_TRUE(m2.has_value());
  ASSERT_EQ(m2->dependencies.size(), 1u);
  EXPECT_EQ(m2->dependencies[0].name, "util");
  EXPECT_EQ(m2->dependencies[0].path, "../util");
  EXPECT_TRUE(m2->dependencies[0].url.empty());
}

TEST_F(ManifestTest, SaveSortsDepsAlphabetically) {
  Manifest m;
  m.name = "app";
  m.kind = "binary";
  // Add in reverse order — save must sort them.
  m.dependencies.push_back({"zebra", "", "", "", "", "../zebra"});
  m.dependencies.push_back({"alpha", "", "", "", "", "../alpha"});
  m.dependencies.push_back({"middle", "", "", "", "", "../middle"});

  auto path = (tmp / "project.saga").string();
  ASSERT_TRUE(m.save(path));

  auto m2 = Manifest::load(path);
  ASSERT_TRUE(m2.has_value());
  ASSERT_EQ(m2->dependencies.size(), 3u);
  EXPECT_EQ(m2->dependencies[0].name, "alpha");
  EXPECT_EQ(m2->dependencies[1].name, "middle");
  EXPECT_EQ(m2->dependencies[2].name, "zebra");
}

TEST_F(ManifestTest, SaveCreatesParentDirs) {
  auto path = (tmp / "nested" / "dir" / "project.saga").string();
  Manifest m;
  m.name = "x";
  m.kind = "binary";
  ASSERT_TRUE(m.save(path));
  EXPECT_TRUE(fs::is_regular_file(path));
}

// ===========================================================================
// Manifest::find_dep
// ===========================================================================

TEST_F(ManifestTest, FindDepExisting) {
  Manifest m;
  m.dependencies.push_back({"foo", "github.com/user/foo", "abc", "", "main", ""});
  m.dependencies.push_back({"bar", "", "", "", "", "../bar"});

  auto *found = m.find_dep("foo");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->url, "github.com/user/foo");
}

TEST_F(ManifestTest, FindDepMissing) {
  Manifest m;
  m.dependencies.push_back({"foo", "", "", "", "", "../foo"});
  EXPECT_EQ(m.find_dep("nonexistent"), nullptr);
}

TEST_F(ManifestTest, FindDepConst) {
  Manifest m;
  m.dependencies.push_back({"lib", "", "", "", "", "../lib"});
  const Manifest &cm = m;
  EXPECT_NE(cm.find_dep("lib"), nullptr);
  EXPECT_EQ(cm.find_dep("other"), nullptr);
}

// ===========================================================================
// find_manifest
// ===========================================================================

TEST_F(ManifestTest, FindManifestInCurrentDir) {
  write("project.saga", "[package]\nname = \"x\"\nkind = \"binary\"\n");
  auto found = saga::find_manifest(tmp.string());
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(fs::path(*found).filename().string(), "project.saga");
}

TEST_F(ManifestTest, FindManifestInParentDir) {
  write("project.saga", "[package]\nname = \"x\"\nkind = \"binary\"\n");
  // Create a nested subdirectory and search from there.
  fs::path sub = tmp / "src" / "nested";
  fs::create_directories(sub);

  auto found = saga::find_manifest(sub.string());
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(fs::path(*found).parent_path(), tmp);
}

TEST_F(ManifestTest, FindManifestNotFound) {
  // No project.saga in tmp or any parent (unlikely, but safe in /tmp).
  // Use a path we know won't have project.saga ancestors.
  auto found = saga::find_manifest("/");
  EXPECT_FALSE(found.has_value());
}

// ===========================================================================
// pkg_cache_dir
// ===========================================================================

TEST(PkgCacheDir, StructureFromRemoteDep) {
  ManifestDep dep;
  dep.url    = "github.com/user/mathlib";
  dep.commit = "abc123def456";

  auto dir = saga::pkg_cache_dir(dep);
  // Should contain the URL path segments.
  std::string s = dir.string();
  EXPECT_NE(s.find("github.com"), std::string::npos);
  EXPECT_NE(s.find("user"),       std::string::npos);
  EXPECT_NE(s.find("mathlib"),    std::string::npos);
  // Short commit (8 chars).
  EXPECT_NE(s.find("abc123de"),   std::string::npos);
}

TEST(PkgCacheDir, ShortCommitUsed) {
  ManifestDep dep;
  dep.url    = "github.com/user/pkg";
  dep.commit = "0123456789abcdef";

  auto dir = saga::pkg_cache_dir(dep);
  // Last component should be first 8 chars of commit.
  EXPECT_EQ(dir.filename().string(), "01234567");
}

TEST(PkgCacheDir, EmptyCommitUsesHEAD) {
  ManifestDep dep;
  dep.url    = "github.com/user/pkg";
  dep.commit = "";

  auto dir = saga::pkg_cache_dir(dep);
  EXPECT_EQ(dir.filename().string(), "HEAD");
}

// ===========================================================================
// pkg_name_from_url
// ===========================================================================

TEST(PkgNameFromUrl, SimpleUrl) {
  EXPECT_EQ(saga::pkg_name_from_url("github.com/user/mathlib"), "mathlib");
}

TEST(PkgNameFromUrl, StripsGitSuffix) {
  EXPECT_EQ(saga::pkg_name_from_url("github.com/user/repo.git"), "repo");
}

TEST(PkgNameFromUrl, NoSlash) {
  EXPECT_EQ(saga::pkg_name_from_url("mathlib"), "mathlib");
}

// ===========================================================================
// parse_pkg_url
// ===========================================================================

TEST(ParsePkgUrl, NoRef) {
  auto [url, ref] = saga::parse_pkg_url("github.com/user/pkg");
  EXPECT_EQ(url, "github.com/user/pkg");
  EXPECT_TRUE(ref.empty());
}

TEST(ParsePkgUrl, WithBranch) {
  auto [url, ref] = saga::parse_pkg_url("github.com/user/pkg@main");
  EXPECT_EQ(url, "github.com/user/pkg");
  EXPECT_EQ(ref, "main");
}

TEST(ParsePkgUrl, WithTag) {
  auto [url, ref] = saga::parse_pkg_url("github.com/user/pkg@v1.2.3");
  EXPECT_EQ(url, "github.com/user/pkg");
  EXPECT_EQ(ref, "v1.2.3");
}

TEST(ParsePkgUrl, WithCommit) {
  auto [url, ref] = saga::parse_pkg_url("github.com/user/pkg@abc123def");
  EXPECT_EQ(url, "github.com/user/pkg");
  EXPECT_EQ(ref, "abc123def");
}
