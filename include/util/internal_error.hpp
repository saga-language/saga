// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace saga {

/// The compiler broke a promise it had already made to itself — the analyzer
/// accepted the program and a later stage found it could not proceed. Neither
/// valid code nor a user error, so it goes to neither channel: it names the
/// compiler and stops, rather than emitting something built on the broken
/// assumption. Exits rather than aborts, so a user who trips one gets a
/// message instead of a core dump.
[[noreturn]] inline void internal_error(std::string_view what) {
  std::fprintf(stderr,
               "internal compiler error: %.*s\n"
               "This is a bug in the compiler, not an error in your code.\n",
               static_cast<int>(what.size()), what.data());
  std::exit(70); // EX_SOFTWARE
}

} // namespace saga
