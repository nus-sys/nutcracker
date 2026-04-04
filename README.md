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

   > The egglog rewrite rules live in `egglog_rules/`. The `NUTCRACKER_ROOT`
   > environment variable tells the compiler where to find them when invoked
   > from a different directory:
   > ```bash
   export NUTCRACKER_ROOT="$PWD"
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

## Building the Generated Pipeline

After NutCracker runs, it produces a `vdsa_output/deploy/` directory containing
the generated pipeline sources and a `meson.build` that references the runtime
sources by relative path.

**Prerequisites:** DOCA, DPDK, FlexIO, and ibverbs libraries must be installed
(available via the BlueField BSP / DOCA SDK).

```bash
cd vdsa_output/deploy
meson setup _build
ninja -C _build
```

This produces the `nc_pipeline` executable, which runs the compiled P4 pipeline
on the BlueField-3 DPU.

> **Note:** The `meson.build` references the nutcracker runtime sources at a
> relative path computed at compile time. If you move the `deploy/` directory,
> set `NUTCRACKER_ROOT` before running NutCracker so the path is resolved
> correctly:
> ```bash
> export NUTCRACKER_ROOT=/path/to/nutcracker
> ./build/bin/nutcracker-opt <input.mlir> [options]
> ```

## Running the Pipeline

After building the generated pipeline, run it on the BlueField-3 DPU with the
following command (run as root or with `sudo`):

```bash
sudo ./deploy/build_deploy/nc_runtime \
    -l 0-3 -n 4 \
    -a 0000:03:00.0,dv_flow_en=2 \
    -a 0000:03:00.1,dv_flow_en=2 \
    -- --device mlx5_0 --queues 1
```

### EAL arguments (before `--`)

| Argument | Description |
|---|---|
| `-l 0-3` | Use lcores 0–3 (one main + up to 3 ARM workers) |
| `-n 4` | 4 memory channels |
| `-a 0000:03:00.0,dv_flow_en=2` | Bind PF 0 with DOCA Flow HWS (hardware steering) enabled |
| `-a 0000:03:00.1,dv_flow_en=2` | Bind PF 1 with DOCA Flow HWS enabled |

> **`dv_flow_en=2` is required.** DOCA Flow hardware-steering mode (`hws`) will
> fail to start the port without it.  Both PFs must be listed even when only one
> port carries traffic, because the runtime opens both ports and calls
> `doca_flow_port_pair` to enable VNF forwarding between them.

### Application arguments (after `--`)

| Argument | Default | Description |
|---|---|---|
| `--device mlx5_0` | `mlx5_0` | IBV device name used by FlexIO/DPA init (skipped automatically when no DPA blocks are present) |
| `--queues N` | `1` | Number of RSS queues per port; must match the number of ARM worker lcores for ARM-mapped blocks |

### Expected output

A successful run prints (via DOCA logging) messages similar to:

```
[NC_RUNTIME][INF] DPDK port 0 started (1 RX/TX queues)
[NC_RUNTIME][INF] DPDK port 1 started (1 RX/TX queues)
[NC_PIPELINE][INF] Created pipe 0: …
[NC_PIPELINE][INF] Created pipe 1: …
[NC_PIPELINE][INF] Created pipe 2: …
[NC_PIPELINE][INF] Created pipe 3: …
[NC_PIPELINE][INF] Pipeline entries committed to HW: 4 processed, failure=0
[NC_RUNTIME][INF] nutcracker runtime running (Ctrl-C to stop)
```

Press **Ctrl-C** to shut down gracefully.

### Hardware prerequisites

- BlueField-3 DPU with both PFs (`0000:03:00.0`, `0000:03:00.1`) bound to the
  DPDK-compatible driver (typically `mlx5_core`/`mlx5_pci`).
- Hugepages configured (e.g. `4096 × 2 MB` pages).  Check with:
  ```bash
  cat /proc/meminfo | grep HugePages
  ```
- DOCA SDK and FlexIO libraries installed (available in the BlueField BSP).

### Shared-counter example

The `apps/shared_counter/` example is a minimal pipeline with four all-DRMT
blocks.  After building the deploy directory for that example:

```bash
sudo ./deploy/build_deploy/nc_runtime \
    -l 0-1 -n 4 \
    -a 0000:03:00.0,dv_flow_en=2 \
    -a 0000:03:00.1,dv_flow_en=2 \
    -- --queues 1
```

All four blocks are handled in hardware (DRMT); no ARM worker threads are
launched.  The runtime reaches steady state immediately after the pipeline
entries are committed.
