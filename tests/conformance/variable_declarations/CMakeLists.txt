# Conformance: docs/language.md "Variable declarations" (and the cross-cutting
# explicit-typed-with-value form, e.g. `x Int = 5`).

file(GLOB SECTION_TESTS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.sg")
foreach(src ${SECTION_TESTS})
  conformance_test(variable_declarations "${src}")
endforeach()
