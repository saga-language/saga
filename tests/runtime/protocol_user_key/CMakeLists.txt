set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CMAKE_CURRENT_SOURCE_DIR}/../generic_aggregate/run_test.sh")

function(protocol_user_key_test name)
  add_test(
    NAME protocol_user_key_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

protocol_user_key_test(struct_key)
protocol_user_key_test(dispatch)
