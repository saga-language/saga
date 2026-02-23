#include "frontend/error_list.hpp"

#include <iostream>

namespace mc {
void ErrorList::report_error(Position p, std::string message) {
  if (!max_reached()) {
    errors.push_back(Error{p, std::move(message)});
  }
}

bool ErrorList::max_reached() const { return errors.size() >= max_errors; }

void ErrorList::print_errors(std::ostream &os) const {
  for (auto err : errors) {
    os << std::format("[{}] Error: {}\n", err.p, err.message);
  }

  if (max_reached()) {
    os << "Max errors reached. Further messages suppressed.\n";
  }
}
} // namespace mc
