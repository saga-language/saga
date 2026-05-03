// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// project.saga manifest — package metadata and dependency declarations.
//
// Format (TOML subset):
//
//   [package]
//   name = "my_app"
//   kind = "binary"         # or "library"
//   description = "..."
//   license = "mit"
//
//   [dependencies]
//   # Remote: locked to a specific commit + content hash
//   mathlib = { url = "github.com/user/mathlib", commit = "abc123...",
//               hash = "sha256-XYZ...", branch = "main" }
//   # Local path (relative to the manifest file)
//   util = { path = "../util" }
//
//   [tools]
//   # Same structure as [dependencies]; installed as binaries.
//   linter = { url = "github.com/user/linter", commit = "...", hash = "..." }

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace saga {

// A single dependency or tool entry.
struct ManifestDep {
  std::string name;    // alias / import name
  // Remote fields (at least one of url/path must be set)
  std::string url;     // e.g. "github.com/user/mathlib"
  std::string commit;  // locked full commit SHA
  std::string hash;    // content hash, e.g. "sha256-ABC..."
  std::string branch;  // branch or tag the commit was resolved from
  // Local field
  std::string path;    // relative path to the package source

  bool is_local() const { return !path.empty(); }
  bool is_remote() const { return !url.empty(); }
};

struct Manifest {
  std::string name;
  std::string kind;         // "binary" or "library"
  std::string description;
  std::string license;
  std::vector<ManifestDep> dependencies;
  std::vector<ManifestDep> tools;

  // Load from file; returns nullopt on parse error.
  static std::optional<Manifest> load(const std::string &path);

  // Write to file; overwrites existing content.
  bool save(const std::string &path) const;

  // Find dep by name in dependencies (returns pointer or nullptr).
  ManifestDep *find_dep(const std::string &name);
  const ManifestDep *find_dep(const std::string &name) const;
};

// Walk up from 'start' to find the nearest project.saga.
// Returns the path if found, otherwise nullopt.
std::optional<std::string> find_manifest(const std::string &start);

// Return the cache directory for a remote dep.
// Structure: ~/.cache/saga/pkgs/<url>/<commit_8chars>/
// The directory need not exist yet.
std::filesystem::path pkg_cache_dir(const ManifestDep &dep);

// The package name inferred from a URL (last path segment, no .git).
// e.g. "github.com/user/mathlib" → "mathlib"
std::string pkg_name_from_url(const std::string &url);

// Parse a raw URL argument that may include @ref:
//   "github.com/user/pkg"        → {url, ref=""}
//   "github.com/user/pkg@main"   → {url, ref="main"}
//   "github.com/user/pkg@v1.2"   → {url, ref="v1.2"}
// Returns {url_without_ref, ref}.
std::pair<std::string, std::string> parse_pkg_url(const std::string &raw);

} // namespace saga
