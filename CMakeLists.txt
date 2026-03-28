cmake_minimum_required(VERSION 3.20)
project(compiler LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: #{LLVM_DIR}")

add_definitions(${LLVM_DEFINITIONS})

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(include)

# Gather source files
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    src/frontend/*.cpp
    src/ir/*.cpp
    src/semantic/*.cpp
)

add_library(compiler_core STATIC ${CORE_SOURCES})

target_link_libraries(compiler_core
    PUBLIC
    LLVM
)

target_include_directories(compiler_core
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Saga runtime library (C)
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.c)

add_library(saga_runtime STATIC ${RUNTIME_SOURCES})

target_include_directories(saga_runtime
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Install the runtime archive so the compiler can find it at link time.
set(SAGA_RUNTIME_DIR "${CMAKE_BINARY_DIR}/lib")
set_target_properties(saga_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SAGA_RUNTIME_DIR}"
)

# Compiler CLI
add_executable(compiler src/main.cpp)

target_link_libraries(compiler PRIVATE LLVM compiler_core)

# Pass the runtime library path to the compiler binary as a compile definition.
target_compile_definitions(compiler PRIVATE
    SAGA_RUNTIME_LIB="${SAGA_RUNTIME_DIR}/libsaga_runtime.a"
)

enable_testing()
add_subdirectory(tests)
