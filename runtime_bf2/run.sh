#!/bin/bash
# run.sh — Build and run a nutcracker BF2 pipeline on DOCA 2.2.
#
# Usage:
#   ./run.sh                          # build + run (uses ../vdsa_output/doca_flow_pipeline.c)
#   ./run.sh /path/to/pipeline.c      # build + run with custom pipeline
#   ./run.sh --run-only               # skip build, just run existing binary
#
# Prereqs:
#   1. nutcracker.sh compile <app>   — produces vdsa_output/ with vdrmt.mlir
#   2. DOCA_TEMPLATE=$NUTCRACKER_ROOT/templates/doca-2.2.toml \
#        $NUTCRACKER_ROOT/build/bin/nutcracker-opt --vdrmt-to-bf2drmt
#        (from the directory containing vdsa_output/)
#   3. This script builds + runs the result.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN_ONLY=false
PIPELINE_C="../vdsa_output/doca_flow_pipeline.c"

for arg in "$@"; do
    case "$arg" in
        --run-only) RUN_ONLY=true ;;
        *)          PIPELINE_C="$arg" ;;
    esac
done

export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}

if [ "$RUN_ONLY" = false ]; then
    echo "=== Building nc_pipeline (DOCA 2.2 / BF2) ==="
    echo "  pipeline.c: $PIPELINE_C"

    if [ ! -f "$PIPELINE_C" ]; then
        echo "ERROR: $PIPELINE_C not found."
        echo "  1. cd \$NUTCRACKER_ROOT && bash nutcracker.sh compile <app>"
        echo "  2. DOCA_TEMPLATE=templates/doca-2.2.toml ./build/bin/nutcracker-opt --vdrmt-to-bf2drmt"
        exit 1
    fi

    rm -rf _build
    meson setup _build -Dpipeline_c="$PIPELINE_C"
    ninja -C _build

    echo ""
    echo "=== Build OK: _build/nc_pipeline ==="
fi

if [ ! -f _build/nc_pipeline ]; then
    echo "ERROR: _build/nc_pipeline not found. Run without --run-only first."
    exit 1
fi

echo ""
echo "=== Running on BF2 (Ctrl-C to stop) ==="
sudo _build/nc_pipeline \
    -l 0-3 -n 4 \
    -a 03:00.0,dv_flow_en=2 \
    -a 03:00.1,dv_flow_en=2
