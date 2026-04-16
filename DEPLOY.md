# Deploying a NutCracker Pipeline on BlueField-3

This guide covers everything after `bash nutcracker.sh compile <app>` has
produced `vdsa_output/`.  It explains how to compile the generated C code,
transfer it to a BlueField-3 DPU, and run the pipeline.

---

## What `nutcracker.sh compile` generates

```
vdsa_output/
├── doca_flow_pipeline.c   # DOCA Flow pipe definitions (DRMT blocks)
├── dpa_handler.c          # DPA block functions (DPA blocks, if any)
├── dpa_dev_entry.c        # DPA device-side event handler
├── nc_types.h             # Shared header/metadata struct layouts
├── nc_pipeline.h          # Pipeline function declarations
├── mapper.egg             # Egglog mapper config (internal)
├── deploy/
│   ├── meson.build        # Build description for the host runtime
│   ├── doca_flow_pipeline.c  (symlink / copy)
│   ├── dpa_handler.c         (symlink / copy)
│   ├── dpa_dev_entry.c       (symlink / copy)
│   └── nc_types.h            (symlink / copy)
└── block*/                # Per-block MLIR artifacts
```

Pipelines that use **only DRMT blocks** need only the DOCA Flow path.
Pipelines with **DPA blocks** additionally require DPACC compilation.

---

## Prerequisites

### On the build host (x86 or BlueField Arm)

| Tool | Version | Purpose |
|------|---------|---------|
| DOCA SDK | ≥ 2.8 | DOCA Flow headers & libraries |
| DPDK | bundled with DOCA | DPDK libraries |
| FlexIO SDK | bundled with DOCA | DPA host-side libraries |
| `dpacc` | bundled with DOCA | DPA device-side compiler |
| `meson` | ≥ 0.61.2 | Build system |
| `ninja` | any recent | Build backend |

The DOCA SDK is installed at `/opt/mellanox/doca/` on both the host (x86)
and on BlueField itself.  The recommended approach is to **compile directly
on the BlueField** ARM cores (OOB shell), which avoids cross-compilation
complexity and ensures the correct library ABI.

---

## Step 1 — Transfer the deploy directory to BlueField

From the x86 host, copy the generated files to the BlueField OOB interface
(default management IP is `192.168.100.2`):

```bash
scp -r vdsa_output/deploy/ vdsa_output/runtime/ \
    user@192.168.100.2:~/nc_deploy/
```

> If your BlueField management IP differs, substitute the correct address.

---

## Step 2 — Compile the DRMT (DOCA Flow) host binary

SSH into the BlueField:

```bash
ssh user@192.168.100.2
cd ~/nc_deploy/deploy
```

Configure and build with Meson:

```bash
meson setup _build
ninja -C _build
```

This produces `_build/nc_pipeline` — an aarch64 ELF that links DOCA Flow,
DPDK, and (optionally) FlexIO.

> **Troubleshooting:** if `pkg-config` cannot find `doca-flow`, add the
> DOCA pkgconfig path:
> ```bash
> export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
> ```

---

## Step 3 — Compile the DPA device binary (DPA blocks only)

Skip this step if `dpa_handler.c` is empty (no DPA blocks in your pipeline).

Compile the DPA device-side code with DPACC:

```bash
cd ~/nc_deploy/deploy
dpacc dpa_handler.c dpa_dev_entry.c \
    -hostcc aarch64-linux-gnu-gcc \
    -I. \
    -I/opt/mellanox/doca/include \
    -o nc_dpa_app
```

`dpacc` produces `nc_dpa_app` (a FlexIO application object) and a
stub `dpa_dev_entry.c.host.o` that must be linked into `nc_pipeline`.
Add it to the meson build by passing it as a link argument, or simply
rebuild with the object included:

```bash
aarch64-linux-gnu-gcc -o _build/nc_pipeline \
    doca_flow_pipeline.c \
    ../../runtime/nc_runtime_main.c \
    ../../runtime/nc_doca_flow.c \
    ../../runtime/nc_flexio_host.c \
    ../../runtime/nc_arm_worker.c \
    dpa_dev_entry.c.host.o \
    $(pkg-config --cflags --libs doca-common doca-flow doca-argp libdpdk libflexio libibverbs libmlx5) \
    -I. -I../../runtime/include \
    -DDOCA_ALLOW_EXPERIMENTAL_API -Wno-missing-braces
```

---

## Step 4 — BlueField setup

### 4a. Confirm PCI addresses

Find the BlueField Ethernet function PCI addresses:

```bash
lspci | grep -i mellanox
```

Typical addresses are `03:00.0` (port 0) and `03:00.1` (port 1).

### 4b. Allocate hugepages

DPDK requires 2 MB hugepages:

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

Verify:

```bash
cat /proc/meminfo | grep Huge
# HugePages_Total should be 1024
```

### 4c. Load the kernel modules (if needed)

```bash
sudo modprobe mlx5_core
sudo modprobe mlx5_ib
```

---

## Step 5 — Run the pipeline

Choose the invocation that matches your pipeline's hardware mapping.

### All-DRMT pipeline (DOCA Flow hardware steering)

Used for pipelines where all blocks run on the NIC match-action engine
(e.g. `shared_counter`, `simple_forwarding`, `meter`, `ingressINT`).

```bash
sudo ./_build/nc_pipeline \
    -l 0-3 -n 4 \
    -a 03:00.0,dv_flow_en=2 \
    -a 03:00.1,dv_flow_en=2 \
    -- --device mlx5_0
```

| Flag | Meaning |
|------|---------|
| `-l 0-3` | Use lcores 0–3 |
| `-n 4` | 4 memory channels |
| `-a 03:00.0,dv_flow_en=2` | Bind port 0; `dv_flow_en=2` enables HWS |
| `-a 03:00.1,dv_flow_en=2` | Bind port 1 |
| `-- --device mlx5_0` | DOCA device name |

### DPA-only pipeline

Used for pipelines where blocks run on the DPA accelerator.

```bash
sudo ./_build/nc_pipeline \
    -l 0 -n 4 \
    -a 03:00.0 \
    -- --device mlx5_0
```

### Mixed DRMT + DPA pipeline

No additional flags are needed; the runtime automatically routes packets
to the correct hardware via the generated RSS queues and FlexIO threads:

```bash
sudo ./_build/nc_pipeline \
    -l 0-3 -n 4 \
    -a 03:00.0,dv_flow_en=2 \
    -a 03:00.1,dv_flow_en=2 \
    -- --device mlx5_0
```

---

## Step 6 — Verify the pipeline is running

A successful startup prints:

```
[NC_PIPELINE][INF] Created pipe 0 (type=basic) …
[NC_PIPELINE][INF] Created pipe 1 (type=basic) …
…
[NC_RUNTIME][INF] nutcracker runtime running (Ctrl-C to stop)
```

Send test traffic through the physical ports (e.g. with `pktgen` or
`trex` on the connected host) and confirm packets are processed.

Press **Ctrl-C** to shut down gracefully.

---

## Quick-reference: compile + run on BlueField

```bash
# On x86 host
bash nutcracker.sh compile <app>
scp -r vdsa_output/deploy/ vdsa_output/runtime/ user@192.168.100.2:~/nc_deploy/

# On BlueField (SSH)
cd ~/nc_deploy/deploy
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
meson setup _build && ninja -C _build

# DPA blocks only — compile with dpacc first:
# dpacc dpa_handler.c dpa_dev_entry.c -hostcc aarch64-linux-gnu-gcc -I. -I/opt/mellanox/doca/include -o nc_dpa_app

echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo ./_build/nc_pipeline -l 0-3 -n 4 -a 03:00.0,dv_flow_en=2 -a 03:00.1,dv_flow_en=2 -- --device mlx5_0
```
