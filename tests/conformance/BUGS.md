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

- **A struct literal cannot be a bare binary operand** (found 2026-08-16).
  `a + Money{cents: 7}` reports "cannot use type 'Money' as a value"; the
  literal has to be parenthesised. The `{` that opens a struct literal is an
  infix operator at binding power 1 — the lowest non-zero, chosen so a context
  that must stop before a `{ body }` block can do it with `parse_expr_bp(1)` —
  and every real infix operator parses its right side well above that, so the
  `{` is never reached.

  Precedence is the wrong instrument: the question is not how tightly `{`
  binds but *where* a literal is allowed, and those are different axes. Go
  answers it with a parser flag (`exprLev`) that forbids composite literals
  only in `if`/`for`/`switch` headers and allows them everywhere else,
  including as binary operands. Fix shape: make `{` an ordinary suffix on a
  type-name operand and suppress it while parsing a statement header, rather
  than encoding the restriction as a binding power that applies everywhere.

- **`arr[i] = v` is a silent no-op** (found 2026-08-16). Every element type,
  not just structs: `xs := [1, 2, 3]` then `xs[0] = 99` leaves `xs[0]` at 1,
  with no error and no diagnostic. `emit_assign`'s array branch is an empty
  `else if` carrying `// TODO: implement saga_runtime_array_set when
  available`. The map branch beside it is implemented, which is why
  `m[k] = v` works.

  **The TODO is stale — the runtime function has been available.**
  `saga_array_set` is defined at `src/runtime/runtime.c:1990` and already
  declared into the module by `codegen_runtime.cpp:171`. The reason it is not
  simply a call is its signature: it *returns* an array, because a shared
  backing buffer is copied on write, so the result has to be written back to
  whatever holds the array — a local, a field, or another element. That
  write-back is the actual work, and it is the same question `Push`/`Append`
  already answer for the method spelling.

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
