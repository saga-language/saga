cmake_minimum_required(VERSION 3.20)
project(compiler LANGUAGES C CXX)

if(APPLE)
  set(CMAKE_OSX_SYSROOT "macosx")
  add_compile_options("-isysroot${CMAKE_OSX_SYSROOT}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_definitions(${LLVM_DEFINITIONS})

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(include)

# Gather source files
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DPENDS
    src/frontend/*.cpp
    src/ir/*.cpp
    src/runtime/*.cpp
    src/semantic/*.cpp
)

add_library(compiler_core STATIC ${CORE_SOURCES})

# Map components to actual library names (Essential for Windows/LNK1181)
llvm_map_components_to_libnames(llvm_libs support core irreader)
target_link_libraries(compiler_core PUBLIC ${llvm_libs})

target_include_directories(compiler_core
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Compiler CLI
add_executable(compiler src/main.cpp)

target_link_libraries(compiler PRIVATE ${llvm_libs} compiler_core)

enable_testing()
add_subdirectory(tests)
