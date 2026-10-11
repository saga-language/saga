# Spec conformance tests: every <section>/<test>.sg, with run_test.sh deciding
# positive or negative from its sibling .golden or .error.
#
# This file is included from tests/CMakeLists.txt.

set(CONFORMANCE_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/run_test.sh")
set(CONFORMANCE_SAGA   "$<TARGET_FILE:saga>")

function(conformance_test src)
  get_filename_component(section "${src}" DIRECTORY)
  get_filename_component(name "${src}" NAME_WE)
  add_test(
    NAME conformance_${section}_${name}
    COMMAND bash "${CONFORMANCE_RUNNER}" "${CONFORMANCE_SAGA}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            "${CMAKE_CURRENT_BINARY_DIR}/${section}/${name}"
  )
endfunction()

# An expectation file with no source is never registered, so nothing else
# would notice that its test is gone.
function(reject_orphan_expectations)
  file(GLOB expectations RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
       "*/*.golden" "*/*.error")
  foreach(expectation ${expectations})
    string(REGEX REPLACE "\\.(golden|error)$" ".sg" src "${expectation}")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
      message(FATAL_ERROR "conformance: ${expectation} has no ${src}")
    endif()
  endforeach()
endfunction()

reject_orphan_expectations()

file(GLOB CONFORMANCE_TESTS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "*/*.sg")
foreach(src ${CONFORMANCE_TESTS})
  conformance_test("${src}")
endforeach()
