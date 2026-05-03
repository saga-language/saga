# Debugging Saga Programs

This guide covers the workflows for diagnosing failures in Saga code. The
language is small and the toolchain is one binary; most debugging boils
down to picking the right `saga` subcommand or compiler flag.

## Triage: where in the pipeline did it fail?

A Saga program goes through four stages. The error surface is different
in each, and the first step is always identifying which stage produced
the failure.

| Stage | Symptom | Fastest tool |
|---|---|---|
| Parse | "expected `…`" / "unexpected token" | `saga check` |
| Type check | "cannot assign", "method not found" | `saga check` |
| Codegen / link | "undefined reference", LLVM verifier error | `saga build --emit-ir` |
| Runtime | actor traps, wrong output, hang | `saga run` + `Task.Wait()` |

`saga check` runs only the front end (parse + analyze) and is the
fastest feedback loop. Use it as the first cut whenever the question is
"is the code well-formed?"

## Useful flags

These all go to `saga build` (the default subcommand) unless noted:

- `--emit-ir` — write `<name>.ll` next to the output. Read this when you
  suspect a codegen issue.
- `--dump-ir` — same content, printed to stderr instead.
- `--emit-obj` — stop after the object file; skip linking. Useful when a
  link error is masking the real diagnostic.
- `-v` — verbose driver output. Shows the underlying compiler invocations
  and link command.
- `--sgi-path <dir>` — add a `.sgi` search path. If a cross-package
  symbol is missing, the most common cause is a missing or stale `.sgi`;
  point this at the directory holding it.

`saga run` accepts the same flags and uses the JIT, which avoids the
link step entirely.

## Front-end errors

Diagnostics carry file, line, and column. The error text is the most
useful single signal — Saga's analyzer produces specific messages
(`expected Int32, got Int`) rather than fold-everything-into-one
catch-alls.

When a type error names two types that look identical (e.g. two structs
both named `Point`), check origin packages — Saga keys types by
`(origin, name)`, and a value whose origin is `pkg_a` is not assignable
to a parameter typed as `pkg_b.Point` even when the layouts match.

## Codegen errors

Symptoms: LLVM verifier fires, `undefined reference` at link time, the
program crashes immediately on launch, segfault inside a runtime helper.

Workflow:

1. `saga build --emit-ir <input>` and open `<name>.ll`.
2. Search for the function whose name matches the failing call. Saga
   mangles symbols as `<package>__<name>` (free functions) or
   `<package>__<Type>__<method>` (methods). Generic instantiations append
   the type-arg key.
3. Cross-package issues: check the importing module's `declare`
   statements at the top of the `.ll` against the exporter's `define`s.
   A `declare i64 @pkg_a__Foo__bar(...)` with no matching `define`
   anywhere is the smoking gun.
4. If the LLVM verifier fires (`Function return type does not match…`),
   the type signature in the `declare` and the `define` disagree. The
   usual cause is a missed origin-substitution in the analyzer; check
   the relevant `node_types` entry.

## Runtime failures

A spawned actor that traps or is killed leaves a `Task` in an error
state. The reason is propagated through `Task.Wait()`'s error branch:

```
t := spawn child()
result := t.Wait() or |err| {
  intrinsic_print("child failed: " + err.Message())
  return
}
```

Failure causes:

- **Explicit `intrinsic_trap("...")`** — `err.Message()` returns the
  reason string passed to `intrinsic_trap`.
- **Reduction quota exhausted** — the actor ran a long loop without
  yielding. `err.Message()` currently returns `"killed"`; reduction
  exhaustion does not yet stamp a reason. Look at the loop and either
  call `intrinsic_yield()` periodically or restructure to use the
  channel/await machinery.
- **Heartbeat timeout** — the actor was stuck in a syscall or FFI for
  longer than `SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS`. Same symptom and
  same workaround as reduction exhaustion.

If the parent never calls `Task.Wait()`, child failures are silent. The
runtime does not currently log unhandled failures to stderr. Always
`Wait()` in development, or wrap spawn sites in a supervisor that does.

## When to reach for gdb / lldb

Useful for:

- Native segfaults inside the runtime (`runtime.c`).
- Verifying a generated `.ll` produces the layout you expect (set a
  breakpoint at a Saga function name and inspect the SSA values).

Build with debug info and no optimizer first:

```
saga build --emit-obj <input>          # compile only
clang -g -O0 -o prog <name>.o <runtime>  # link with -g
gdb ./prog
```

Avoid setting conditional breakpoints on hot LLVM symbols inside the
compiler itself when running under WSL2 — the kernel-level event load
can wedge the host. Read the LLVM IR or instrument with `--dump-ir`
first; reach for gdb only after you have a specific address or symbol.

## Reproducing test failures

The test harness is `saga test` (root-level `tests/`). Individual
runtime tests live under `tests/runtime/<name>/` as a `main.sg` plus a
golden `expected.txt`. To re-run one test in isolation:

```
saga run tests/runtime/<name>/main.sg
```

Compare the output to `expected.txt`. The test harness is a thin diff
wrapper; running directly under `saga run` is identical and gives
faster feedback.

## Common pitfalls

- **`arr[i]` vs `arr.At(i)`** — `arr[i]` returns `T | Error` and forces
  an `or` clause; `arr.At(i)` returns `T` directly and assumes the index
  is in range. Mixing them up produces type errors that look strange
  until you remember the distinction.
- **Stale `.sgi` files** — if a cross-package symbol vanishes after a
  source edit, delete the `.sgi` and rebuild. The driver does not yet
  invalidate `.sgi` based on source mtime in every code path.
- **Missing `pub`** — non-`pub` functions and types are package-private.
  A "method not found" error from a different package is almost always
  a missing `pub`.
- **Generic struct method bodies** — bodies of methods on generic
  structs are type-checked lazily on first cross-package use. A subtle
  type bug inside a generic method body may not surface until a
  consumer in another package instantiates it.
