// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "semantic/types.hpp"

#include <llvm/IR/Constants.h>

namespace mc {

// ---------------------------------------------------------------------------
// materialize_imports_from_source
// Scans a SourceNode for ImportDeclNode entries, resolves each to its
// module TypePtr via the analyzer's package_scope_, and delegates to
// materialize_import for each unique module.
// ---------------------------------------------------------------------------

void CodeGen::materialize_imports_from_source(const SourceNode &src) {
  if (!analyzer.package_scope_)
    return;

  for (auto &decl : src.declarations) {
    auto *imp = std::get_if<ImportDeclNode>(&decl->data);
    if (!imp)
      continue;

    std::string path(imp->path);
    auto slash = path.rfind('/');
    std::string local_name =
        (slash != std::string::npos) ? path.substr(slash + 1) : path;

    auto it = analyzer.package_scope_->symbols.find(local_name);
    if (it == analyzer.package_scope_->symbols.end())
      continue;
    if (!it->second.type || it->second.type->kind != TypeKind::Module)
      continue;

    materialize_import(it->second.type);
  }
}

// ---------------------------------------------------------------------------
// materialize_import
// Eagerly populates all codegen registries (struct_types, struct_fields,
// enum_types, enum_variants, iface_vtable_types, iface_method_names,
// named_sem_types, struct_method_links) for every export of the given
// module.  Also forward-declares exported functions.
// ---------------------------------------------------------------------------

void CodeGen::materialize_import(const TypePtr &module_type) {
  if (!module_type || module_type->kind != TypeKind::Module)
    return;
  auto &mod = std::get<ModuleTypeInfo>(module_type->detail);

  for (auto &exp : mod.exports) {
    if (!exp.type)
      continue;

    switch (exp.type->kind) {

    case TypeKind::Struct: {
      auto &sinfo = std::get<StructTypeInfo>(exp.type->detail);
      // key_for falls back to package_name for empty origin, but for imports
      // we always have the module name as fallback.
      std::string origin =
          sinfo.origin_package.empty() ? mod.name : sinfo.origin_package;
      std::string key = mangle(origin, sinfo.name);

      auto exist = struct_types.find(key);
      if (exist != struct_types.end() && !exist->second->isOpaque())
        break; // already materialized with a body

      // Build the LLVM struct type from the semantic field list.
      // Field LLVM types are resolved via llvm_type, which itself may
      // create opaque forward declarations for not-yet-materialized
      // structs — those will be filled when their materialize_import
      // call runs.
      std::vector<llvm::Type *> ftypes;
      std::vector<std::string> fnames;
      for (auto &f : sinfo.fields) {
        ftypes.push_back(llvm_type(f.type));
        fnames.push_back(f.name);
      }
      llvm::StructType *st = nullptr;
      if (exist != struct_types.end() && exist->second->isOpaque()) {
        st = exist->second;
        st->setBody(ftypes);
      } else {
        st = llvm::StructType::create(context, ftypes, "mc." + key);
        struct_types[key] = st;
      }
      struct_fields[key] = std::move(fnames);
      named_sem_types[key] = exp.type;

      // Forward-declare non-generic methods and populate struct_method_links.
      for (auto &m : sinfo.methods) {
        if (!m.signature || m.signature->kind != TypeKind::Func)
          continue;
        if (has_type_params(m.signature))
          continue; // generic methods are handled at call sites (P6)

        std::string link_name = mangle(origin, sinfo.name + "__" + m.name);
        struct_method_links[key].push_back({link_name, m.name});

        if (module->getFunction(link_name))
          continue;

        auto &fi = std::get<FuncTypeInfo>(m.signature->detail);
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        // Sret lowering for struct returns.
        llvm::Type *sret_ty = nullptr;
        llvm::Type *ret_ll = void_ll_type;
        if (!fi.returns.empty()) {
          auto *r = llvm_type(fi.returns[0]);
          if (r && r->isStructTy()) {
            sret_ty = r;
            ret_ll = void_ll_type;
          } else {
            ret_ll = r;
          }
        }
        std::vector<llvm::Type *> param_ll;
        std::vector<llvm::Type *> byval_attached(fi.params.size(), nullptr);
        if (sret_ty)
          param_ll.push_back(ptr_type);
        param_ll.push_back(ptr_type); // self pointer
        for (size_t pi = 0; pi < fi.params.size(); ++pi) {
          auto *p = llvm_type(fi.params[pi]);
          if (p && p->isStructTy()) {
            byval_attached[pi] = p;
            param_ll.push_back(ptr_type);
          } else {
            param_ll.push_back(p);
          }
        }
        auto *ft = llvm::FunctionType::get(ret_ll, param_ll, false);
        auto *func = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, link_name, module.get());

        unsigned aidx = 0;
        if (sret_ty) {
          llvm::AttrBuilder ab(context);
          ab.addStructRetAttr(sret_ty);
          ab.addAlignmentAttr(
              module->getDataLayout().getABITypeAlign(sret_ty));
          func->addParamAttrs(aidx++, ab);
        }
        ++aidx; // self
        for (size_t pi = 0; pi < fi.params.size(); ++pi) {
          if (byval_attached[pi]) {
            llvm::AttrBuilder ab(context);
            ab.addByValAttr(byval_attached[pi]);
            ab.addAlignmentAttr(
                module->getDataLayout().getABITypeAlign(byval_attached[pi]));
            func->addParamAttrs(aidx, ab);
          }
          ++aidx;
        }
      }
      break;
    }

    case TypeKind::Enum: {
      auto &einfo = std::get<EnumTypeInfo>(exp.type->detail);
      std::string origin =
          einfo.origin_package.empty() ? mod.name : einfo.origin_package;
      std::string key = mangle(origin, einfo.name);

      if (enum_types.count(key))
        break;

      enum_types[key] = true;
      int64_t next_index = 0;
      for (auto &v : einfo.variants) {
        if (v.index >= 0)
          next_index = v.index;
        enum_variants[key + "." + v.name] = next_index++;
      }
      break;
    }

    case TypeKind::Interface: {
      auto &iinfo = std::get<InterfaceTypeInfo>(exp.type->detail);
      std::string origin =
          iinfo.origin_package.empty() ? mod.name : iinfo.origin_package;
      std::string key = mangle(origin, iinfo.name);

      if (iface_vtable_types.count(key))
        break;

      auto *ptr_type = llvm::PointerType::getUnqual(context);
      std::vector<llvm::Type *> vtable_fields;
      std::vector<std::string> method_names;
      for (auto &m : iinfo.methods) {
        vtable_fields.push_back(ptr_type);
        method_names.push_back(m.name);
      }
      auto *vtable_st = llvm::StructType::create(context, vtable_fields,
                                                  "mc.vtable." + key);
      iface_vtable_types[key] = vtable_st;
      iface_method_names[key] = std::move(method_names);
      named_sem_types[key] = exp.type;
      break;
    }

    case TypeKind::Func:
      // Forward-declare exported functions.
      declare_import(mod.name, exp.name, exp.type);
      break;

    default:
      break;
    }
  }
}

} // namespace mc
