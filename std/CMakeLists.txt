# Standard library packages (written in Saga)
set(SAGA_STD_DIR "${PROJECT_SOURCE_DIR}/std")
set(SAGA_STD_LIBS_DIR "${CMAKE_BINARY_DIR}/lib")
set(SAGA_STD_SGI_DIR_LOCAL "${CMAKE_BINARY_DIR}/lib/std")

# Each stdlib package compiles to an object + interface file.
# Type packages (int, float, bool, string) are leaf packages with no deps.
# Build order: type packages first, then unsafe, sys, os, io.
set(SAGA_STD_PACKAGES proto int float bool string array map unsafe sys os io)
set(proto_SGI_DEPS  "")
set(int_SGI_DEPS    "")
set(float_SGI_DEPS  "")
set(bool_SGI_DEPS   "")
set(string_SGI_DEPS "")
set(array_SGI_DEPS  "")
set(map_SGI_DEPS    "")
set(unsafe_SGI_DEPS "")
set(sys_SGI_DEPS "")
set(os_SGI_DEPS  "${SAGA_STD_SGI_DIR_LOCAL}/sys.sgi" "${SAGA_STD_SGI_DIR_LOCAL}/unsafe.sgi")
set(io_SGI_DEPS  "${SAGA_STD_SGI_DIR_LOCAL}/os.sgi")

foreach(pkg ${SAGA_STD_PACKAGES})
    file(GLOB ${pkg}_SOURCES "${SAGA_STD_DIR}/${pkg}/*.sg")
    add_custom_command(
        OUTPUT  "${SAGA_STD_SGI_DIR_LOCAL}/${pkg}.o"
                "${SAGA_STD_SGI_DIR_LOCAL}/${pkg}.sgi"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SAGA_STD_SGI_DIR_LOCAL}"
        COMMAND saga
                --lib --stdlib
                -o "${SAGA_STD_SGI_DIR_LOCAL}/${pkg}.o"
                --sgi-path "${SAGA_STD_SGI_DIR_LOCAL}"
                "${SAGA_STD_DIR}/${pkg}"
        DEPENDS saga ${${pkg}_SOURCES} ${${pkg}_SGI_DEPS}
        COMMENT "Compiling stdlib package: ${pkg}"
    )
    list(APPEND SAGA_STD_OBJECTS "${SAGA_STD_SGI_DIR_LOCAL}/${pkg}.o")
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

# Export paths for the parent CMakeLists to wire into the compiler binary.
set(SAGA_STD_SGI_DIR "${SAGA_STD_SGI_DIR_LOCAL}" PARENT_SCOPE)
set(SAGA_STD_LIB    "${SAGA_STD_LIBS_DIR}/libsaga_std.a" PARENT_SCOPE)
