# NutCracker
Compilation Framework For Hybrid DPU

## Prerequisites 

Before building the project you should initialize and update the repository submodules (this will fetch the `third_party` sources used below):

```bash
git submodule update --init --recursive
```

### Third-party Dependencies

To set up the required third-party dependencies, run the following commands:

1. **Initialize LLVM**
   ```bash
   bash build/build_llvm.sh
   export LLVM="$PWD/third_party/llvm/build/bin"
   ```

2. **Initialize P4MLIR**
    ```bash
    git apply ../../patches/01_fix_p4mlir_build_script.patch
    pushd third_party/p4mlir/build_tools/
    bash build_p4c_with_p4mlir_ext.sh
    popd
    ```

3. **Initialize egglog**
    ```bash
   pushd third_party/egglog
   cargo build --release
   popd
   export EGGLOG="$PWD/third_party/egglog/target/release"
   ```

4. **Set environment variable:**
   ```bash
   export PATH=$LLVM:$EGGLOG:$PATH
   ```

## Building NutCracker

After setting up the third-party dependencies, build NutCracker:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

## Pre-compilation Steps

Before using NutCracker, you need to compile your source code to the appropriate intermediate representation:

### Compiling P4 Programs to P4MLIR

For P4 programs, use the P4MLIR compiler to generate MLIR representation:

```bash
./third_party/p4mlir/third_party/p4c/build/p4mlir-translate \
  -I./p4include \
  --typeinference-only \
  <input.p4> > <output.mlir>
```

**Example:**
```bash
./third_party/p4mlir/third_party/p4c/build/p4mlir-translate \
  -I./p4include \
  --typeinference-only \
  apps/simple_fowarding/simple_forwarding.p4 > apps/simple_fowarding/simple_forwarding.mlir
```

### Compiling C/C++ Programs to LLVM IR

For C/C++ programs, use Clang to generate LLVM IR:

```bash
clang -S -emit-llvm -O2 <input.c> -o <output.ll>
```

**Example:**
```bash
clang -S -emit-llvm -O2 example.c -o example.ll
```

Alternatively, to generate MLIR from C/C++:

```bash
mlir-translate --import-llvm <input.ll> -o <output.mlir>
```

## Usage

After pre-compilation, process your MLIR files with NutCracker:

```bash
./build/bin/nutcracker-opt <input.mlir> [options]
```
