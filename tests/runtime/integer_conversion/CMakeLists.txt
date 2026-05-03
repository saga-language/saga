set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CMAKE_CURRENT_SOURCE_DIR}/../generic_aggregate/run_test.sh")

function(integer_conversion_test name)
  add_test(
    NAME integer_conversion_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

integer_conversion_test(narrow_widen)
