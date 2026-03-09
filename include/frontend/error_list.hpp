
#pragma once

#include "position.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace mc {
struct Error {
  Position p;
  std::string message;
};

struct ErrorList {
  std::vector<Error> errors;
  int max_errors = 10;

  bool max_reached() const;
  void print_errors(std::ostream &os = std::cerr) const;
  void report_error(Position p, std::string message);
};
} // namespace mc
