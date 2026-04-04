// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "build/build_graph.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace mc {

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash
// ---------------------------------------------------------------------------

static uint64_t fnv1a_update(uint64_t h, const char *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t fnv1a_update(uint64_t h, const std::string &s) {
  return fnv1a_update(h, s.data(), s.size());
}

static std::string to_hex(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(v));
  return buf;
}

// ---------------------------------------------------------------------------
// Platform helpers (same logic as analyzer.cpp / main.cpp)
// ---------------------------------------------------------------------------

static bool is_platform_suffix(const std::string &stem) {
  static const std::vector<std::string> platforms = {"_linux", "_darwin",
                                                     "_windows"};
  for (auto &p : platforms)
    if (stem.ends_with(p))
      return true;
  return false;
}

static std::string current_platform() {
#if defined(__linux__)
  return "_linux";
#elif defined(__APPLE__)
  return "_darwin";
#elif defined(_WIN32)
  return "_windows";
#else
  return "";
#endif
}

// ---------------------------------------------------------------------------
// Enumerate .sg source files in a directory (platform-filtered)
// ---------------------------------------------------------------------------

static std::vector<std::string> list_sg_files(const std::string &dir) {
  std::vector<std::string> result;
  std::error_code ec;
  for (auto &entry : fs::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".sg")
      continue;
    std::string stem = entry.path().stem().string();
    if (!is_platform_suffix(stem) ||
        stem.ends_with(current_platform()))
      result.push_back(entry.path().string());
  }
  std::sort(result.begin(), result.end());
  return result;
}

// ---------------------------------------------------------------------------
// scan_imports — lightweight import path extractor
//
// Scans a .sg file line by line.  Handles:
//   import "path"
//   import alias "path"
//
// Does NOT handle multi-line imports or block imports (the language spec
// uses one-per-line form).  Fast O(lines) scan with no full AST.
// ---------------------------------------------------------------------------

static bool try_parse_string(const std::string &line, size_t pos,
                              std::string &out) {
  // Skip whitespace.
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
    ++pos;
  if (pos >= line.size() || line[pos] != '"')
    return false;
  ++pos;
  std::string s;
  while (pos < line.size() && line[pos] != '"' && line[pos] != '\n') {
    s += line[pos++];
  }
  if (pos >= line.size() || line[pos] != '"')
    return false;
  out = s;
  return true;
}

std::vector<std::string>
BuildGraph::scan_imports(const std::string &file_path) {
  std::ifstream f(file_path);
  if (!f)
    return {};

  std::vector<std::string> result;
  std::string line;
  bool past_imports = false;

  while (std::getline(f, line)) {
    // Strip leading whitespace.
    size_t start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t'))
      ++start;

    // Skip blank lines and comments.
    if (start >= line.size() || line[start] == '/')
      continue;

    // Once we see a non-import, non-blank, non-comment, non-package line
    // we can stop scanning (imports must come before declarations).
    if (line.substr(start, 7) == "package") {
      continue;
    }
    if (line.substr(start, 6) != "import") {
      // Allow blank lines and comment lines past the import block.
      past_imports = true;
      (void)past_imports;
      break;
    }

    size_t pos = start + 6; // skip "import"
    // Skip whitespace after "import".
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
      ++pos;
    if (pos >= line.size())
      continue;

    if (line[pos] == '"') {
      // import "path"
      std::string path;
      if (try_parse_string(line, pos, path))
        result.push_back(path);
    } else {
      // import alias "path" — skip the alias identifier.
      while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' &&
             line[pos] != '"')
        ++pos;
      std::string path;
      if (try_parse_string(line, pos, path))
        result.push_back(path);
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// compute_hash
// ---------------------------------------------------------------------------

std::string
BuildGraph::compute_hash(const std::vector<std::string> &file_paths) {
  // Sort paths so that hash is stable regardless of discovery order.
  std::vector<std::string> sorted = file_paths;
  std::sort(sorted.begin(), sorted.end());

  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  uint64_t h = FNV_OFFSET;

  for (auto &path : sorted) {
    // Hash the path itself as a separator.
    h = fnv1a_update(h, path);
    h = fnv1a_update(h, "\0", 1);

    std::ifstream f(path, std::ios::binary);
    if (!f)
      continue;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    h = fnv1a_update(h, content);
    h = fnv1a_update(h, "\0", 1);
  }

  return to_hex(h);
}

// ---------------------------------------------------------------------------
// needs_rebuild / save_hash
// ---------------------------------------------------------------------------

bool BuildGraph::needs_rebuild(const Node &node) {
  std::string base = node.output_dir + "/" + node.name;
  if (!fs::is_regular_file(base + ".o"))
    return true;
  if (!fs::is_regular_file(base + ".sgi"))
    return true;
  std::string hash_path = base + ".hash";
  if (!fs::is_regular_file(hash_path))
    return true;
  // Read stored hash.
  std::ifstream f(hash_path);
  if (!f)
    return true;
  std::string stored;
  std::getline(f, stored);
  return stored != node.content_hash;
}

bool BuildGraph::save_hash(const Node &node) {
  std::string hash_path = node.output_dir + "/" + node.name + ".hash";
  // Create output dir if needed.
  std::error_code ec;
  fs::create_directories(node.output_dir, ec);
  std::ofstream f(hash_path);
  if (!f)
    return false;
  f << node.content_hash << "\n";
  return f.good();
}

// ---------------------------------------------------------------------------
// scan — find the source directory for an import path
// ---------------------------------------------------------------------------

/// Resolve an import path to an absolute source directory.
/// Returns empty string if not found.
static std::string resolve_source_dir(
    const std::string &import_path,
    const std::vector<std::string> &search_paths) {
  // Try each search path.  The import path is appended to the search path.
  for (auto &sp : search_paths) {
    fs::path candidate = fs::path(sp) / import_path;
    std::error_code ec;
    if (fs::is_directory(candidate, ec))
      return fs::canonical(candidate, ec).string();
  }
  return "";
}

// ---------------------------------------------------------------------------
// scan — recursive DFS with cycle detection
// ---------------------------------------------------------------------------

bool BuildGraph::scan(const std::string &source_dir_in,
                      const std::string &import_path_in,
                      const std::string &output_dir,
                      const std::vector<std::string> &search_paths) {
  // Canonicalize source_dir.
  std::error_code ec;
  std::string source_dir =
      fs::canonical(fs::path(source_dir_in), ec).string();
  if (ec) {
    error = std::format("cannot resolve path '{}': {}", source_dir_in,
                        ec.message());
    return false;
  }

  // Derive import_path from directory name if not provided.
  std::string import_path = import_path_in.empty()
      ? fs::path(source_dir).filename().string()
      : import_path_in;

  // Maps import_path → index in nodes (also serves as visited set).
  std::unordered_map<std::string, size_t> index_map;
  // Stack of import paths currently being traversed (for cycle detection).
  std::unordered_set<std::string> in_progress;

  // DFS helper — recursive lambda using explicit self parameter.
  // Returns false on error.
  auto dfs = [&](auto &self,
                 const std::string &ip,
                 const std::string &src_dir) -> bool {
    if (in_progress.count(ip)) {
      error = std::format("import cycle detected involving '{}'", ip);
      return false;
    }
    if (index_map.count(ip))
      return true; // Already visited.

    in_progress.insert(ip);

    // Collect source files.
    auto sg_files = list_sg_files(src_dir);
    if (sg_files.empty()) {
      // Empty directories are treated as errors for explicit packages.
      // (The root package is always explicit; deps might be auto-resolved.)
      error = std::format("package '{}' contains no source files in '{}'",
                          ip, src_dir);
      in_progress.erase(ip);
      return false;
    }

    // Scan all imports across all source files.
    std::vector<std::string> all_deps;
    {
      std::unordered_set<std::string> seen;
      for (auto &sg : sg_files) {
        for (auto &dep : scan_imports(sg)) {
          if (seen.insert(dep).second)
            all_deps.push_back(dep);
        }
      }
    }

    // Recursively resolve each dependency.
    for (auto &dep : all_deps) {
      if (index_map.count(dep))
        continue; // Already in graph.
      std::string dep_dir = resolve_source_dir(dep, search_paths);
      if (dep_dir.empty()) {
        error = std::format("cannot find package '{}' (imported by '{}')",
                            dep, ip);
        in_progress.erase(ip);
        return false;
      }
      if (!self(self, dep, dep_dir)) {
        in_progress.erase(ip);
        return false;
      }
    }

    // Compute content hash over this package's sources + dep hashes.
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    uint64_t h = FNV_OFFSET;
    // Hash source files.
    std::string src_hash = compute_hash(sg_files);
    h = fnv1a_update(h, src_hash);
    // Mix in dependency content hashes (in sorted order for stability).
    std::vector<std::string> sorted_deps = all_deps;
    std::sort(sorted_deps.begin(), sorted_deps.end());
    for (auto &dep : sorted_deps) {
      auto it = index_map.find(dep);
      if (it != index_map.end())
        h = fnv1a_update(h, nodes[it->second].content_hash);
    }
    std::string content_hash = to_hex(h);

    // Derive package name (last component of import path).
    std::string pkg_name;
    auto slash = ip.rfind('/');
    pkg_name = (slash != std::string::npos) ? ip.substr(slash + 1) : ip;

    // Build output dir for this package (output_dir/<name>).
    std::string pkg_output_dir = output_dir + "/" + pkg_name;

    // Register node.
    index_map[ip] = nodes.size();
    nodes.push_back(
        Node{ip, pkg_name, src_dir, pkg_output_dir, all_deps, content_hash});

    in_progress.erase(ip);
    return true;
  };

  if (!dfs(dfs, import_path, source_dir))
    return false;

  // Fix up root node's output_dir to be output_dir itself (not nested).
  // The root package is always the last node added (DFS post-order means
  // deps come first, root last).
  if (!nodes.empty()) {
    nodes.back().output_dir = output_dir;
    nodes.back().name =
        (import_path.rfind('/') != std::string::npos)
            ? import_path.substr(import_path.rfind('/') + 1)
            : import_path;
  }

  return true;
}

// ---------------------------------------------------------------------------
// sorted — Kahn's algorithm for topological sort
// ---------------------------------------------------------------------------

std::vector<const BuildGraph::Node *> BuildGraph::sorted() const {
  // Build import_path → index map.
  std::unordered_map<std::string, size_t> idx;
  for (size_t i = 0; i < nodes.size(); ++i)
    idx[nodes[i].import_path] = i;

  // Compute in-degree for each node.
  std::vector<size_t> in_degree(nodes.size(), 0);
  std::unordered_map<size_t, std::vector<size_t>> dependents;
  // dependents[dep_idx] = list of nodes that depend on dep.

  for (size_t i = 0; i < nodes.size(); ++i) {
    for (auto &dep : nodes[i].deps) {
      auto it = idx.find(dep);
      if (it == idx.end())
        continue; // Dep not in this graph (e.g. resolved via .sgi only).
      size_t dep_idx = it->second;
      ++in_degree[i];
      dependents[dep_idx].push_back(i);
    }
  }

  // Queue all nodes with no incoming edges (leaves).
  std::queue<size_t> q;
  for (size_t i = 0; i < nodes.size(); ++i)
    if (in_degree[i] == 0)
      q.push(i);

  std::vector<const Node *> result;
  result.reserve(nodes.size());

  while (!q.empty()) {
    size_t cur = q.front();
    q.pop();
    result.push_back(&nodes[cur]);

    // For each node that depends on cur, reduce in-degree.
    auto it = dependents.find(cur);
    if (it == dependents.end())
      continue;
    for (size_t dep : it->second) {
      if (--in_degree[dep] == 0)
        q.push(dep);
    }
  }

  if (result.size() != nodes.size()) {
    // Cycle: not all nodes were drained.
    // const_cast is unavoidable here since sorted() is const but sets error.
    const_cast<BuildGraph *>(this)->error =
        "dependency cycle detected in build graph";
    return {};
  }

  return result;
}

} // namespace mc
