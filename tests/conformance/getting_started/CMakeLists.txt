file(GLOB SECTION_TESTS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.sg")
foreach(src ${SECTION_TESTS})
  conformance_test(getting_started "${src}")
endforeach()
