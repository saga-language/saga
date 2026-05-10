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
- `T[]` type-erased storage (perf footnote, not correctness). This is
  for adding "stencil" like behaviour where all allocated types use the
  current (slow) behaviour, but concrete basic types have sized arrays
  that take advantage of type-specific commands for operations, each
  basic type used will get its own monomorphic implementation.
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

---

## Open

### Calling through a union-of-interfaces value segfaults at runtime

- Spec source: `docs/language.md:961-965` — wider interface assignable
  to narrower one when subset.
- Conformance test:
  `tests/conformance/interfaces/subset_assignable.sg`
- Behavior: `const RC = Reader | Closer; rc RC = File{}; ReadOnly(rc)`
  builds clean (analyzer recognizes interface widening) but SIGSEGVs
  at the call site.  Codegen path for passing a union-of-interfaces
  value to a single-interface parameter doesn't extract the right
  vtable.
- Priority: medium — affects the canonical interface-composition
  pattern but only at runtime; declaration and type-check work.


### `T[]` array element storage is type-erased to machine words

- Spec source: `docs/stdlib.md:18` documents `T[]` as "compiled once
  with type-erased `T`." This is intentional, but worth tracking as a
  performance footnote — `Int8[]` consumes the same memory as `Int[]`,
  so small-type arrays don't shrink RSS or improve cache locality.
- Decision (2026-05-06): defer. Not a correctness bug.
- Fix shape: monomorphize `T[]` for primitive element types
  (Int8/16/32, Float32, Bool, Char), keep type-erased path for
  arbitrary `T`.

### Reading an ignored (`_`-prefixed) variable produces a misleading diagnostic

- Spec source: `docs/language.md:25-27` — "Identifiers that start with
  [...] an underscore are 'ignored' variables. They can not be accessed
  once they are assigned a value."
- Conformance test:
  `tests/conformance/identifiers/ignored_var_cannot_read.sg`
- Behavior: the access is correctly rejected, but the error reads
  `type Void has no member 'String'`, suggesting the compiler treats
  reading `_x` as reading a Void value rather than reporting the
  ignored-variable rule directly.
- Priority: low — correctness is fine; this is a diagnostic-quality
  issue.

