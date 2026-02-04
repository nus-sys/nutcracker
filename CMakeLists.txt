cmake_minimum_required(VERSION 3.20.0)
project(edamlir LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Enable C++17 standard
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED YES)

# Enable export of compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Define build and binary directories
set(PROJECT_SOURCE_DIR ${CMAKE_SOURCE_DIR})
set(PROJECT_BINARY_DIR ${CMAKE_BINARY_DIR})
set(LLVM_RUNTIME_OUTPUT_INTDIR ${PROJECT_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${PROJECT_BINARY_DIR}/lib)

set(LLVM_DIR "${CMAKE_SOURCE_DIR}/third_party/llvm/build-release/lib/cmake/llvm")
set(MLIR_DIR "${CMAKE_SOURCE_DIR}/third_party/llvm/build-release/lib/cmake/mlir")

# Find MLIR and LLVM
find_package(MLIR REQUIRED CONFIG)
message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Set module paths for MLIR and LLVM
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

# Include LLVM and MLIR utilities
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

# Fine P4MLIR 
set(P4MLIR_DIR "${CMAKE_SOURCE_DIR}/third_party/p4mlir")

set(P4MLIR_INCLUDE_SRC "${P4MLIR_DIR}/include")
set(P4MLIR_INCLUDE_GEN "${P4MLIR_DIR}/third_party/p4c/build/extensions/p4mlir/include")
set(P4MLIR_LIB_DIR "${P4MLIR_DIR}/third_party/p4c/build/extensions/p4mlir/lib")

include_directories(SYSTEM ${P4MLIR_INCLUDE_SRC} ${P4MLIR_INCLUDE_GEN})

find_library(LIB_P4HIR        NAMES P4MLIR_P4HIR        PATHS ${P4MLIR_LIB_DIR} NO_DEFAULT_PATH)
find_library(LIB_TRANSFORMS   NAMES P4MLIRTransforms    PATHS ${P4MLIR_LIB_DIR} NO_DEFAULT_PATH)

# Include directories
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_BINARY_DIR}/include)

# Link directories
link_directories(${LLVM_BUILD_LIBRARY_DIR})

# Add LLVM definitions
add_definitions(${LLVM_DEFINITIONS})

# Output
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Subdirectories for your project components
add_subdirectory(include)
add_subdirectory(lib)
# add_subdirectory(tests)
add_subdirectory(tools)

# Optional: If third-party dependencies are needed
if(EXISTS "${CMAKE_SOURCE_DIR}/third-party")
    add_subdirectory(third-party)
endif()
