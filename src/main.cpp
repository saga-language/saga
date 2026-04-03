// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "cmd/cmd.hpp"

#include <format>
#include <iostream>
#include <string_view>

static void usage(const char *prog) {
  std::cerr << std::format("Usage: {} <command> [options] [args]\n\n"
                           "Commands:\n"
                           "  build   Compile a binary or library\n"
                           "  check   Type-check without generating code\n"
                           "  run     Compile and run immediately\n"
                           "  get     Download and install packages\n"
                           "  init    Initialise a new project\n"
                           "  lsp     Run the language server (LSP)\n"
                           "  doc     Serve package documentation over HTTP\n"
                           "  mcp     Run the MCP documentation server\n"
                           "\nRun '{} <command> --help' for command options.\n",
                           prog, prog);
}

int main(int argc, char **argv) {
  const char *prog = argv[0];

  if (argc >= 2) {
    std::string_view cmd = argv[1];
    if (cmd == "build") return cmd_build(prog, argc - 1, argv + 1);
    if (cmd == "check") return cmd_check(prog, argc - 1, argv + 1);
    if (cmd == "run")   return cmd_run  (prog, argc - 1, argv + 1);
    if (cmd == "get")   return cmd_get  (prog, argc - 1, argv + 1);
    if (cmd == "init")  return cmd_init (prog, argc - 1, argv + 1);
    if (cmd == "lsp")   return cmd_lsp  (prog, argc - 1, argv + 1);
    if (cmd == "doc")   return cmd_doc  (prog, argc - 1, argv + 1);
    if (cmd == "mcp")   return cmd_mcp  (prog, argc - 1, argv + 1);

    // If the first arg looks like a file/directory (not a known command),
    // treat the whole invocation as 'build' for backwards compatibility:
    //   compiler main.sg -o out
    if (cmd != "--help" && cmd != "-h") {
      return cmd_build(prog, argc, argv);
    }
  }

  usage(prog);
  return (argc >= 2) ? 0 : 1; // --help exits 0, no args exits 1
}
