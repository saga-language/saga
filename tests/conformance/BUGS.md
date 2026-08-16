# Spec/Implementation Drift

Open:
- **A nominal alias is second-class in the analyzer** (found 2026-08-16, while
  fixing the codegen half below). Four symptoms, one theme — the analyzer
  decides what an alias *is* by structure where it should decide by identity
  and behaviour:

  1. It is not assignable to a union that names it: `Coord` into `int | Coord`
     is rejected, which makes a nominal alias unusable as a union alternative
     at all.
  2. It does not satisfy an interface its methods satisfy, whether the methods
     are its own or inherited from the underlying.
  3. `is` does not narrow through one: with `c Choice` and
     `type Choice int | string`, `if c is int` leaves `c` typed `Choice`.
  4. A string literal is not untyped for the nominal-slot carve-out, so
     `f(7)` is accepted where `f("hi")` is not.

  (1) and (2) share a root cause: `is_assignable_to` (`semantic/types.cpp`)
  rejects any alias source outright, *before* the union-membership and
  interface-satisfaction checks further down. Every structural rule below that
  point keys on `source->kind`, which is `Alias` and matches none of them — so
  deleting the early return yields exactly the intended semantics on paper.

  **Do not just delete it.** Tried 2026-08-16: it compiles, and then
  `viaalias(c)` silently takes the wrong branch. `union_tag_for_type` matches
  alternatives with `types_equal` against codegen's `semantic_type`, which
  unwraps aliases — so `Coord` never matches the `Coord` alternative, the tag
  lookup returns -1, and `emit_union_wrap` passes the value through untagged.
  Opening assignability without making union alternative identity alias-aware
  end-to-end trades a clean compile error for a silent wrong answer.

  So the real question is a design one: does `int | Coord` distinguish `Coord`
  from `Point` when the two have identical layout? It has to if nominal
  aliases are distinct types (Go's type switch does distinguish them), and
  that means union tagging is an *identity* question — the same side of the
  line as method dispatch, not the shape side. (2) additionally needs the two
  implementations of interface satisfaction reconciled: `is_assignable_to` has
  an inline partial replica that only gathers methods from a `Struct` source,
  and says so in a comment.

Fixed:
- **A nominal alias of a struct read as empty** (filed 2026-08-15, fixed
  2026-08-16). Codegen's `semantic_type` unwrapped *structural* aliases only,
  so every `kind == Struct` test in codegen — 40 of them — had to finish the
  job itself, and did not. Split into `semantic_type` (the shape, aliases
  unwrapped) and `declared_type` (the recorded type, aliases intact); method
  dispatch is the one caller that wants the latter, because an alias-bound
  method is found by the alias's own name. The caller/callee ABI split that
  surfaced underneath it — the call site deciding byval by type *kind* while
  the declaration decides by LLVM *shape* — is now one predicate,
  `byval_param_type`.

Deferred:
- `T[]` type-erased storage (perf footnote, not correctness). Currently
  all `T[]` storage uses i64 slots; small element types (int8/16/32,
  float32, bool, byte) waste space and cache locality.  Fix shape:
  monomorphize `T[]` for primitive element types, keep the type-erased
  path for arbitrary `T`.  Decision 2026-05-06: defer.
- Re-test interface composition under the new syntax (per
  `ai/slimmer-type-system-proposal.md`).  When `interface X < Y, Z {}`
  lands, port the deleted `subset_assignable` scenario: a value of the
  composed interface should be assignable to either constituent
  interface and dispatch correctly through it.  Same canonical pattern,
  in the syntax that's actually going to ship.
