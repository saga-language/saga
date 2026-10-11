#!/usr/bin/env bash
# run_package_test.sh <saga-bin> <case-dir> <test-dir>
#
# Builds <case-dir>/app with <case-dir> on the import path, and decides
# positive/negative by the expectation file in <case-dir>:
#   golden.txt -> positive (build+run must succeed, stdout == golden.txt)
#   error.txt  -> negative (build must fail, output contains error.txt)

set -u

source "$(dirname "${BASH_SOURCE[0]}")/../run_program.sh"

SAGA="$1"
CASE_DIR="$2"
TEST_DIR="$3"

out_bin="$TEST_DIR/out"

# saga's build cache keys on source hash, not compiler identity, so a stale
# artifacts dir would mask a compiler change.
rm -rf "$TEST_DIR/artifacts"
mkdir -p "$TEST_DIR"

build_case() {
  "$SAGA" build --build "$CASE_DIR/app" -I "$CASE_DIR" \
      --out-dir "$TEST_DIR/artifacts" -o "$out_bin" 2>&1
}

if [ -f "$CASE_DIR/golden.txt" ]; then
  expected="$(cat "$CASE_DIR/golden.txt")"

  build_out=$(build_case)
  if [ $? -ne 0 ]; then
    echo "BUILD FAILED (positive test):"
    echo "$build_out"
    exit 1
  fi

  actual=$(run_program "$out_bin" 2>&1)
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

elif [ -f "$CASE_DIR/error.txt" ]; then
  needle="$(cat "$CASE_DIR/error.txt")"

  build_out=$(build_case)
  if [ $? -eq 0 ]; then
    echo "EXPECTED BUILD FAILURE BUT IT SUCCEEDED. Output:"
    echo "$build_out"
    exit 1
  fi

  if printf '%s' "$build_out" | grep -qF -- "$needle"; then
    exit 0
  fi
  echo "=== EXPECTED SUBSTRING ==="
  echo "$needle"
  echo "=== ACTUAL OUTPUT ==="
  echo "$build_out"
  exit 1

else
  echo "MISCONFIGURED: $CASE_DIR has no golden.txt or error.txt"
  exit 2
fi
