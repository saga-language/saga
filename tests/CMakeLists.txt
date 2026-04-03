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

add_executable(compiler_tests ${TEST_SOURCES})

target_link_libraries(compiler_tests
    PRIVATE
    compiler_core
    GTest::gtest_main
)

target_include_directories(compiler_tests
    PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(compiler_tests)

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
