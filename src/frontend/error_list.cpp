#include "frontend/error_list.hpp"

#include <iostream>

namespace mc {
void ErrorList::error(Position p, std::string message) {
  if (errors.size() < max_errors) {
    errors.push_back(Error{p, message});
  }
}

void ErrorList::print() {
  for (auto err : errors) {
    std::cerr << std::format("{}\n", err);
  }

  if (errors.size() == max_errors - 1) {
    std::cerr << "Too many errors. Further messages suppressed.";
  }
}
} // namespace mc
