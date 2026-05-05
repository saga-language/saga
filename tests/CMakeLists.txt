# Fetch GoogleTest automatically
include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

# Prevent GoogleTest from overriding compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Gather test sources (exclude runtime/ — those have their own target)
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/build/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ir/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pkg/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/semantic/*.cpp
)

add_executable(saga_tests ${TEST_SOURCES})

target_link_libraries(saga_tests
    PRIVATE
    saga_core
    GTest::gtest_main
)

target_include_directories(saga_tests
    PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(saga_tests
    PRIVATE
    SAGA_STD_SGI_DIR="${SAGA_STD_SGI_DIR}"
)

include(GoogleTest)
gtest_discover_tests(saga_tests)

# Runtime tests (C runtime linked into C++ GTest harness)
file(GLOB_RECURSE RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp
)

if(RUNTIME_TEST_SOURCES)
  add_executable(runtime_tests ${RUNTIME_TEST_SOURCES})

  target_link_libraries(runtime_tests
      PRIVATE
      saga_runtime
      pthread
      GTest::gtest_main
  )

  target_include_directories(runtime_tests
      PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )

  gtest_discover_tests(runtime_tests)
endif()

# Cross-package integration tests (shell-script driven, all red until P1+).
add_subdirectory(runtime/cross_package)

# Single-package embed integration tests.
add_subdirectory(runtime/embed)

# Generic struct ABI / per-instantiation layout regression tests.
add_subdirectory(runtime/generic_aggregate)

# Integer conversion regression tests (exercises Phase 6 sext/zext path).
add_subdirectory(runtime/integer_conversion)

# Stdlib method coverage (Array.Equals, Map.Equals, string predicates/Split).
add_subdirectory(runtime/stdlib_methods)

# Phase 4 spike: user-defined struct keys routed through the runtime ops table.
add_subdirectory(runtime/protocol_user_key)
