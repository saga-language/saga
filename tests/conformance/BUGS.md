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

- **`docs/language.md` is half-migrated to the current type syntax** (found
  2026-08-21). The file mixes two spellings of every primitive — 87 capitalised
  (`Int`, `String`, `Bool`, `Float`, `Void`) against 139 lowercase — and the
  capitalised ones no longer resolve: `xs Int[] = [1, 2, 3]` reports `undefined
  name 'Int'`. Eight sites also use the `Type[]` array form, which the parser
  no longer accepts; the current spellings are `array{int}` and
  `map{string: int}`. §374-419 ("The array type form is deliberately a
  *suffix* — `Int[]`, not `[Int]`") argues for a syntax the language does not
  have, so it needs rewriting rather than search-and-replace. Distinct from the
  Phase 8 doc sweep, which lists the *other* `docs/*.md` files and not
  `language.md` itself.

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

- **Binding a collection to a second name does not retain it** (found
  2026-08-21, while implementing the `arr[i] = v` write-back below). `ys := xs`
  copies the array pointer and emits no `saga_retain_array`, but both names are
  registered with `track_managed`, so scope exit releases the buffer twice. In
  `cow.sg` the IR is one `saga_array_new`, zero retains, two
  `saga_release_array` — a use-after-free that currently goes unnoticed because
  it happens as `Main` returns.

  The same missing retain is why copy-on-write never fires.
  `saga_array_make_unique` clones only when `refcount != 1`, so with the count
  stuck at 1 a write through either name is seen through both: `ys[0] = 99`
  changes `xs[0]`. That contradicts the value semantics in
  `docs/language.md` §Mutability. Maps have shown this since `m[k] = v`
  started working; arrays show it now that `xs[i] = v` does.

  Boxing shows the same hole: `b Bumper = c` stores `c`'s address in the
  interface box instead of a copy, so a method that writes through the receiver
  changes `c`. Under value semantics the box should hold its own copy.

  Root cause is that codegen has no ownership convention. `emit_expr` hands
  back a fresh +1 reference for a constructor (`saga_array_new`) and a borrowed
  +0 one for an identifier, field or element read, and nothing distinguishes
  them, so the binder retains neither. Fix shape: pick the convention — every
  producer returns +1 and every binder consumes it — and retain where a
  borrowed read becomes a binding. That is a codegen-wide change touching
  strings, arrays and maps alike, which is why it is not folded into the
  write-back.

Fixed:
- **`MultiFileImportTest` and friends shared one temp directory** (found and
  fixed 2026-08-22). Three fixtures in `tests/semantic/test_modules.cpp` built
  their scratch path from a fixed name and `remove_all`-ed it in `SetUp`, so
  under `ctest -j8` — where each case is its own process — one test wiped
  another's package files mid-run. A different subset failed on each run and
  every one passed alone. Now suffixed with the test's line number, the pattern
  `test_manifest.cpp` and `test_build_graph.cpp` already used.

- **`arr[i] = v` was a silent no-op** (filed 2026-08-16, fixed 2026-08-21).
  `emit_assign`'s array branch was an empty `else if` carrying a stale TODO —
  `saga_array_set` had been available at `src/runtime/runtime.c:1990` all
  along. What it needed was the write-back: the function *returns* an array,
  because a shared buffer is copied on write, so the result has to replace the
  one the object expression named. Index assignment now resolves its holder
  through `assign_target_address`, the same helper field assignment uses, and
  both collection kinds write their slot through `collection_slot_address` —
  which incidentally fixed a map whose value is a struct, where the old branch
  stored the pointer instead of the struct. Nested (`xss[0][1]`) and compound
  (`xs[0] += 1`) forms need no handling: both are already analyzer errors,
  because an element read is typed `T | error`.

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
