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

# Package programs: every runtime/<group>/<case>/app, with run_package_test.sh
# deciding positive or negative from the case's golden.txt or error.txt.
function(package_test app_dir)
  get_filename_component(case_dir "${app_dir}" DIRECTORY)
  get_filename_component(case "${case_dir}" NAME)
  get_filename_component(group_dir "${case_dir}" DIRECTORY)
  get_filename_component(group "${group_dir}" NAME)
  add_test(
    NAME ${group}_${case}
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/runtime/run_package_test.sh"
            "$<TARGET_FILE:saga>" "${case_dir}"
            "${CMAKE_CURRENT_BINARY_DIR}/runtime/${group}/${case}"
  )
endfunction()

file(GLOB PACKAGE_TEST_APPS LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*/*/app")
foreach(app_dir ${PACKAGE_TEST_APPS})
  package_test("${app_dir}")
endforeach()

# Spec conformance tests, organized by docs/language.md section.
add_subdirectory(conformance)
