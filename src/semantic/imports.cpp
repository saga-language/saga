// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Bringing another package in: resolving the path, reading a pre-compiled SGI
// or compiling from source, and loading the generic method bodies a cross-
// package instantiation will need before it can be monomorphised.

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include "semantic/sgi.hpp"
#include "frontend/parser.hpp"
#include <filesystem>
#include <format>

namespace saga {

namespace fs = std::filesystem;

void Analyzer::process_imports(const std::vector<NodePtr> &declarations) {
  for (auto &decl : declarations) {
    std::visit(
        overloaded{
            [&](const ImportDeclNode &imp) {
              std::string path(imp.path);

              // Check for duplicate imports.
              if (imported_paths_.count(path)) {
                error(imp.span, std::format("duplicate import of '{}'", path));
                return;
              }
              imported_paths_.insert(path);

              // Derive the local name from the last path segment.
              auto last_slash = path.rfind('/');
              std::string name = (last_slash != std::string::npos)
                                     ? path.substr(last_slash + 1)
                                     : path;

              // Resolve the import to a module type.
              auto module_type = resolve_import(path, imp.span);

              // Update the forward-declared module symbol with the resolved
              // type.
              auto sym_it = current_scope->symbols.find(name);
              if (sym_it != current_scope->symbols.end()) {
                sym_it->second.type = module_type;
              }
            },
            [&](const ConstDeclNode &c) {
              // Handle `const Name = import "path"` — import expression
              // bound to a named constant.
              if (!c.value)
                return;
              auto *import_expr = std::get_if<ImportExprNode>(&c.value->data);
              if (!import_expr)
                return;

              std::string path(import_expr->path);

              // Check for duplicate imports.
              if (imported_paths_.count(path)) {
                error(c.value->span,
                      std::format("duplicate import of '{}'", path));
                return;
              }
              imported_paths_.insert(path);

              // Resolve the import.
              auto module_type = resolve_import(path, c.value->span);

              // Update the constant symbol to be a module symbol.
              auto sym_it =
                  current_scope->symbols.find(std::string(c.name.name));
              if (sym_it != current_scope->symbols.end()) {
                sym_it->second.type = module_type;
                sym_it->second.kind = SymbolKind::Module;
              }
            },
            [&](const auto &) { /* not an import */ },
        },
        decl->data);
  }
}

std::optional<TypePtr>
Analyzer::resolve_import_cached(const std::string &import_path, Span span) {
  if (!package_resolver)
    return std::nullopt;

  auto mock_it = package_resolver->mock_packages.find(import_path);
  if (mock_it != package_resolver->mock_packages.end())
    return mock_it->second;

  auto cache_it = package_resolver->cache.find(import_path);
  if (cache_it != package_resolver->cache.end())
    return cache_it->second;

  if (package_resolver->in_progress.count(import_path)) {
    error(span, std::format("circular import detected: '{}'", import_path));
    return builtins.invalid_type;
  }

  return std::nullopt;
}

void Analyzer::merge_sgi_receiver_methods(const SgiFile &sgi) {
  auto get_canonical = [this](const std::string &tn) -> const Type * {
    if (tn == "Int")    return builtins.int_type.get();
    if (tn == "Int8")   return builtins.int8_type.get();
    if (tn == "Int16")  return builtins.int16_type.get();
    if (tn == "Int32")  return builtins.int32_type.get();
    if (tn == "Int64")  return builtins.int64_type.get();
    if (tn == "Uint8")  return builtins.uint8_type.get();
    if (tn == "Uint16") return builtins.uint16_type.get();
    if (tn == "Uint32") return builtins.uint32_type.get();
    if (tn == "Uint64") return builtins.uint64_type.get();
    if (tn == "Float")  return builtins.float_type.get();
    if (tn == "Bool")   return builtins.bool_type.get();
    if (tn == "String") return builtins.string_type.get();
    return nullptr;
  };
  auto get_recv_kind = [](const std::string &tn) -> std::optional<TypeKind> {
    if (tn == "Array") return TypeKind::Array;
    if (tn == "Map")   return TypeKind::Map;
    return std::nullopt;
  };
  auto push_unique = [](std::vector<MethodInfo> &vec,
                        const MethodInfo &method) {
    for (auto &e : vec)
      if (e.name == method.name)
        return;
    vec.push_back(method);
  };

  for (auto &rm : sgi.receiver_methods) {
    if (const Type *canonical = get_canonical(rm.type_name)) {
      for (auto &method : rm.methods)
        push_unique(type_methods_[canonical], method);
      continue;
    }
    if (auto kind_opt = get_recv_kind(rm.type_name)) {
      auto kind = *kind_opt;
      for (auto &method : rm.methods) {
        auto sig = normalize_generic_receiver_sig(method.signature, kind);
        push_unique(kind_methods_[kind],
                    {method.name, sig, method.is_public});
      }
    }
  }
}

std::optional<TypePtr>
Analyzer::load_import_from_sgi(const std::string &import_path, Span span) {
  if (!package_resolver)
    return std::nullopt;

  std::string sgi_path = package_resolver->find_sgi_file(import_path);
  if (sgi_path.empty())
    return std::nullopt;

  std::string parse_err;
  auto sgi = load_sgi(sgi_path, &parse_err);
  if (!sgi) {
    error(span, std::format("failed to load interface for '{}' from '{}': {}",
                            import_path, sgi_path,
                            parse_err.empty() ? "unknown error" : parse_err));
    return std::nullopt;
  }

  auto module_type = sgi_to_module_type(*sgi, import_path);
  package_resolver->cache[import_path] = module_type;
  package_resolver->sgi_resolved_dirs[import_path] =
      fs::path(sgi_path).parent_path().string();
  if (!sgi->source_dir.empty()) {
    auto last_slash = import_path.rfind('/');
    std::string pkg_short = (last_slash != std::string::npos)
                                ? import_path.substr(last_slash + 1)
                                : import_path;
    package_resolver->source_dirs[pkg_short] = sgi->source_dir;
    package_resolver->source_dirs[import_path] = sgi->source_dir;
  }
  merge_sgi_receiver_methods(*sgi);
  return module_type;
}

void Analyzer::merge_sub_analyzer_receiver_methods(const Analyzer &sub) {
  auto map_scalar_to_canonical =
      [this](const Type *sub_type_ptr) -> const Type * {
    switch (sub_type_ptr->kind) {
    case TypeKind::Int: {
      auto &ii = std::get<IntType>(sub_type_ptr->detail);
      if (ii.bits == 0)
        return builtins.int_type.get();
      if (ii.is_signed) {
        switch (ii.bits) {
        case 8:  return builtins.int8_type.get();
        case 16: return builtins.int16_type.get();
        case 32: return builtins.int32_type.get();
        case 64: return builtins.int64_type.get();
        }
      } else {
        switch (ii.bits) {
        case 8:  return builtins.uint8_type.get();
        case 16: return builtins.uint16_type.get();
        case 32: return builtins.uint32_type.get();
        case 64: return builtins.uint64_type.get();
        }
      }
      return nullptr;
    }
    case TypeKind::Float:  return builtins.float_type.get();
    case TypeKind::Bool:   return builtins.bool_type.get();
    case TypeKind::String: return builtins.string_type.get();
    default: return nullptr;
    }
  };
  auto push_unique = [](std::vector<MethodInfo> &vec,
                        const MethodInfo &method) {
    for (auto &e : vec)
      if (e.name == method.name)
        return;
    vec.push_back(method);
  };

  for (auto &[sub_type_ptr, methods] : sub.type_methods_) {
    const Type *our_type = map_scalar_to_canonical(sub_type_ptr);
    if (!our_type)
      continue;
    for (auto &method : methods)
      push_unique(type_methods_[our_type], method);
  }

  for (auto &[kind, methods] : sub.kind_methods_)
    for (auto &method : methods)
      push_unique(kind_methods_[kind], method);
}

std::vector<ModuleExport>
Analyzer::extract_module_exports(const Analyzer &sub_analyzer) {
  std::vector<ModuleExport> exports;
  if (!sub_analyzer.package_scope_)
    return exports;
  for (auto &[sym_name, sym] : sub_analyzer.package_scope_->symbols)
    if (sym.is_public && !sym.is_builtin && sym.type)
      exports.push_back({sym_name, sym.type});
  return exports;
}

TypePtr
Analyzer::compile_import_from_source(const std::string &import_path, Span span) {
  if (!package_resolver) {
    error(span, std::format("cannot resolve import '{}': no package resolver",
                            import_path));
    return builtins.invalid_type;
  }

  std::string pkg_dir;
  if (import_path.starts_with("./") || import_path.starts_with("../")) {
    if (!current_package_dir.empty()) {
      pkg_dir = current_package_dir + "/" + import_path;
      if (std::filesystem::exists(pkg_dir))
        pkg_dir = std::filesystem::canonical(pkg_dir).string();
      else
        pkg_dir.clear();
    }
  } else {
    pkg_dir = package_resolver->find_package_dir(import_path);
  }

  if (pkg_dir.empty()) {
    error(span, std::format("cannot find package '{}'", import_path));
    return builtins.invalid_type;
  }

  auto source_files = package_resolver->list_source_files(pkg_dir);
  if (source_files.empty()) {
    error(span,
          std::format("package '{}' contains no source files", import_path));
    return builtins.invalid_type;
  }

  package_resolver->in_progress.insert(import_path);

  FileSet sub_fileset;
  for (auto &f : source_files) {
    auto file = File::from_path(f);
    if (file)
      sub_fileset.add_file(std::move(file));
  }

  if (sub_fileset.files.empty()) {
    error(span, std::format("failed to read source files for package '{}'",
                            import_path));
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  Parser sub_parser(sub_fileset);
  auto sub_ast = sub_parser.parse();
  if (!sub_ast || !sub_parser.errors.errors.empty()) {
    error(span,
          std::format("parse errors in imported package '{}'", import_path));
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  auto pkg_last_slash = import_path.rfind('/');
  std::string pkg_short = (pkg_last_slash != std::string::npos)
                              ? import_path.substr(pkg_last_slash + 1)
                              : import_path;

  Analyzer sub_analyzer(sub_fileset, package_resolver);
  sub_analyzer.current_package_dir = pkg_dir;
  sub_analyzer.current_package_name_override = pkg_short;
  sub_analyzer.analyze(*sub_ast);

  if (!sub_analyzer.errors.errors.empty()) {
    error(span, std::format("errors in imported package '{}'", import_path));
    for (auto &e : sub_analyzer.errors.errors)
      errors.errors.push_back(e);
    package_resolver->in_progress.erase(import_path);
    return builtins.invalid_type;
  }

  merge_sub_analyzer_receiver_methods(sub_analyzer);

  const std::string &pkg_name = pkg_short;
  auto exports = extract_module_exports(sub_analyzer);
  auto module_type =
      make_module_type(pkg_name, import_path, std::move(exports));

  package_resolver->cache[import_path] = module_type;
  package_resolver->in_progress.erase(import_path);

  // Record the source directory under both the short package name and the
  // full import path so codegen can find generic method bodies later.
  std::error_code abs_ec;
  std::string abs_pkg_dir =
      fs::absolute(pkg_dir, abs_ec).lexically_normal().string();
  if (!abs_ec) {
    package_resolver->source_dirs[pkg_name] = abs_pkg_dir;
    package_resolver->source_dirs[import_path] = abs_pkg_dir;
  }
  return module_type;
}

TypePtr Analyzer::resolve_import(const std::string &import_path, Span span) {
  if (auto cached = resolve_import_cached(import_path, span))
    return *cached;
  size_t err_count_before = errors.errors.size();
  if (auto from_sgi = load_import_from_sgi(import_path, span))
    return *from_sgi;
  // If load_import_from_sgi reported a parse error, don't fall through to
  // source compilation — surface the diagnostic instead of silently masking
  // a malformed .sgi with a fresh source build.
  if (errors.errors.size() > err_count_before)
    return builtins.invalid_type;
  return compile_import_from_source(import_path, span);
}

// ===========================================================================
// Cross-package generic method body loading (D8)
// ===========================================================================

Analyzer *Analyzer::ensure_source_loaded(const std::string &origin) {
  if (origin.empty()) return nullptr;
  auto it = loaded_source_packages_.find(origin);
  if (it != loaded_source_packages_.end())
    return it->second.sub_analyzer.get();

  if (!package_resolver) return nullptr;
  std::string source_dir;
  auto dir_it = package_resolver->source_dirs.find(origin);
  if (dir_it != package_resolver->source_dirs.end())
    source_dir = dir_it->second;
  else
    source_dir = package_resolver->find_package_dir(origin);
  if (source_dir.empty() || !fs::is_directory(source_dir))
    return nullptr;

  auto source_files = package_resolver->list_source_files(source_dir);
  if (source_files.empty())
    return nullptr;

  auto loaded_fileset = std::make_unique<FileSet>();
  for (auto &f : source_files) {
    auto file = File::from_path(f);
    if (file)
      loaded_fileset->add_file(std::move(file));
  }
  if (loaded_fileset->files.empty())
    return nullptr;

  Parser sub_parser(*loaded_fileset);
  auto sub_ast = sub_parser.parse();
  if (!sub_ast || !sub_parser.errors.errors.empty())
    return nullptr;

  auto sub_analyzer =
      std::make_unique<Analyzer>(*loaded_fileset, package_resolver);
  sub_analyzer->current_package_dir = source_dir;
  // ensure_source_loaded is keyed by origin (package name), so it doubles
  // as the authoritative name for the loaded sub-analyzer.
  sub_analyzer->current_package_name_override = origin;
  // Inherit stdlib mode when loading a stdlib package's source — stdlib
  // packages use intrinsics and define receiver methods on intrinsic
  // types, both of which are gated to stdlib mode.
  static const std::unordered_set<std::string> stdlib_origins{
      "proto", "int", "float", "bool", "string", "array", "map",
      "unsafe", "sys", "os", "io"};
  if (stdlib_origins.count(origin))
    sub_analyzer->is_stdlib = true;
  sub_analyzer->analyze(*sub_ast);
  if (!sub_analyzer->errors.errors.empty())
    return nullptr;

  auto *sub_ptr = sub_analyzer.get();

  // Merge node-keyed tables. Keys are pointers into the sub-AST that we
  // hold alive in loaded_source_packages_, so they remain valid for the
  // lifetime of this analyzer.
  for (auto &[k, v] : sub_ptr->node_types) node_types.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_symbols) node_symbols.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_captures) node_captures.emplace(k, v);
  for (auto &[k, v] : sub_ptr->node_type_args) node_type_args.emplace(k, v);
  for (auto &[k, v] : sub_ptr->iterable_next_elem_type)
    iterable_next_elem_type.emplace(k, v);
  for (auto &[k, v] : sub_ptr->spawn_channel_elem_types)
    spawn_channel_elem_types.emplace(k, v);
  for (auto &[k, v] : sub_ptr->spawn_captures) spawn_captures.emplace(k, v);
  for (auto &[k, v] : sub_ptr->struct_operator_methods)
    struct_operator_methods.emplace(k, v);
  for (auto &[k, v] : sub_ptr->generic_templates_)
    generic_templates_.emplace(k, v);
  for (auto &[k, v] : sub_ptr->func_decl_by_type_)
    func_decl_by_type_.emplace(k, v);
  for (auto *fd : sub_ptr->kind_method_uses_typeparam_dispatch_)
    kind_method_uses_typeparam_dispatch_.insert(fd);
  for (auto &[kind, methods] : sub_ptr->kind_method_decls_) {
    auto &dst = kind_method_decls_[kind];
    for (auto &[name, kmd] : methods) {
      dst.emplace(name, kmd);
    }
  }

  loaded_source_packages_.emplace(
      origin,
      LoadedSourcePackage{std::move(loaded_fileset), std::move(sub_ast),
                          std::move(sub_analyzer)});
  return sub_ptr;
}

Analyzer::GenericMethodDecl
Analyzer::generic_method_decl(const std::string &struct_name,
                              const std::string &method_name,
                              const std::vector<TypePtr> &type_args) {
  GenericMethodDecl result;
  if (!package_scope_) return result;

  auto sym_it = package_scope_->symbols.find(struct_name);
  if (sym_it == package_scope_->symbols.end()) return result;
  auto struct_type = sym_it->second.type;
  if (!struct_type || struct_type->kind != TypeKind::Struct) return result;

  auto &sinfo = std::get<StructTypeInfo>(struct_type->detail);
  for (auto &m : sinfo.methods) {
    if (m.name != method_name || !m.signature) continue;
    auto fd_it = func_decl_by_type_.find(m.signature.get());
    if (fd_it == func_decl_by_type_.end()) return result;
    result.decl = fd_it->second;
    result.template_signature = m.signature;
    result.struct_type_params = sinfo.type_params;
    break;
  }
  if (!result.decl) return result;

  // Drive body type-checking under the caller's bindings so codegen can
  // read concrete TypePtrs out of the resulting BodyInstantiation. Bind by
  // the function's GenericTemplate IDs (which P5's receiver remap aligned
  // with the struct's type-param IDs).
  auto tpl_it = generic_templates_.find(result.decl);
  if (tpl_it != generic_templates_.end()) {
    auto &tpl_params = tpl_it->second.type_params;
    size_t n = std::min(tpl_params.size(), type_args.size());
    for (size_t i = 0; i < n; ++i)
      result.bindings[tpl_params[i].id] = type_args[i];
  } else {
    // Fall back to positional alignment with the struct's type params if
    // the template wasn't recorded (e.g. non-generic receiver method).
    size_t n =
        std::min(result.struct_type_params.size(), type_args.size());
    for (size_t i = 0; i < n; ++i)
      result.bindings[result.struct_type_params[i].id] = type_args[i];
  }
  if (!result.bindings.empty() && result.decl->body) {
    result.instantiation =
        instantiate_generic_body(*result.decl, result.bindings,
                                 *result.decl->body);
  }
  return result;
}

Analyzer::GenericMethodDecl
Analyzer::load_imported_method_decl(const std::string &origin,
                                     const std::string &struct_name,
                                     const std::string &method_name,
                                     const std::vector<TypePtr> &type_args) {
  auto *sub = ensure_source_loaded(origin);
  if (!sub) return {};
  return sub->generic_method_decl(struct_name, method_name, type_args);
}

} // namespace saga
