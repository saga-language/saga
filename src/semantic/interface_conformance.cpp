// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Does a type satisfy an interface, and what it costs to say it does not.
// Conformance is structural, checked against the method set, so a failure has
// to name the method that is missing or mismatched rather than the interface.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

// signatures_match_with_self — compare an interface method signature against
// a concrete method signature, treating any reference to `iface_self` inside
// `iface_sig` as matching `concrete_self` in `concrete_sig`.  This is the
// Go-style trick that lets `Equals(Hashable) Bool` declared in `interface
// Hashable` match `Equals(Int) Bool` on the Int receiver.  Outside the
// self-reference position, signatures must be `types_equal`.
static bool signatures_match_with_self(const TypePtr &iface_sig,
                                       const TypePtr &concrete_sig,
                                       const TypePtr &iface_self,
                                       const TypePtr &concrete_self) {
  if (!iface_sig || !concrete_sig)
    return false;
  if (iface_sig->kind != TypeKind::Func ||
      concrete_sig->kind != TypeKind::Func)
    return false;

  auto &a = std::get<FuncTypeInfo>(iface_sig->detail);
  auto &b = std::get<FuncTypeInfo>(concrete_sig->detail);
  if (a.params.size() != b.params.size())
    return false;
  if (static_cast<bool>(a.return_type) != static_cast<bool>(b.return_type))
    return false;
  if (a.is_variadic != b.is_variadic)
    return false;

  // An interface position counts as "the interface itself" when it points
  // to the same nominal interface (same name + origin package).  SGI loads
  // produce fresh TypePtrs, so pointer equality alone is insufficient.
  auto refers_to_self = [&](const TypePtr &t) -> bool {
    if (!t || t->kind != TypeKind::Interface || !iface_self ||
        iface_self->kind != TypeKind::Interface)
      return false;
    if (t.get() == iface_self.get())
      return true;
    auto &a = std::get<InterfaceTypeInfo>(t->detail);
    auto &b = std::get<InterfaceTypeInfo>(iface_self->detail);
    return a.name == b.name && a.origin_package == b.origin_package;
  };

  auto self_match = [&](const TypePtr &iv, const TypePtr &cv) -> bool {
    if (refers_to_self(iv))
      return types_equal(cv, concrete_self) ||
             (cv && concrete_self &&
              cv->kind == concrete_self->kind &&
              cv->kind == TypeKind::Interface);
    return types_equal(iv, cv);
  };

  for (size_t i = 0; i < a.params.size(); ++i) {
    if (!self_match(a.params[i], b.params[i]))
      return false;
  }
  if (a.return_type && !self_match(a.return_type, b.return_type))
    return false;
  return true;
}

// Gather the methods visible on `concrete` from the analyzer's stdlib and
// built-in tables.  Shared between satisfies_interface and the named-protocol
// diagnostic so missing-method reports stay aligned with what the structural
// match actually consulted.
static std::vector<MethodInfo> collect_concrete_methods(
    const TypePtr &concrete,
    const std::unordered_map<const Type *, std::vector<MethodInfo>>
        &type_methods,
    const std::unordered_map<TypeKind, std::vector<MethodInfo>> &kind_methods,
    const BuiltinTypes &builtins) {
  std::vector<MethodInfo> methods;
  if (!concrete)
    return methods;

  if (concrete->kind == TypeKind::Struct) {
    auto &s = std::get<StructTypeInfo>(concrete->detail);
    methods = s.methods;
    return methods;
  }

  const Type *raw = concrete.get();
  auto tm_it = type_methods.find(raw);
  if (tm_it == type_methods.end()) {
    const Type *canonical = nullptr;
    switch (concrete->kind) {
    case TypeKind::Int:    canonical = builtins.int_type.get(); break;
    case TypeKind::Float:  canonical = builtins.float_type.get(); break;
    case TypeKind::Bool:   canonical = builtins.bool_type.get(); break;
    case TypeKind::String: canonical = builtins.string_type.get(); break;
    default: break;
    }
    if (canonical && canonical != raw)
      tm_it = type_methods.find(canonical);
  }
  if (tm_it != type_methods.end()) {
    for (auto &m : tm_it->second)
      methods.push_back(m);
  }

  auto km_it = kind_methods.find(concrete->kind);
  if (km_it != kind_methods.end()) {
    for (auto &m : km_it->second)
      methods.push_back(m);
  }

  for (auto &m : builtin_methods(concrete->kind, builtins))
    methods.push_back(m);

  return methods;
}

bool Analyzer::satisfies_interface(const TypePtr &concrete,
                                   const TypePtr &iface) {
  if (!concrete || !iface)
    return false;
  if (iface->kind != TypeKind::Interface)
    return false;

  auto &iface_info = std::get<InterfaceTypeInfo>(iface->detail);
  auto concrete_methods =
      collect_concrete_methods(concrete, type_methods_, kind_methods_, builtins);

  // Every interface method must be present on the concrete type with a
  // compatible signature.  Self-references to `iface` inside an interface
  // method signature stand for the concrete receiver type.
  for (auto &im : iface_info.methods) {
    bool found = false;
    for (auto &cm : concrete_methods) {
      if (cm.name == im.name) {
        found = true;
        if (im.signature && cm.signature) {
          if (!signatures_match_with_self(im.signature, cm.signature, iface,
                                          concrete)) {
            return false;
          }
        }
        break;
      }
    }
    if (!found)
      return false;
  }

  return true;
}

// Render an interface method's signature into a single-line form suitable for
// citing in a "missing method" diagnostic, e.g. `Hash() Int64` or
// `Equals(Self) Bool`.  Self-typed positions (the protocol's own interface
// type appearing in its method signature) print as `Self` so the user reads
// the protocol contract rather than its declared `Equals(Hashable) Bool`
// shape.
static std::string format_protocol_method(const std::string &name,
                                          const TypePtr &sig,
                                          const TypePtr &iface) {
  if (!sig || sig->kind != TypeKind::Func)
    return name + "(...)";
  auto &fi = std::get<FuncTypeInfo>(sig->detail);
  auto &iinfo = std::get<InterfaceTypeInfo>(iface->detail);

  auto render = [&](const TypePtr &t) -> std::string {
    if (t && t->kind == TypeKind::Interface) {
      auto &ti = std::get<InterfaceTypeInfo>(t->detail);
      if (ti.name == iinfo.name && ti.origin_package == iinfo.origin_package)
        return "Self";
    }
    return type_to_string(t);
  };

  std::string s = name + "(";
  for (size_t i = 0; i < fi.params.size(); ++i) {
    if (i)
      s += ", ";
    s += render(fi.params[i]);
  }
  s += ")";
  if (fi.return_type)
    s += " " + render(fi.return_type);
  return s;
}

bool Analyzer::check_satisfies_protocol(const TypePtr &concrete,
                                         ProtocolKind p, Span at,
                                         const std::string &context) {
  if (!concrete || is_invalid_type(concrete))
    return true;
  // Inside a generic body the binding isn't known yet — the concrete check
  // happens at every monomorphisation site.
  if (concrete->kind == TypeKind::TypeParam)
    return true;

  TypePtr iface;
  const char *proto_name = nullptr;
  switch (p) {
  case ProtocolKind::Hashable:
    iface = builtins.hashable_iface;
    proto_name = "Hashable";
    break;
  case ProtocolKind::Stringable:
    iface = builtins.stringable_iface;
    proto_name = "Stringable";
    break;
  }
  // proto.sgi unavailable (bootstrap of std/proto, or no resolver) — skip.
  if (!iface)
    return true;

  // Float keys: NaN ≠ NaN breaks the consistency invariant, and the named
  // diagnostic should explain that rather than report a missing Hash().
  if (p == ProtocolKind::Hashable && concrete->kind == TypeKind::Float) {
    std::string ctx = context.empty() ? "" : (" (" + context + ")");
    error(at, std::format(
                  "{} is not Hashable: floats lack total equality "
                  "(NaN != NaN), which the hash-map consistency invariant "
                  "requires{}",
                  type_to_string(concrete), ctx));
    return false;
  }

  return report_interface_unsatisfied(concrete, iface, proto_name, at, context);
}

bool Analyzer::report_interface_unsatisfied(const TypePtr &concrete,
                                            const TypePtr &iface,
                                            const std::string &iface_name,
                                            Span at,
                                            const std::string &context) {
  if (satisfies_interface(concrete, iface))
    return true;

  // Build the "missing methods" list against the same method table the
  // structural matcher consulted.
  auto &iface_info = std::get<InterfaceTypeInfo>(iface->detail);
  auto concrete_methods =
      collect_concrete_methods(concrete, type_methods_, kind_methods_, builtins);

  std::vector<std::string> missing;
  for (auto &im : iface_info.methods) {
    bool ok = false;
    for (auto &cm : concrete_methods) {
      if (cm.name != im.name)
        continue;
      if (im.signature && cm.signature &&
          !signatures_match_with_self(im.signature, cm.signature, iface,
                                      concrete))
        break;
      ok = true;
      break;
    }
    if (!ok)
      missing.push_back(format_protocol_method(im.name, im.signature, iface));
  }

  std::string list;
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i)
      list += ", ";
    list += missing[i];
  }
  std::string ctx = context.empty() ? "" : (" (" + context + ")");
  error(at, std::format(
                "type {} does not satisfy {}{}: missing {}",
                type_to_string(concrete), iface_name, ctx, list));
  return false;
}

bool Analyzer::check_stringable_recursive(const TypePtr &t, Span at,
                                          const std::string &context) {
  if (!t || is_invalid_type(t))
    return true;
  if (t->kind == TypeKind::Array) {
    auto &ai = std::get<ArrayTypeInfo>(t->detail);
    return check_stringable_recursive(ai.element, at, context);
  }
  if (t->kind == TypeKind::Map) {
    auto &mi = std::get<MapTypeInfo>(t->detail);
    bool k = check_stringable_recursive(mi.key, at, context);
    bool v = check_stringable_recursive(mi.value, at, context);
    return k && v;
  }
  return check_satisfies_protocol(t, ProtocolKind::Stringable, at, context);
}

// ---------------------------------------------------------------------------
// Constant-expression evaluator
// ---------------------------------------------------------------------------

namespace {

int64_t parse_int_literal_text(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit)
    if (c != '_') clean += c;

  int base = 10;
  std::string_view digits = clean;
  if (digits.size() > 2 && digits[0] == '0') {
    switch (digits[1]) {
      case 'b': case 'B': base = 2;  digits = digits.substr(2); break;
      case 'o': case 'O': base = 8;  digits = digits.substr(2); break;
      case 'x': case 'X': base = 16; digits = digits.substr(2); break;
      default: break;
    }
  }
  int64_t v = 0;
  std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
  return v;
}

double parse_float_literal_text(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit)
    if (c != '_') clean += c;
  double v = 0.0;
  std::from_chars(clean.data(), clean.data() + clean.size(), v);
  return v;
}

} // namespace

std::optional<ConstValue> Analyzer::evaluate_constant(const Node &expr) {
  if (auto *il = std::get_if<IntegerLiteralNode>(&expr.data))
    return ConstValue::make_int(parse_int_literal_text(il->literal));

  if (auto *fl = std::get_if<FloatLiteralNode>(&expr.data))
    return ConstValue::make_float(parse_float_literal_text(fl->literal));

  if (auto *bl = std::get_if<BoolLiteralNode>(&expr.data))
    return ConstValue::make_bool(bl->literal == "true");

  if (auto *grp = std::get_if<GroupExprNode>(&expr.data))
    return evaluate_constant(*grp->inner);

  if (auto *un = std::get_if<UnaryExprNode>(&expr.data)) {
    auto inner = evaluate_constant(*un->operand);
    if (!inner) return std::nullopt;
    if (un->op == Token::Kind::Sub) {
      if (inner->kind == ConstValue::Kind::Int)
        return ConstValue::make_int(-inner->i);
      if (inner->kind == ConstValue::Kind::Float)
        return ConstValue::make_float(-inner->f);
    }
    if (un->op == Token::Kind::Not) {
      if (inner->kind == ConstValue::Kind::Bool)
        return ConstValue::make_bool(!inner->b);
    }
    if (un->op == Token::Kind::BitwiseNot) {
      if (inner->kind == ConstValue::Kind::Int)
        return ConstValue::make_int(~inner->i);
    }
    return std::nullopt;
  }

  if (auto *bin = std::get_if<BinaryExprNode>(&expr.data)) {
    auto lhs = evaluate_constant(*bin->lhs);
    auto rhs = evaluate_constant(*bin->rhs);
    if (!lhs || !rhs) return std::nullopt;
    if (lhs->kind != rhs->kind) return std::nullopt;

    if (lhs->kind == ConstValue::Kind::Int) {
      int64_t l = lhs->i, r = rhs->i;
      switch (bin->op) {
        case Token::Kind::Add:        return ConstValue::make_int(l + r);
        case Token::Kind::Sub:        return ConstValue::make_int(l - r);
        case Token::Kind::Multiply:   return ConstValue::make_int(l * r);
        case Token::Kind::Divide:
          if (r == 0) {
            error(expr.span, "division by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_int(l / r);
        case Token::Kind::Modulo:
          if (r == 0) {
            error(expr.span, "modulo by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_int(l % r);
        case Token::Kind::BitwiseAnd: return ConstValue::make_int(l & r);
        case Token::Kind::BitwiseOr:  return ConstValue::make_int(l | r);
        case Token::Kind::BitwiseXor: return ConstValue::make_int(l ^ r);
        case Token::Kind::LeftShift:
        case Token::Kind::RightShift:
          if (r < 0 || r >= 64) {
            error(expr.span,
                  std::format("shift count {} out of range [0, 64) in "
                              "constant expression", r));
            return std::nullopt;
          }
          return bin->op == Token::Kind::LeftShift
                     ? ConstValue::make_int(l << r)
                     : ConstValue::make_int(l >> r);
        default: return std::nullopt;
      }
    }
    if (lhs->kind == ConstValue::Kind::Float) {
      double l = lhs->f, r = rhs->f;
      switch (bin->op) {
        case Token::Kind::Add:      return ConstValue::make_float(l + r);
        case Token::Kind::Sub:      return ConstValue::make_float(l - r);
        case Token::Kind::Multiply: return ConstValue::make_float(l * r);
        case Token::Kind::Divide:
          if (r == 0.0) {
            error(expr.span, "division by zero in constant expression");
            return std::nullopt;
          }
          return ConstValue::make_float(l / r);
        default: return std::nullopt;
      }
    }
    return std::nullopt;
  }

  if (auto *id = std::get_if<IdentifierNode>(&expr.data)) {
    auto it = const_decl_values_.find(std::string(id->name));
    if (it != const_decl_values_.end())
      return it->second;
    return std::nullopt;
  }

  return std::nullopt;
}

} // namespace saga
