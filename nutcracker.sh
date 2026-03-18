#!/bin/bash
# ============================================================================
# NutCracker Compiler Build and Run Script
# ============================================================================

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Directories
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
APPS_DIR="$ROOT_DIR/apps"
OUTPUT_DIR="$ROOT_DIR/vdsa_output"
P4C_DIR="$ROOT_DIR/third_party/p4mlir/third_party/p4c"
P4_INCLUDE_DIR="$ROOT_DIR/p4include"

# Tools
P4MLIR_TRANSLATE="$P4C_DIR/build/p4mlir-translate"
NUTCRACKER_OPT="$BUILD_DIR/bin/nutcracker-opt"

# ============================================================================
# Helper Functions
# ============================================================================

print_header() {
    local title="$1"
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "🚀 ${BOLD}${GREEN}$title${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "📦 ${BOLD}${CYAN}$1${NC}"
    echo ""
}

print_step() {
    echo -e "${GREEN}  ▸${NC} $1"
}

print_substep() {
    echo -e "    ${CYAN}→${NC} $1"
}

print_error() {
    echo -e "${RED}❌ Error:${NC} $1" >&2
}

print_success() {
    echo -e "${GREEN}✅${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠️${NC}  $1"
}

print_info() {
    echo -e "${BLUE}ℹ️${NC}  $1"
}

print_file() {
    echo -e "    ${MAGENTA}📄${NC} $1"
}

print_dir() {
    echo -e "    ${BLUE}📁${NC} $1"
}

# ============================================================================
# Setup Functions
# ============================================================================

setup_p4_includes() {
    print_header "Setting up P4 Include Directory"
    
    mkdir -p "$P4_INCLUDE_DIR"
    
    # Check if nc.p4 exists
    if [ ! -f "$P4_INCLUDE_DIR/nc.p4" ]; then
        print_warning "nc.p4 not found in p4include/"
        print_step "Creating nc.p4 architecture definition..."
        
        cat > "$P4_INCLUDE_DIR/nc.p4" << 'EOF'
// nc.p4 - NutCracker Architecture Definition
#ifndef _NC_P4_
#define _NC_P4_

#include <core.p4>

// ============================================================================
// Standard Metadata
// ============================================================================

struct nc_standard_metadata_t {
    bit<9>  ingress_port;
    bit<9>  egress_port;
    bit<32> packet_length;
    bit<1>  drop;
}

// ============================================================================
// Extern Functions (Fixed-Function Accelerators)
// ============================================================================

extern Counter<I> {
    Counter(bit<32> size);
    void count(in I index);
}

extern Hash5Tuple {
    Hash5Tuple();
    void apply(out bit<32>, in bit<32>, in bit<32>, 
               in bit<16>, in bit<16>, in bit<8>);
}

// ============================================================================
// Programmable Block Type Definitions
// ============================================================================

parser MainParserT<MH, MM>(
    packet_in pkt,
    out MH main_hdr,
    inout MM main_meta,
    in nc_standard_metadata_t standard_meta
);

control MainControlT<MH, MM>(
    inout MH main_hdr,
    inout MM main_meta,
    inout nc_standard_metadata_t standard_meta
);

control MainDeparserT<MH, MM>(
    packet_out pkt,
    in MH main_hdr,
    in MM main_meta,
    in nc_standard_metadata_t standard_meta
);

// ============================================================================
// NC Pipeline Package
// ============================================================================

package NC_PIPELINE<MH, MM>(
    MainParserT<MH, MM> main_parser,
    MainControlT<MH, MM> main_control,
    MainDeparserT<MH, MM> main_deparser
);

#endif  // _NC_P4_
EOF
        print_success "Created nc.p4"
    else
        print_success "nc.p4 already exists"
    fi
}

# ============================================================================
# Build Functions
# ============================================================================

build_llvm() {
    print_header "Building LLVM"
    
    if [ -d "$ROOT_DIR/third_party/llvm/build" ]; then
        print_warning "LLVM already built, skipping..."
        return 0
    fi
    
    print_step "Running LLVM build script..."
    cd "$ROOT_DIR"
    bash build_scripts/build_llvm.sh
    print_success "LLVM built successfully"
}

build_p4mlir() {
    print_header "Building P4MLIR"
    
    if [ -f "$P4MLIR_TRANSLATE" ]; then
        print_warning "P4MLIR already built, skipping..."
        return 0
    fi
    
    print_step "Building P4MLIR..."
    cd "$P4C_DIR"
    mkdir -p build
    cd build
    cmake .. -DENABLE_MLIR=ON
    make -j$(nproc)
    print_success "P4MLIR built successfully"
}

build_nutcracker() {
    print_header "Building NutCracker"
    
    print_step "Configuring CMake..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. -DCMAKE_BUILD_TYPE=Debug 
    
    print_step "Building NutCracker..."
    make -j$(nproc)
    
    print_success "NutCracker built successfully"
}

# ============================================================================
# Compilation Functions
# ============================================================================

compile_p4_to_mlir() {
    local p4_file=$1
    local output_file=$2
    local force=${3:-false}
    
    # Skip if MLIR exists and not forcing
    if [ -f "$output_file" ] && [ "$force" = false ]; then
        print_substep "MLIR already exists, skipping: $(basename $output_file)"
        return 0
    fi
    
    print_step "P4 → P4HIR MLIR: $(basename $p4_file)"
    print_substep "Input:  $p4_file"
    print_substep "Output: $output_file"
    
    if [ ! -f "$P4MLIR_TRANSLATE" ]; then
        print_error "P4MLIR translator not found. Please build P4MLIR first."
        exit 1
    fi
    
    # Build P4 include paths
    local include_args=""
    
    if [ -d "$P4_INCLUDE_DIR" ]; then
        include_args="-I$P4_INCLUDE_DIR"
    fi
    
    local p4c_include="$P4C_DIR/p4include"
    if [ -d "$p4c_include" ]; then
        include_args="$include_args -I$p4c_include"
    fi
    
    local app_dir=$(dirname "$p4_file")
    if [ -d "$app_dir" ]; then
        include_args="$include_args -I$app_dir"
    fi
    
    "$P4MLIR_TRANSLATE" $include_args --typeinference-only "$p4_file" > "$output_file" 2>&1
    
    if [ $? -eq 0 ]; then
        local line_count=$(wc -l < "$output_file")
        local op_count=$(grep -c "p4hir\." "$output_file" || echo "0")
        print_success "Generated P4HIR MLIR ($line_count lines, ~$op_count ops)"
    else
        print_error "P4 to MLIR compilation failed"
        cat "$output_file"
        exit 1
    fi
}

run_partition_pass() {
    local mlir_file=$1
    local output_dir=$2  # Will be vdsa_output/
    
    print_step "Step 2: Partitioning P4HIR into Basic Blocks"
    print_substep "Input:  $mlir_file"
    print_substep "Output: $output_dir"
    
    if [ ! -f "$NUTCRACKER_OPT" ]; then
        print_error "nutcracker-opt not found. Please build NutCracker first."
        exit 1
    fi
    
    if [ ! -f "$mlir_file" ]; then
        print_error "Input MLIR file not found: $mlir_file"
        exit 1
    fi
    
    # Get directories
    local mlir_dir=$(dirname "$mlir_file")
    local mlir_name=$(basename "$mlir_file")
    
    # Change to MLIR directory and run pass
    cd "$mlir_dir"
    
    print_substep "Running partition pass (creates vdsa_output in current dir)..."
    
    # Pass creates vdsa_output/ in current directory
    "$NUTCRACKER_OPT" --p4hir-partition "$mlir_name" 2>&1
    local result=$?
    
    cd "$ROOT_DIR"
    
    if [ $result -ne 0 ]; then
        print_error "Partition pass failed"
        return 1
    fi
    
    # Pass created vdsa_output in MLIR directory
    local pass_output_dir="$mlir_dir/vdsa_output"
    
    if [ ! -d "$pass_output_dir" ]; then
        print_error "Pass did not create vdsa_output directory"
        return 1
    fi
    
    # Move to ROOT_DIR/vdsa_output/
    rm -rf "$output_dir"
    mv "$pass_output_dir" "$output_dir"
    
    if [ ! -d "$output_dir" ]; then
        print_error "Failed to move output"
        return 1
    fi
    
    # Count blocks
    local block_count=$(find "$output_dir" -maxdepth 1 -type d -name "block*" 2>/dev/null | wc -l)
    
    if [ "$block_count" -eq 0 ]; then
        print_error "No blocks generated"
        return 1
    fi
    
    print_success "Partitioned into $block_count basic blocks"
    echo ""
}

run_lowering_passes() {
    local mlir_file=$1
    local output_dir=$2
    
    print_step "Step 3: Lowering P4HIR to Target Dialects"
    print_substep "Partition dir: $output_dir"
    
    if [ ! -f "$NUTCRACKER_OPT" ]; then
        print_error "nutcracker-opt not found."
        exit 1
    fi
    
    if [ ! -d "$output_dir" ]; then
        print_error "Partition output directory not found: $output_dir"
        return 1
    fi
    
    cd "$ROOT_DIR"
    
    # Run vDRMT lowering (handles simple blocks)
    print_substep "Trying vDRMT lowering (Match-Action)..."
    "$NUTCRACKER_OPT" --p4hir-to-vdrmt "$mlir_file" 2>&1
    local vdrmt_result=$?
    
    # Run vDPP lowering (handles complex blocks)
    print_substep "Trying vDPP lowering (Programmable)..."
    "$NUTCRACKER_OPT" --p4hir-to-vdpp "$mlir_file" 2>&1
    local vdpp_result=$?
    
    # Check results
    local vdrmt_count=$(find "$output_dir" -name "vdrmt.mlir" 2>/dev/null | wc -l)
    local vdpp_count=$(find "$output_dir" -name "vdpp.mlir" 2>/dev/null | wc -l)
    
    echo ""
    print_substep "Lowering Results:"
    
    if [ "$vdrmt_count" -gt 0 ]; then
        echo -e "    ${GREEN}✅ vDRMT:${NC} $vdrmt_count blocks"
        for block_dir in "$output_dir"/block*; do
            if [ -f "$block_dir/vdrmt.mlir" ]; then
                echo -e "      ${GREEN}→${NC} $(basename $block_dir)/vdrmt.mlir"
            fi
        done
    else
        echo -e "    ${YELLOW}⚠️  vDRMT:${NC} 0 blocks"
    fi
    
    if [ "$vdpp_count" -gt 0 ]; then
        echo -e "    ${BLUE}✅ vDPP:${NC}  $vdpp_count blocks"
        for block_dir in "$output_dir"/block*; do
            if [ -f "$block_dir/vdpp.mlir" ]; then
                echo -e "      ${BLUE}→${NC} $(basename $block_dir)/vdpp.mlir"
            fi
        done
    else
        echo -e "    ${YELLOW}⚠️  vDPP:${NC}  0 blocks"
    fi
    
    local total_lowered=$((vdrmt_count + vdpp_count))
    
    if [ "$total_lowered" -eq 0 ]; then
        print_warning "No blocks were lowered to any target"
        return 1
    fi
    
    print_success "Lowered $total_lowered blocks total"
    echo ""
    
    return 0
}

generate_visualization() {
    local dot_file=$1
    local output_pdf=$2
    
    if [ ! -f "$dot_file" ]; then
        print_warning "DOT file not found: $dot_file"
        return 1
    fi
    
    print_step "Step 4: Generating Control Flow Visualization"
    
    if command -v dot &> /dev/null; then
        dot -Tpdf "$dot_file" -o "$output_pdf" 2>/dev/null
        print_success "Generated PDF: $(basename $output_pdf)"
        
        dot -Tpng "$dot_file" -o "${output_pdf%.pdf}.png" 2>/dev/null
        print_success "Generated PNG: $(basename ${output_pdf%.pdf}.png)"
    else
        print_warning "Graphviz not installed. Skipping visualization."
        print_info "Install with: sudo apt-get install graphviz"
    fi
    echo ""
}

# ============================================================================
# Precompile Functions
# ============================================================================

precompile_app() {
    local app_name=$1
    local app_dir="$APPS_DIR/$app_name"
    local p4_file="$app_dir/${app_name}.p4"
    local mlir_file="$app_dir/${app_name}.mlir"
    local force=${2:-false}
    
    if [ ! -f "$p4_file" ]; then
        print_error "P4 file not found: $p4_file"
        return 1
    fi
    
    if [ -f "$mlir_file" ] && [ "$force" = false ]; then
        print_info "MLIR already exists for $app_name (use --force to regenerate)"
        return 0
    fi
    
    print_section "Precompiling: $app_name"
    compile_p4_to_mlir "$p4_file" "$mlir_file" "$force"
    echo ""
}

precompile_all_apps() {
    print_header "Precompiling All Applications (P4 → MLIR)"
    
    local force=false
    if [ "$1" = "--force" ]; then
        force=true
        print_info "Force regeneration enabled"
    fi
    
    setup_p4_includes
    
    local success_count=0
    local skip_count=0
    local fail_count=0
    
    for app_dir in "$APPS_DIR"/*; do
        if [ -d "$app_dir" ]; then
            local app_name=$(basename "$app_dir")
            local p4_file="$app_dir/${app_name}.p4"
            local mlir_file="$app_dir/${app_name}.mlir"
            
            if [ -f "$p4_file" ]; then
                if [ -f "$mlir_file" ] && [ "$force" = false ]; then
                    print_info "✓ $app_name (already compiled)"
                    ((skip_count++))
                else
                    if precompile_app "$app_name" "$force"; then
                        ((success_count++))
                    else
                        ((fail_count++))
                        print_error "Failed to precompile $app_name"
                    fi
                fi
            fi
        fi
    done
    
    print_header "Precompile Summary"
    echo -e "  ${GREEN}✅ Compiled:${NC} $success_count"
    echo -e "  ${BLUE}⏭️  Skipped:${NC}  $skip_count"
    echo -e "  ${RED}❌ Failed:${NC}   $fail_count"
    echo ""
    
    if [ $success_count -gt 0 ]; then
        print_success "Precompile complete! MLIR files ready for partition/lowering."
    fi
}

# ============================================================================
# Compile Application
# ============================================================================

compile_app() {
    local app_name=$1
    local app_dir="$APPS_DIR/$app_name"
    local p4_file="$app_dir/${app_name}.p4"
    local mlir_file="$app_dir/${app_name}.mlir"
    local output_dir="$OUTPUT_DIR"
    
    print_header "Compiling: $app_name"
    
    if [ ! -f "$p4_file" ]; then
        print_error "P4 file not found: $p4_file"
        exit 1
    fi
    
    setup_p4_includes
    
    # Step 1: P4 → MLIR (skip if exists)
    compile_p4_to_mlir "$p4_file" "$mlir_file" false
    
    rm -rf "$output_dir"
    mkdir -p "$output_dir"
    
    # Step 2: Partition into blocks
    run_partition_pass "$mlir_file" "$output_dir"
    
    # Step 3: Lower to both vDRMT and vDPP
    run_lowering_passes "$mlir_file" "$output_dir"
    
    # Step 4: Visualize
    generate_visualization "$output_dir/dependency_graph.dot" "$output_dir/graph.pdf"
    
    print_header "Compilation Summary"
    
    if [ -d "$output_dir" ]; then
        local block_count=$(find "$output_dir" -maxdepth 1 -type d -name "block*" | wc -l)
        
        echo -e "${BOLD}📦 Application:${NC} $app_name"
        echo -e "${BOLD}🧩 Blocks:${NC}      $block_count"
        echo -e "${BOLD}📂 Output:${NC}      $output_dir"
        echo ""
        
        print_section "Block Statistics"
        local block_id=0
        local vdrmt_count=0
        local vdpp_count=0
        local unmapped_count=0
        
        for block_dir in "$output_dir"/block*; do
            if [ -f "$block_dir/metadata.json" ]; then
                local ops=$(jq -r '.operations' "$block_dir/metadata.json" 2>/dev/null || echo "?")
                local has_if=$(jq -r '.hasConditionalBranch' "$block_dir/metadata.json" 2>/dev/null || echo "false")
                local has_table=$(jq -r '.hasTableApply' "$block_dir/metadata.json" 2>/dev/null || echo "false")
                
                echo -e "  ${CYAN}📦 Block $block_id:${NC} $ops ops"
                [ "$has_if" = "true" ] && echo -e "    ${YELLOW}🔀${NC} Conditional branch"
                [ "$has_table" = "true" ] && echo -e "    ${YELLOW}📊${NC} Table apply"
                
                local has_vdrmt=false
                local has_vdpp=false
                
                if [ -f "$block_dir/vdrmt.mlir" ]; then
                    echo -e "    ${GREEN}✅ vDRMT (Match-Action)${NC}"
                    echo -e "    ${GREEN}📄 vdrmt.mlir${NC}"
                    has_vdrmt=true
                    ((vdrmt_count++))
                fi
                
                if [ -f "$block_dir/vdpp.mlir" ]; then
                    echo -e "    ${BLUE}✅ vDPP (Programmable)${NC}"
                    echo -e "    ${BLUE}📄 vdpp.mlir${NC}"
                    has_vdpp=true
                    ((vdpp_count++))
                fi
                
                if [ "$has_vdrmt" = false ] && [ "$has_vdpp" = false ]; then
                    echo -e "    ${RED}❌ Not lowered to any target${NC}"
                    ((unmapped_count++))
                fi
                
                echo ""
                ((block_id++))
            fi
        done
        
        echo -e "${BOLD}Lowering Summary:${NC}"
        echo -e "  ${GREEN}vDRMT blocks:${NC}    $vdrmt_count"
        echo -e "  ${BLUE}vDPP blocks:${NC}     $vdpp_count"
        echo -e "  ${RED}Unmapped:${NC}        $unmapped_count"
        echo -e "  ${CYAN}Total blocks:${NC}    $block_id"
        echo ""
        
        print_section "Generated Files"
        echo -e "${BLUE}📁${NC} $output_dir/"
        echo -e "  ${MAGENTA}📄${NC} dependency_graph.dot"
        [ -f "$output_dir/graph.pdf" ] && echo -e "  ${MAGENTA}📄${NC} graph.pdf"
        [ -f "$output_dir/graph.png" ] && echo -e "  ${MAGENTA}📄${NC} graph.png"
        
        for block_dir in "$output_dir"/block*; do
            if [ -d "$block_dir" ]; then
                local block_name=$(basename "$block_dir")
                echo -e "  ${BLUE}📁${NC} $block_name/"
                echo -e "    ${MAGENTA}📄${NC} p4hir.mlir"
                [ -f "$block_dir/vdrmt.mlir" ] && echo -e "    ${GREEN}📄${NC} vdrmt.mlir"
                [ -f "$block_dir/vdpp.mlir" ] && echo -e "    ${BLUE}📄${NC} vdpp.mlir"
                echo -e "    ${MAGENTA}📄${NC} metadata.json"
            fi
        done
        echo ""
    fi
    
    print_success "Compilation complete!"
    echo ""
}

# ============================================================================
# Clean Functions
# ============================================================================

clean_build() {
    print_header "Cleaning Build"
    
    print_step "Removing build directory..."
    rm -rf "$BUILD_DIR"
    
    print_step "Removing output directory..."
    rm -rf "$OUTPUT_DIR"
    
    print_success "Clean complete"
}

clean_mlir() {
    print_header "Cleaning MLIR Files"
    
    print_step "Removing generated MLIR files..."
    find "$APPS_DIR" -name "*.mlir" -type f -delete
    
    local count=$(find "$APPS_DIR" -name "*.mlir" -type f 2>/dev/null | wc -l)
    print_success "Removed MLIR files"
}

clean_all() {
    print_header "Deep Clean"
    
    clean_build
    clean_mlir
    
    print_step "Removing LLVM build..."
    rm -rf "$ROOT_DIR/third_party/llvm/build"
    
    print_step "Removing P4MLIR build..."
    rm -rf "$P4C_DIR/build"
    
    print_success "Deep clean complete"
}

# ============================================================================
# Main Menu
# ============================================================================

show_usage() {
    cat << EOF

${BOLD}${BLUE}🌰 NutCracker Compiler${NC} - Multi-Target vDSA Code Generator

${BOLD}USAGE:${NC}
    $0 <command> [options]

${BOLD}COMMANDS:${NC}
    ${GREEN}🔨 build${NC}              Build all components
    ${GREEN}🔨 build-llvm${NC}         Build LLVM only
    ${GREEN}🔨 build-p4mlir${NC}       Build P4MLIR only
    ${GREEN}🔨 build-nutcracker${NC}   Build NutCracker only
    
    ${GREEN}⚙️  setup${NC}              Setup P4 include directory
    
    ${GREEN}🎯 precompile${NC} <app>   Precompile P4 → MLIR for one app
    ${GREEN}🎯 precompile-all${NC}     Precompile all apps (P4 → MLIR)
    
    ${GREEN}🚀 compile${NC} <app>      Compile app (MLIR → vDRMT/vDPP)
    ${GREEN}🚀 compile-all${NC}        Compile all applications
    
    ${GREEN}🧹 clean${NC}              Clean build artifacts
    ${GREEN}🧹 clean-mlir${NC}         Clean generated MLIR files
    ${GREEN}🧹 clean-all${NC}          Deep clean (including dependencies)
    
    ${GREEN}▶️  run${NC} <app>          Build and compile application
    ${GREEN}📋 list-apps${NC}          List available applications
    
    ${GREEN}❓ help${NC}               Show this help message

${BOLD}COMPILATION PIPELINE:${NC}
    1. P4 → P4HIR MLIR           (p4mlir-translate) [precompile]
    2. P4HIR → Basic Blocks      (p4hir-partition)  [compile]
    3. P4HIR → vDRMT + vDPP      (both passes)      [compile]
       - Each block tries both lowering passes
       - vDRMT for simple match-action blocks
       - vDPP for complex programmable blocks
    4. Control Flow Graph        (graphviz)         [compile]

${BOLD}EXAMPLES:${NC}
    $0 build                       # Build everything
    $0 precompile-all              # P4 → MLIR for all apps
    $0 compile simple_forwarding   # MLIR → vDRMT/vDPP
    $0 run simple_forwarding       # Build + full compile

${BOLD}WORKFLOW:${NC}
    # One-time setup
    $0 build
    $0 precompile-all
    
    # Fast iteration
    $0 compile simple_forwarding   # Uses cached MLIR
    # Edit pass, rebuild
    $0 build-nutcracker
    $0 compile simple_forwarding   # Instant!

${BOLD}OUTPUT STRUCTURE:${NC}
    apps/<app>/
    ├── <app>.p4              # Source
    └── <app>.mlir            # Precompiled (cached)
    
    vdsa_output/
    ├── block0/
    │   ├── p4hir.mlir        # Partitioned
    │   ├── vdrmt.mlir        # vDRMT lowered (if applicable)
    │   ├── vdpp.mlir         # vDPP lowered (if applicable)
    │   └── metadata.json
    └── dependency_graph.dot

EOF
}

list_apps() {
    print_header "Available Applications"
    
    if [ ! -d "$APPS_DIR" ]; then
        print_error "Apps directory not found: $APPS_DIR"
        exit 1
    fi
    
    for app_dir in "$APPS_DIR"/*; do
        if [ -d "$app_dir" ]; then
            local app_name=$(basename "$app_dir")
            local p4_file="$app_dir/${app_name}.p4"
            
            if [ -f "$p4_file" ]; then
                echo -e "  ${CYAN}📦${NC} ${BOLD}$app_name${NC}"
                
                local desc=$(head -n 5 "$p4_file" | grep -E "^//" | sed 's|^// *||' | head -1)
                [ -n "$desc" ] && echo -e "    $desc"
                
                local mlir_file="$app_dir/${app_name}.mlir"
                if [ -f "$mlir_file" ]; then
                    echo -e "    ${GREEN}✅ Precompiled MLIR available${NC}"
                else
                    echo -e "    ${YELLOW}⚠️  No MLIR (run: precompile $app_name)${NC}"
                fi
                
                echo ""
            fi
        fi
    done
}

compile_all_apps() {
    print_header "Compiling All Applications"
    
    local success_count=0
    local fail_count=0
    
    for app_dir in "$APPS_DIR"/*; do
        if [ -d "$app_dir" ]; then
            local app_name=$(basename "$app_dir")
            local p4_file="$app_dir/${app_name}.p4"
            
            if [ -f "$p4_file" ]; then
                if compile_app "$app_name"; then
                    ((success_count++))
                else
                    ((fail_count++))
                    print_error "Failed to compile $app_name"
                fi
            fi
        fi
    done
    
    print_header "Summary"
    echo -e "  ${GREEN}✅ Success:${NC} $success_count"
    echo -e "  ${RED}❌ Failed:${NC}  $fail_count"
    echo ""
}

# ============================================================================
# Main Entry Point
# ============================================================================

main() {
    cd "$ROOT_DIR"
    
    if [ $# -eq 0 ]; then
        show_usage
        exit 0
    fi
    
    local command=$1
    shift
    
    case "$command" in
        build)
            build_llvm
            build_p4mlir
            build_nutcracker
            setup_p4_includes
            ;;
        
        build-llvm)
            build_llvm
            ;;
        
        build-p4mlir)
            build_p4mlir
            ;;
        
        build-nutcracker)
            build_nutcracker
            ;;
        
        setup)
            setup_p4_includes
            ;;
        
        precompile)
            if [ $# -eq 0 ]; then
                print_error "Application name required"
                echo "Usage: $0 precompile <app_name> [--force]"
                exit 1
            fi
            local force=false
            if [ "$2" = "--force" ]; then
                force=true
            fi
            precompile_app "$1" "$force"
            ;;
        
        precompile-all)
            precompile_all_apps "$@"
            ;;
        
        compile)
            if [ $# -eq 0 ]; then
                print_error "Application name required"
                echo "Usage: $0 compile <app_name>"
                exit 1
            fi
            compile_app "$1"
            ;;
        
        compile-all)
            compile_all_apps
            ;;
        
        run)
            if [ $# -eq 0 ]; then
                print_error "Application name required"
                echo "Usage: $0 run <app_name>"
                exit 1
            fi
            build_nutcracker
            compile_app "$1"
            ;;
        
        clean)
            clean_build
            ;;
        
        clean-mlir)
            clean_mlir
            ;;
        
        clean-all)
            clean_all
            ;;
        
        list-apps)
            list_apps
            ;;
        
        help|--help|-h)
            show_usage
            ;;
        
        *)
            print_error "Unknown command: $command"
            show_usage
            exit 1
            ;;
    esac
}

main "$@"