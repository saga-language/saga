# Spec conformance tests, one section per subdir. Each section's CMakeLists
# enumerates its .sg files and registers them via conformance_test().
#
# This file is included from tests/CMakeLists.txt.

set(CONFORMANCE_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/run_test.sh")
set(CONFORMANCE_SAGA   "$<TARGET_FILE:saga>")

function(conformance_test section src)
  get_filename_component(name "${src}" NAME_WE)
  add_test(
    NAME conformance_${section}_${name}
    COMMAND bash "${CONFORMANCE_RUNNER}" "${CONFORMANCE_SAGA}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            "${CMAKE_CURRENT_BINARY_DIR}/${section}"
  )
endfunction()

# Register sections here as they come online.
add_subdirectory(array_map_access)
add_subdirectory(assignment)
add_subdirectory(binary_operators)
add_subdirectory(closures)
add_subdirectory(concurrency)
add_subdirectory(conditionals)
add_subdirectory(constants)
add_subdirectory(enums)
add_subdirectory(expression_statements)
add_subdirectory(getting_started)
add_subdirectory(identifiers)
add_subdirectory(import)
add_subdirectory(initialisation)
add_subdirectory(interfaces)
add_subdirectory(looping)
add_subdirectory(method_calling)
add_subdirectory(methods)
add_subdirectory(mutability)
add_subdirectory(range)
add_subdirectory(selectors)
add_subdirectory(string_interpolation)
add_subdirectory(strings)
add_subdirectory(structs)
add_subdirectory(type_aliases)
add_subdirectory(type_literals)
add_subdirectory(types)
add_subdirectory(unary_operators)
add_subdirectory(variable_declarations)
add_subdirectory(visibility)
