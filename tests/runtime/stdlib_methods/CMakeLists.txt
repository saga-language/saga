set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CMAKE_CURRENT_SOURCE_DIR}/../generic_aggregate/run_test.sh")

function(stdlib_methods_test name)
  add_test(
    NAME stdlib_methods_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

stdlib_methods_test(array_equals)
stdlib_methods_test(map_array_string)
stdlib_methods_test(map_equals)
stdlib_methods_test(string_methods)
