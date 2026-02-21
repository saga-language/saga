#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
namespace mc {
    struct Symbol {
        std::string name;
    };

    struct Scope {
      std::shared_ptr<Scope> parent; // top-level scope is null
      std::unordered_map<std::string, Symbol> symbols;

      bool declare(const Symbol& sym);

      std::optional<Symbol> lookup(const std::string& name) const;
    };
}
