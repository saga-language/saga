// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga run — compile to a temp binary and execute it.
// Args after '--' are passed to the program, not to the compiler.

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"

#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static void usage_run() {
  std::cerr << "Usage: saga run [options] <source.sg | directory> [-- <program-args>]\n"
               "\n"
               "Options:\n"
               "  -I <dir>          Add directory to package search path\n"
               "  --sgi-path <dir>  Add directory to .sgi search path\n"
               "  -v                Verbose compilation output\n";
}

int cmd_run(const char *prog, int argc, char **argv) {
  std::string source_path;
  bool verbose = false;
  std::vector<std::string> search_paths;
  std::vector<std::string> sgi_search_paths;
  std::vector<std::string> prog_args; // args passed to the compiled program

  bool past_dashdash = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (past_dashdash) {
      prog_args.push_back(arg);
      continue;
    }
    if (arg == "--") {
      past_dashdash = true;
      continue;
    }

    if (arg == "-v")               { verbose = true; }
    else if (arg == "--sgi-path" && i + 1 < argc)
      sgi_search_paths.push_back(argv[++i]);
    else if (arg == "-I" && i + 1 < argc)
      search_paths.push_back(argv[++i]);
    else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage_run();
      return 1;
    } else {
      source_path = arg;
    }
  }

  if (source_path.empty()) {
    usage_run();
    return 1;
  }

  // Apply deps from the nearest project.saga.
  apply_manifest_deps(prog, fs::current_path().string(), search_paths,
                      sgi_search_paths);

  // ── Compile ───────────────────────────────────────────────────────────────

  mc::FileSet fileset;
  std::string package_dir = load_sources(source_path, fileset);
  if (package_dir.empty() && fileset.files.empty())
    return 1;

  mc::Parser parser(fileset);
  auto ast = parser.parse();
  if (!ast) { std::cerr << "Error: parse failed\n"; return 1; }
  if (!parser.errors.errors.empty()) { parser.errors.print_errors(); return 1; }

  mc::Analyzer analyzer(fileset);
  setup_analyzer_paths(analyzer, package_dir, search_paths, sgi_search_paths,
                       prog);
  analyzer.analyze(*ast);
  if (!analyzer.errors.errors.empty()) { analyzer.errors.print_errors(); return 1; }

  fs::path input_path(source_path);
  std::string module_name = fs::is_directory(input_path)
      ? input_path.filename().string()
      : input_path.stem().string();

  mc::CodeGen codegen(module_name, analyzer);
  codegen.emit(*ast);

  // Write a temp object file.
  auto tmp_obj = fs::temp_directory_path() /
                 std::format("saga_run_{}.o", std::hash<std::string>{}(source_path));
  auto tmp_bin = fs::temp_directory_path() /
                 std::format("saga_run_{}", std::hash<std::string>{}(source_path));

  if (!codegen.write_object(tmp_obj.string())) {
    std::cerr << "Error: failed to emit object file\n";
    return 1;
  }

  std::string runtime_lib = saga_runtime_lib();
  std::string std_lib_path = saga_std_lib(prog);
  std::string std_lib_arg =
      fs::is_regular_file(std_lib_path) ? " " + std_lib_path : "";

  std::string dep_objects;
  std::string std_sgi_dir = saga_std_sgi_dir(prog);
  for (auto &[imp_path, dir] : analyzer.package_resolver->sgi_resolved_dirs) {
    if (!std_lib_arg.empty() && dir == std_sgi_dir)
      continue;
    auto slash = imp_path.rfind('/');
    std::string pname = slash != std::string::npos
                            ? imp_path.substr(slash + 1)
                            : imp_path;
    std::string dep_obj = dir + "/" + pname + ".o";
    if (fs::is_regular_file(dep_obj))
      dep_objects += " " + dep_obj;
  }

  std::string link_cmd =
      std::format("cc {} {} {}{} -o {} -no-pie", tmp_obj.string(), runtime_lib,
                  std_lib_arg, dep_objects, tmp_bin.string());
  if (verbose)
    std::cerr << std::format("  link: {}\n", link_cmd);

  if (std::system(link_cmd.c_str()) != 0) {
    std::cerr << "Error: linking failed\n";
    fs::remove(tmp_obj);
    return 1;
  }
  fs::remove(tmp_obj);

  // ── Execute ───────────────────────────────────────────────────────────────

  int exit_code = 1;

#ifndef _WIN32
  pid_t pid = fork();
  if (pid == 0) {
    // Child: build argv for execv.
    std::vector<std::string> exec_args_storage;
    exec_args_storage.push_back(tmp_bin.string());
    for (auto &a : prog_args)
      exec_args_storage.push_back(a);

    std::vector<char *> exec_argv;
    for (auto &s : exec_args_storage)
      exec_argv.push_back(const_cast<char *>(s.c_str()));
    exec_argv.push_back(nullptr);

    execv(exec_argv[0], exec_argv.data());
    // execv only returns on error.
    std::cerr << std::format("Error: failed to exec '{}'\n", tmp_bin.string());
    _exit(127);
  }

  if (pid < 0) {
    std::cerr << "Error: fork failed\n";
  } else {
    int status = 0;
    waitpid(pid, &status, 0);
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
#else
  // Windows fallback: use system() (no exec semantics).
  std::string run_cmd = tmp_bin.string();
  for (auto &a : prog_args)
    run_cmd += " " + a;
  exit_code = std::system(run_cmd.c_str());
#endif

  fs::remove(tmp_bin);
  return exit_code;
}
