// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// BuildGraph — dependency graph for incremental multi-package compilation.
//
// Workflow:
//   BuildGraph g;
//   if (!g.scan(source_dir, "myapp", output_dir, search_paths))
//     fatal(g.error);
//   for (auto *node : g.sorted())
//     compile(*node);
// ---------------------------------------------------------------------------

struct BuildGraph {
  // ── Per-package node ─────────────────────────────────────────────────────

  struct Node {
    std::string import_path;  // canonical import path, e.g. "std/io"
    std::string name;         // last segment, e.g. "io"
    std::string source_dir;   // filesystem path to package source
    std::string output_dir;   // where .o and .sgi are written
    std::vector<std::string> deps;  // direct dependency import paths
    std::string content_hash; // FNV-1a of all source file bytes + dep hashes
  };

  // ── Graph state ───────────────────────────────────────────────────────────

  /// All nodes in discovery order.  Use sorted() for build order.
  std::vector<Node> nodes;

  /// Human-readable error from the last failing operation.
  std::string error;

  // ── Construction ──────────────────────────────────────────────────────────

  /// Recursively scan a package and all its transitive dependencies.
  ///
  /// @param source_dir   Filesystem directory that contains the package's
  ///                     .sg files.
  /// @param import_path  Canonical import path for the root package
  ///                     (e.g. "myapp", "std/io").  Pass "" to derive it
  ///                     from the last path component of source_dir.
  /// @param output_dir   Directory where .o and .sgi artifacts will live.
  ///                     Dependency packages use output_dir/<pkg>/ unless
  ///                     they are found via search_paths (in which case the
  ///                     same output_dir is used).
  /// @param search_paths Directories to search when resolving bare import
  ///                     paths (e.g. "/usr/lib/saga", "std/").
  ///
  /// Returns false and sets `error` if a package directory is not found or a
  /// dependency cycle is detected.
  bool scan(const std::string &source_dir,
            const std::string &import_path,
            const std::string &output_dir,
            const std::vector<std::string> &search_paths);

  // ── Build ordering ────────────────────────────────────────────────────────

  /// Return pointers to nodes in topological order (leaves first — i.e.
  /// packages with no dependencies come before packages that depend on them).
  ///
  /// Returns an empty vector and sets `error` if the graph contains a cycle.
  std::vector<const Node *> sorted() const;

  // ── Incremental build helpers ─────────────────────────────────────────────

  /// Returns true if the package needs (re)compilation:
  ///   • output_dir/<name>.o  is missing, OR
  ///   • output_dir/<name>.sgi is missing, OR
  ///   • output_dir/<name>.hash is missing or differs from node.content_hash.
  static bool needs_rebuild(const Node &node);

  /// Persist the content hash to output_dir/<name>.hash.
  /// Call after a successful compilation of the package.
  static bool save_hash(const Node &node);

  // ── Utilities ─────────────────────────────────────────────────────────────

  /// Quickly extract import paths from a single .sg source file without a
  /// full parse.  Scans for  import "..."  and  import name "..."  lines.
  static std::vector<std::string> scan_imports(const std::string &file_path);

  /// Compute an FNV-1a content hash over a set of source files.
  /// The hash is stable across platforms and independent of file ordering
  /// (files are sorted before hashing).
  static std::string compute_hash(const std::vector<std::string> &file_paths);
};

} // namespace mc
