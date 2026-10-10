# Sourced by the runners of tests that build and run a Saga program.

# run_program <bin>: runs <bin>. Under SAGA_LEAK_CHECK=1 it runs with libasan
# preloaded, so a leak, a double free or an invalid free makes it exit non-zero.
run_program() {
  if [ "${SAGA_LEAK_CHECK:-}" != 1 ]; then
    "$1"
    return
  fi
  # saga links with the cc on PATH, so this libasan matches the program's libc.
  local asan
  asan=$(cc -print-file-name=libasan.so)
  if [ "${asan#/}" = "$asan" ]; then
    echo "SAGA_LEAK_CHECK=1, but cc has no libasan.so"
    return 1
  fi
  LD_PRELOAD="$asan" ASAN_OPTIONS=detect_leaks=1 "$1"
}
