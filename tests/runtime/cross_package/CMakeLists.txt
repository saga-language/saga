set(CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR  "${CMAKE_CURRENT_BINARY_DIR}")
set(SAGA_BIN   "$<TARGET_FILE:saga>")
set(RUN_POS    "${CASES_DIR}/run_test.sh")
set(RUN_NEG    "${CASES_DIR}/run_error_test.sh")

function(cross_pkg_test name)
  add_test(
    NAME cross_pkg_${name}
    COMMAND bash "${RUN_POS}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

function(cross_pkg_error_test name)
  add_test(
    NAME cross_pkg_${name}
    COMMAND bash "${RUN_NEG}" "${SAGA_BIN}" "${CASES_DIR}/${name}" "${BUILD_DIR}/${name}"
  )
endfunction()

cross_pkg_test(const_scalar)
cross_pkg_test(const_array)
cross_pkg_test(const_map)
cross_pkg_test(fn_scalar_params)
cross_pkg_test(fn_struct_param)
cross_pkg_test(fn_struct_return)
cross_pkg_test(struct_literal)
cross_pkg_test(struct_method)
cross_pkg_test(struct_field_method)
cross_pkg_test(struct_in_collection)
cross_pkg_test(struct_embedding_cross_pkg)
cross_pkg_test(enum_basic)
cross_pkg_test(enum_with_fields)
cross_pkg_test(interface_decl_and_impl)
cross_pkg_test(generic_struct_nongeneric_method)
cross_pkg_test(generic_constrained)
cross_pkg_error_test(generic_method_negative)
cross_pkg_test(selector_chain)
cross_pkg_error_test(receiver_bind_negative)
cross_pkg_test(name_collision)
