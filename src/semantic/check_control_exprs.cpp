// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Expressions that branch: if, switch, and the `or` that strips a union's
// error alternatives. Each yields a value, so the checker's job is finding the
// one type every arm agrees on — and saying which arm disagreed when they do not.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr Analyzer::check_if_expr(const IfExprNode &node) {
  auto cond_type = check_expr(*node.condition);
  expect_bool(node.condition->span, cond_type);

  // Detect type-test pattern: `if value is Type` to narrow the variable in the
  // then-block (to Type) and the else-block (to the union minus Type).
  std::string narrowed_var;
  TypePtr narrowed_type = nullptr;
  TypePtr else_narrowed_type = nullptr;

  if (auto *is_expr = std::get_if<IsExpr>(&node.condition->data)) {
    if (auto *val_id = std::get_if<IdentifierNode>(&is_expr->value->data)) {
      auto lhs_sym = lookup(std::string(val_id->name));
      auto matched = resolve_type(*is_expr->type);
      if (lhs_sym && lhs_sym->type &&
          lhs_sym->type->kind == TypeKind::Union && matched &&
          !is_invalid_type(matched)) {
        narrowed_var = std::string(val_id->name);
        narrowed_type = matched;
        else_narrowed_type = union_without(lhs_sym->type, matched);
      }
    }
  }

  push_scope(ScopeKind::Block);
  if (!narrowed_var.empty() && narrowed_type) {
    // Narrow the variable in the then-scope.
    current_scope->symbols[narrowed_var] =
        Symbol::variable(narrowed_var, narrowed_type, node.then_block->span);
  }
  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto then_type = check_block(then_block);
  pop_scope();

  if (node.else_block) {
    push_scope(ScopeKind::Block);
    if (!narrowed_var.empty() && else_narrowed_type) {
      current_scope->symbols[narrowed_var] = Symbol::variable(
          narrowed_var, else_narrowed_type, (*node.else_block)->span);
    }
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    auto else_type = check_block(else_block);
    pop_scope();
    return common_type(then_type, else_type);
  }

  return then_type;
}

TypePtr Analyzer::check_switch_expr(const SwitchExprNode &node) {
  auto subject_type = check_expr(*node.subject);
  TypePtr result_type = nullptr;

  // Detect if this is a type-matching switch (subject is a union and
  // patterns are type names).
  bool is_type_match = false;
  std::string subject_var;
  if (subject_type && subject_type->kind == TypeKind::Union) {
    if (auto *id = std::get_if<IdentifierNode>(&node.subject->data)) {
      subject_var = std::string(id->name);
    }
    // Check if the first arm's first pattern is a type name.
    if (!node.arms.empty() && !node.arms[0].patterns.empty()) {
      if (auto *pid =
              std::get_if<IdentifierNode>(&node.arms[0].patterns[0]->data)) {
        auto sym = lookup(std::string(pid->name));
        if (sym && sym->kind == SymbolKind::Type)
          is_type_match = true;
      }
    }
  }

  for (auto &arm : node.arms) {
    TypePtr first_pattern_type;
    for (auto &pat : arm.patterns) {
      TypePtr pattern_type;
      if (auto *sh = std::get_if<EnumShorthandNode>(&pat->data))
        pattern_type = check_enum_shorthand(*sh, *pat, subject_type);
      else
        pattern_type = check_type_or_value_expr(*pat);
      if (!first_pattern_type)
        first_pattern_type = pattern_type;

      if (is_type_match) {
        // Type matching: verify the pattern type is an alternative of the union.
        if (!is_invalid_type(pattern_type) && !is_invalid_type(subject_type)) {
          auto &info = std::get<UnionTypeInfo>(subject_type->detail);
          bool found = false;
          for (auto &alt : info.alternatives) {
            if (types_equal(alt, pattern_type) ||
                is_assignable_to(pattern_type, alt))
              found = true;
          }
          if (!found) {
            error(pat->span,
                  std::format("type {} is not an alternative of {}",
                              type_to_string(pattern_type),
                              type_to_string(subject_type)));
          }
        }
      } else {
        // Value matching: pattern must be same type as subject.
        if (!is_invalid_type(pattern_type) && !is_invalid_type(subject_type)) {
          expect_assignable(pat->span, subject_type, pattern_type,
                            "case pattern");
        }
      }
    }

    // Narrow the subject inside the arm body when there's a single
    // pattern; with multiple patterns the narrowed type would be their
    // union, which the analyzer doesn't synthesize here.  Narrowing
    // applies whether the body is a block or an expression — without
    // this, `case Int: x.String()` saw x as the unnarrowed union.
    bool narrowed = is_type_match && !subject_var.empty() &&
                    arm.patterns.size() == 1 &&
                    !is_invalid_type(first_pattern_type);
    push_scope(ScopeKind::Block);
    if (narrowed) {
      current_scope->symbols[subject_var] = Symbol::variable(
          subject_var, first_pattern_type, arm.patterns[0]->span);
    }
    TypePtr arm_type;
    if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
      arm_type = check_block(*block);
    } else {
      arm_type = check_expr(*arm.body);
    }
    pop_scope();

    if (!result_type)
      result_type = arm_type;
    else
      result_type = common_type(result_type, arm_type);
  }

  if (node.else_body) {
    TypePtr else_type;
    if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
      push_scope(ScopeKind::Block);
      else_type = check_block(*block);
      pop_scope();
    } else {
      else_type = check_expr(**node.else_body);
    }
    if (!result_type)
      result_type = else_type;
    else
      result_type = common_type(result_type, else_type);
  } else if (is_type_match && !is_invalid_type(subject_type)) {
    // Spec: type-matching without `else` must be exhaustive.
    // (docs/language.md:1174-1177)
    auto &info = std::get<UnionTypeInfo>(subject_type->detail);
    std::vector<TypePtr> uncovered;
    for (auto &alt : info.alternatives) {
      bool covered = false;
      for (auto &arm : node.arms) {
        for (auto &pat : arm.patterns) {
          auto pat_t = check_type_or_value_expr(*pat);
          if (is_invalid_type(pat_t)) continue;
          if (types_equal(alt, pat_t) || is_assignable_to(pat_t, alt)) {
            covered = true;
            break;
          }
        }
        if (covered) break;
      }
      if (!covered) uncovered.push_back(alt);
    }
    if (!uncovered.empty()) {
      std::string names;
      for (size_t i = 0; i < uncovered.size(); ++i) {
        if (i) names += ", ";
        names += type_to_string(uncovered[i]);
      }
      error(node.span,
            std::format("non-exhaustive type-switch: missing case(s) for {}",
                        names));
    }
  }

  return result_type ? result_type : builtins.void_type;
}

TypePtr Analyzer::check_for_expr(const ForExprNode &node,
                                 TypePtr accumulator_hint) {
  push_scope(ScopeKind::Loop);
  break_value_types_.emplace_back();

  if (node.mode) {
    std::visit(overloaded{
                   [&](const ForRangeClauseNode &range) {
                     auto iter_type = check_expr(*range.iterable);
                     // Infer loop variable types from the iterable.
                     TypePtr elem_type = builtins.invalid_type;
                     TypePtr key_type = builtins.int_type;

                     if (!is_invalid_type(iter_type)) {
                       switch (iter_type->kind) {
                       case TypeKind::Array: {
                         auto &arr = std::get<ArrayTypeInfo>(iter_type->detail);
                         elem_type = arr.element;
                         break;
                       }
                       case TypeKind::Map: {
                         auto &m = std::get<MapTypeInfo>(iter_type->detail);
                         key_type = m.key;
                         elem_type = m.value;
                         break;
                       }
                       case TypeKind::String:
                         elem_type = builtins.string_type;
                         break;
                       case TypeKind::Struct: {
                         // Task<T> is iterable — yields T from its channel.
                         auto &si = std::get<StructTypeInfo>(iter_type->detail);
                         if (si.name == "Task" && !si.type_params.empty()) {
                           // If T has been substituted, use the concrete
                           // element type; otherwise fall back to the param.
                           auto tp_id = si.type_params[0].id;
                           // Look for a concrete binding in fields/methods;
                           // for an instantiated Task the type_params list
                           // still contains the original TypeParam, but the
                           // methods have been substituted.  The Wait()
                           // method returns T|Error — grab T from there.
                           bool found = false;
                           for (auto &m : si.methods) {
                             if (m.name == "Wait" && m.signature &&
                                 m.signature->kind == TypeKind::Func) {
                               auto &fi = std::get<FuncTypeInfo>(
                                   m.signature->detail);
                               if (fi.return_type) {
                                 auto &ret = fi.return_type;
                                 if (ret->kind == TypeKind::Union) {
                                   auto &ui = std::get<UnionTypeInfo>(
                                       ret->detail);
                                   for (auto &alt : ui.alternatives) {
                                     if (alt->kind != TypeKind::Interface) {
                                       elem_type = alt;
                                       found = true;
                                       break;
                                     }
                                   }
                                 } else {
                                   elem_type = ret;
                                   found = true;
                                 }
                               }
                               break;
                             }
                           }
                           if (!found)
                             elem_type = builtins.invalid_type;
                         } else {
                           // Not a Task — check for the Iterable protocol:
                           // a Next() method returning T | Error.
                           bool found_iterable = false;
                           for (auto &m : si.methods) {
                             if (m.name != "Next" || !m.signature ||
                                 m.signature->kind != TypeKind::Func)
                               continue;
                             auto &fi =
                                 std::get<FuncTypeInfo>(m.signature->detail);
                             // Next() takes no explicit params (just self).
                             if (!fi.params.empty())
                               break;
                             if (!fi.return_type ||
                                 fi.return_type->kind != TypeKind::Union)
                               break;
                             // Extract T from T | Error.
                             auto &ui = std::get<UnionTypeInfo>(
                                 fi.return_type->detail);
                             for (auto &alt : ui.alternatives) {
                               if (alt->kind == TypeKind::Interface)
                                 continue; // skip Error
                               elem_type = alt;
                               found_iterable = true;
                               // Record for codegen so it knows which
                               // element type to extract.
                               if (current_instantiation_) {
                                 current_instantiation_
                                     ->iterable_next_elem_type[range.iterable
                                                                   .get()] =
                                     elem_type;
                               } else {
                                 iterable_next_elem_type[range.iterable.get()] =
                                     elem_type;
                               }
                               break;
                             }
                             break; // found Next(), stop method search
                           }
                           if (!found_iterable) {
                             error(
                                 range.iterable->span,
                                 std::format(
                                     "type {} is not iterable (no "
                                     "Next() T | Error method)",
                                     type_to_string(iter_type)));
                           }
                         }
                         break;
                       }
                       default:
                         error(range.iterable->span,
                               std::format("type {} is not iterable",
                                           type_to_string(iter_type)));
                         break;
                       }
                     }

                     if (range.vars.size() == 1) {
                       current_scope->symbols.emplace(
                           std::string(range.vars[0].name),
                           Symbol::variable(std::string(range.vars[0].name),
                                            elem_type, range.vars[0].span));
                     } else if (range.vars.size() == 2) {
                       current_scope->symbols.emplace(
                           std::string(range.vars[0].name),
                           Symbol::variable(std::string(range.vars[0].name),
                                            key_type, range.vars[0].span));
                       current_scope->symbols.emplace(
                           std::string(range.vars[1].name),
                           Symbol::variable(std::string(range.vars[1].name),
                                            elem_type, range.vars[1].span));
                     }
                   },
                   [&](const ForIterClauseNode &iter) {
                     check_stmt(*iter.init);
                     auto cond = check_expr(*iter.condition);
                     expect_bool(iter.condition->span, cond, "for condition");
                     check_stmt(*iter.update);
                   },
                   [&](const auto &) {
                     // Bare condition expression.
                     auto cond = check_expr(**node.mode);
                     expect_bool((*node.mode)->span, cond, "for condition");
                   },
               },
               (*node.mode)->data);
  }

  // Accumulator pipe — typed from the variable declaration's type hint.
  TypePtr acc_type = accumulator_hint ? accumulator_hint : builtins.void_type;
  if (node.accumulator) {
    current_scope->symbols.emplace(
        std::string(node.accumulator->name),
        Symbol::variable(std::string(node.accumulator->name), acc_type,
                         node.accumulator->span));
  }

  auto &body_block = std::get<BlockNode>(node.body->data);
  check_block(body_block);

  // A `break <value>` anywhere in the body shapes the for-expression's
  // result as `T | Error` (spec language.md:1284-1295).  When multiple
  // breaks have differing types the result is `T1 | T2 | … | Error`.
  std::vector<TypePtr> break_types = std::move(break_value_types_.back());
  break_value_types_.pop_back();

  pop_scope();

  if (!break_types.empty()) {
    std::vector<TypePtr> alts;
    for (auto &bt : break_types) {
      bool seen = false;
      for (auto &existing : alts)
        if (types_equal(existing, bt)) { seen = true; break; }
      if (!seen) alts.push_back(bt);
    }
    alts.push_back(builtins.error_base);
    return make_union_type(std::move(alts));
  }

  return node.accumulator ? acc_type : builtins.void_type;
}

TypePtr Analyzer::check_spawn_expr(const SpawnExprNode &node,
                                   const Node &parent) {
  push_scope(ScopeKind::Spawn);

  if (node.pipe) {
    current_scope->symbols.emplace(
        std::string(node.pipe->name),
        Symbol::variable(std::string(node.pipe->name), builtins.context_type,
                         node.pipe->span));
  }

  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    check_block(*block);
  } else {
    check_expr(*node.body);
  }

  // Update spawn capture types now that type-checking is complete.
  auto &caps_map =
      current_instantiation_ ? current_instantiation_->spawn_captures
                             : spawn_captures;
  auto cap_it = caps_map.find(&parent);
  if (cap_it != caps_map.end()) {
    for (auto &cap : cap_it->second) {
      auto sym = current_scope->lookup(cap.name);
      if (!sym) {
        // Try the parent scope (the capture is from outside spawn).
        auto outer = current_scope->parent;
        while (outer) {
          sym = outer->lookup_local(cap.name);
          if (sym)
            break;
          outer = outer->parent;
        }
      }
      if (sym && sym->type) {
        cap.type = sym->type;
        // Re-classify with the resolved type.
        switch (cap.type->kind) {
        case TypeKind::String:
        case TypeKind::Array:
        case TypeKind::Map:
          cap.kind = SpawnCaptureKind::Share;
          break;
        default:
          cap.kind = SpawnCaptureKind::Copy;
          break;
        }
      }
    }
  }

  pop_scope();

  // Default Task instantiation: spawn-with-no-explicit-T produces Task<Null>.
  // Wait() returns T | error, so T has to be a value — a task that produces
  // nothing still completes, and completion is what Wait hands back. `void`
  // here meant "no value", which is why every union path needed a special case
  // to skip over it.
  TypePtr chan_type = builtins.null_type;
  if (node.generic && !node.generic->type_params.empty()) {
    auto &t_node = *node.generic->type_params[0];
    auto explicit_t = resolve_type(t_node);
    if (explicit_t && !is_invalid_type(explicit_t) &&
        !reject_void_value(t_node.span, explicit_t, "a spawn channel"))
      chan_type = explicit_t;
  }
  return instantiate_task_type(chan_type);
}

TypePtr Analyzer::instantiate_task_type(const TypePtr &chan_type) {
  std::unordered_map<uint32_t, TypePtr> bindings;
  bindings[0] = chan_type; // T (id 0) in Task<T>

  auto &info = std::get<StructTypeInfo>(builtins.task_type->detail);
  std::vector<FieldInfo> new_fields;
  for (auto &f : info.fields)
    new_fields.push_back({f.name, substitute(f.type, bindings), f.is_public});
  std::vector<MethodInfo> new_methods;
  for (auto &m : info.methods)
    new_methods.push_back({m.name, substitute(m.signature, bindings),
                           m.is_public, m.origin_package});

  auto result = make_struct_type(info.name, std::move(new_fields),
                                 std::move(new_methods), {},
                                 info.origin_package);
  auto &ri = std::get<StructTypeInfo>(result->detail);
  ri.type_params = info.type_params;
  ri.type_args.push_back(chan_type);
  ri.embeds = info.embeds;
  return result;
}

// The type bound to an `or |err|` pipe: the union's error alternative(s). A base
// `error` slot (or a mix that includes it) widens to base `error`; otherwise the
// concrete error type, or a union of the concrete errors.
TypePtr Analyzer::or_error_type(const TypePtr &union_type) {
  if (!union_type || union_type->kind != TypeKind::Union)
    return builtins.error_base;
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  std::vector<TypePtr> errs;
  for (auto &alt : info.alternatives) {
    if (is_abstract_error(alt))
      return builtins.error_base;
    if (is_error_valued(alt))
      errs.push_back(alt);
  }
  if (errs.size() == 1)
    return errs[0];
  if (errs.empty())
    return builtins.error_base;
  return make_union_type(std::move(errs));
}

TypePtr Analyzer::check_or_expr(const OrExprNode &node) {
  auto expr_type = check_expr(*node.expr);

  if (is_invalid_type(expr_type)) {
    // Still check the fallback block for internal errors.
    push_scope(ScopeKind::Block);
    if (node.pipe) {
      current_scope->symbols.emplace(
          std::string(node.pipe->name),
          Symbol::variable(std::string(node.pipe->name), builtins.error_base,
                           node.pipe->span));
    }
    auto &block = std::get<BlockNode>(node.fallback->data);
    check_block(block);
    pop_scope();
    return builtins.invalid_type;
  }

  // The or-clause strips the error from the union.
  push_scope(ScopeKind::Block);

  if (node.pipe) {
    current_scope->symbols.emplace(
        std::string(node.pipe->name),
        Symbol::variable(std::string(node.pipe->name),
                         or_error_type(expr_type), node.pipe->span));
  }

  auto &block = std::get<BlockNode>(node.fallback->data);
  auto fallback_type = check_block(block);

  pop_scope();

  // Strip error members from the union to get the purified type.
  if (expr_type->kind == TypeKind::Union) {
    auto &info = std::get<UnionTypeInfo>(expr_type->detail);
    std::vector<TypePtr> purified;
    for (auto &alt : info.alternatives) {
      if (!is_error_valued(alt))
        purified.push_back(alt);
    }
    if (purified.empty())
      return fallback_type ? fallback_type : builtins.void_type;
    if (purified.size() == 1) {
      // Validate fallback type matches the purified type (if non-empty block).
      if (fallback_type && !is_invalid_type(fallback_type) &&
          !types_equal(fallback_type, builtins.void_type) &&
          !block.stmts.empty()) {
        expect_assignable(node.fallback->span, purified[0], fallback_type,
                          "or fallback");
      }
      return purified[0];
    }
    TypePtr result = make_union_type(std::move(purified));
    return result;
  }

  return expr_type;
}

TypePtr Analyzer::check_func_expr(const FuncExprNode &node,
                                  const Node &parent) {
  push_scope(ScopeKind::Function);

  if (node.generic)
    enter_generics(*node.generic);

  auto fn_type = resolve_signature(node.signature);

  if (node.signature.return_type)
    current_scope->return_types.push_back(resolve_type(*node.signature.return_type));

  // Re-declare parameters into the type-checking scope.
  for (auto &p : node.signature.params) {
    auto pt = resolve_type(*p.type);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      current_scope->symbols.emplace(
          std::string(ident.name),
          Symbol::parameter(std::string(ident.name), param_type, ident.span));
    }
  }

  auto &block = std::get<BlockNode>(node.body->data);
  auto body_type = check_block(block);

  // Check tail expression matches return type (unless tail always returns).
  auto &fn_info = std::get<FuncTypeInfo>(fn_type->detail);
  bool tail_is_return =
      !block.stmts.empty() && always_returns(*block.stmts.back());
  if (!tail_is_return && fn_info.return_type &&
      !is_invalid_type(body_type)) {
    if (!types_equal(fn_info.return_type, builtins.void_type)) {
      expect_assignable(node.body->span, fn_info.return_type, body_type,
                        "return type");
    }
  }

  pop_scope();

  // Update capture types now that type checking is complete.
  // During resolve phase, capture types may have been nullptr.
  auto &caps_map =
      current_instantiation_ ? current_instantiation_->node_captures
                             : node_captures;
  auto cap_it = caps_map.find(&parent);
  if (cap_it != caps_map.end()) {
    for (auto &cap : cap_it->second) {
      if (!cap.type) {
        auto sym = lookup(cap.name);
        if (sym)
          cap.type = sym->type;
      }
    }
  }

  return fn_type;
}

TypePtr Analyzer::check_import_expr(const ImportExprNode &node) {
  // Import expressions used as `const X = import "path"` are already
  // processed during the import phase.  If we reach here, look up the
  // resolved module type from the cache.
  std::string path(node.path);
  if (package_resolver) {
    auto cache_it = package_resolver->cache.find(path);
    if (cache_it != package_resolver->cache.end()) {
      return cache_it->second;
    }
    auto mock_it = package_resolver->mock_packages.find(path);
    if (mock_it != package_resolver->mock_packages.end()) {
      return mock_it->second;
    }
  }
  // Expected to have been reported when the import failed; poison() checks
  // that claim rather than trusting it.
  return poison(node.span, "import '" + path + "' resolved to no package");
}

} // namespace saga
