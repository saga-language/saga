# Spec/Implementation Drift

Each entry: spec source line, the failing conformance test (or a one-liner
repro until a test exists), and a brief note on what's broken.

## Order of attack

Tackle the open list in clusters, not top-to-bottom — bugs in the same
cluster share their mental context, so fixing one makes the next much
faster than starting cold.

1. **Parser & shallow checks** — small, isolated grammar fixes plus
   missing analyzer checks. Same surface (parser/analyzer); fastest
   way to drop the count. Examples: `**`, `or` in parens, C-style
   `for`, multi-value `case`, `const X = A | B`, typed array decl,
   non-exhaustive type-switch acceptance, array-to-variadic.
2. **Union / narrowing / for-expression typing** — high leverage,
   shared codegen + type-inference paths. Fixing the narrowing
   segfault likely unblocks the optional-field and `break <value>`
   bugs at once. Examples: type narrowing crash, optional field
   with `or`, capture-form `or |err|`, accumulator typing, union
   method dispatch.
3. **Standalones** — each is its own mini-investigation; do in any
   order. Examples: `Task.Wait()` ordering, range iteration, string
   indexing/slicing, closures with params/captures, alias method
   dispatch, COW on function call, generic-struct ABI mismatch.

Deferred:
- Comma-separated struct fields on a single line (`struct Point { x Int,
  y Int }`). The parser currently requires one field per line. Decide
  whether the spec should allow `,`/`;`-separated fields inline as a
  shorthand, or whether the line-per-field form stays the only one.
- Arrays should support sizing: `T[42]`. Pre-allocated arrays. These 
  are not fixed arrays but arrays that start at a fixed size.
- Are SGIs regenerated or cleared appropriately? Loads of issues with 
  stale SGI files.
- `const` for locals. Needs some thinking/talking out to decide if and
  how to implement.
- `T[]` type-erased storage (perf footnote, not correctness). Currently
  all `T[]` storage uses i64 slots; small element types (Int8/16/32,
  Float32, Bool, Char) waste space and cache locality.  Fix shape:
  monomorphize `T[]` for primitive element types, keep the type-erased
  path for arbitrary `T`.  Decision 2026-05-06: defer.
- Re-test interface composition under the new syntax (per
  `ai/slimmer-type-system-proposal.md`).  When `interface X < Y, Z {}`
  lands, port the deleted `subset_assignable` scenario: a value of the
  composed interface should be assignable to either constituent
  interface and dispatch correctly through it.  Same canonical pattern,
  in the syntax that's actually going to ship.

---

## Open

(none)

