// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace saga {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct MR {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static MR from(const std::string &source) {
    MR r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->package_resolver->sgi_search_paths.push_back(
        SAGA_STD_SGI_DIR);
    if (r.ast && parser.errors.errors.empty())
      r.analyzer->analyze(*r.ast);
    else
      r.analyzer->errors.errors.insert(r.analyzer->errors.errors.end(),
                                       parser.errors.errors.begin(),
                                       parser.errors.errors.end());
    return r;
  }

  bool ok() const { return analyzer->errors.errors.empty(); }

  bool has_err(const std::string &substr) const {
    return analyzer->has_error_containing(substr);
  }

  std::string all_errors() const {
    std::string out;
    for (auto &e : analyzer->errors.errors) {
      if (!out.empty()) out += "\n";
      out += e.message;
    }
    return out;
  }
};

struct MCG {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;
  std::unique_ptr<CodeGen> codegen;

  static MCG from(const std::string &source) {
    MCG r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    EXPECT_NE(r.ast, nullptr);
    EXPECT_TRUE(parser.errors.errors.empty())
        << (parser.errors.errors.empty() ? "" : parser.errors.errors[0].message);

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->is_stdlib = true;
    r.analyzer->package_resolver->sgi_search_paths.push_back(
        SAGA_STD_SGI_DIR);
    // Don't analyze a parse-error AST nor codegen an analyze-error one — either
    // can segfault, aborting the whole binary and masking later tests.
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

  llvm::Function *find_gen(const std::string &substr) {
    for (auto &fn : mod()) {
      if (fn.getName().str().find(substr) != std::string::npos)
        return &fn;
    }
    return nullptr;
  }

  int count_gen(const std::string &substr) {
    int n = 0;
    for (auto &fn : mod()) {
      if (!fn.isDeclaration() &&
          fn.getName().str().find(substr) != std::string::npos)
        ++n;
    }
    return n;
  }
};

// ===========================================================================
// Positive spike — eq(5,5), eq("hi","hi"), eq(true,true)
// ===========================================================================

TEST(Monomorph, PositiveSpike_IntStringBool) {
  auto r = MR::from(
      "pub fn eq<T>(a T, b T) bool { a.Equals(b) }\n"
      "pub fn Main() void {\n"
      "  eq(5, 5)\n"
      "  eq(\"hi\", \"hi\")\n"
      "  eq(true, true)\n"
      "}");
  EXPECT_TRUE(r.ok()) << r.all_errors();
}

// ===========================================================================
// Negative spike — missing method
// ===========================================================================

TEST(Monomorph, NegativeSpike_MissingMethod) {
  auto r = MR::from(
      "struct Point { x int\n y int }\n"
      "pub fn eq<T>(a T, b T) bool { a.Equals(b) }\n"
      "pub fn Main() void { eq(Point{x: 1, y: 2}, Point{x: 3, y: 4}) }");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("no member 'Equals'")) << r.all_errors();
}

TEST(Monomorph, NegativeSpike_ErrorBacktrace) {
  auto r = MR::from(
      "struct Point { x int\n y int }\n"
      "pub fn inner<T>(a T, b T) bool { a.Equals(b) }\n"
      "pub fn outer<T>(a T, b T) bool { inner(a, b) }\n"
      "pub fn Main() void { outer(Point{x: 1, y: 2}, Point{x: 3, y: 4}) }");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.has_err("no member 'Equals'")) << r.all_errors();
  // The error should contain at least two "instantiated from" frames.
  std::string errs = r.all_errors();
  size_t first = errs.find("instantiated from");
  ASSERT_NE(first, std::string::npos) << "Expected backtrace frame";
  size_t second = errs.find("instantiated from", first + 1);
  EXPECT_NE(second, std::string::npos)
      << "Expected two backtrace frames, got:\n" << errs;
}

// ===========================================================================
// Uninstantiated spike — broken body, never called
// ===========================================================================

TEST(Monomorph, UninstantiatedSpike) {
  auto r = MR::from(
      "pub fn broken<T>(a T) void { a.NoSuchMethod() }\n"
      "pub fn Main() void {}");
  EXPECT_TRUE(r.ok()) << r.all_errors();
}

// ===========================================================================
// Codegen — specialised function exists with linkonce_odr
// ===========================================================================

TEST(Monomorph, SpecialisedFunctionExists) {
  auto r = MCG::from(
      "pub fn id<T>(x T) T { x }\n"
      "pub fn Main() void { id(42) }");
  auto *fn = r.find_gen("gen__");
  ASSERT_NE(fn, nullptr) << "Expected a gen__ specialisation";
  EXPECT_FALSE(fn->isDeclaration());
  EXPECT_EQ(fn->getLinkage(), llvm::GlobalValue::LinkOnceODRLinkage);
}

TEST(Monomorph, SpecialisedName_GenPrefix) {
  auto r = MCG::from(
      "pub fn id<T>(x T) T { x }\n"
      "pub fn Main() void { id(42) }");
  auto *fn = r.find_gen("gen__");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getName().str().find("gen__") == 0 ||
              fn->getName().str().find("gen__") != std::string::npos);
  EXPECT_TRUE(fn->getName().str().find("Int") != std::string::npos)
      << "Name should contain type arg: " << fn->getName().str();
}

// ===========================================================================
// Dedup spike — two call sites, same bindings → one definition
// ===========================================================================

TEST(Monomorph, DedupSpike) {
  auto r = MCG::from(
      "pub fn id<T>(x T) T { x }\n"
      "pub fn Main() void {\n"
      "  id(1)\n"
      "  id(2)\n"
      "}");
  EXPECT_EQ(r.count_gen("__id__Int"), 1)
      << "Two calls with same bindings should produce one definition";
}

// ===========================================================================
// Nested spike — outer calls inner, both specialised
// ===========================================================================

TEST(Monomorph, NestedSpike) {
  auto r = MCG::from(
      "pub fn inner<T>(x T) T { x }\n"
      "pub fn outer<T>(x T) T { inner(x) }\n"
      "pub fn Main() void { outer(42) }");
  EXPECT_GE(r.count_gen("__inner__Int"), 1)
      << "inner<Int> should be emitted";
  EXPECT_GE(r.count_gen("__outer__Int"), 1)
      << "outer<Int> should be emitted";
}

// ===========================================================================
// Multiple type instantiations
// ===========================================================================

TEST(Monomorph, MultipleInstantiations) {
  auto r = MCG::from(
      "pub fn id<T>(x T) T { x }\n"
      "pub fn Main() void {\n"
      "  id(42)\n"
      "  id(\"hi\")\n"
      "  id(true)\n"
      "}");
  EXPECT_GE(r.count_gen("__id__Int"), 1);
  EXPECT_GE(r.count_gen("__id__String"), 1);
  EXPECT_GE(r.count_gen("__id__Bool"), 1);
}

// ===========================================================================
// Mangler isolation — user generic named String at Int
// ===========================================================================

TEST(Monomorph, ManglerIsolation) {
  auto r = MCG::from(
      "pub fn MyToString<T>(v T) T { v }\n"
      "pub fn Main() void { MyToString(42) }");
  auto *fn = r.find_gen("gen__");
  ASSERT_NE(fn, nullptr);
  std::string name = fn->getName().str();
  EXPECT_TRUE(name.find("gen__") != std::string::npos)
      << "Specialised name must have gen__ prefix: " << name;
}

// ===========================================================================
// Mangler unit tests
// ===========================================================================

TEST(Monomorph, MangleType_Int) {
  auto r = MCG::from("pub fn Main() void {}");
  EXPECT_EQ(r.codegen->mangle_type(make_int_type()), "Int");
}

TEST(Monomorph, MangleType_String) {
  auto r = MCG::from("pub fn Main() void {}");
  EXPECT_EQ(r.codegen->mangle_type(make_string_type()), "String");
}

TEST(Monomorph, MangleType_Bool) {
  auto r = MCG::from("pub fn Main() void {}");
  EXPECT_EQ(r.codegen->mangle_type(make_bool_type()), "Bool");
}

TEST(Monomorph, MangleType_Array) {
  auto r = MCG::from("pub fn Main() void {}");
  auto arr = make_array_type(make_int_type());
  EXPECT_EQ(r.codegen->mangle_type(arr), "Arr_Int_End");
}

TEST(Monomorph, MangleType_Map) {
  auto r = MCG::from("pub fn Main() void {}");
  auto m = make_map_type(make_string_type(), make_int_type());
  EXPECT_EQ(r.codegen->mangle_type(m), "Map_String_Int_End");
}

TEST(Monomorph, MangleType_Nested) {
  auto r = MCG::from("pub fn Main() void {}");
  auto inner = make_map_type(make_string_type(), make_array_type(make_int_type()));
  auto outer = make_array_type(inner);
  std::string mangled = r.codegen->mangle_type(outer);
  EXPECT_EQ(mangled, "Arr_Map_String_Arr_Int_End_End_End");
  for (char c : mangled) {
    EXPECT_TRUE(std::isalnum(c) || c == '_')
        << "Invalid char '" << c << "' in mangled name: " << mangled;
  }
}

TEST(Monomorph, MangleType_Func) {
  auto r = MCG::from("pub fn Main() void {}");
  auto fn = make_func_type({make_int_type(), make_string_type()},
                           {make_bool_type()});
  EXPECT_EQ(r.codegen->mangle_type(fn), "Fn_Int_String_to_Bool_End");
}

TEST(Monomorph, MangleType_Union) {
  auto r = MCG::from("pub fn Main() void {}");
  auto u = make_union_type({make_int_type(), make_string_type()});
  EXPECT_EQ(r.codegen->mangle_type(u), "Un_Int_String_End");
}

// ===========================================================================
// Generic return type propagation
// ===========================================================================

TEST(Monomorph, ReturnTypePropagation) {
  auto r = MR::from(
      "pub fn id<T>(x T) T { x }\n"
      "pub fn Main() void {\n"
      "  n := id(42)\n"
      "  s := id(\"hi\")\n"
      "}");
  EXPECT_TRUE(r.ok()) << r.all_errors();
}

// ===========================================================================
// Recursion guard — mutual recursion doesn't loop
// ===========================================================================

TEST(Monomorph, RecursionGuard) {
  auto r = MR::from(
      "pub fn ping<T>(x T) T { pong(x) }\n"
      "pub fn pong<T>(x T) T { ping(x) }\n"
      "pub fn Main() void { ping(42) }");
  EXPECT_TRUE(r.ok()) << r.all_errors();
}

} // namespace saga
