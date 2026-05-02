set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CASES_DIR}/run_test.sh")

function(generic_aggregate_test name)
  add_test(
    NAME generic_aggregate_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

generic_aggregate_test(wide_field)
