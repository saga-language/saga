// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "pkg/manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace mc {

// ---------------------------------------------------------------------------
// Minimal TOML-subset parser
// Handles:
//   [section]
//   key = "value"
//   key = { field = "value", field2 = "value2", ... }
//   # comments, blank lines
// ---------------------------------------------------------------------------

namespace {

// Trim leading/trailing whitespace.
static std::string trim(std::string_view sv) {
  size_t a = 0, b = sv.size();
  while (a < b && (sv[a] == ' ' || sv[a] == '\t')) ++a;
  while (b > a && (sv[b-1] == ' ' || sv[b-1] == '\t' || sv[b-1] == '\r')) --b;
  return std::string(sv.substr(a, b - a));
}

// Parse a quoted string starting after the opening `"`.
// pos should point to the first character of content (after `"`).
// Advances pos past the closing `"`. Returns the string.
static std::string parse_quoted(const std::string &s, size_t &pos) {
  std::string out;
  while (pos < s.size() && s[pos] != '"') {
    if (s[pos] == '\\' && pos + 1 < s.size()) {
      ++pos;
      switch (s[pos]) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case 'n':  out += '\n'; break;
        default:   out += s[pos]; break;
      }
    } else {
      out += s[pos];
    }
    ++pos;
  }
  if (pos < s.size()) ++pos; // skip closing "
  return out;
}

// Parse an inline table { key = "v", key2 = "v2" } from a string.
// Returns map of key → value.
static std::vector<std::pair<std::string, std::string>>
parse_inline_table(const std::string &s) {
  std::vector<std::pair<std::string, std::string>> result;
  size_t pos = 0;

  // skip leading whitespace and '{'
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
  if (pos < s.size() && s[pos] == '{') ++pos;

  while (pos < s.size()) {
    // skip whitespace and commas
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == ',' || s[pos] == '\r')) ++pos;
    if (pos >= s.size() || s[pos] == '}') break;

    // read key
    size_t key_start = pos;
    while (pos < s.size() && s[pos] != '=' && s[pos] != ' ' &&
           s[pos] != '\t') ++pos;
    std::string key = trim(s.substr(key_start, pos - key_start));

    // skip whitespace + '='
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '=')) ++pos;

    // read value (must be a quoted string for our subset)
    std::string val;
    if (pos < s.size() && s[pos] == '"') {
      ++pos;
      val = parse_quoted(s, pos);
    }

    if (!key.empty())
      result.push_back({key, val});
  }
  return result;
}

// Fill a ManifestDep from an inline table field list.
static ManifestDep dep_from_table(
    const std::string &name,
    const std::vector<std::pair<std::string, std::string>> &fields) {
  ManifestDep dep;
  dep.name = name;
  for (auto &[k, v] : fields) {
    if (k == "url")    dep.url    = v;
    else if (k == "commit") dep.commit = v;
    else if (k == "hash")   dep.hash   = v;
    else if (k == "branch") dep.branch = v;
    else if (k == "path")   dep.path   = v;
  }
  return dep;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Manifest::load
// ---------------------------------------------------------------------------

std::optional<Manifest> Manifest::load(const std::string &path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;

  Manifest m;
  std::string section;
  std::string line;

  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;

    // Section header
    if (t[0] == '[') {
      size_t end = t.find(']', 1);
      if (end != std::string::npos)
        section = trim(t.substr(1, end - 1));
      continue;
    }

    // key = ...
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;

    std::string key = trim(t.substr(0, eq));
    std::string val_raw = trim(t.substr(eq + 1));

    if (section == "package") {
      // Simple string values
      std::string val;
      if (!val_raw.empty() && val_raw[0] == '"') {
        size_t pos = 1;
        val = parse_quoted(val_raw, pos);
      } else {
        val = val_raw;
      }
      if (key == "name")        m.name        = val;
      else if (key == "kind")        m.kind        = val;
      else if (key == "description") m.description = val;
      else if (key == "license")     m.license     = val;

    } else if (section == "dependencies" || section == "tools") {
      // key = { ... }
      if (!val_raw.empty() && val_raw[0] == '{') {
        auto fields = parse_inline_table(val_raw);
        auto dep = dep_from_table(key, fields);
        if (section == "dependencies")
          m.dependencies.push_back(dep);
        else
          m.tools.push_back(dep);
      }
    }
  }

  return m;
}

// ---------------------------------------------------------------------------
// Manifest::save
// ---------------------------------------------------------------------------

// Escape a string for TOML output.
static std::string toml_quote(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out + "\"";
}

// Serialize one dep as an inline table.
static std::string dep_to_toml(const ManifestDep &dep) {
  std::string out = "{ ";
  bool first = true;
  auto add = [&](const std::string &k, const std::string &v) {
    if (v.empty()) return;
    if (!first) out += ", ";
    out += k + " = " + toml_quote(v);
    first = false;
  };
  if (dep.is_local()) {
    add("path", dep.path);
  } else {
    add("url",    dep.url);
    add("commit", dep.commit);
    add("hash",   dep.hash);
    add("branch", dep.branch);
  }
  return out + " }";
}

bool Manifest::save(const std::string &path) const {
  std::error_code ec;
  fs::path p(path);
  fs::create_directories(p.parent_path(), ec);

  std::ofstream out(path);
  if (!out) return false;

  out << "[package]\n";
  out << "name = " << toml_quote(name) << "\n";
  out << "kind = " << toml_quote(kind.empty() ? "binary" : kind) << "\n";
  if (!description.empty())
    out << "description = " << toml_quote(description) << "\n";
  if (!license.empty())
    out << "license = " << toml_quote(license) << "\n";

  if (!dependencies.empty()) {
    out << "\n[dependencies]\n";
    // Sort alphabetically (per tools.md spec).
    auto sorted = dependencies;
    std::sort(sorted.begin(), sorted.end(),
              [](auto &a, auto &b) { return a.name < b.name; });
    for (auto &dep : sorted)
      out << dep.name << " = " << dep_to_toml(dep) << "\n";
  }

  if (!tools.empty()) {
    out << "\n[tools]\n";
    auto sorted = tools;
    std::sort(sorted.begin(), sorted.end(),
              [](auto &a, auto &b) { return a.name < b.name; });
    for (auto &dep : sorted)
      out << dep.name << " = " << dep_to_toml(dep) << "\n";
  }

  return out.good();
}

// ---------------------------------------------------------------------------
// Manifest member helpers
// ---------------------------------------------------------------------------

ManifestDep *Manifest::find_dep(const std::string &n) {
  for (auto &d : dependencies)
    if (d.name == n) return &d;
  return nullptr;
}

const ManifestDep *Manifest::find_dep(const std::string &n) const {
  for (auto &d : dependencies)
    if (d.name == n) return &d;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

std::optional<std::string> find_manifest(const std::string &start) {
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(start, ec);
  if (ec || !fs::is_directory(dir)) dir = fs::path(start).parent_path();

  while (true) {
    auto candidate = dir / "project.saga";
    if (fs::is_regular_file(candidate))
      return candidate.string();
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return std::nullopt;
}

fs::path pkg_cache_dir(const ManifestDep &dep) {
  // Base: $HOME/.cache/saga/pkgs/<url>/<commit[:8]>
  const char *home = std::getenv("HOME");
  if (!home) home = "/tmp";

  std::string commit_short = dep.commit.size() >= 8
                                 ? dep.commit.substr(0, 8)
                                 : dep.commit;
  if (commit_short.empty()) commit_short = "HEAD";

  return fs::path(home) / ".cache" / "saga" / "pkgs" /
         dep.url / commit_short;
}

std::string pkg_name_from_url(const std::string &url) {
  // Last path segment, strip any trailing ".git"
  auto slash = url.rfind('/');
  std::string name = (slash != std::string::npos) ? url.substr(slash + 1) : url;
  if (name.ends_with(".git"))
    name = name.substr(0, name.size() - 4);
  return name;
}

std::pair<std::string, std::string> parse_pkg_url(const std::string &raw) {
  auto at = raw.rfind('@');
  if (at == std::string::npos)
    return {raw, ""};
  return {raw.substr(0, at), raw.substr(at + 1)};
}

} // namespace mc
