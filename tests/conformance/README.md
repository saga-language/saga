# Spec Conformance Tests

Each top-level section of `docs/language.md` gets one directory here. Inside,
every documented syntactic form or behavior is one test — a `.sg` source plus
an expectation file. The aim is to validate the spec is the contract; if a
test fails, either the implementation has a bug or the spec is wrong.

## Layout

```
tests/conformance/
  README.md
  run_test.sh         # shared runner
  CMakeLists.txt      # auto-discovers per-section subdirs
  <section_name>/
    CMakeLists.txt
    <test_name>.sg
    <test_name>.golden     # positive: expected stdout (exact match)
    <other_test>.sg
    <other_test>.error     # negative: substring expected in stderr,
                           #           compile/run must exit non-zero
```

## Test types

- **Positive** (`.sg` + `.golden`): build must succeed, run must succeed,
  stdout must equal `.golden` exactly (trailing newline included).
- **Negative** (`.sg` + `.error`): build OR run must exit non-zero, and
  combined stderr+stdout must contain the substring in `.error`.

If a `.sg` file has neither sibling, the runner fails it as misconfigured.

## Naming

- Section dirs use `snake_case` and match the spec section title where
  reasonable (`variable_declarations`, `type_literals`, `string_interpolation`).
- Test names describe the *form being tested*, not the assertion
  (`explicit_type_zero.sg`, not `prints_0.sg`).

## When a test fails

Don't suppress it. The default disposition is **the implementation is
wrong**: fix it in the same change. A conformance test lands green, so if
the fix is out of scope, keep the test out and record the bug, with the
spec line it derives from, wherever the work is tracked.

If you're convinced the *spec* is the one that's wrong (the design moved
and the doc didn't catch up), do **not** edit `docs/language.md` based on
the failing test alone. Raise it for an explicit human decision. Spec
changes are never automatic — the chicken-and-egg risk is that a buggy
implementation silently rewrites the spec to match itself.

## Scope

These tests cover *language-level* claims from `docs/language.md` only.
Stdlib method behavior belongs in `tests/runtime/stdlib_methods/`. Compiler
internals belong in `tests/{frontend,ir,semantic,...}/`.
