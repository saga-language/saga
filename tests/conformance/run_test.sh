#!/usr/bin/env bash
# run_test.sh <saga-bin> <test.sg> <build-dir>
#
# Decides positive/negative by sibling expectation file:
#   <test>.golden -> positive (build+run must succeed, stdout == golden)
#   <test>.error  -> negative (build OR run must fail, combined output
#                              contains the substring in .error)

set -u

SAGA="$1"
SRC="$2"
BUILD_DIR="$3"

base="${SRC%.sg}"
test_name="$(basename "$base")"

# Per-test working directory: `saga build` writes its intermediate
# `<module>.o` into the current directory, so two tests sharing the
# same category cwd would race on the same .o filename when CTest
# parallelises.  Give each test its own scratch directory and `cd`
# into it so artifacts can never collide.
work_dir="$BUILD_DIR/$test_name.d"
mkdir -p "$work_dir"
cd "$work_dir"

out_bin="$BUILD_DIR/$test_name"

if [ -f "$base.golden" ]; then
  expected="$(cat "$base.golden")"

  build_out=$("$SAGA" build "$SRC" -o "$out_bin" 2>&1)
  build_rc=$?
  if [ $build_rc -ne 0 ]; then
    echo "BUILD FAILED (positive test):"
    echo "$build_out"
    exit 1
  fi

  actual=$("$out_bin" 2>&1)
  run_rc=$?
  if [ $run_rc -ne 0 ]; then
    echo "RUN FAILED (positive test, exit $run_rc):"
    echo "$actual"
    exit 1
  fi

  if [ "$actual" = "$expected" ]; then
    exit 0
  fi
  echo "=== EXPECTED ==="
  echo "$expected"
  echo "=== ACTUAL ==="
  echo "$actual"
  exit 1

elif [ -f "$base.error" ]; then
  needle="$(cat "$base.error")"

  combined=$("$SAGA" build "$SRC" -o "$out_bin" 2>&1)
  build_rc=$?

  if [ $build_rc -eq 0 ]; then
    # Build succeeded — try running. Some negative tests are runtime errors.
    combined="$combined"$'\n'"$("$out_bin" 2>&1)"
    run_rc=$?
    if [ $run_rc -eq 0 ]; then
      echo "EXPECTED FAILURE BUT EVERYTHING SUCCEEDED. Output:"
      echo "$combined"
      exit 1
    fi
  fi

  if printf '%s' "$combined" | grep -qF -- "$needle"; then
    exit 0
  fi
  echo "=== EXPECTED SUBSTRING ==="
  echo "$needle"
  echo "=== ACTUAL OUTPUT ==="
  echo "$combined"
  exit 1

else
  echo "MISCONFIGURED: $SRC has no sibling .golden or .error file"
  exit 2
fi
