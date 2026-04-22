# NutCracker
Compilation Framework For Hybrid DPU

## Prerequisites 

Before building the project you should initialize and update the repository submodules (this will fetch the `third_party` sources used below):

```bash
git submodule update --init
```

### System Requirements

- **Rust toolchain** (version 1.74.0 or later)
  
  Install Rust using rustup:
  ```bash
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
  source $HOME/.cargo/env
  ```
  
  Verify installation:
  ```bash
  rustc --version  # Should show 1.74.0 or later
  cargo --version
  ```

- **CMake** (version 3.20 or later)
- **Ninja** build system

### Third-party Dependencies

To set up the required third-party dependencies, run the following commands:

1. **Initialize LLVM**
   ```bash
   bash build_scripts/build_llvm.sh
   export LLVM="$PWD/third_party/llvm/build/bin"
   ```

2. **Initialize P4MLIR**
    ```bash
    pushd third_party/p4mlir/build_tools/
    git apply ../../../patches/01_fix_p4mlir_build_script.patch
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

4. **Build the mapper**
    ```bash
    cd deps/mapper && cargo build --release && cd ../..
    ```

5. **Set environment variable:**
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

## Architecture

NutCracker compiles P4 programs into code that runs on NVIDIA BlueField DPUs.
The compiler generates **two parallel representations** for each basic block:

```
                        P4HIR (per block)
                       /                \
                      /                  \
               P4HIR → vDRMT        P4HIR → vDPP
                    |                    |
                 vdrmt.mlir          vdpp.mlir
                    |                    |
             (target-specific       (target-specific
              fine-grained)          fine-grained)
                    |                    |
              ┌─────┴─────┐        ┌────┴────┐
              │            │        │         │
         BF3: bf3drmt  BF2: bf2drmt  BF3: bf3dpa  ARM: arm.ll
              │            │        │         │
         DOCA 2.9    DOCA 2.2   DPA code  DPDK handler
```

- **vDRMT** — virtual DRMT: match-action representation targeting NIC hardware
- **vDPP** — virtual DPP: software/programmable representation targeting DPA or ARM cores
- Both representations exist for every block; the target backend decides which to use

### Supported Targets

| Target | Flag | DOCA Version | Hardware | NIC Engine | Software Fallback |
|--------|------|-------------|----------|------------|-------------------|
| BF3 | `--target bf3` (default) | DOCA 2.8+ | BlueField-3 | bf3drmt (HWS builder API) | DPA (FlexIO) |
| BF2 | `--target bf2` | DOCA 2.2 | BlueField-2 | bf2drmt (HWS value-struct API) | ARM cores (DPDK) |

### Block-to-Hardware Mapping (BF2)

On BF2, each block is assigned to either **DRMT** (NIC hardware) or **ARM** (software):

- Blocks without register operations → **DRMT** (DOCA Flow pipes)
- Blocks with pure-increment register patterns (read → add constant → write back, no other use) → **DRMT** via Pattern A rewrite (register → `bf2drmt.counter.count`)
- Blocks with complex register usage (threshold checks, conditional writes, resets) → **ARM** fallback (routed via `FWD_RSS` to ARM worker cores)

## Compiling a Sample App

```bash
bash nutcracker.sh compile <app_name> [--target bf2|bf3]
```

**Examples:**
```bash
# BF3 (default)
bash nutcracker.sh compile shared_counter

# BF2
bash nutcracker.sh compile register --target bf2
```

This produces `vdsa_output/` with:
- `block*/p4hir.mlir` — partitioned P4HIR per block
- `block*/vdrmt.mlir` — vDRMT representation per block
- `block*/vdpp.mlir` — vDPP representation per block
- `block*/bf3drmt.mlir` or `block*/bf2drmt.mlir` — target-specific lowered IR
- `block*/arm.ll` — ARM LLVM IR (for blocks with vDPP)
- `doca_flow_pipeline.c` — generated DOCA Flow C code
- `dependency_graph.dot` / `graph.png` — block dependency visualization
- `memory_analysis.json` — VFFA locality analysis

Available sample apps: `register`, `syn_flood`, `meter`, `load_balance`,
`simple_forwarding`, `shared_counter`, `example`, `ingressINT`, `ingressMazuNAT`.

## Deploying on BlueField-3

### Build the runtime

```bash
cd vdsa_output/deploy
meson setup _build
ninja -C _build
```

Produces `_build/nc_pipeline` (aarch64 ELF linked against DOCA Flow, DPDK, FlexIO).

**Prerequisites:** DOCA SDK 2.8+, DPDK, FlexIO, libibverbs, libmlx5.

### Run

```bash
# All-DRMT pipeline (DOCA Flow HWS)
sudo ./vdsa_output/deploy/_build/nc_pipeline \
    -l 0-15 -n 4 \
    -a 03:00.0,dv_flow_en=2 \
    -a 03:00.1,dv_flow_en=2

# DPA-only pipeline
sudo ./vdsa_output/deploy/_build/nc_pipeline -l 0 -n 4 -a 03:00.0
```

## Deploying on BlueField-2

### Build the runtime

The BF2 runtime uses NF-testkit's DOCA 2.2 init pattern (value-struct APIs,
`"vnf,hws"` mode, hairpin queues via `dpdk_queues_and_ports_init`).

```bash
cd runtime_bf2
bash run.sh                              # build + run (uses ../vdsa_output/doca_flow_pipeline.c)
bash run.sh /path/to/pipeline.c          # build + run with custom pipeline
bash run.sh --run-only                   # skip build, re-run existing binary
```

Or manually:

```bash
cd runtime_bf2
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
meson setup _build
ninja -C _build
```

Produces `_build/nc_pipeline` (aarch64 ELF linked against DOCA 2.2, DPDK).

**Prerequisites:** DOCA SDK 2.2, DPDK 22.11, libibverbs, libmlx5.

### Run

```bash
sudo runtime_bf2/_build/nc_pipeline \
    -l 0-3 -n 4 \
    -a 03:00.0,dv_flow_en=2 \
    -a 03:00.1,dv_flow_en=2
```

`dv_flow_en=2` enables hardware steering (required for DOCA Flow on both BF2 and BF3).

### DOCA 2.2 vs 2.9 API Differences

The BF2 codegen (`templates/doca-2.2.toml`) handles DOCA API differences
automatically:

| Feature | DOCA 2.9 (BF3) | DOCA 2.2 (BF2) |
|---------|----------------|-----------------|
| Pipe config | Builder: `doca_flow_pipe_cfg_create` + setters + `_destroy` | Value-struct: `struct doca_flow_pipe_cfg` + field assignments |
| Monitor | `monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED` | `monitor.flags \|= DOCA_FLOW_MONITOR_COUNT` |
| RSS field | `fwd.rss_flags` | `fwd.rss_outer_flags` |
| Hash entry add | 7-arg `doca_flow_pipe_hash_add_entry` | 9-arg (adds `pipe_queue`, `entry_index`) |
| Error string | `doca_error_get_descr(result)` | `doca_get_error_string(result)` |
| Action desc | `.field.orig` / `.field.modif` | `.copy.src` / `.copy.dst` |
| Flow init | `doca_flow_cfg_create` + setters | `struct doca_flow_cfg` value-struct |
| Port start | `doca_flow_port_cfg_create` + setters | `struct doca_flow_port_cfg` value-struct |

### Expected Output

```
[DOCA][INF] Doca flow initialized successfully
[DOCA][INF] doca flow port with id=0, type=0 started
[DOCA][INF] doca flow port with id=1, type=0 started
[NC_BF2_PIPELINE][INF] nutcracker BF2 pipeline running (Ctrl-C to stop)
```

### Hardware-Validated Apps (BF2)

| App | Pipes | Counter | Register | Status |
|-----|-------|---------|----------|--------|
| `simple_forwarding` | 1 DRMT | no | no | runs |
| `load_balance` | 2 DRMT (BASIC + HASH) | no | no | runs |
| `syn_flood` | 4 DRMT | yes | no | runs |
| `shared_counter` | 4 DRMT | yes | no | runs |
| `register` | 3 DRMT + 1 ARM | no | yes (ARM fallback) | runs |
| `ingressINT` | 6 DRMT + 3 ARM | no | yes (ARM fallback) | compiles (template gap for INT headers) |
| `meter` | DRMT | no | no | compiles (shared-meter init issue) |

## Project Structure

```
nutcracker/
  apps/                          # P4 sample applications
  build/                         # CMake build directory
  deps/mapper/                   # DialEgg equality-saturation mapper (Rust)
  egglog_rules/                  # Egglog rewrite rules for dialect conversion
  include/
    Dialect/
      vDRMT/IR/                  # vDRMT dialect (match-action, hardware-agnostic)
      vDPP/IR/                   # vDPP dialect (programmable, hardware-agnostic)
      Backend/BF2/DRMT/IR/       # bf2drmt dialect (BF2-specific DRMT)
      Backend/BF3/DRMT/IR/       # bf3drmt dialect (BF3-specific DRMT)
    Pass/                        # Pass headers
  lib/Pass/
    P4HIRPartition/              # P4HIR → basic blocks
    P4HIRToVDRMT/                # P4HIR → vDRMT (coarse-grained)
    P4HIRToVDPP/                 # P4HIR → vDPP (coarse-grained)
    VDRMTToBF3DRMT/              # vDRMT → bf3drmt (BF3 fine-grained)
    VDRMTToBF2DRMT/              # vDRMT → bf2drmt (BF2 fine-grained)
    BF3DRMTToDocaFlow/           # bf3drmt → DOCA Flow C (BF3)
    BF2DRMTToDocaFlow/           # bf2drmt → DOCA Flow C (BF2)
    VDPPToLLVM/                  # vDPP → LLVM IR (ARM handler)
    VDPPToBF3DPA/                # vDPP → bf3dpa + arm.ll
    EmitHandlerCode/             # Mapping-aware codegen (types, handlers, deploy)
  runtime/                       # BF3 runtime (DOCA 2.9 builder API)
  runtime_bf2/                   # BF2 runtime (DOCA 2.2 value-struct API)
  templates/
    doca-2.9.toml                # DOCA Flow 2.9 code templates (BF3)
    doca-2.2.toml                # DOCA Flow 2.2 code templates (BF2)
  nutcracker.sh                  # End-to-end compile + deploy script
```
