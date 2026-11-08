// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "frontend/ast.hpp"

#include <optional>
#include <string>

namespace saga {

// Why an operand written without parentheses can't sit beside `op`, if it can't.
std::optional<std::string> left_operand_grouping_error(Token::Kind op,
                                                       const Node &operand);
std::optional<std::string> right_operand_grouping_error(Token::Kind op,
                                                        const Node &operand);

} // namespace saga
