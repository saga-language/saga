# Standard library packages (written in Saga)
set(SAGA_STD_DIR "${PROJECT_SOURCE_DIR}/std")
set(SAGA_STD_LIBS_DIR "${CMAKE_BINARY_DIR}/lib")
set(SAGA_STD_SGI_DIR_LOCAL "${CMAKE_BINARY_DIR}/lib/std")

# Each stdlib package compiles to an object + interface file.
# Type packages (int, float, bool, string) are leaf packages with no deps.
# Non-type packages auto-load int/float/bool/string/proto via the
# analyzer's prelude (load_prelude, src/semantic/analyzer.cpp:198).
# Those .sgi files must therefore exist before any non-type package
# compiles, so we declare them as explicit deps.  Without these
# declarations, ninja parallelism schedules the dependent package
# before its prelude is built — failing under any clean rebuild.
set(SAGA_STD_PACKAGES proto int float bool string array map unsafe sys os io)
set(SAGA_STD_PRELUDE_SGI
    "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/int.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/float.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/bool.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/string.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/array.sgi"
    "${SAGA_STD_SGI_DIR_LOCAL}/map.sgi")
set(proto_SGI_DEPS  "")
# Type packages skip the scalar-prelude load but still load proto.sgi
# for generic-method dispatch through Hashable/Stringable.
set(int_SGI_DEPS    "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(float_SGI_DEPS  "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(bool_SGI_DEPS   "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(string_SGI_DEPS "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(array_SGI_DEPS  "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(map_SGI_DEPS    "${SAGA_STD_SGI_DIR_LOCAL}/proto.sgi")
set(unsafe_SGI_DEPS ${SAGA_STD_PRELUDE_SGI})
set(sys_SGI_DEPS    ${SAGA_STD_PRELUDE_SGI})
set(os_SGI_DEPS  ${SAGA_STD_PRELUDE_SGI}
    "${SAGA_STD_SGI_DIR_LOCAL}/sys.sgi" "${SAGA_STD_SGI_DIR_LOCAL}/unsafe.sgi")
set(io_SGI_DEPS  ${SAGA_STD_PRELUDE_SGI}
    "${SAGA_STD_SGI_DIR_LOCAL}/os.sgi")

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
