---
name: saga
description: Best-practices guide for writing idiomatic Saga code. Covers entry point, error handling, indexed access, methods, concurrency, types, stdlib reach, build/run/test commands, and common pitfalls. Load when editing or authoring `.sg` source.
---

# Saga — Best-Practices Guide

A compact reference for writing idiomatic Saga code. Authoritative spec
is in `docs/language.md`; this file is opinionated about *style*, not
*grammar*.

## Entry point

A Saga binary starts at:

```
pub fn Main() Void { ... }
```

`fn main()` and `func Main()` will both fail to compile.

## Indexed access: prefer `arr[i]`

**Default to `arr[i]`.** It returns `T | Error` and the compiler forces
you to resolve the error with `or`, which keeps out-of-bounds bugs out
of the language by construction. ~90% of indexed accesses in real Saga
code should use this form.

```
n := arr[idx] or { 0 }                  // resolve to a default
v := arr[idx] or |err| { return err }   // propagate
i := arr[99] or {}                      // shorthand: zero value of T

email := map["email"] or { "unknown" }  // same form for maps
```

`arr.At(i)` is an escape hatch. It returns `T` directly, with no error
type involved — the caller takes responsibility for the index being in
bounds (out-of-range access is UB at the language level). Reach for it
only when:

- Profiling shows the `or`-clause is a real hot-path bottleneck, or
- The proof-by-construction that the index is valid is local and
  obvious, and the `or` clause is pure noise.

Even then, prefer iterating (`for x : arr`) over indexing. **`At()` is
a last resort, not a habit.**

## Error handling

Saga has no exceptions and no panic-by-default. Functions that can fail
return `T | Error` and callers resolve with `or`:

```
n := parse_int(s) or { 0 }

n := parse_int(s) or |err| {
  intrinsic_print("bad input: " + err.Message())
  return
}

fn fetch() Data | Error {
  raw := read_file(path) or |err| { return err }
  parse(raw)
}
```

Define custom error types by implementing `Error`:

```
struct ParseError {
  message String
  line    Int

  pub fn Message() String { "parse error at line {line}: {message}" }
}
```

`Missing` is the built-in error returned by indexed access on
out-of-range / missing keys.

## Methods and receivers

Use a short, lowercase, Go-style receiver name — not `self`, `this`, or
`me`:

```
pub fn (a [T]) Size() Int { ... }
pub fn (m {K: V}) Keys() [K] { ... }
pub fn (p Point) Distance(q Point) Float { ... }
```

A short letter (first letter of the type) is conventional. Stdlib
follows this rule throughout.

## Visibility

`pub` exposes a function, method, or type to other packages. Default is
package-private. Most "method not found" errors when calling across
packages are a missing `pub`.

## Concurrency

Spawn an actor with `spawn`:

```
t := spawn |captures| {
  // Body runs in a separate actor with its own arena.
  // captures are deep-copied in.
  ctx.Exit(result)
}
```

Always either `Wait()` on the returned `Task` or hand it to a
supervisor. Failures of un-`Wait()`ed tasks are not logged today.

Inside long-running loops, call `intrinsic_yield()` periodically so the
reduction-counter does not kill the actor.

## Types

- `Int` and `Float` are the platform word-size aliases (currently both
  i64/f64; becomes target-dependent on 32-bit platforms when Phase 6
  lands).
- Sized variants: `Int8/16/32/64`, `Uint8/16/32/64`, `Float32/64`.
- Untyped integer literals coerce to any compatible width — `x Int32 =
  5` works without a cast.
- `Byte` and `Char` are aliases.
- `Bool`, `String`, `Void` are primitives.
- Arrays are `[T]`, maps are `{K: V}`, ranges are `(a..b)`.
- Unions: `Int | String`, `T | Error`. Errors must be resolved with
  `or` before the value flows.

## Stdlib reaches

- `std/array`, `std/map`, `std/string`, `std/int`, `std/float` — methods
  on intrinsic types. Imported automatically.
- `String` methods: `Trim`, `Capitalize`, `Title`, `HasPrefix`,
  `HasSuffix`, `Contains`, `Split`, `Upper`, `Lower`.
- `Array.Equals` / `Map.Equals` for deep comparison.
- Use `Find` rather than rolling your own search loop.

## Build & run

```
saga run path/to/main.sg          # JIT, fastest feedback
saga build path/to/main.sg        # produce a.out
saga check path/to/main.sg        # parse + type-check only
saga test                          # run tests under tests/
saga get github.com/user/pkg      # add a dependency to project.saga
```

`saga get`:
- Caches sources under `<cache>/src` and compiled artifacts (`.sgi`,
  `.o`) at `<cache>/`. Re-runs skip clone and re-compile when those
  exist.
- **Does not** transitively walk a fetched package's manifest. If the
  package you fetch has its own `[dependencies]`, you currently need to
  fetch them by hand.

## Style

- Files are `.sg`. Tests live under `tests/`.
- Comments explain *why*, not *what*. Reserve them for hidden
  invariants and surprising decisions.
- Keep functions short. If the name needs an "and" you have two
  functions glued together.
- Match expressions over chained `if`/`else` whenever the cases are
  exhaustive — the compiler enforces exhaustiveness on enums.

## When something is wrong

See `docs/debugging.md` for the full triage workflow. Quick checks:

1. Did `saga check` accept it? If not, fix the front-end error first.
2. Wrong runtime output? Sprinkle `intrinsic_print`. The runtime is
   single-threaded per actor; printf-debugging works.
3. Cross-package link failure? Check `--sgi-path` and confirm the
   `.sgi` is current.
4. A spawned actor "vanished"? Check whether you called `Wait()` —
   silent child failure is a feature today, not a bug.
