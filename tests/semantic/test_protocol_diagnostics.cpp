// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// Phase 3 spike: at every protocol use site (map keys, Array/Map .String()
// receivers, string interpolation), the analyzer must reject types that
// don't satisfy the relevant std/proto interface and the diagnostic must
// cite the protocol by name.

#include "semantic/analyzer.hpp"
#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"

#include <gtest/gtest.h>

namespace saga {

namespace {

struct Diag {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;

  static Diag from(const std::string &source) {
    Diag r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    auto resolver = std::make_shared<PackageResolver>();
    resolver->sgi_search_paths.push_back(SAGA_STD_SGI_DIR);
    r.analyzer = std::make_unique<Analyzer>(r.fileset, resolver);
    r.analyzer->is_stdlib = true;
    if (r.ast)
      r.analyzer->analyze(*r.ast);
    return r;
  }

  bool has(const std::string &substr) const {
    return analyzer->has_error_containing(substr);
  }
};

} // namespace

// ===========================================================================
// Hashable use sites
// ===========================================================================

TEST(ProtocolDiag, IntKeyOk) {
  // Int satisfies Hashable via std/int.  No diagnostic.
  auto r = Diag::from(R"(
    fn f() {
      m := {1: "a", 2: "b"}
    }
  )");
  EXPECT_FALSE(r.has("Hashable"));
}

TEST(ProtocolDiag, StringKeyOk) {
  auto r = Diag::from(R"(
    fn f() {
      m := {"a": 1, "b": 2}
    }
  )");
  EXPECT_FALSE(r.has("Hashable"));
}

TEST(ProtocolDiag, StructWithoutHashRejectedAsMapKey) {
  auto r = Diag::from(R"(
    struct Foo { x Int }

    fn f() {
      foo := Foo{x: 1}
      m := {foo: 1}
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Hashable"));
  EXPECT_TRUE(r.has("Hash()"));
}

TEST(ProtocolDiag, StructWithoutHashRejectedInMapTypePosition) {
  // Map appears as a parameter type: the use site is the type position,
  // not a literal.
  auto r = Diag::from(R"(
    struct Foo { x Int }

    fn f(m {Foo: Int}) Int { 0 }
  )");
  EXPECT_TRUE(r.has("does not satisfy Hashable"));
}

TEST(ProtocolDiag, FloatKeyEmitsNanAwareDiagnostic) {
  auto r = Diag::from(R"(
    fn f() {
      m := {3.14: "pi"}
    }
  )");
  EXPECT_TRUE(r.has("Float is not Hashable"));
  EXPECT_TRUE(r.has("NaN"));
}

TEST(ProtocolDiag, FloatKeyInTypePositionEmitsNanDiagnostic) {
  auto r = Diag::from(R"(
    fn f(m {Float: String}) Int { 0 }
  )");
  EXPECT_TRUE(r.has("Float is not Hashable"));
}

TEST(ProtocolDiag, StructWithHashAndEqualsAcceptedAsMapKey) {
  // A user struct that satisfies Hashable structurally must pass.
  // Methods declared outside the struct body are the conventional form;
  // they pick up the fully-resolved struct TypePtr, unlike in-body methods
  // which see the struct symbol mid-resolution.
  auto r = Diag::from(R"(
    struct UserId {
      raw Int
    }

    pub fn (u UserId) Hash() Int64 { u.raw.Hash() }
    pub fn (u UserId) Equals(other UserId) Bool { u.raw == other.raw }

    fn f() {
      u := UserId{raw: 1}
      m := {u: "alice"}
    }
  )");
  EXPECT_FALSE(r.has("Hashable"));
}

// ===========================================================================
// Stringable use sites
// ===========================================================================

TEST(ProtocolDiag, IntInterpolationOk) {
  auto r = Diag::from(R"(
    fn f() String {
      x := 42
      "{x}"
    }
  )");
  EXPECT_FALSE(r.has("Stringable"));
}

TEST(ProtocolDiag, StructWithoutStringRejectedInInterpolation) {
  auto r = Diag::from(R"(
    struct Bar { x Int }

    fn f() String {
      b := Bar{x: 1}
      "{b}"
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Stringable"));
  EXPECT_TRUE(r.has("String()"));
}

TEST(ProtocolDiag, ArrayOfNonStringableRejectedInInterpolation) {
  // Recursive descent: [Bar] is "stringable" iff Bar is.
  auto r = Diag::from(R"(
    struct Bar { x Int }

    fn f() String {
      a := [Bar{x: 1}, Bar{x: 2}]
      "{a}"
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Stringable"));
}

TEST(ProtocolDiag, ArrayStringCallOnNonStringableElementRejected) {
  // `.String()` on an Array<Bar> requires Bar : Stringable.
  auto r = Diag::from(R"(
    struct Bar { x Int }

    fn f() String {
      a := [Bar{x: 1}, Bar{x: 2}]
      a.String()
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Stringable"));
}

TEST(ProtocolDiag, NestedArrayStringRecursiveCheck) {
  // `[[Bar]]` is itself trivially "Stringable" via the Array shell, but the
  // Phase 3 rule recurses through Array/Map so the inner Bar is required to
  // satisfy Stringable.
  auto r = Diag::from(R"(
    struct Bar { x Int }

    fn f() String {
      a := [[Bar{x: 1}], [Bar{x: 2}]]
      a.String()
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Stringable"));
}

TEST(ProtocolDiag, MapStringCallOnNonStringableValueRejected) {
  auto r = Diag::from(R"(
    struct Bar { x Int }

    fn f() String {
      b := Bar{x: 1}
      m := {1: b}
      m.String()
    }
  )");
  EXPECT_TRUE(r.has("does not satisfy Stringable"));
}

TEST(ProtocolDiag, StructWithStringAcceptedInInterpolation) {
  auto r = Diag::from(R"(
    struct Pretty {
      x Int

      pub fn (p Pretty) String() String { "P" }
    }

    fn f() String {
      p := Pretty{x: 1}
      "{p}"
    }
  )");
  EXPECT_FALSE(r.has("Stringable"));
}

} // namespace saga
