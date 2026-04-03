# Standard library packages (written in Saga)
set(SAGA_STD_DIR "${PROJECT_SOURCE_DIR}/std")
set(SAGA_STD_LIBS_DIR "${CMAKE_BINARY_DIR}/lib")

# Each stdlib package compiles to an object file
set(SAGA_STD_PACKAGES sys os io)

foreach(pkg ${SAGA_STD_PACKAGES})
    file(GLOB ${pkg}_SOURCES "${SAGA_STD_DIR}/${pkg}/*.sg")
    add_custom_command(
        OUTPUT "${SAGA_STD_LIBS_DIR}/${pkg}.o" "${SAGA_STD_LIBS_DIR}/${pkg}.sgi"
        COMMAND compiler --lib -o "${SAGA_STD_LIBS_DIR}/${pkg}.o"
                "${SAGA_STD_DIR}/${pkg}"
        DEPENDS compiler ${${pkg}_SOURCES}
        COMMENT "Compiling stdlib package: ${pkg}"
    )
    list(APPEND SAGA_STD_OBJECTS "${SAGA_STD_LIBS_DIR}/${pkg}.o")
endforeach()

# Bundle all stdlib objects into libsaga_std.a
add_custom_command(
    OUTPUT "${SAGA_STD_LIBS_DIR}/libsaga_std.a"
    COMMAND ${CMAKE_AR} rcs "${SAGA_STD_LIBS_DIR}/libsaga_std.a"
            ${SAGA_STD_OBJECTS}
    DEPENDS ${SAGA_STD_OBJECTS}
    COMMENT "Creating libsaga_std.a"
)
add_custom_target(saga_std ALL
    DEPENDS "${SAGA_STD_LIBS_DIR}/libsaga_std.a")
