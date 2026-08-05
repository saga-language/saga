// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

// Helpers shared between the analyzer's translation units and nothing else.
// A helper lands here only when the decomposition puts its users in different
// files; anything used from one file stays file-local there. Not part of the
// analyzer's interface — nothing outside src/semantic/ should include this.

#include "frontend/ast.hpp"
#include "semantic/types.hpp"

#include <optional>
#include <string>

namespace saga {

/// The text of a plain (non-interpolated) string literal, else nothing.
/// Shared by enum resolution and enum checking, which sit either side of the
/// resolve/check split.
std::optional<std::string> plain_string_literal(const NodePtr &n);

/// The declared name of a type declaration node (struct, enum, interface,
/// alias), else nothing. Declaration collection needs it to register the name
/// and type resolution needs it to find the declaration again.
std::optional<std::string> type_decl_name(const Node &node);

/// Rewrite a generic receiver's signature so its element type reads as the
/// receiver kind's own parameter. The prelude loader and the cross-package
/// method loader both need it, and they sit either side of the import split.
TypePtr normalize_generic_receiver_sig(const TypePtr &t, TypeKind recv_kind);

} // namespace saga
