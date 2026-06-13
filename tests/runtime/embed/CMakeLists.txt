set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CASES_DIR}/run_test.sh")

function(embed_test name)
  add_test(
    NAME embed_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

# Deferred to Phase 2: struct embedding is unimplemented in Phase 1 (the cpp
# embed tests are likewise DISABLED_). Cases kept for re-authoring once the
# embedding syntax (no longer `<`, which is now generics) lands.
# embed_test(field_access)
# embed_test(method_dispatch)
