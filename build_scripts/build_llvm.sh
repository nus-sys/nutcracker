#!/usr/bin/env bash
set -ex

# --- 1. SET PATHS RELATIVE TO PROJECT ROOT ---
# Assumes this script is at: <ProjectRoot>/scripts/build_llvm.sh
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
PROJECT_ROOT=$( cd "$SCRIPT_DIR"/.. &> /dev/null && pwd )

# Point to your submodule
LLVM_REPO_DIR="$PROJECT_ROOT/third_party/llvm"
LLVM_BUILD_DIR="$PROJECT_ROOT/third_party/llvm/build"
LLVM_INSTALL_DIR="$PROJECT_ROOT/third_party/llvm/install"

echo "Building LLVM in: $LLVM_BUILD_DIR"
echo "Installing to:    $LLVM_INSTALL_DIR"

mkdir -p "$LLVM_BUILD_DIR"
cd "$LLVM_BUILD_DIR"

# --- 2. CONFIGURE ---
# We enable 'clang' because P4C often needs it.
CMAKE_FLAGS="-DCMAKE_INSTALL_PREFIX=$LLVM_INSTALL_DIR"
CMAKE_FLAGS+=" -DLLVM_ENABLE_PROJECTS=mlir;clang" 
CMAKE_FLAGS+=" -DLLVM_BUILD_EXAMPLES=OFF"
CMAKE_FLAGS+=" -DLLVM_TARGETS_TO_BUILD=Native"
CMAKE_FLAGS+=" -DCMAKE_BUILD_TYPE=Release"
CMAKE_FLAGS+=" -DLLVM_ENABLE_ASSERTIONS=ON"

# Use Clang to build Clang (Faster/Less RAM) if available
if command -v clang >/dev/null 2>&1; then
    CMAKE_FLAGS+=" -DCMAKE_C_COMPILER=clang"
    CMAKE_FLAGS+=" -DCMAKE_CXX_COMPILER=clang++"
fi

CMAKE_FLAGS+=" -DLLVM_ENABLE_LLD=ON"
CMAKE_FLAGS+=" -DLLVM_CCACHE_BUILD=OFF"
CMAKE_FLAGS+=" -DLLVM_INSTALL_UTILS=ON"
CMAKE_FLAGS+=" -DLLVM_INCLUDE_BENCHMARKS=OFF"

# --- CRITICAL FOR P4C COMPATIBILITY ---
CMAKE_FLAGS+=" -DLLVM_ENABLE_RTTI=ON"
CMAKE_FLAGS+=" -DLLVM_ENABLE_EH=ON"

cmake -G Ninja "$LLVM_REPO_DIR"/llvm $CMAKE_FLAGS

# --- 3. BUILD ---
ninja
ninja check-mlir
ninja install
