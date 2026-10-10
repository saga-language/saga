# Cross-Package Integration Tests

End-to-end tests for cross-package compilation. Each case is a directory
containing one or more packages and an expectation file.

## Structure

```
<case>/
  lib/         library package (imported by app)
  app/         binary package that imports lib
  golden.txt   positive: expected stdout
  error.txt    negative: fragment the build's output must contain
```

The `name_collision` case has `pka/`, `pkb/`, and `app/` instead of `lib/`.

## How tests run

Every package test, here and in the sibling directories, runs through
`tests/runtime/run_package_test.sh`, which builds `app/` with
`saga build --build app/ -I <case-dir>`. A positive case must build and run,
and its stdout must equal `golden.txt`. A negative case must fail to build,
with `error.txt` a substring of the output.

## Adding a case

1. Create `tests/runtime/cross_package/<name>/{lib,app}/` with `.sg` sources
2. Write `golden.txt` (expected stdout) or `error.txt` (error fragment)

`tests/CMakeLists.txt` registers every `<group>/<case>/app` as the test
`<group>_<case>`, so the next build picks a new case up.
