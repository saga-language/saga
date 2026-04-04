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
    src/build/*.cpp
    src/frontend/*.cpp
    src/ir/*.cpp
    src/pkg/*.cpp
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

# Compiler CLI — main.cpp dispatcher + subcommand implementations
file(GLOB CMD_SOURCES CONFIGURE_DEPENDS src/cmd/*.cpp)
add_executable(compiler src/main.cpp ${CMD_SOURCES})

target_link_libraries(compiler PRIVATE LLVM compiler_core)

# Standard library must be processed before wiring its paths into the compiler,
# because std/CMakeLists.txt exports SAGA_STD_SGI_DIR and SAGA_STD_LIB.
add_subdirectory(std)

# Pass library paths to the compiler binary as compile-time definitions so it
# can auto-find the runtime, stdlib objects, and interface files without flags.
target_compile_definitions(compiler PRIVATE
    SAGA_RUNTIME_LIB="${SAGA_RUNTIME_DIR}/libsaga_runtime.a"
    SAGA_STD_LIB="${SAGA_STD_LIB}"
    SAGA_STD_SGI_DIR="${SAGA_STD_SGI_DIR}"
)

# Install targets ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

# Compiler binary
install(TARGETS compiler
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Runtime and std archives
install(FILES
    "${SAGA_RUNTIME_DIR}/libsaga_runtime.a"
    "${SAGA_STD_LIB}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/saga"
)

# Std interface files (.sgi)
install(DIRECTORY "${SAGA_STD_SGI_DIR}/"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/saga/std"
    FILES_MATCHING PATTERN "*.sgi"
)

enable_testing()
add_subdirectory(tests)
