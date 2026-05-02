# Cross-Package Integration Tests

End-to-end tests for cross-package compilation. Each case is a directory
containing one or more packages and a `golden.txt` file.

## Structure

```
<case>/
  lib/         library package (imported by app)
  app/         binary package that imports lib
  golden.txt   expected stdout (positive tests) or error fragment (negative)
```

The `name_collision` case has `pka/`, `pkb/`, and `app/` instead of `lib/`.

## How tests run

Positive tests (`cross_pkg_*` via `run_test.sh`):
1. `saga build --build app/ -I <case-dir>` — compiles lib then app
2. Run the binary; compare stdout to `golden.txt`

Negative tests (cases `generic_method_negative`, `receiver_bind_negative`):
1. `saga build` is expected to exit non-zero
2. Compiler stderr must contain the `golden.txt` fragment

## Adding a case

1. Create `tests/runtime/cross_package/<name>/{lib,app}/` with `.sg` sources
2. Write `golden.txt` — exact expected stdout or error fragment
3. Add `cross_pkg_test(<name>)` (or `cross_pkg_error_test`) in `CMakeLists.txt`

## Phase schedule

All 19 cases start red. They turn green as phases land:
- P1 (type-system foundation): no cases green yet
- P3 (codegen import registry): cases 1–4, 12–13
- P4 (struct ABI): cases 5–11, 15, 17, 19
- P5 (analyzer enforcement): cases 16, 18
- P6 (interfaces + generics): cases 14, 15
