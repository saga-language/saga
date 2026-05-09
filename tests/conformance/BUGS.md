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

### `Task.Wait()` doesn't block reliably / output ordering wrong

- Spec source: `docs/language.md:1445` — `t.Wait() // block until the
  thread finishes`.
- Conformance test: `tests/conformance/concurrency/spawn_basic.sg`
- Behavior: `t := spawn { io.Println("from task") }; t.Wait();
  io.Println("done")` produces output ordering `done\nfrom task`
  (parent's "done" runs before the spawned task's print). Spec
  semantics require Wait to block until the task's body has run to
  completion, which means "from task" must appear before "done".
- Priority: high — concurrency primitive correctness.

### Range iteration produces no output and `.Array()` doesn't return an array

- Spec source: `docs/language.md:1038-1054` — `(0..N)` is iterable
  via `for i : (0..N) { ... }`, and `.Array()` converts to `T[]`.
- Conformance tests:
  - `tests/conformance/range/range_in_for_loop.sg` — the for-loop
    builds and runs but produces no output. The loop body never
    executes.
  - `tests/conformance/range/range_to_array.sg` — `(0..3).Array()`
    is rejected: `type Void has no member 'Size'`. Either `.Array()`
    isn't implemented on Range, or it returns Void.
- Priority: high — ranges are a documented core iteration form and
  the for-loop case is the headline example in the spec.

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

### String indexing and slicing aren't implemented

- Spec source: `docs/language.md:687-705` — string can be indexed
  (`str[i]`) and sliced in four forms (`str[a..b]`, `str[..b]`,
  `str[a..]`, `str[..]`).
- Conformance tests:
  `tests/conformance/strings/string_index.sg`,
  `tests/conformance/strings/string_slice_*.sg`
- Behavior:
  - `str[1]` builds but `c := str[1]` produces no IR for the
    indexing — the LHS alloca is allocated but never stored to. The
    `io.Println(c)` call then reads uninitialized memory and prints
    process environment bytes.
  - Slice forms (`str[a..b]`, `str[..b]`, etc.) all SIGSEGV at
    runtime — the slice node's codegen path appears to call into a
    runtime function that doesn't exist or returns garbage.
- Fix shape: needs runtime functions (a `saga_string_at`-equivalent
  returning a one-char saga_runtime_string, plus a
  `saga_string_slice(start, end)`), and codegen branches in
  `emit_index_expr` (currently has only Array and Map paths) and
  the slice-form path.
- Priority: high — both forms are documented in the spec and used
  in the §Strings examples.

### Capture-form `or |err| { err.Message() }` for indexed access

- Spec source: `docs/language.md:411-414, 425-429` — `or |err| { ... }`
  binds the captured error and is documented as supporting method
  calls on it (e.g. `log.Error(err)`, `err.Message()`).
- Conformance test:
  `tests/conformance/types/or_clause_capture_error.sg`
- Behavior: now that `arr[i]` correctly produces a tagged `T | Error`
  union (see Resolved entry below), the err-branch of `or |err|` runs.
  But the err-payload slot holds a zero-valued empty struct (Missing),
  not a runtime fat pointer to a Missing instance with vtable.  Method
  dispatch via the vtable then derefs zero and segfaults.
- Fix shape: needs runtime additions —
  - a `saga_runtime_missing` data type and a
    `saga_missing_vtable_instance` global that maps `Message` to a C
    function returning a constant string,
  - a constructor (`saga_missing_error_new` or similar) that returns
    a heap-allocated `saga_runtime_iface_fat_ptr` matching the shape
    used by `saga_error_from_trap`,
  - codegen change to call that function in place of the current
    "wrap a zero-sized empty struct" path used by both
    `intrinsic_runtime_try`'s failure path and the array/map index
    null branch.
- Note: the same gap means `intrinsic_runtime_try` failures *also*
  produce err values whose `.Method()` calls would segfault — this
  isn't unique to indexed access, it's just the first place a
  conformance test exercises it.
- Priority: medium — default-value form (`or { default }`) works,
  which is the more common case.


### Cross-package generic struct method has calling-convention mismatch (SIGSEGV)

- Spec source: this is a runtime test, not derived from a single
  language-spec sentence; covers the generic-struct + non-generic
  method form that `docs/language.md` §Methods/§Structs collectively
  promise.
- Existing test (pre-existing failure, not introduced by this work):
  `tests/runtime/cross_package/generic_struct_nongeneric_method`
  (`lib.Box{value T}` with `Get() T`, called as `b.Get()` from `app`).
- Behavior: the binary segfaults at runtime.  The generated IR shows
  the specialization is declared with a pointer parameter
  (`define linkonce_odr i64 @gen__app__Box__Get__Int(ptr %b)`) but the
  call site passes the struct by value
  (`call i64 @gen__app__Box__Get__Int(%"saga.lib__Box<Int>" %b1)`).
  The body then GEPs through `%b` as if it were a pointer, derefs the
  scalar `42` as an address, and crashes.  Either the call site needs
  to pass-by-pointer (place the struct in an alloca and pass `&b`) or
  the specialization body needs to receive by value.
- Priority: high — generic structs are a documented core feature and
  this crashes the simplest user program that combines them with a
  cross-package import.
- Side note: the specialization is mangled `gen__app__Box__...` even
  though `Box` is defined in `lib`. May be related, may be cosmetic.

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

