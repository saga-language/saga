#!/usr/bin/env bash
# run_error_test.sh <saga-bin> <case-dir> <build-dir>
# Negative test: build must fail; stderr must contain golden.txt fragment.

SAGA="$1"
CASE_DIR="$2"
BUILD_DIR="$3"

# Hermetic build: saga's cache keys on source hash, not compiler identity,
# so a stale artifacts dir would mask a compiler change.
rm -rf "$BUILD_DIR/artifacts"
mkdir -p "$BUILD_DIR"

stderr_out=$("$SAGA" build --build "$CASE_DIR/app" -I "$CASE_DIR" \
    --out-dir "$BUILD_DIR/artifacts" -o "$BUILD_DIR/out" 2>&1)
exit_code=$?

if [ $exit_code -eq 0 ]; then
  echo "FAIL: expected compile error but build succeeded"
  exit 1
fi

expected=$(cat "$CASE_DIR/golden.txt")
if echo "$stderr_out" | grep -qF "$expected"; then
  exit 0
fi

echo "=== EXPECTED FRAGMENT ==="
echo "$expected"
echo "=== ACTUAL OUTPUT ==="
echo "$stderr_out"
exit 1
