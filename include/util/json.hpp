// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// Minimal JSON library — header-only.
//
// Covers the subset of JSON needed for JSON-RPC 2.0 (LSP and MCP transports):
//   null, bool, int64, double, string, array, object.
//
// Usage:
//   json::Value v = json::parse(raw_string);
//   auto* field = v.get("method");
//   json::Value resp = json::obj({
//       {"jsonrpc", "2.0"},
//       {"id",      id},
//       {"result",  json::obj({{"hello", "world"}})},
//   });
//   std::string out = resp.dump();

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace json {

// ---------------------------------------------------------------------------
// Value — the discriminated JSON type.
// ---------------------------------------------------------------------------

struct Value {
  enum class Kind { Null, Bool, Int, Double, String, Array, Object };

  Kind kind = Kind::Null;

  bool   b_  = false;
  int64_t i_ = 0;
  double  d_ = 0.0;
  std::string s_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;

  // ── Constructors ──────────────────────────────────────────────────────

  Value() = default;                                         // null
  Value(bool b)        : kind(Kind::Bool),   b_(b)         {}
  Value(int i)         : kind(Kind::Int),    i_(i)         {}
  Value(int64_t i)     : kind(Kind::Int),    i_(i)         {}
  Value(double d)      : kind(Kind::Double), d_(d)         {}
  Value(const char* s) : kind(Kind::String), s_(s)         {}
  Value(std::string s) : kind(Kind::String), s_(std::move(s)) {}
  Value(std::string_view s) : kind(Kind::String), s_(s)   {}

  // ── Type predicates ───────────────────────────────────────────────────

  bool is_null()   const { return kind == Kind::Null; }
  bool is_bool()   const { return kind == Kind::Bool; }
  bool is_int()    const { return kind == Kind::Int; }
  bool is_double() const { return kind == Kind::Double; }
  bool is_string() const { return kind == Kind::String; }
  bool is_array()  const { return kind == Kind::Array; }
  bool is_object() const { return kind == Kind::Object; }

  // ── Value accessors ───────────────────────────────────────────────────

  bool               as_bool()   const { return b_; }
  int64_t            as_int()    const { return i_; }
  double             as_double() const { return kind == Kind::Int
                                             ? static_cast<double>(i_) : d_; }
  const std::string& as_string() const { return s_; }

  // ── Array accessors ───────────────────────────────────────────────────

  size_t       size()       const { return arr_.size(); }
  const Value& at(size_t i) const { return arr_[i]; }
  Value&       at(size_t i)       { return arr_[i]; }

  void push(Value v) { arr_.push_back(std::move(v)); }

  auto begin() const { return arr_.begin(); }
  auto end()   const { return arr_.end(); }

  // ── Object accessors ──────────────────────────────────────────────────

  // Return a pointer to the value for key, or nullptr if absent.
  const Value* get(std::string_view k) const {
    for (auto& [ek, ev] : obj_)
      if (ek == k) return &ev;
    return nullptr;
  }

  // Set a key in the object (overwrites if present).
  void set(std::string k, Value v) {
    for (auto& [ek, ev] : obj_)
      if (ek == k) { ev = std::move(v); return; }
    obj_.push_back({std::move(k), std::move(v)});
  }

  auto& fields() const { return obj_; }

  // Convenience: typed field access with defaults.
  std::string str(std::string_view k,
                  std::string def = "") const {
    auto* v = get(k);
    return (v && v->is_string()) ? v->as_string() : def;
  }
  int64_t integer(std::string_view k, int64_t def = 0) const {
    auto* v = get(k);
    return (v && v->is_int()) ? v->as_int() : def;
  }
  bool boolean(std::string_view k, bool def = false) const {
    auto* v = get(k);
    return (v && v->is_bool()) ? v->as_bool() : def;
  }

  // ── Serialisation ─────────────────────────────────────────────────────

  std::string dump() const {
    switch (kind) {
    case Kind::Null:   return "null";
    case Kind::Bool:   return b_ ? "true" : "false";
    case Kind::Int:    return std::to_string(i_);
    case Kind::Double: {
      // Avoid unnecessary trailing zeros.
      char buf[64];
      int n = std::snprintf(buf, sizeof(buf), "%.17g", d_);
      return std::string(buf, n < 0 ? 0 : n);
    }
    case Kind::String: {
      std::string out = "\"";
      for (char c : s_) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
      }
      return out + "\"";
    }
    case Kind::Array: {
      std::string out = "[";
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out += ",";
        out += arr_[i].dump();
      }
      return out + "]";
    }
    case Kind::Object: {
      std::string out = "{";
      for (size_t i = 0; i < obj_.size(); ++i) {
        if (i) out += ",";
        out += Value(obj_[i].first).dump() + ":" + obj_[i].second.dump();
      }
      return out + "}";
    }
    }
    return "null";
  }
};

// ---------------------------------------------------------------------------
// Factory helpers
// ---------------------------------------------------------------------------

inline Value obj(std::initializer_list<std::pair<std::string, Value>> fields) {
  Value v;
  v.kind = Value::Kind::Object;
  for (auto& [k, val] : fields)
    v.obj_.push_back({k, val});
  return v;
}

inline Value arr(std::initializer_list<Value> items) {
  Value v;
  v.kind = Value::Kind::Array;
  for (auto& item : items)
    v.arr_.push_back(item);
  return v;
}

inline Value make_array() {
  Value v;
  v.kind = Value::Kind::Array;
  return v;
}

inline Value make_object() {
  Value v;
  v.kind = Value::Kind::Object;
  return v;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace detail {

inline size_t skip_ws(std::string_view s, size_t pos) {
  while (pos < s.size() &&
         (s[pos] == ' ' || s[pos] == '\t' ||
          s[pos] == '\n' || s[pos] == '\r'))
    ++pos;
  return pos;
}

inline std::string parse_string(std::string_view s, size_t &pos) {
  ++pos; // skip opening "
  std::string out;
  while (pos < s.size() && s[pos] != '"') {
    if (s[pos] == '\\' && pos + 1 < s.size()) {
      ++pos;
      switch (s[pos]) {
      case '"':  out += '"';  break;
      case '\\': out += '\\'; break;
      case '/':  out += '/';  break;
      case 'n':  out += '\n'; break;
      case 'r':  out += '\r'; break;
      case 't':  out += '\t'; break;
      default:   out += s[pos]; break;
      }
    } else {
      out += s[pos];
    }
    ++pos;
  }
  if (pos < s.size()) ++pos; // skip closing "
  return out;
}

inline Value parse_value(std::string_view s, size_t &pos);

inline Value parse_value(std::string_view s, size_t &pos) {
  pos = skip_ws(s, pos);
  if (pos >= s.size()) return {};

  char c = s[pos];

  // Literals
  if (c == 'n' && pos + 3 < s.size()) { pos += 4; return {}; }
  if (c == 't' && pos + 3 < s.size()) { pos += 4; return Value(true); }
  if (c == 'f' && pos + 4 < s.size()) { pos += 5; return Value(false); }

  // String
  if (c == '"') return Value(parse_string(s, pos));

  // Array
  if (c == '[') {
    ++pos;
    Value v = make_array();
    pos = skip_ws(s, pos);
    if (pos < s.size() && s[pos] == ']') { ++pos; return v; }
    while (pos < s.size()) {
      v.push(parse_value(s, pos));
      pos = skip_ws(s, pos);
      if (pos >= s.size()) break;
      if (s[pos] == ']') { ++pos; break; }
      if (s[pos] == ',') ++pos;
    }
    return v;
  }

  // Object
  if (c == '{') {
    ++pos;
    Value v = make_object();
    pos = skip_ws(s, pos);
    if (pos < s.size() && s[pos] == '}') { ++pos; return v; }
    while (pos < s.size()) {
      pos = skip_ws(s, pos);
      if (pos >= s.size() || s[pos] != '"') break;
      std::string key = parse_string(s, pos);
      pos = skip_ws(s, pos);
      if (pos < s.size() && s[pos] == ':') ++pos;
      Value val = parse_value(s, pos);
      v.obj_.push_back({std::move(key), std::move(val)});
      pos = skip_ws(s, pos);
      if (pos >= s.size()) break;
      if (s[pos] == '}') { ++pos; break; }
      if (s[pos] == ',') ++pos;
    }
    return v;
  }

  // Number
  size_t start = pos;
  bool is_float = false;
  if (c == '-') ++pos;
  while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
  if (pos < s.size() && s[pos] == '.') { is_float = true; ++pos; }
  while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
  if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
    is_float = true; ++pos;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
  }
  if (pos == start) return {}; // unrecognised
  std::string num(s.substr(start, pos - start));
  if (is_float) return Value(std::stod(num));
  return Value(static_cast<int64_t>(std::stoll(num)));
}

} // namespace detail

inline Value parse(std::string_view s) {
  size_t pos = 0;
  return detail::parse_value(s, pos);
}

} // namespace json
