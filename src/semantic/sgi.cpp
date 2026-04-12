// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "semantic/sgi.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>

namespace mc {

// ===========================================================================
// Type serialization → .sgi text
// ===========================================================================

std::string type_to_sgi(const TypePtr &t) {
  if (!t)
    return "Void";

  switch (t->kind) {
  case TypeKind::Void:
    return "Void";
  case TypeKind::Bool:
    return "Bool";
  case TypeKind::Int: {
    auto &info = std::get<IntType>(t->detail);
    if (info.bits == 0)
      return info.is_signed ? "Int" : "Byte";
    if (!info.is_signed)
      return "Uint" + std::to_string(info.bits);
    return "Int" + std::to_string(info.bits);
  }
  case TypeKind::Float: {
    auto &info = std::get<FloatType>(t->detail);
    if (info.bits == 0)
      return "Float";
    return "Float" + std::to_string(info.bits);
  }
  case TypeKind::String:
    return "String";
  case TypeKind::Error:
    return "Void"; // error types shouldn't appear in .sgi

  case TypeKind::Array: {
    auto &info = std::get<ArrayTypeInfo>(t->detail);
    return "[" + type_to_sgi(info.element) + "]";
  }

  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(t->detail);
    return "{" + type_to_sgi(info.key) + ": " + type_to_sgi(info.value) + "}";
  }

  case TypeKind::Range: {
    auto &info = std::get<RangeTypeInfo>(t->detail);
    return "Range|" + type_to_sgi(info.element) + "|";
  }

  case TypeKind::Func: {
    auto &info = std::get<FuncTypeInfo>(t->detail);
    std::ostringstream os;
    os << "fn(";
    for (size_t i = 0; i < info.params.size(); ++i) {
      if (i > 0)
        os << ", ";
      if (info.is_variadic && i == info.params.size() - 1)
        os << "...";
      os << type_to_sgi(info.params[i]);
    }
    os << ")";
    if (info.returns.size() == 1) {
      os << " " << type_to_sgi(info.returns[0]);
    } else if (info.returns.size() > 1) {
      os << " (";
      for (size_t i = 0; i < info.returns.size(); ++i) {
        if (i > 0)
          os << ", ";
        os << type_to_sgi(info.returns[i]);
      }
      os << ")";
    }
    return os.str();
  }

  case TypeKind::Struct: {
    auto &info = std::get<StructTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Enum: {
    auto &info = std::get<EnumTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Interface: {
    auto &info = std::get<InterfaceTypeInfo>(t->detail);
    return info.name;
  }

  case TypeKind::Union: {
    auto &info = std::get<UnionTypeInfo>(t->detail);
    std::ostringstream os;
    for (size_t i = 0; i < info.alternatives.size(); ++i) {
      if (i > 0)
        os << " | ";
      os << type_to_sgi(info.alternatives[i]);
    }
    return os.str();
  }

  case TypeKind::TypeParam: {
    auto &info = std::get<TypeParamInfo>(t->detail);
    return info.param.name;
  }

  case TypeKind::Module:
    return "Void"; // modules shouldn't appear in export types
  }

  return "Void";
}

// ===========================================================================
// .sgi writer — serialize exports to text
// ===========================================================================

/// Write a doc comment block. Each line gets a "// " prefix.
static void write_doc(std::ostringstream &os, const std::string &doc) {
  if (doc.empty())
    return;
  std::istringstream lines(doc);
  std::string line;
  while (std::getline(lines, line)) {
    os << "// " << line << "\n";
  }
}

/// Serialize a function type as a top-level func declaration.
static void write_func_export(std::ostringstream &os, const std::string &name,
                               const TypePtr &t, const std::string &doc) {
  write_doc(os, doc);
  auto &info = std::get<FuncTypeInfo>(t->detail);
  os << "func " << name << "(";
  for (size_t i = 0; i < info.params.size(); ++i) {
    if (i > 0)
      os << ", ";
    if (info.is_variadic && i == info.params.size() - 1) {
      os << "...";
      // The variadic param is already array-wrapped internally;
      // unwrap it for the .sgi since "..." implies the array.
      if (info.params[i]->kind == TypeKind::Array) {
        auto &arr = std::get<ArrayTypeInfo>(info.params[i]->detail);
        os << type_to_sgi(arr.element);
      } else {
        os << type_to_sgi(info.params[i]);
      }
    } else {
      os << type_to_sgi(info.params[i]);
    }
  }
  os << ")";
  if (!info.returns.empty()) {
    if (info.returns.size() == 1 &&
        info.returns[0]->kind != TypeKind::Void) {
      os << " " << type_to_sgi(info.returns[0]);
    } else if (info.returns.size() > 1) {
      os << " (";
      for (size_t i = 0; i < info.returns.size(); ++i) {
        if (i > 0)
          os << ", ";
        os << type_to_sgi(info.returns[i]);
      }
      os << ")";
    }
  }
  os << "\n";
}

/// Serialize a struct type as a struct block.
static void write_struct_export(std::ostringstream &os,
                                 const std::string &name, const TypePtr &t,
                                 const std::string &doc) {
  write_doc(os, doc);
  auto &info = std::get<StructTypeInfo>(t->detail);
  os << "struct " << name;

  // Generic parameters
  if (!info.type_params.empty()) {
    os << "|";
    for (size_t i = 0; i < info.type_params.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << info.type_params[i].name;
    }
    os << "|";
  }

  os << " {\n";

  // Fields (both public and private, for layout compatibility)
  for (auto &field : info.fields) {
    if (field.is_public)
      os << "  pub ";
    else
      os << "  ";
    os << field.name << " " << type_to_sgi(field.type) << "\n";
  }

  // Embeds
  for (auto &embed : info.embeds) {
    os << "  embed " << type_to_sgi(embed) << "\n";
  }

  // Public methods
  for (auto &method : info.methods) {
    if (method.is_public) {
      auto &sig = std::get<FuncTypeInfo>(method.signature->detail);
      os << "  pub fn " << method.name << "(";
      for (size_t i = 0; i < sig.params.size(); ++i) {
        if (i > 0)
          os << ", ";
        if (sig.is_variadic && i == sig.params.size() - 1)
          os << "...";
        os << type_to_sgi(sig.params[i]);
      }
      os << ")";
      if (!sig.returns.empty()) {
        if (sig.returns.size() == 1 &&
            sig.returns[0]->kind != TypeKind::Void) {
          os << " " << type_to_sgi(sig.returns[0]);
        } else if (sig.returns.size() > 1) {
          os << " (";
          for (size_t i = 0; i < sig.returns.size(); ++i) {
            if (i > 0)
              os << ", ";
            os << type_to_sgi(sig.returns[i]);
          }
          os << ")";
        }
      }
      os << "\n";
    }
  }

  os << "}\n";
}

/// Serialize an enum type.
static void write_enum_export(std::ostringstream &os, const std::string &name,
                               const TypePtr &t, const std::string &doc) {
  write_doc(os, doc);
  auto &info = std::get<EnumTypeInfo>(t->detail);
  os << "enum " << name << " {";

  for (size_t i = 0; i < info.variants.size(); ++i) {
    if (i > 0)
      os << ";";
    os << " " << info.variants[i].name;
    // Emit variant data: index and/or associated fields.
    bool has_data_fields = false;
    for (auto &f : info.variants[i].fields) {
      if (f.name != "index") { has_data_fields = true; break; }
    }
    if (has_data_fields || info.variants[i].index >= 0) {
      os << "(";
      bool first = true;
      // Emit explicit index if present.
      if (info.variants[i].index >= 0) {
        os << "#" << info.variants[i].index;
        first = false;
      }
      // Emit associated data fields (skip "index" metadata fields).
      for (auto &f : info.variants[i].fields) {
        if (f.name == "index") continue;
        if (!first) os << ", ";
        if (!f.name.empty()) os << f.name << " ";
        os << type_to_sgi(f.type);
        first = false;
      }
      os << ")";
    }
  }
  os << " }\n";
}

/// Serialize an interface type.
static void write_interface_export(std::ostringstream &os,
                                    const std::string &name, const TypePtr &t,
                                    const std::string &doc) {
  write_doc(os, doc);
  auto &info = std::get<InterfaceTypeInfo>(t->detail);
  os << "interface " << name;

  if (!info.type_params.empty()) {
    os << "|";
    for (size_t i = 0; i < info.type_params.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << info.type_params[i].name;
    }
    os << "|";
  }

  os << " {\n";
  for (auto &method : info.methods) {
    auto &sig = std::get<FuncTypeInfo>(method.signature->detail);
    os << "  fn " << method.name << "(";
    for (size_t i = 0; i < sig.params.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << type_to_sgi(sig.params[i]);
    }
    os << ")";
    if (!sig.returns.empty()) {
      if (sig.returns.size() == 1 &&
          sig.returns[0]->kind != TypeKind::Void) {
        os << " " << type_to_sgi(sig.returns[0]);
      } else if (sig.returns.size() > 1) {
        os << " (";
        for (size_t i = 0; i < sig.returns.size(); ++i) {
          if (i > 0)
            os << ", ";
          os << type_to_sgi(sig.returns[i]);
        }
        os << ")";
      }
    }
    os << "\n";
  }
  os << "}\n";
}

/// Serialize a constant export (type only, no value).
static void write_const_export(std::ostringstream &os, const std::string &name,
                                const TypePtr &t, const std::string &doc) {
  write_doc(os, doc);
  os << "const " << name << " " << type_to_sgi(t) << "\n";
}

/// Serialize a receiver-methods block for one intrinsic type.
static void write_receiver_methods(std::ostringstream &os,
                                    const SgiReceiverMethod &rm) {
  os << "methods " << rm.type_name << " {\n";
  for (auto &method : rm.methods) {
    if (!method.is_public)
      continue;
    auto &sig = std::get<FuncTypeInfo>(method.signature->detail);
    os << "  pub fn " << method.name << "(";
    for (size_t i = 0; i < sig.params.size(); ++i) {
      if (i > 0)
        os << ", ";
      if (sig.is_variadic && i == sig.params.size() - 1)
        os << "...";
      os << type_to_sgi(sig.params[i]);
    }
    os << ")";
    if (!sig.returns.empty()) {
      if (sig.returns.size() == 1 &&
          sig.returns[0]->kind != TypeKind::Void) {
        os << " " << type_to_sgi(sig.returns[0]);
      } else if (sig.returns.size() > 1) {
        os << " (";
        for (size_t i = 0; i < sig.returns.size(); ++i) {
          if (i > 0)
            os << ", ";
          os << type_to_sgi(sig.returns[i]);
        }
        os << ")";
      }
    }
    os << "\n";
  }
  os << "}\n";
}

std::string generate_sgi(const std::string &package_name,
                          const std::vector<SgiImport> &imports,
                          const std::vector<SgiExport> &exports,
                          const std::vector<SgiReceiverMethod> &receiver_methods) {
  std::ostringstream os;
  os << "sgi 1\n";
  os << "package " << package_name << "\n";

  // Imports
  for (auto &imp : imports) {
    os << "import " << imp.name << " \"" << imp.import_path << "\"\n";
  }

  if (!imports.empty() || !exports.empty())
    os << "\n";

  // Sort exports for deterministic output: types (struct/enum/interface)
  // first, then functions, then constants/values.
  struct SortedExport {
    int order; // 0=struct, 1=enum, 2=interface, 3=func, 4=const/other
    size_t index;
  };
  std::vector<SortedExport> sorted;
  for (size_t i = 0; i < exports.size(); ++i) {
    int order = 4;
    if (exports[i].type && exports[i].is_type) {
      // Type exports: categorize by their type kind.
      switch (exports[i].type->kind) {
      case TypeKind::Struct:
        order = 0;
        break;
      case TypeKind::Enum:
        order = 1;
        break;
      case TypeKind::Interface:
        order = 2;
        break;
      default:
        order = 4;
        break;
      }
    } else if (exports[i].type && exports[i].type->kind == TypeKind::Func) {
      order = 3;
    }
    sorted.push_back({order, i});
  }
  std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) {
    if (a.order != b.order)
      return a.order < b.order;
    return a.index < b.index;
  });

  for (size_t si = 0; si < sorted.size(); ++si) {
    auto &exp = exports[sorted[si].index];
    if (!exp.type)
      continue;

    if (si > 0)
      os << "\n";

    if (exp.is_type) {
      // Type export: serialize the full type definition.
      switch (exp.type->kind) {
      case TypeKind::Struct:
        write_struct_export(os, exp.name, exp.type, exp.doc);
        break;
      case TypeKind::Enum:
        write_enum_export(os, exp.name, exp.type, exp.doc);
        break;
      case TypeKind::Interface:
        write_interface_export(os, exp.name, exp.type, exp.doc);
        break;
      default:
        write_const_export(os, exp.name, exp.type, exp.doc);
        break;
      }
    } else if (exp.type->kind == TypeKind::Func) {
      write_func_export(os, exp.name, exp.type, exp.doc);
    } else {
      // Value export: always serialize as const.
      write_const_export(os, exp.name, exp.type, exp.doc);
    }
  }

  // Receiver methods for intrinsic types (stdlib packages only).
  for (auto &rm : receiver_methods) {
    if (!rm.methods.empty()) {
      os << "\n";
      write_receiver_methods(os, rm);
    }
  }

  return os.str();
}

bool write_sgi(const std::string &path, const std::string &package_name,
               const std::vector<SgiImport> &imports,
               const std::vector<SgiExport> &exports,
               const std::vector<SgiReceiverMethod> &receiver_methods) {
  std::ofstream out(path);
  if (!out)
    return false;
  out << generate_sgi(package_name, imports, exports, receiver_methods);
  return out.good();
}

// ===========================================================================
// .sgi reader — parse text back into types
// ===========================================================================

namespace {

/// Simple tokenizer/cursor for parsing .sgi text.
struct SgiParser {
  std::string content;
  size_t pos = 0;

  // Types defined so far in this .sgi — lets const/func declarations
  // reference struct/enum/interface types by name without producing stubs.
  std::unordered_map<std::string, TypePtr> defined_types;

  bool at_end() const { return pos >= content.size(); }

  char peek() const { return at_end() ? '\0' : content[pos]; }

  char advance() { return at_end() ? '\0' : content[pos++]; }

  void skip_whitespace() {
    while (!at_end() && (content[pos] == ' ' || content[pos] == '\t'))
      ++pos;
  }

  void skip_line() {
    while (!at_end() && content[pos] != '\n')
      ++pos;
    if (!at_end())
      ++pos; // skip newline
  }

  /// Read a word (alphanumeric + underscore, with optional trailing '?').
  std::string read_word() {
    skip_whitespace();
    size_t start = pos;
    while (!at_end() && (std::isalnum(content[pos]) || content[pos] == '_'))
      ++pos;
    // Include trailing '?' for boolean-convention names like Key?.
    if (!at_end() && content[pos] == '?')
      ++pos;
    return content.substr(start, pos - start);
  }

  /// Read a quoted string (assumes pos is at the opening quote).
  std::string read_quoted() {
    skip_whitespace();
    if (at_end() || content[pos] != '"')
      return "";
    ++pos; // skip opening quote
    size_t start = pos;
    while (!at_end() && content[pos] != '"')
      ++pos;
    std::string result = content.substr(start, pos - start);
    if (!at_end())
      ++pos; // skip closing quote
    return result;
  }

  /// Read a number.
  int read_int() {
    skip_whitespace();
    size_t start = pos;
    while (!at_end() && std::isdigit(content[pos]))
      ++pos;
    return std::stoi(content.substr(start, pos - start));
  }

  /// Check if the next non-whitespace chars match the given string.
  bool match(const std::string &s) {
    skip_whitespace();
    if (pos + s.size() > content.size())
      return false;
    if (content.substr(pos, s.size()) == s) {
      pos += s.size();
      return true;
    }
    return false;
  }

  /// Read until end of line, trim leading/trailing whitespace.
  std::string read_rest_of_line() {
    skip_whitespace();
    size_t start = pos;
    while (!at_end() && content[pos] != '\n')
      ++pos;
    size_t end = pos;
    if (!at_end())
      ++pos; // skip newline
    // Trim trailing whitespace
    while (end > start && (content[end - 1] == ' ' || content[end - 1] == '\t'
                           || content[end - 1] == '\r'))
      --end;
    return content.substr(start, end - start);
  }

  /// Read consecutive // comment lines as a doc comment block.
  std::string read_doc_comment() {
    std::string doc;
    while (true) {
      skip_whitespace();
      size_t saved = pos;
      if (at_end() || content[pos] != '/') {
        pos = saved;
        break;
      }
      ++pos;
      if (at_end() || content[pos] != '/') {
        pos = saved;
        break;
      }
      ++pos;
      // Skip single space after //
      if (!at_end() && content[pos] == ' ')
        ++pos;
      // Read the rest of the line
      size_t start = pos;
      while (!at_end() && content[pos] != '\n')
        ++pos;
      size_t end = pos;
      if (!at_end())
        ++pos; // skip newline
      // Trim trailing whitespace
      while (end > start &&
             (content[end - 1] == ' ' || content[end - 1] == '\r'))
        --end;
      if (!doc.empty())
        doc += "\n";
      doc += content.substr(start, end - start);
    }
    return doc;
  }

  /// Skip blank lines.
  void skip_blank_lines() {
    while (!at_end()) {
      size_t saved = pos;
      skip_whitespace();
      if (at_end() || content[pos] == '\n') {
        if (!at_end())
          ++pos;
        continue;
      }
      pos = saved;
      break;
    }
  }

  // ── Type parsing ──────────────────────────────────────────────────

  /// Parse a type expression. This handles:
  /// - Primitive: Void, Bool, Int, Float, String, Byte, Int8, etc.
  /// - Array: [Element]
  /// - Map: {Key: Value}
  /// - Function: fn(Params) Returns
  /// - Union: A | B | C
  /// - Range: Range|Element|
  /// - Named: StructName, EnumName, etc.
  TypePtr parse_type() {
    skip_whitespace();
    if (at_end())
      return nullptr;

    TypePtr t = parse_single_type();
    if (!t)
      return nullptr;

    // Check for union: Type | Type | ...
    skip_whitespace();
    if (!at_end() && content[pos] == '|') {
      std::vector<TypePtr> alts;
      alts.push_back(t);
      while (!at_end() && content[pos] == '|') {
        ++pos; // skip |
        // Disambiguate: if next char is not a space/alpha, this might be
        // a Range|T| delimiter, not a union.
        skip_whitespace();
        auto next = parse_single_type();
        if (!next)
          break;
        alts.push_back(next);
        skip_whitespace();
      }
      if (alts.size() > 1)
        return make_union_type(std::move(alts));
    }

    return t;
  }

  TypePtr parse_single_type() {
    skip_whitespace();
    if (at_end())
      return nullptr;

    char c = content[pos];

    // Array: [Element]
    if (c == '[') {
      ++pos;
      auto elem = parse_type();
      skip_whitespace();
      if (!at_end() && content[pos] == ']')
        ++pos;
      return elem ? make_array_type(elem) : nullptr;
    }

    // Map: {Key: Value}
    if (c == '{') {
      ++pos;
      auto key = parse_type();
      skip_whitespace();
      if (!at_end() && content[pos] == ':')
        ++pos;
      auto val = parse_type();
      skip_whitespace();
      if (!at_end() && content[pos] == '}')
        ++pos;
      return (key && val) ? make_map_type(key, val) : nullptr;
    }

    // Function: fn(Params) Returns
    if (c == 'f' && pos + 1 < content.size() && content[pos + 1] == 'n' &&
        (pos + 2 >= content.size() || content[pos + 2] == '(')) {
      pos += 2;
      return parse_func_type();
    }

    // Variadic marker: ...Type
    if (c == '.' && pos + 2 < content.size() && content[pos + 1] == '.' &&
        content[pos + 2] == '.') {
      // Don't consume here — handled by the param list parser
      return nullptr;
    }

    // Named type or keyword
    std::string name = read_word();
    if (name.empty())
      return nullptr;

    // Primitives
    if (name == "Void")
      return make_void_type();
    if (name == "Bool")
      return make_bool_type();
    if (name == "Int")
      return make_int_type(0, true);
    if (name == "Int8")
      return make_int_type(8, true);
    if (name == "Int16")
      return make_int_type(16, true);
    if (name == "Int32")
      return make_int_type(32, true);
    if (name == "Int64")
      return make_int_type(64, true);
    if (name == "Byte")
      return make_int_type(0, false);
    if (name == "Uint8")
      return make_int_type(8, false);
    if (name == "Uint16")
      return make_int_type(16, false);
    if (name == "Uint32")
      return make_int_type(32, false);
    if (name == "Uint64")
      return make_int_type(64, false);
    if (name == "Float")
      return make_float_type(0);
    if (name == "Float32")
      return make_float_type(32);
    if (name == "Float64")
      return make_float_type(64);
    if (name == "String")
      return make_string_type();

    // Range|Element|
    if (name == "Range") {
      skip_whitespace();
      if (!at_end() && content[pos] == '|') {
        ++pos;
        auto elem = parse_single_type();
        skip_whitespace();
        if (!at_end() && content[pos] == '|')
          ++pos;
        return elem ? make_range_type(elem) : nullptr;
      }
    }

    // Named type reference: check if it was already defined in this .sgi.
    // If so, return the real type so methods/fields are visible.
    auto it = defined_types.find(name);
    if (it != defined_types.end())
      return it->second;

    // Otherwise return a stub with just the name — resolved later if needed.
    return make_struct_type(name);
  }

  TypePtr parse_func_type() {
    skip_whitespace();
    if (at_end() || content[pos] != '(')
      return nullptr;
    ++pos; // skip (

    std::vector<TypePtr> params;
    bool is_variadic = false;

    skip_whitespace();
    if (!at_end() && content[pos] != ')') {
      while (true) {
        skip_whitespace();
        // Check for variadic
        if (!at_end() && content[pos] == '.' && pos + 2 < content.size() &&
            content[pos + 1] == '.' && content[pos + 2] == '.') {
          pos += 3;
          is_variadic = true;
        }
        auto pt = parse_type();
        if (pt) {
          // Variadic params are stored array-wrapped internally.
          if (is_variadic && pt->kind != TypeKind::Array)
            pt = make_array_type(std::move(pt));
          params.push_back(pt);
        }
        skip_whitespace();
        if (at_end() || content[pos] == ')')
          break;
        if (content[pos] == ',')
          ++pos; // skip comma
      }
    }

    skip_whitespace();
    if (!at_end() && content[pos] == ')')
      ++pos;

    // Return types
    std::vector<TypePtr> returns;
    skip_whitespace();
    if (!at_end() && content[pos] != '\n' && content[pos] != '}' &&
        content[pos] != ')') {
      // Check for multiple return types: (A, B)
      if (content[pos] == '(') {
        ++pos;
        while (true) {
          skip_whitespace();
          auto rt = parse_type();
          if (rt)
            returns.push_back(rt);
          skip_whitespace();
          if (at_end() || content[pos] != ',')
            break;
          ++pos;
        }
        skip_whitespace();
        if (!at_end() && content[pos] == ')')
          ++pos;
      } else {
        auto rt = parse_type();
        if (rt && rt->kind != TypeKind::Void)
          returns.push_back(rt);
      }
    }

    if (returns.empty())
      returns.push_back(make_void_type());

    return make_func_type(std::move(params), std::move(returns), is_variadic);
  }

  // ── Top-level declaration parsing ─────────────────────────────────

  /// Parse a func declaration.
  SgiExport parse_func_decl(const std::string &doc) {
    std::string name = read_word();
    skip_whitespace();
    auto type = parse_func_type();
    skip_line();
    return {doc, name, type};
  }

  /// Parse a struct declaration block.
  SgiExport parse_struct_decl(const std::string &doc) {
    std::string name = read_word();

    // Generic parameters
    std::vector<TypeParam> type_params;
    skip_whitespace();
    if (!at_end() && content[pos] == '|') {
      ++pos; // skip |
      uint32_t param_id = 0;
      while (true) {
        skip_whitespace();
        std::string pname = read_word();
        if (!pname.empty())
          type_params.push_back({param_id++, pname});
        skip_whitespace();
        if (at_end() || content[pos] != ',')
          break;
        ++pos;
      }
      if (!at_end() && content[pos] == '|')
        ++pos;
    }

    skip_whitespace();
    if (!at_end() && content[pos] == '{')
      ++pos;
    skip_line();

    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
    std::vector<TypePtr> embeds;

    while (true) {
      skip_whitespace();
      if (at_end() || content[pos] == '}') {
        if (!at_end())
          ++pos;
        skip_line();
        break;
      }

      std::string kw = read_word();

      if (kw == "pub") {
        skip_whitespace();
        std::string next = read_word();
        if (next == "fn") {
          // Method
          std::string mname = read_word();
          skip_whitespace();
          auto sig = parse_func_type();
          skip_line();
          methods.push_back({mname, sig, true});
        } else {
          // Field: "pub <name> <type>"
          std::string fname = next;
          auto ftype = parse_type();
          skip_line();
          fields.push_back({fname, ftype, true});
        }
      } else if (kw == "embed") {
        auto etype = parse_type();
        skip_line();
        if (etype)
          embeds.push_back(etype);
      } else if (kw == "fn") {
        // Private method — skip
        read_word(); // method name
        skip_whitespace();
        parse_func_type();
        skip_line();
      } else if (!kw.empty() && kw != "}") {
        // Private field: "<name> <type>"
        std::string fname = kw;
        auto ftype = parse_type();
        skip_line();
        fields.push_back({fname, ftype, false});
      } else {
        skip_line();
      }
    }

    auto t = make_struct_type(name, std::move(fields), std::move(methods),
                               std::move(type_params));
    // Set embeds
    auto &info = std::get<StructTypeInfo>(t->detail);
    info.embeds = std::move(embeds);

    return {doc, name, t};
  }

  /// Parse an enum declaration.
  SgiExport parse_enum_decl(const std::string &doc) {
    std::string name = read_word();
    skip_whitespace();
    if (!at_end() && content[pos] == '{')
      ++pos;

    std::vector<EnumVariant> variants;
    while (true) {
      skip_whitespace();
      if (at_end() || content[pos] == '}') {
        if (!at_end())
          ++pos;
        skip_line();
        break;
      }
      if (content[pos] == ';') {
        ++pos;
        continue;
      }

      std::string vname = read_word();
      if (vname.empty()) {
        skip_line();
        break;
      }

      int64_t variant_index = -1;
      std::vector<FieldInfo> vfields;
      skip_whitespace();
      if (!at_end() && content[pos] == '(') {
        ++pos;
        skip_whitespace();
        // Check for #N index prefix.
        if (!at_end() && content[pos] == '#') {
          ++pos;
          size_t start = pos;
          if (!at_end() && content[pos] == '-') ++pos;
          while (!at_end() && std::isdigit(content[pos])) ++pos;
          variant_index = std::stoll(content.substr(start, pos - start));
          skip_whitespace();
          // Skip comma after index if fields follow.
          if (!at_end() && content[pos] == ',')
            ++pos;
        }
        // Parse remaining field declarations.
        while (true) {
          skip_whitespace();
          if (at_end() || content[pos] == ')')
            break;
          size_t saved = pos;
          std::string maybe_name = read_word();
          skip_whitespace();
          if (!at_end() && content[pos] != ')' && content[pos] != ',' &&
              !maybe_name.empty() && std::isupper(maybe_name[0]) == false) {
            auto ft = parse_type();
            vfields.push_back({maybe_name, ft, false});
          } else {
            pos = saved;
            auto ft = parse_type();
            vfields.push_back({"", ft, false});
          }
          skip_whitespace();
          if (at_end() || content[pos] != ',')
            break;
          ++pos;
        }
        if (!at_end() && content[pos] == ')')
          ++pos;
      }

      variants.push_back({vname, std::move(vfields), variant_index});
    }

    auto t = make_enum_type(name, std::move(variants));
    return {doc, name, t};
  }

  /// Parse an interface declaration block.
  SgiExport parse_interface_decl(const std::string &doc) {
    std::string name = read_word();

    std::vector<TypeParam> type_params;
    skip_whitespace();
    if (!at_end() && content[pos] == '|') {
      ++pos;
      uint32_t param_id = 0;
      while (true) {
        skip_whitespace();
        std::string pname = read_word();
        if (!pname.empty())
          type_params.push_back({param_id++, pname});
        skip_whitespace();
        if (at_end() || content[pos] != ',')
          break;
        ++pos;
      }
      if (!at_end() && content[pos] == '|')
        ++pos;
    }

    skip_whitespace();
    if (!at_end() && content[pos] == '{')
      ++pos;
    skip_line();

    std::vector<MethodInfo> methods;
    while (true) {
      skip_whitespace();
      if (at_end() || content[pos] == '}') {
        if (!at_end())
          ++pos;
        skip_line();
        break;
      }

      std::string kw = read_word();
      if (kw == "fn") {
        std::string mname = read_word();
        skip_whitespace();
        auto sig = parse_func_type();
        skip_line();
        methods.push_back({mname, sig, true});
      } else {
        skip_line();
      }
    }

    auto t = make_interface_type(name, std::move(methods),
                                  std::move(type_params));
    return {doc, name, t};
  }

  /// Parse a const declaration.
  SgiExport parse_const_decl(const std::string &doc) {
    std::string name = read_word();
    auto type = parse_type();
    skip_line();
    return {doc, name, type};
  }

  /// Parse a "methods TypeName { ... }" block.
  SgiReceiverMethod parse_receiver_methods_decl() {
    SgiReceiverMethod rm;
    rm.type_name = read_word();
    skip_whitespace();
    if (!at_end() && content[pos] == '{')
      ++pos;
    skip_line();

    while (true) {
      skip_whitespace();
      if (at_end() || content[pos] == '}') {
        if (!at_end())
          ++pos;
        skip_line();
        break;
      }

      std::string kw = read_word();
      if (kw == "pub") {
        skip_whitespace();
        std::string next = read_word();
        if (next == "fn") {
          std::string mname = read_word();
          skip_whitespace();
          auto sig = parse_func_type();
          skip_line();
          rm.methods.push_back({mname, sig, true});
        } else {
          skip_line();
        }
      } else if (kw == "fn") {
        // Private method — skip.
        read_word();
        skip_whitespace();
        parse_func_type();
        skip_line();
      } else {
        skip_line();
      }
    }

    return rm;
  }
};

} // anonymous namespace

std::optional<SgiFile> parse_sgi(const std::string &content) {
  SgiParser p;
  p.content = content;
  p.pos = 0;

  SgiFile sgi;

  // Header: "sgi <version>"
  p.skip_blank_lines();
  std::string magic = p.read_word();
  if (magic != "sgi")
    return std::nullopt;
  sgi.version = p.read_int();
  p.skip_line();
  if (sgi.version != 1)
    return std::nullopt;

  // Package name
  p.skip_blank_lines();
  std::string pkg_kw = p.read_word();
  if (pkg_kw != "package")
    return std::nullopt;
  sgi.package_name = p.read_word();
  p.skip_line();

  // Parse imports and exports
  while (!p.at_end()) {
    p.skip_blank_lines();
    if (p.at_end())
      break;

    // Read doc comments
    std::string doc = p.read_doc_comment();

    p.skip_whitespace();
    if (p.at_end())
      break;

    std::string kw = p.read_word();
    if (kw.empty())
      break;

    if (kw == "import") {
      std::string imp_name = p.read_word();
      std::string imp_path = p.read_quoted();
      p.skip_line();
      sgi.imports.push_back({imp_name, imp_path});
    } else if (kw == "func") {
      sgi.exports.push_back(p.parse_func_decl(doc));
    } else if (kw == "struct") {
      auto exp = p.parse_struct_decl(doc);
      if (exp.type) p.defined_types[exp.name] = exp.type; // register for forward refs
      sgi.exports.push_back(std::move(exp));
    } else if (kw == "enum") {
      auto exp = p.parse_enum_decl(doc);
      if (exp.type) p.defined_types[exp.name] = exp.type;
      sgi.exports.push_back(std::move(exp));
    } else if (kw == "interface") {
      auto exp = p.parse_interface_decl(doc);
      if (exp.type) p.defined_types[exp.name] = exp.type;
      sgi.exports.push_back(std::move(exp));
    } else if (kw == "const") {
      sgi.exports.push_back(p.parse_const_decl(doc));
    } else if (kw == "methods") {
      sgi.receiver_methods.push_back(p.parse_receiver_methods_decl());
    } else {
      p.skip_line();
    }
  }

  return sgi;
}

std::optional<SgiFile> load_sgi(const std::string &path) {
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_sgi(ss.str());
}

TypePtr sgi_to_module_type(const SgiFile &sgi,
                            const std::string &import_path) {
  std::vector<ModuleExport> exports;
  for (auto &exp : sgi.exports) {
    if (exp.type)
      exports.push_back({exp.name, exp.type});
  }
  return make_module_type(sgi.package_name, import_path, std::move(exports));
}

TypePtr sgi_to_type(const std::string &text) {
  SgiParser p;
  p.content = text;
  p.pos = 0;
  return p.parse_type();
}

} // namespace mc
