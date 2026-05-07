# Spec/Implementation Drift

Each entry: spec source line, the failing conformance test (or a one-liner
repro until a test exists), and a brief note on what's broken.

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

### `**` (exponential) returns 0 instead of the power

- Spec source: `docs/language.md:1377-1378` — `**` is listed as an
  arithmetic operator alongside `+ - * / %`.
- Conformance test:
  `tests/conformance/binary_operators/arithmetic.sg`
- Behavior: `2 ** 10` evaluates to `0` instead of `1024`. Either the
  operator isn't lowered or it's mis-lowered to a no-op.
- Priority: medium — useful operator, narrow workaround (manual loop).

### `or` clause inside parentheses doesn't parse

- Repro: `(20 / 4 or { 0 }).String()` cascades:
  `expected ')', got 'or'`.  Workaround is to bind the result first:
  `x := 20 / 4 or { 0 }; x.String()`.
- Spec source: not explicitly forbidden — the spec shows
  `(6 / 0 or { 0 }) + 1` at `docs/language.md:1399`, suggesting the
  parens-wrapped form is intended to work.
- Priority: low — workaround is trivial, but the spec example
  suggests this should parse.

### C-style index for-loop (`for i Int; i < N; i += 1 {}`) doesn't parse

- Spec source: `docs/language.md:1223-1224` — explicit example.
- Conformance test:
  `tests/conformance/looping/c_style_index_iterator.sg`
- Behavior: parser cascades on `for i Int;`:
  `expected '{', got identifier`. The form is documented as one of
  the four valid `for` shapes but the parser doesn't recognize it.
- Priority: high — this is the canonical numeric-counter form,
  workaround requires manual condition + `+=` plumbing.

### `break <value>` doesn't shape the for-expression as `T | Error`

- Spec source: `docs/language.md:1284-1295` — "If `break` is present
  anywhere in the block, the expression becomes impure, returning
  from the loop immediately, and returning a `Missing` error. The
  return type of the `for` expression becomes `T | Error`."
- Conformance test:
  `tests/conformance/looping/break_with_value.sg`
- Behavior: `result := for word : [...] { if cond { break word } }`
  is type-checked as Void rather than `String | Error`, so `result or { "?" }`
  fails: `argument 1: expected String, got Void`.
- Priority: medium — search-pattern uses of `for` are common.

### Type narrowing (via `if value == T` and `switch value { case T: ... }`) segfaults at runtime

- Spec source: `docs/language.md:1136-1166` — type matching with
  conditionals narrows union types within branches.
- Conformance tests:
  - `tests/conformance/conditionals/type_narrowing_if.sg`
  - `tests/conformance/conditionals/type_matching_switch.sg`
- Behavior: build clean, runtime SIGSEGV. The `if v == Int { v.String() }`
  pattern (and the switch-case-on-types equivalent) over a `Int | Float`
  union segfaults when the branch body uses `v` after narrowing — the
  union extraction probably reads through a payload pointer that hasn't
  been set up correctly.
- Priority: high — type matching is the primary mechanism for working
  with union values.

### Multi-value case (`case 0, 2, 4:`) doesn't parse

- Spec source: `docs/language.md:1110-1111` — "Case clauses can have
  multiple expressions separated by commas to match multiple values
  to a single clause."
- Conformance test:
  `tests/conformance/conditionals/switch_multi_value_case.sg`
- Behavior: parser cascades on the first comma:
  `expected ':', got ','`. The form is documented but not recognized.
- Priority: medium.

### Non-exhaustive type-switch is silently accepted

- Spec source: `docs/language.md:1174-1177` — non-exhaustive type
  matching without `else` must be an error.
- Conformance test:
  `tests/conformance/conditionals/non_exhaustive_type_switch_rejected.sg`
- Behavior: the analyzer accepts a switch that covers only one of
  three union alternatives (no `else`).  Should be rejected at
  semantic analysis.
- Priority: medium.

### Range iteration produces no output and `.Array()` doesn't return an array

- Spec source: `docs/language.md:1038-1054` — `(0..N)` is iterable
  via `for i : (0..N) { ... }`, and `.Array()` converts to `[T]`.
- Conformance tests:
  - `tests/conformance/range/range_in_for_loop.sg` — the for-loop
    builds and runs but produces no output. The loop body never
    executes.
  - `tests/conformance/range/range_to_array.sg` — `(0..3).Array()`
    is rejected: `type Void has no member 'Size'`. Either `.Array()`
    isn't implemented on Range, or it returns Void.
- Priority: high — ranges are a documented core iteration form and
  the for-loop case is the headline example in the spec.

### `const X = Iface1 | Iface2` (interface-union alias) doesn't parse as a type

- Spec source: `docs/language.md:945-948` — explicit examples like
  `const ReadWriter = Reader | Writer`,
  `const ReadCloser = Reader | Closer`, etc.
- Conformance test:
  `tests/conformance/interfaces/subset_assignable.sg`
- Behavior: declaration emits two errors:
  `bitwise operator requires integer type, got Reader` (the `|` is
  parsed as bitwise OR over typed values rather than a type-union
  constructor), then `'ReadCloser' is not a type` at the use site.
  The corresponding *type-position* form (e.g. `f Reader | Writer`
  in a parameter list) does work in some contexts.
- Priority: high — this is the documented composition mechanism for
  interfaces and the spec leans heavily on it.

### Optional field `Type | Missing` resolved with `or { default }` returns empty value

- Spec source: `docs/language.md:833-846` — optional field via
  `String | Missing` and `data.optional or { "unknown" }`.
- Conformance test:
  `tests/conformance/structs/optional_field_with_or.sg`
- Behavior: `p := Payload{optional: Missing{}}` then
  `v := p.optional or { "unknown" }` builds and runs but prints
  the empty string — neither the value nor the `or` default. The
  union extraction or the or-clause isn't routing to the fallback.
- Priority: medium — common pattern for parsing optional data.

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

### Typed-value-to-alias passing crashes the compiler (and may be too permissive)

- Spec source: `docs/language.md:558-559` — "Aliases are not shadows
  of the original type, they are unique types".
- Conformance test:
  `tests/conformance/type_aliases/alias_distinct_from_underlying.sg`
- Behavior: `const UserID = Int; fn TakeUserID(u UserID) Void {...};
  i Int = 7; TakeUserID(i)` — the analyzer's `is_assignable_to`
  currently unwraps aliases on either side (transparent) and accepts
  the call.  Codegen then SIGSEGVs trying to lower the conversion.
- Two questions tangled here:
  1. **Spec-strict reading**: typed `Int` to alias `UserID` should be
     a *type error*; the user must explicitly convert.  In that case
     `is_assignable_to`'s alias-transparency is too aggressive — it
     should let through *untyped* numeric literals (so
     `uid UserID = 5` still works) but reject typed values without
     conversion.
  2. Either way, the codegen crash on the currently-permissive path
     is a separate bug.
- Priority: medium — flagged as a possible spec/impl-direction
  decision before fixing.

### User-defined method on an alias isn't dispatched correctly

- Spec source: `docs/language.md:559-562` — aliases inherit methods
  AND "methods can be bound to any type provided it is within the
  same file scope."
- Conformance test:
  `tests/conformance/type_aliases/alias_extension_method.sg`
- Behavior: `const UserID = Int; pub fn (u UserID) Display() String
  {...}; uid UserID = 7; uid.Display()` builds cleanly but SIGSEGVs
  at runtime. The method-dispatch alias-unwrap landed earlier (so
  the underlying-Int method table works) probably erases the alias's
  own method table — the `Display` user method either isn't being
  registered against UserID, or the dispatch's unwrap is bypassing
  the alias's methods slot.
- Priority: high — defining methods on aliases is a documented core
  feature.

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

### Method dispatch on union types doesn't try alternatives

- Spec source: `docs/language.md:363-371` (pure unions); also §Method
  Calling and the broader spec promise that intrinsic types carry
  methods like `.String()`.
- Conformance test:
  `tests/conformance/types/pure_union_holds_either.sg`
- Behavior: `x Bool | Int = 5; io.Println(x.String())` is rejected
  with "type Bool | Int has no member 'String'". Both alternatives
  individually have a `String()` method (Int via std/int, Bool via
  std/bool), so the dispatch should accept the call when *every*
  alternative has a method of the same shape (similar to how the
  alias-unwrap fix made aliases transparent).
- Priority: medium — affects any code that prints a union directly.
  Workaround: narrow first via type matching before calling.

### Float32 and Float64 do not carry stdlib methods like String

- Spec source: `docs/language.md:302-303` lists Int8/16/32/64,
  UInt8/16/32/64, Float32, Float64 as full-citizen numeric types;
  §"Methods on Intrinsic Types" (line 338-349) says intrinsic types
  have `String()` etc.
- Conformance test (the failing portion of):
  `tests/conformance/types/numeric_width_variants.sg`
- Behavior: `f32 Float32 = 5.0; f32.String()` is rejected with "type
  Float32 has no member 'String'". `std/float/float.sg` only defines
  methods on `Float`; there's no `Float32`/`Float64` overload.  The
  Int family covers all widths in `std/int/int.sg`; the Float family
  doesn't.
- Priority: medium — stdlib coverage gap, same pattern as Int but
  unfilled.

### Closures with parameters or captures are broken

- Spec source: `docs/language.md:1015-1036`
- Conformance tests:
  - `tests/conformance/expression_statements/fn_as_expression.sg` —
    `add1 := fn(x Int) Int { x + 1 }; add1(5)` returns a stack-address
    value instead of `6`.
  - `tests/conformance/closures/closure_captures_local.sg` —
    `i := 1; closure := fn (x Int) Int { x + i }; closure(2)` SIGSEGVs
    at runtime.
- Surface map:
  - Zero-param anonymous fn (`fn () Int { 42 }`) — works
    (`tests/conformance/closures/anonymous_no_capture.sg` passes).
  - One+ param, no capture — body runs but the call returns
    uninitialized memory.
  - Capture of a local — segfault on call.
- Suggests parameter-binding is wrong inside the closure body, with
  capture additionally tripping over an upvalue layout that isn't
  populated correctly (or at all).
- Priority: high — closures are a documented core feature.

### `for` as expression with accumulator: result is wrong or rejected

- Spec source: `docs/language.md:1267-1294` — accumulator semantics:
  "the compiler uses the type of the left-hand value to initialize an
  internal accumulator [...] The result of the expression is the
  accumulator."
- Conformance tests:
  - `tests/conformance/expression_statements/for_as_expression.sg`
    (with explicit `sum Int =` LHS)
  - `tests/conformance/methods/variadic_call.sg` and
    `tests/conformance/methods/variadic_with_array.sg` (when the
    for-as-expression is the tail expression of a function whose
    signature declares the return type)
- Behavior:
  - With explicit LHS type, builds & runs but evaluates to 0 instead
    of the accumulated total.
  - As a function tail expression with declared return type Int, the
    analyzer infers the accumulator as Void, then rejects
    `acc += i` ("compound assignment requires numeric type, got
    Void") and the function as a whole ("return type: expected Int,
    got Void"). Suggests the accumulator-type inference picks up the
    LHS type only when the LHS is a typed declaration, not a function
    return position.
- Priority: high — accumulator-form `for` is described in the spec
  as "the secret superpower of `for`."

### Array passed to variadic parameter is rejected

- Spec source: `docs/language.md:276-285` — "Arrays of the same type
  do not need to be spread into a variadic. There is literally no work
  for the compiler to do if you pass an array in."
- Conformance test: `tests/conformance/methods/variadic_with_array.sg`
- Behavior: with `fn Sum(args ...Int) Int { ... }`, calling
  `Sum([1, 2, 3, 4, 5])` is rejected with "variadic argument 1:
  expected Int, got [Int]". The spec explicitly permits this form.
- Priority: medium — convenient form, documented, currently unusable.

### Forward-declared constant reference crashes the compiler

- Spec source: `docs/language.md:120-122` — "A constant initialiser
  must not read a constant declared later in the same package; the
  read will see the type's zero value with no diagnostic."
- Conformance test:
  `tests/conformance/initialisation/forward_const_read_returns_zero.sg`
- Behavior: `const Second = First * 2; const First = 10` causes the
  saga compiler to terminate with SIGSEGV inside `emit_const_decl`
  while creating Second's GlobalVariable.  The semantic analyzer
  presumably returns Error/void for the unresolved forward reference,
  and that flows into `llvm_type` → `void` → `getNullValue(void)` →
  crash.
- Spec note: the rule itself ("zero value with no diagnostic") is
  unusual — most languages either implement two-pass declaration so
  textual order doesn't matter, or report an error.  Worth raising
  with you whether this is the design you actually want before fixing
  the compiler to honor it.  Either way, the current crash is
  unambiguously wrong.
- Priority: high — small program, hard crash with no diagnostic.

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

### Mutation of global const collections is not rejected

- Spec source: `docs/language.md:96` — "Mutation of global objects is
  prohibited."
- Conformance test: `tests/conformance/constants/mutate_global_array.sg`
- Behavior: `const Primes = [2, 3, 5]` followed by `Primes.Push(7)` is
  silently accepted, and the runtime push succeeds (size becomes 4).
  Same underlying problem as const-scalar-mutability above: the analyzer
  doesn't flag mutating method calls on Constant-kind symbols.
- Priority: high.

### Function parameters do not enforce value semantics for arrays

- Spec source: `docs/language.md:51` — "Values that escape their scope
  are copied."
- Conformance test:
  `tests/conformance/mutability/value_semantics_function.sg`
- Behavior: passing `arr [1,2,3]` to `fn Append(a [Int])` that calls
  `a.Push(99)` mutates the caller's array (Size goes 3 -> 4). Either the
  function-call boundary isn't being treated as an escape, or the
  copy-on-write logic in the array runtime isn't triggering before the
  push when the value's refcount is shared with the caller.
- Priority: high — this breaks the documented memory model and would
  silently corrupt code that relies on it.

### `[T]` array element storage is type-erased to machine words

- Spec source: `docs/stdlib.md:18` documents `[T]` as "compiled once
  with type-erased `T`." This is intentional, but worth tracking as a
  performance footnote — `[Int8]` consumes the same memory as `[Int]`,
  so small-type arrays don't shrink RSS or improve cache locality.
- Decision (2026-05-06): defer. Not a correctness bug.
- Fix shape: monomorphize `[T]` for primitive element types
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

### Typed array declarations are unrecognized (`arr [Int]` and `arr [Int] = ...`)

- Spec source: `docs/language.md:600-605` — the spec lists three valid
  array declaration forms:
  - `arr2 [Int] = []` (typed with empty literal initializer)
  - `arr3 [Int]` (bare typed declaration, zero-value)
  - `arr4 := [1]` (inferred from initializer)
- Repro: any of
  ```
  arr [Int] = [1, 2, 3]   // Error: undefined name 'arr'
  arr [Int]               // Error: undefined name 'arr'
  ```
  The same forms for scalars (`x Int = 5`, `x Int`) parse fine — only
  the array-type-prefix form fails.  The inferred form (`arr := [1]`)
  works.
- Conformance tests:
  `tests/conformance/type_literals/array_typed_with_initializer.sg`,
  `tests/conformance/type_literals/array_typed_zero_value.sg`
- Priority: high — both are documented, recommended forms.

### Empty array literal with no inferrable type is silently accepted

- Spec source: `docs/language.md:601` — "arr1 := [] // invalid, no
  inferrable type"
- Conformance test:
  `tests/conformance/type_literals/empty_array_no_type_rejected.sg`
- Behavior: `arr := []` builds and runs without diagnostic; the spec
  says this should be a type error.  The analyzer apparently picks
  some default element type rather than rejecting the ambiguous case.
- Priority: medium — silent acceptance of an explicitly-disallowed
  form.

### Nested map literal with explicit type prefix doesn't parse

- Spec source: `docs/language.md:609-614`:
  ```
  registry := {String: {String: Int}}{
    "production": {"port": 80, "timeout": 30},
    ...
  }
  ```
- Conformance test:
  `tests/conformance/type_literals/nested_map_with_type_prefix.sg`
- Behavior: parser cascades:
  `expected identifier, got string literal`,
  `expected ':', got string literal`. The `{KType: VType}{...}` map
  literal-with-type-prefix syntax isn't recognized by the parser.
- Priority: medium — documented form for instantiating typed maps,
  workaround is to declare via `const` first.

---

## Resolved

### Struct methods broadly broken (compiler crash, link error, runtime crash) — FIXED

- Symptoms (single-package mode):
  1. Out-bound method `fn (p Point) Add(...) ...` SIGFPE'd in LLVM's
     instruction selection.
  2. In-bound method (`pub fn FullName() String { ... }` inside a
     struct body) wasn't codegen'd, leading to a link-time
     "undefined reference" error.
  3. Calling an inherited method through `struct Child < Parent`
     SIGSEGV'd at runtime.
- Root cause #1 (the LLVM crash): the analyzer derived the package
  name from the *directory* (per spec — "A directory forms the scope
  and name of a package"), but `cmd/build.cpp` (and `cmd/run.cpp`)
  derived codegen's `module_name` from the *file stem* when invoked
  on a single `.sg` file.  Mismatched mangling produced two distinct
  versions of every type (`%saga.<dir>__T` and `%saga.<file>__T`)
  and a call site that didn't match the emitted definition's
  signature, which LLVM choked on during lowering.
- Root cause #2 (out-bound method body): the receiver-method
  forward-declaration path included the receiver in the LLVM
  signature, but `emit_func_decl` didn't skip receiver methods, so it
  emitted *another* function for the same source decl using a
  receiver-less signature.  The body wired the wrong arg index for
  the receiver, never wrote the result back through the sret pointer,
  and the call site dispatched to the right symbol but landed on a
  body that had been emitted into the *other* (mismangled) function.
- Fixes:
  - `src/cmd/build.cpp`, `src/cmd/run.cpp`: derive `module_name` from
    `analyzer.current_package_name()` (with file-stem as fallback).
  - `src/ir/codegen_decls.cpp::declare_functions`: skip any
    receiver-bearing decl — these are forward-declared by the
    method-symbol path with the correct signature.
  - `src/ir/codegen_stmts.cpp::emit_func_decl`: same — skip
    receiver-bearing decls; method bodies are emitted by
    `emit_struct_methods` (and the intrinsic / generic-collection
    paths).
  - `src/ir/codegen_decls.cpp::emit_struct_methods` (out-bound body
    section): account for the sret-arg offset when binding the
    receiver and regular params; on tail-expression with sret, memcpy
    the result through `func->getArg(0)` (the sret pointer) before
    `ret void`.
- Tests: `tests/conformance/structs/outbound_method.sg`,
  `tests/conformance/structs/inbound_method_field_in_scope.sg`, and
  `tests/conformance/structs/embedding_inherits_method.sg` all pass.
  Two struct-related issues remain open: the optional-field union
  extraction, and the alias-receiver method dispatch (a different
  shape of "method through alias", separate from struct methods).

### `arr[i] or { default }` segfaulted on out-of-bounds index — FIXED

- Symptom: `saga_array_at` correctly returned NULL on out-of-range,
  but codegen unconditionally `load`ed an `i64` through that pointer
  before the or-clause's tag check ran.  The same pattern also broke
  `map[k] or { default }`.
- Fix: `src/ir/codegen_control.cpp::emit_index_expr` — both array and
  map indexing now route through a new
  `wrap_indexed_lookup_in_error_union` helper that null-checks the
  runtime pointer, and on NULL wraps a Missing into the union (using
  the existing `emit_union_wrap`) instead of loading.  On non-NULL
  it loads the value and wraps it with the ok tag.  Returns a PHI'd
  pointer to a properly-tagged union, which `emit_or_expr` accepts
  directly.
- Tests:
  `tests/conformance/types/or_clause_default_value.sg` passes.
- Caveat: the *capture-form* (`or |err| { err.Method() }`) now
  reaches the err branch but segfaults inside the vtable dispatch
  because the Missing fat pointer isn't constructed at runtime — see
  the open "Capture-form `or |err|`..." entry above.

### Reading a top-level scalar constant segfaults at runtime — FIXED

- Symptom: `const Pi = 3; io.Println(Pi.String())` built cleanly but
  segfaulted at runtime because `emit_identifier` had no path for
  same-package globals — the value silently dropped.
- Fix: `src/ir/codegen_calls.cpp` — `emit_identifier` now looks up
  `module->getGlobalVariable(mangle(name))` and emits a load (or
  returns the GV pointer for struct-typed consts).
- Tests: `tests/conformance/constants/value_const.sg` now passes.

### `const X = 2 * 1024 * 1024` initialized to 0 — FIXED

- Symptom: const declarations whose value isn't a single literal
  (e.g. `2 * 1024 * 1024`) fell through to a null-value initializer.
- Fix: `src/ir/codegen_decls.cpp` — non-literal initializers now route
  through the deferred-init path used for arrays/maps; the global is
  emitted with `isConstant=false` (mutable storage, logically
  immutable from user code) and the value expression runs from
  `<pkg>__init__`.
- Tests: `tests/conformance/constants/compile_time_expr.sg` passes.

### `const Foo = import "..."` crashes the compiler — FIXED

- Symptom: SIGSEGV inside `emit_const_decl` because a Module-typed
  semantic type fell through `llvm_type` to `void` and was passed to
  `GlobalVariable`.
- Fix: `src/ir/codegen_decls.cpp` — early-return for ConstDecl whose
  value is `ImportExprNode`. Belt-and-braces: also bail out on null
  `ll_type`.
- Tests: `tests/conformance/constants/named_import.sg` passes.

### Top-level constants assignable at runtime — FIXED

- Symptom: `Pi = 4` (assignment to a top-level `const Pi = 3`) was
  silently accepted by the analyzer.
- Fix: `src/semantic/analyzer.cpp` — `check_assign`, `check_increment`,
  and `check_decrement` now reject targets whose symbol is
  `SymbolKind::Constant` with a clear "cannot assign to constant 'X'"
  diagnostic.
- Tests: `tests/conformance/mutability/const_immutable.sg` passes.
- Note: this only catches direct assignment forms (`x = ...`, `x += 1`,
  `x++`, `x--`).  Method-call mutation of const collections
  (`Primes.Push(7)`) needs a separate method-mutation classification —
  see "Mutation of global const collections" above.

### Type aliases not transparent for assignability or method dispatch — FIXED

- Symptom: with `const MyInt = Int`, `x MyInt = 5` rejected as
  "expected MyInt, got Int"; method calls on alias-typed values lost
  their arguments at codegen.
- Fixes:
  - `src/semantic/types.cpp::is_assignable_to` now unwraps aliases on
    either side before deciding.
  - `src/ir/codegen.cpp::llvm_type` now unwraps aliases up front (was
    falling through to `void_ll_type`).
  - `src/ir/codegen_decls.cpp::emit_const_decl` skips
    SymbolKind::Type entries (no runtime storage to emit).
  - `src/ir/codegen_method_dispatch.cpp::emit_method_or_module_call`
    unwraps the receiver type so kind-keyed dispatch works on aliases.
- Tests: `tests/conformance/constants/type_alias.sg` passes.
