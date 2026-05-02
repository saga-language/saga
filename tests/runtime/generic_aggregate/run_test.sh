#!/usr/bin/env bash
# run_test.sh <saga-bin> <case-dir> <build-dir>
# Single-package positive test: build app, run binary, compare to golden.txt.

SAGA="$1"
CASE_DIR="$2"
BUILD_DIR="$3"

mkdir -p "$BUILD_DIR"

build_out=$("$SAGA" build --build "$CASE_DIR/app" \
    --out-dir "$BUILD_DIR/artifacts" -o "$BUILD_DIR/out" 2>&1)
if [ $? -ne 0 ]; then
  echo "BUILD FAILED:"
  echo "$build_out"
  exit 1
fi

actual=$("$BUILD_DIR/out" 2>&1)
expected=$(cat "$CASE_DIR/golden.txt")

if [ "$actual" = "$expected" ]; then
  exit 0
fi

echo "=== EXPECTED ==="
echo "$expected"
echo "=== ACTUAL ==="
echo "$actual"
exit 1
