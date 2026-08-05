// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/block_string.hpp"

#include <algorithm>

namespace saga {

namespace {

constexpr std::string_view kQuotes = R"(""")";
constexpr size_t kNone = std::string_view::npos;

bool is_indent(char c) { return c == ' ' || c == '\t'; }

bool all_indent(std::string_view s) { return std::ranges::all_of(s, is_indent); }

std::string_view drop_cr(std::string_view line) {
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  return line;
}

// A `"""` is in block form when its content starts on the next line. Otherwise
// it is the inline form, which is content verbatim and stays on one line.
bool opens_block(std::string_view first) {
  if (!first.starts_with(kQuotes))
    return false;
  std::string_view rest = first.substr(kQuotes.size());
  return rest.starts_with('\n') || rest.starts_with("\r\n");
}

// The whitespace between the last line break and the closing `"""`, which is
// the margin. Empty when the delimiter shares its line with content.
std::optional<std::string_view> closing_margin(std::string_view last) {
  if (!last.ends_with(kQuotes))
    return std::nullopt;
  last.remove_suffix(kQuotes.size());
  size_t nl = last.rfind('\n');
  if (nl == kNone)
    return std::nullopt;
  std::string_view indent = last.substr(nl + 1);
  if (!all_indent(indent))
    return std::nullopt;
  return indent;
}

// A line carries the margin verbatim, or holds nothing but whitespace. A line
// the fragment does not terminate runs on into an interpolation, so it has
// content no matter what its own text looks like.
bool line_ok(std::string_view line, std::string_view margin, bool terminated) {
  if (line.starts_with(margin))
    return true;
  return terminated && all_indent(line);
}

void check_lines(const RawFragment &frag, std::string_view margin, bool last,
                 std::vector<BlockStringError> &out) {
  size_t nl = frag.text.find('\n');
  while (nl != kNone) {
    const size_t start = nl + 1;
    nl = frag.text.find('\n', start);
    if (last && nl == kNone)
      break; // the closing delimiter's own line
    const size_t len = nl == kNone ? kNone : nl - start;
    if (!line_ok(drop_cr(frag.text.substr(start, len)), margin, nl != kNone))
      out.push_back({frag.offset + start,
                     "line is not indented to match the closing \"\"\""});
  }
}

// An inline `"""..."""` has no closing delimiter on its own line to take a
// margin from, so a line break inside it has no defined indentation.
void reject_line_breaks(std::span<const RawFragment> frags,
                        std::vector<BlockStringError> &out) {
  for (const auto &frag : frags) {
    if (size_t nl = frag.text.find('\n'); nl != kNone) {
      out.push_back(
          {frag.offset + nl,
           "a string that spans lines must open with \"\"\" and a line break"});
      return;
    }
  }
}

} // namespace

BlockStringLayout block_string_layout(std::span<const RawFragment> frags) {
  BlockStringLayout layout;
  if (frags.empty() || !frags.front().text.starts_with(kQuotes))
    return layout;

  if (!opens_block(frags.front().text)) {
    reject_line_breaks(frags, layout.errors);
    return layout;
  }

  const RawFragment &last = frags.back();
  auto margin = closing_margin(last.text);
  if (!margin) {
    layout.errors.push_back({last.offset + last.text.size() - kQuotes.size(),
                             "the closing \"\"\" must be on a line of its own"});
    return layout;
  }

  for (size_t i = 0; i < frags.size(); ++i)
    check_lines(frags[i], *margin, i + 1 == frags.size(), layout.errors);

  if (layout.errors.empty())
    layout.margin = margin->size();
  return layout;
}

} // namespace saga
