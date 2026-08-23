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

- **A collection element and an interface box hold a managed value unowned**
  (found 2026-08-21, narrowed twice: 2026-08-22 when local bindings were fixed,
  2026-08-23 when struct fields were). `saga_array_clone` memcpys the element
  bytes and `saga_release_array` frees the buffer without touching them, so
  `[[1, 2], [3, 4]]` leaks both inner arrays — measured under ASan. Map values
  have the same hole. Both are leaks rather than wrong answers, because reading
  an element back through a second name cannot be written through: a nested
  index assignment is an analyzer error.

  Interface boxing is the struct-shaped version: `b Bumper = c` stores `c`'s
  address rather than a copy, so a method that writes through the receiver
  changes `c`.

  A container cannot release what it holds because it does not know what that
  is: the header carries `elem_size` and nothing else, so `saga_release_array`
  has no way to reach the elements. Either the header gains a per-element
  release function that codegen fills in at construction — it is the side that
  knows the type, and the struct walkers already have the matching `void(ptr)`
  signature — or `T[]` is monomorphized, which is already filed below as a
  performance deferral. The sharp edge is the stdlib: an element codegen did
  not retain reaching an array that does release is a double free, so the
  runtime's own array paths need an audit alongside. The struct half needed no
  runtime change, because codegen emits the walk per struct type.

- **A map is not copy-on-write** (found 2026-08-22). `m2 := m1` retains, and
  `m2["a"] = 99` is still visible through `m1`, because the machinery an array
  has is missing entirely: there is no `saga_map_clone` and no
  `saga_map_make_unique`, and `saga_map_set` writes in place and returns void.
  This is not the retain bug — the retain is emitted, there is just nothing
  reading the count. `docs/language.md` §Mutability promises copy-on-write for
  "large/complex types" without qualifying it to arrays.

  Fix shape: mirror the array path — a clone, a `make_unique`, and a
  `saga_map_set` that returns the map to keep. Codegen is already shaped for
  it: `emit_map_index_assign` resolves its holder the same way
  `emit_array_index_assign` does, so it only needs the write-back and the
  matching release. Note `kMutatingIntrinsics` treats `saga_map_set` as an
  in-place mutation for the stdlib's own use; that path wants to stay in-place.

Fixed:
- **Every array argument was deep-copied, and the copy was never freed**
  (found and fixed 2026-08-23, under the ASan run for the struct-field fix).
  `emit_call_expr` called `saga_array_clone` on every array argument to a
  non-extern callee, reading the spec's "values that escape their scope are
  copied" as a copy at the boundary. The callee was meant to release the clone,
  but the tracking asked `semantic_type` for the parameter's type and a type
  node has no entry in `node_types`, so it silently never fired: every call
  taking an array leaked one.

  The clone predates copy-on-write working. A parameter slot is a binding like
  any other, so it takes a reference the way `ys := xs` does, and a callee that
  writes finds the buffer shared and gets the copy — the same one copy-on-write
  would have made. A callee that only reads now pays an increment instead of a
  full buffer copy. The resolver is `lookup_sem_type`, which asks the analyzer
  rather than the node table.

- **A struct field held a managed value unowned** (filed 2026-08-21, fixed
  2026-08-23). `Box{xs: xs}` stored the array into the field without retaining
  it, so the field and the local both claimed the only reference: writing
  through the field found a refcount of 1, took it for uniquely owned and
  edited the buffer both names saw. Returning such a struct was worse — the
  local was released on the way out and the field pointed at freed memory.

  A struct now owns one reference to each managed value it holds, which is the
  local convention one level down: a field initialised from a binding retains,
  overwriting a field releases what it held, and a struct dying releases its
  fields. The walk is a generated function per struct type rather than inline
  IR, so a nested struct costs one call and a recursive shape terminates.
  Copying a struct retains through the same walk, which is what makes
  copy-on-write fire for a field.

  Errors are excluded: an error box escapes through a union and is never freed,
  so releasing its message would leave the box pointing at freed memory.

- **A second name for a collection did not take a reference** (filed
  2026-08-21, fixed 2026-08-22). `ys := xs` copied the pointer and emitted no
  `saga_retain_array`, so the count stayed at 1: `saga_array_make_unique` read
  that as uniquely owned and wrote in place, making `ys[0] = 99` visible
  through `xs`, and scope exit released the buffer twice.

  The convention now is that a managed local slot owns exactly one reference.
  A value read out of an existing binding is borrowed — `is_borrowed_expr`
  covers an identifier, a field selector and a parenthesised one — so the
  binding retains it; everything else (a literal, a call, a copy-on-write
  method) already hands back a counted reference. Two matching holes closed
  with it: a returned local was released before the `ret` that handed out the
  freed pointer (`fn build() array{int} { xs := [1,2,3]  xs }` exited 1), and
  the `arr[i] = v` write-back dropped the slot's old reference on the floor
  because `saga_array_set` returns its own +1.

  Not covered: struct fields, collection elements and interface boxes, which
  are open above.

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
