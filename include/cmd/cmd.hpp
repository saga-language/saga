// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

// Each subcommand receives the original binary path (prog), a shifted argc,
// and a shifted argv where argv[0] is the subcommand name (or the binary
// name in backwards-compat mode).  Argument parsing starts from argv[1].

int cmd_build(const char *prog, int argc, char **argv);
int cmd_check(const char *prog, int argc, char **argv);
int cmd_run(const char *prog, int argc, char **argv);
