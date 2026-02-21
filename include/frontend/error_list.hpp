
#include "position.hpp"

#include <format>
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

  void error(Position p, std::string message);
  void print();
};
} // namespace mc

template <> struct std::formatter<mc::Error> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }

  auto format(const mc::Error &err, std::format_context &ctx) const {
    return std::format_to(ctx.out(), "[{}] Error : {}", err.p, err.message);
  }
};
