// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"
#include "util/internal_error.hpp"

namespace saga {

LoweredSig CodeGen::lower_signature(const FuncTypeInfo &fi,
                                    llvm::Type *leading) {
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  LoweredSig sig;
  std::vector<llvm::Type *> params;
  llvm::Type *ret = fi.return_type ? llvm_type(fi.return_type) : void_ll_type;
  if (ret->isStructTy()) {
    sig.sret = ret;
    ret = void_ll_type;
    params.push_back(ptr_ty);
    sig.byval.push_back(nullptr);
  }
  if (leading) {
    params.push_back(leading);
    sig.byval.push_back(nullptr);
  }
  for (auto &p : fi.params) {
    auto *indirect = byval_param_type(p);
    params.push_back(indirect ? ptr_ty : llvm_type(p));
    sig.byval.push_back(indirect);
  }
  sig.type = llvm::FunctionType::get(ret, params, /*isVarArg=*/false);
  return sig;
}

llvm::Function *CodeGen::declare_function(const std::string &link,
                                          const LoweredSig &sig,
                                          llvm::GlobalValue::LinkageTypes linkage) {
  auto *fn = llvm::Function::Create(sig.type, linkage, link, module.get());
  stamp_abi(fn, sig);
  return fn;
}

// A function and every call to it carry the same attributes; LLVM reads the
// call's copy when it lowers the call.
std::vector<std::pair<unsigned, llvm::Attribute>>
CodeGen::abi_attrs(const LoweredSig &sig) {
  std::vector<std::pair<unsigned, llvm::Attribute>> out;
  auto add = [&](unsigned i, llvm::Attribute kind, llvm::Type *t) {
    out.push_back({i, kind});
    out.push_back({i, llvm::Attribute::getWithAlignment(context, align_of(t))});
  };
  if (sig.sret)
    add(0, llvm::Attribute::getWithStructRetType(context, sig.sret), sig.sret);
  for (unsigned i = 0; i < sig.byval.size(); ++i)
    if (sig.byval[i])
      add(i, llvm::Attribute::getWithByValType(context, sig.byval[i]),
          sig.byval[i]);
  return out;
}

void CodeGen::stamp_abi(llvm::Function *fn, const LoweredSig &sig) {
  for (auto &[i, attr] : abi_attrs(sig))
    fn->addParamAttr(i, attr);
}

void CodeGen::stamp_abi(llvm::CallBase *call, const LoweredSig &sig) {
  for (auto &[i, attr] : abi_attrs(sig))
    call->addParamAttr(i, attr);
}

const FuncTypeInfo &CodeGen::decl_signature(const FuncDeclNode &fn) {
  auto it = analyzer.decl_signatures_.find(&fn);
  if (it == analyzer.decl_signatures_.end() || !it->second ||
      it->second->kind != TypeKind::Func)
    internal_error("no resolved signature for '" + std::string(fn.name.name) +
                   "'");
  return std::get<FuncTypeInfo>(it->second->detail);
}

// A struct receiver arrives as the caller's address, so a method's writes
// reach the value the caller named; any other receiver arrives as its value.
llvm::Type *CodeGen::receiver_param_type(const FuncDeclNode &fn) {
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  auto *id = std::get_if<IdentifierNode>(&fn.receiver->type->data);
  auto sym = id ? package_symbol(id->name) : std::nullopt;
  auto recv = sym && sym->type ? unwrap_alias(sym->type) : nullptr;
  return recv && recv->kind != TypeKind::Struct ? llvm_type(recv) : ptr_ty;
}

void CodeGen::name_params(llvm::Function *fn, const LoweredSig &sig,
                          const FuncDeclNode &decl) {
  name_params(fn, sig, decl.signature,
              decl.receiver ? decl.receiver->name.name : std::string_view{});
}

void CodeGen::name_params(llvm::Function *fn, const LoweredSig &sig,
                          const SignatureNode &params,
                          std::string_view leading) {
  unsigned idx = 0;
  if (sig.sret)
    fn->getArg(idx++)->setName("sret.out");
  if (!leading.empty())
    fn->getArg(idx++)->setName(std::string(leading));
  for (auto &param : params.params)
    for (auto &ident : param.names.identifiers)
      if (idx < fn->arg_size())
        fn->getArg(idx++)->setName(std::string(ident.name));
}

unsigned CodeGen::first_param_index(llvm::Function *fn, bool has_leading) {
  bool sret = fn->arg_size() > 0 &&
              fn->hasParamAttribute(0, llvm::Attribute::StructRet);
  return (sret ? 1 : 0) + (has_leading ? 1 : 0);
}

// A byval aggregate arrives as a pointer to a copy made for the call and is
// copied into this frame; anything else arrives as the value itself. A
// parameter that owns a reference gives it back when the frame ends.
void CodeGen::bind_params(llvm::Function *fn, unsigned first,
                          const SignatureNode &sig, const FuncTypeInfo &fi) {
  unsigned idx = first;
  size_t pi = 0;
  for (auto &param : sig.params)
    for (auto &ident : param.names.identifiers) {
      auto *arg = fn->getArg(idx++);
      std::string name(ident.name);
      auto *slot_ll =
          arg->hasByValAttr() ? arg->getParamByValType() : arg->getType();
      auto *slot = bind_value_slot(fn, name, arg, slot_ll);
      locals[name] = slot;
      auto sem = pi < fi.params.size() ? fi.params[pi] : nullptr;
      ++pi;
      if (param_owns_reference(sem))
        track_managed(slot, unwrap_alias(sem));
    }
}

} // namespace saga
