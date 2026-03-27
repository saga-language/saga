// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/symbol.hpp"

namespace mc {

Symbol Symbol::variable(const std::string &name, TypePtr type, Span span,
                        bool is_public) {
  return Symbol{name,      SymbolKind::Variable, std::move(type),
                span,      is_public,            true,
                false};
}

Symbol Symbol::constant(const std::string &name, TypePtr type, Span span,
                        bool is_public) {
  return Symbol{name,      SymbolKind::Constant, std::move(type),
                span,      is_public,            false,
                false};
}

Symbol Symbol::function(const std::string &name, TypePtr type, Span span,
                        bool is_public) {
  return Symbol{name,      SymbolKind::Function, std::move(type),
                span,      is_public,            false,
                false};
}

Symbol Symbol::parameter(const std::string &name, TypePtr type, Span span) {
  return Symbol{name,  SymbolKind::Parameter, std::move(type),
                span,  false,                 false,
                false};
}

Symbol Symbol::type_sym(const std::string &name, TypePtr type, Span span,
                        bool is_public) {
  return Symbol{name,      SymbolKind::Type, std::move(type),
                span,      is_public,        false,
                false};
}

Symbol Symbol::type_param(const std::string &name, TypePtr type, Span span) {
  return Symbol{name,  SymbolKind::TypeParam, std::move(type),
                span,  false,                 false,
                false};
}

Symbol Symbol::enum_variant(const std::string &name, TypePtr type, Span span) {
  return Symbol{name,  SymbolKind::EnumVariant, std::move(type),
                span,  false,                   false,
                false};
}

Symbol Symbol::builtin(const std::string &name, SymbolKind kind,
                       TypePtr type) {
  return Symbol{name, kind, std::move(type), {0, 0}, true, false, true};
}

} // namespace mc
