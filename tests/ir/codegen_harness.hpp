// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

namespace saga {

// ---------------------------------------------------------------------------
// Helper — parse + analyze + codegen in one step.
// ---------------------------------------------------------------------------

struct CG {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;
  std::unique_ptr<CodeGen> codegen;

  static CG from(const std::string &source, bool stdlib = true) {
    CG r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    EXPECT_NE(r.ast, nullptr);
    EXPECT_TRUE(parser.errors.errors.empty())
        << (parser.errors.errors.empty() ? "" : parser.errors.errors[0].message);

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->is_stdlib = stdlib;
    r.analyzer->package_resolver->sgi_search_paths.push_back(
        SAGA_STD_SGI_DIR);
    // Never analyze a parse-error AST nor codegen an analyze-error one: feeding
    // the next stage a malformed input can segfault, which aborts the whole
    // test binary and masks every later test.
    r.codegen = std::make_unique<CodeGen>("test", *r.analyzer);
    if (r.ast && parser.errors.errors.empty()) {
      r.analyzer->analyze(*r.ast);
      EXPECT_TRUE(r.analyzer->errors.errors.empty())
          << (r.analyzer->errors.errors.empty()
                  ? ""
                  : r.analyzer->errors.errors[0].message);
      if (r.analyzer->errors.errors.empty())
        r.codegen->emit(*r.ast);
    }
    return r;
  }

  llvm::Module &mod() { return *codegen->module; }

  llvm::Function *func(const std::string &name) {
    // Try exact name first (e.g. "main", runtime functions).
    if (auto *fn = codegen->module->getFunction(name))
      return fn;
    // Try mangled name.
    return codegen->module->getFunction(mangled(name));
  }

  /// Mangle a name the same way CodeGen does for package "test".
  static std::string mangled(const std::string &name) {
    std::string m = "test__" + name;
    // Replace '.' with '__' for struct methods like "Dog.Speak" → "test__Dog__Speak".
    for (size_t pos = 0; (pos = m.find('.', pos)) != std::string::npos; )
      m.replace(pos, 1, "__");
    return m;
  }

  /// Check if a call instruction calls a function with the given (unmangled) name.
  static bool calls_func(llvm::CallInst *call, const std::string &name) {
    if (!call || !call->getCalledFunction())
      return false;
    auto called = call->getCalledFunction()->getName();
    return called == name || called == mangled(name);
  }
};

} // namespace saga
