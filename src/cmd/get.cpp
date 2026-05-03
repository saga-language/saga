// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga get — download/install packages and update project.saga.
//
// Usage:
//   saga get                          # install all deps from manifest
//   saga get github.com/user/pkg      # download, compile, add to manifest
//   saga get github.com/user/pkg@ref  # specific branch/tag
//   saga get ../local/path            # add a local-path dependency

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"
#include "pkg/manifest.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void usage_get() {
  std::cerr <<
    "Usage: saga get [package-url[@ref] | local-path]\n"
    "\n"
    "  saga get                       Install all deps from project.saga\n"
    "  saga get github.com/user/pkg   Download and add remote package\n"
    "  saga get ../local/lib          Add a local-path dependency\n"
    "\n"
    "Options:\n"
    "  --alias <name>   Use this import name instead of the URL basename\n"
    "  -v               Verbose output\n";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Compile a single package to .o + .sgi using the compiler binary.
// Returns true on success.
static bool compile_dep(const char *prog,
                         const std::string &src_dir,
                         const std::string &pkg_name,
                         const std::string &out_dir,
                         const std::vector<std::string> &sgi_dirs,
                         bool verbose) {
  std::error_code ec;
  fs::create_directories(out_dir, ec);

  // Build compiler command.
  std::error_code ce;
  std::string binary = fs::weakly_canonical(prog, ce).string();
  if (ce) binary = prog;

  std::string cmd = std::format("\"{}\" --lib -o \"{}/{}.o\"",
                                binary, out_dir, pkg_name);
  for (auto &d : sgi_dirs)
    cmd += std::format(" --sgi-path \"{}\"", d);
  cmd += std::format(" \"{}\"", src_dir);

  if (verbose)
    std::cerr << std::format("  compile: {}\n", cmd);

  return std::system(cmd.c_str()) == 0;
}

// Run a git command and capture the first line of stdout.
// Returns empty string on failure.
static std::string git_stdout(const std::string &cmd) {
  auto *pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  char buf[256];
  std::string result;
  if (fgets(buf, sizeof(buf), pipe))
    result = buf;
  pclose(pipe);
  // Strip trailing newline
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
}

// ---------------------------------------------------------------------------
// Install a single remote dependency.
// ---------------------------------------------------------------------------

static int install_remote(const char *prog,
                           const std::string &raw_url,
                           const std::string &alias,
                           saga::Manifest &manifest,
                           bool verbose) {
  auto [url, ref] = saga::parse_pkg_url(raw_url);
  std::string pkg_name = alias.empty() ? saga::pkg_name_from_url(url) : alias;

  // Build clone URL.
  std::string clone_url = "https://" + url;
  if (!clone_url.ends_with(".git"))
    clone_url += ".git";

  // Shallow clone into a temp location to get the commit SHA first.
  // We'll rename to the final cache dir once we know the commit.
  std::string temp_dir = (fs::temp_directory_path() /
                          std::format("saga_get_{}", pkg_name)).string();
  fs::remove_all(temp_dir);
  fs::create_directories(temp_dir);

  std::string clone_cmd = std::format("git clone --depth 1{} {} \"{}\" 2>&1",
                                       ref.empty() ? "" : " --branch " + ref,
                                       clone_url, temp_dir);
  if (verbose)
    std::cerr << std::format("  clone: {}\n", clone_cmd);
  else
    clone_cmd += " > /dev/null 2>&1"; // suppress git output when not verbose

  if (std::system(clone_cmd.c_str()) != 0) {
    std::cerr << std::format("Error: git clone failed for '{}'\n", url);
    fs::remove_all(temp_dir);
    return 1;
  }

  // Read commit SHA.
  std::string commit = git_stdout(
      std::format("git -C \"{}\" rev-parse HEAD", temp_dir));
  if (commit.empty()) {
    std::cerr << "Error: could not determine commit SHA\n";
    fs::remove_all(temp_dir);
    return 1;
  }

  // Build final cache dir and move source there.
  saga::ManifestDep dep;
  dep.name   = pkg_name;
  dep.url    = url;
  dep.commit = commit;
  dep.branch = ref;

  fs::path cache = saga::pkg_cache_dir(dep);
  fs::path src_in_cache = cache / "src";

  if (!fs::is_directory(src_in_cache)) {
    std::error_code mec;
    fs::create_directories(cache, mec);
    fs::rename(temp_dir, src_in_cache, mec);
    if (mec) {
      // rename across filesystems fails; copy instead
      fs::copy(temp_dir, src_in_cache,
               fs::copy_options::recursive | fs::copy_options::overwrite_existing);
      fs::remove_all(temp_dir);
    }
  } else {
    fs::remove_all(temp_dir);
    if (verbose)
      std::cerr << std::format("  cached source at {}\n", src_in_cache.string());
  }

  // Compile unless already compiled.
  std::string sgi_path = (cache / (pkg_name + ".sgi")).string();
  std::string obj_path = (cache / (pkg_name + ".o")).string();
  if (!fs::is_regular_file(sgi_path) || !fs::is_regular_file(obj_path)) {
    // Pass the stdlib SGI dir so the package can import std.
    std::vector<std::string> sgi_dirs = {saga_std_sgi_dir(prog)};
    if (!compile_dep(prog, src_in_cache.string(), pkg_name,
                     cache.string(), sgi_dirs, verbose)) {
      std::cerr << std::format("Error: failed to compile '{}'\n", pkg_name);
      return 1;
    }
  } else if (verbose) {
    std::cerr << std::format("  cached artifacts at {}\n", cache.string());
  }

  // Update (or add) the dep in the manifest.
  auto *existing = manifest.find_dep(pkg_name);
  if (existing) {
    *existing = dep;
  } else {
    manifest.dependencies.push_back(dep);
  }

  std::cerr << std::format("Added {} @ {}\n", pkg_name, commit.substr(0, 12));
  return 0;
}

// ---------------------------------------------------------------------------
// Install a local-path dependency.
// ---------------------------------------------------------------------------

static int install_local(const std::string &raw_path,
                          const std::string &alias,
                          saga::Manifest &manifest,
                          const std::string &manifest_dir,
                          bool verbose) {
  fs::path abs_path = fs::weakly_canonical(raw_path);
  if (!fs::is_directory(abs_path)) {
    std::cerr << std::format("Error: '{}' is not a directory\n", raw_path);
    return 1;
  }

  std::string pkg_name = alias.empty() ? abs_path.filename().string() : alias;

  // Store relative path from manifest dir so the manifest is portable.
  std::error_code ec;
  fs::path rel = fs::relative(abs_path,
                               fs::path(manifest_dir), ec);
  std::string store_path = ec ? raw_path : rel.string();

  saga::ManifestDep dep;
  dep.name = pkg_name;
  dep.path = store_path;

  auto *existing = manifest.find_dep(pkg_name);
  if (existing) {
    *existing = dep;
  } else {
    manifest.dependencies.push_back(dep);
  }

  if (verbose)
    std::cerr << std::format("  local path: {}\n", abs_path.string());
  std::cerr << std::format("Added {} @ {}\n", pkg_name, store_path);
  return 0;
}

// ---------------------------------------------------------------------------
// Install all deps from an existing manifest (saga get with no args).
// ---------------------------------------------------------------------------

static int install_all(const char *prog,
                        saga::Manifest &manifest,
                        const std::string &manifest_dir,
                        bool verbose) {
  if (manifest.dependencies.empty() && manifest.tools.empty()) {
    std::cerr << "No dependencies declared in project.saga\n";
    return 0;
  }

  int failures = 0;
  for (auto &dep : manifest.dependencies) {
    if (dep.is_local()) {
      // Validate path exists; nothing to download.
      fs::path p = fs::path(manifest_dir) / dep.path;
      if (!fs::is_directory(p)) {
        std::cerr << std::format("Error: local dep '{}' not found at '{}'\n",
                                 dep.name, p.string());
        ++failures;
      } else if (verbose) {
        std::cerr << std::format("  {} → {} (local)\n", dep.name, p.string());
      }
      continue;
    }

    // Remote dep: ensure compiled.
    fs::path cache = saga::pkg_cache_dir(dep);
    std::string sgi_path = (cache / (dep.name + ".sgi")).string();
    std::string obj_path = (cache / (dep.name + ".o")).string();

    if (fs::is_regular_file(sgi_path) && fs::is_regular_file(obj_path)) {
      if (verbose)
        std::cerr << std::format("  {} already installed\n", dep.name);
      continue;
    }

    // Need to download.
    std::string raw = dep.branch.empty()
                          ? dep.url
                          : dep.url + "@" + dep.branch;
    saga::Manifest tmp = manifest;
    int rc = install_remote(prog, raw, dep.name, tmp, verbose);
    if (rc != 0) ++failures;
    else {
      // Update commit if it changed.
      if (auto *nd = tmp.find_dep(dep.name)) dep = *nd;
    }
  }

  return failures > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// cmd_get
// ---------------------------------------------------------------------------

int cmd_get(const char *prog, int argc, char **argv) {
  std::string pkg_arg;
  std::string alias;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-v")               { verbose = true; }
    else if (arg == "--alias" && i + 1 < argc) alias = argv[++i];
    else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage_get();
      return 1;
    } else {
      pkg_arg = arg;
    }
  }

  // Find nearest project.saga.
  auto manifest_path_opt = saga::find_manifest(fs::current_path().string());
  std::string manifest_path;
  saga::Manifest manifest;

  if (manifest_path_opt) {
    manifest_path = *manifest_path_opt;
    auto m = saga::Manifest::load(manifest_path);
    if (m) manifest = std::move(*m);
  } else {
    // No manifest found — create one in CWD if we're adding a new dep.
    if (!pkg_arg.empty()) {
      manifest_path = (fs::current_path() / "project.saga").string();
      manifest.name = fs::current_path().filename().string();
      manifest.kind = "binary";
      std::cerr << "No project.saga found; creating one in current directory\n";
    } else {
      std::cerr << "Error: no project.saga found\n";
      return 1;
    }
  }

  std::string manifest_dir = fs::path(manifest_path).parent_path().string();

  int rc = 0;

  if (pkg_arg.empty()) {
    // Install all deps.
    rc = install_all(prog, manifest, manifest_dir, verbose);
  } else if (pkg_arg.starts_with("../") || pkg_arg.starts_with("./") ||
             pkg_arg.starts_with("/") ||
             (pkg_arg.size() > 1 && pkg_arg[1] == ':')) {
    // Local path (relative or absolute).
    rc = install_local(pkg_arg, alias, manifest, manifest_dir, verbose);
  } else {
    // Remote URL.
    rc = install_remote(prog, pkg_arg, alias, manifest, verbose);
  }

  // Save updated manifest.
  if (rc == 0 && !pkg_arg.empty()) {
    if (!manifest.save(manifest_path)) {
      std::cerr << std::format("Error: could not write '{}'\n", manifest_path);
      return 1;
    }
  }

  return rc;
}
