// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

// Helpers shared between the analyzer's translation units and nothing else.
// A helper lands here only when the decomposition puts its users in different
// files; anything used from one file stays file-local there. Not part of the
// analyzer's interface — nothing outside src/semantic/ should include this.

#include "frontend/ast.hpp"

#include <optional>
#include <string>

namespace saga {

/// The text of a plain (non-interpolated) string literal, else nothing.
/// Shared by enum resolution and enum checking, which sit either side of the
/// resolve/check split.
std::optional<std::string> plain_string_literal(const NodePtr &n);

} // namespace saga
