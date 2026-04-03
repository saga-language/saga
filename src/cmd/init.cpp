// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga init — initialise a new project in the current directory.
//
// Creates:
//   project.saga      — manifest
//   main.sg           — entry point (if kind=binary)
//   lib.sg            — library stub (if kind=library)

#include "cmd/cmd.hpp"
#include "pkg/manifest.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void usage_init() {
  std::cerr << "Usage: saga init [options] [name]\n"
               "\n"
               "Options:\n"
               "  --type binary|library   Project kind (default: binary)\n"
               "\n"
               "If name is omitted, the current directory name is used.\n";
}

int cmd_init(const char * /*prog*/, int argc, char **argv) {
  std::string kind = "binary";
  std::string name;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--type" && i + 1 < argc) {
      kind = argv[++i];
      if (kind != "binary" && kind != "library") {
        std::cerr << std::format("Error: unknown type '{}'; expected binary or library\n",
                                 kind);
        return 1;
      }
    } else if (arg[0] == '-') {
      std::cerr << std::format("Unknown option: {}\n", arg);
      usage_init();
      return 1;
    } else {
      name = arg;
    }
  }

  // Default name from CWD.
  if (name.empty())
    name = fs::current_path().filename().string();

  // Check for existing manifest.
  if (fs::is_regular_file("project.saga")) {
    std::cerr << "Error: project.saga already exists\n";
    return 1;
  }

  // Write project.saga.
  mc::Manifest m;
  m.name = name;
  m.kind = kind;

  if (!m.save("project.saga")) {
    std::cerr << "Error: could not write project.saga\n";
    return 1;
  }

  // Write source stub.
  if (kind == "binary") {
    if (!fs::is_regular_file("main.sg")) {
      std::ofstream f("main.sg");
      f << std::format(
          "// {}\n"
          "\n"
          "pub fn Main() Void {{}}\n",
          name);
      if (!f.good()) {
        std::cerr << "Warning: could not write main.sg\n";
      }
    }
  } else {
    std::string lib_file = name + ".sg";
    if (!fs::is_regular_file(lib_file)) {
      std::ofstream f(lib_file);
      f << std::format(
          "// {}\n"
          "\n"
          "// TODO: add exported functions here\n",
          name);
      if (!f.good())
        std::cerr << std::format("Warning: could not write {}\n", lib_file);
    }
  }

  std::cerr << std::format("Initialised {} project '{}'\n", kind, name);
  return 0;
}
