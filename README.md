# NutCracker
Compilation Framework For Hybrid DPU

## Prerequisite 

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
