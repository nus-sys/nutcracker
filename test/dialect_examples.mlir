// Concrete dialect op examples illustrating the four-dialect stack.
//
// Verify these parse with:
//   nutcracker-opt --allow-unregistered-dialect dialect_examples.mlir
//
// Each section is a self-contained module.

// ============================================================================
// 1. vDRMT  — virtual DRMT (target-independent match-action)
//    vdrmt.table  : named match-action rule set
//    vdrmt.binop(mul, ...) : multiplication (egglog rewrites this for BF3)
// ============================================================================

module @vdrmt_example {
  // A table containing one action that multiplies a field by a constant.
  vdrmt.table @scale_weight {
    vdrmt.table_action @action_scale
        type((!vdrmt.ref<i32>) -> ())
        @action_scale {
      ^bb0(%dst: !vdrmt.ref<i32>):
        %c2   = vdrmt.constant 2 : i32
        %val  = vdrmt.load %dst : <!vdrmt.ref<i32>> -> i32
        // vdrmt.binop(mul) is legal here — egglog rewrites it to shifts for BF3.
        %prod = vdrmt.binop(mul, %val, %c2) : i32
        vdrmt.store %prod, %dst : i32, <!vdrmt.ref<i32>>
        vdrmt.yield
    }
  }
}

// ============================================================================
// 2. BF3DRMT — BF3 NIC ASIC dialect (hardware-specific)
//    bf3drmt.pipe  : DOCA Flow pipe (replaces vdrmt.table)
//    bf3drmt.binop : mul/div are absent — egglog already eliminated them
// ============================================================================

module @bf3drmt_example {
  // The same "scale" action after egglog rewrites mul → shl(1).
  bf3drmt.pipe @scale_weight type(basic) nr_entries(256) {
    bf3drmt.pipe_action @action_scale {
      ^bb0(%dst: !vdrmt.ref<i32>):
        %val  = bf3drmt.load %dst : <!vdrmt.ref<i32>> -> i32
        // mul(x, 2)  →  shl(x, 1)  by egglog equality saturation.
        // bf3drmt.binop(mul) would be illegal here (no case 1/2 in enum).
        %c1   = bf3drmt.constant 1 : i32
        %prod = bf3drmt.shl %val, %c1 : i32
        bf3drmt.store %prod, %dst : i32, <!vdrmt.ref<i32>>
        bf3drmt.next 0
    }
    bf3drmt.next 0   // miss: forward to block 0
  }
}

// ============================================================================
// 3. vDPP — virtual DPP (target-independent programmable)
//    vdpp.add : integer add (floats blocked by AnyInteger constraint)
// ============================================================================

module @vdpp_example {
  func.func @vdpp_block0(
      %hdr  : !vdpp.ptr<!vdpp.struct<"main_headers_t", (i32)>>,
      %meta : !vdpp.ptr<!vdpp.struct<"main_metadata_t", (i32)>>,
      %std  : !vdpp.ptr<!vdpp.struct<"nc_standard_metadata_t", (i9, i9, i32, i1)>>)
      attributes {vdpp.block_id = 0 : i32} {
    %cst0 = vdpp.constant(0 : i32) : i32
    %field_ref = vdpp.getelementptr %meta[%cst0, %cst0]
        : <!vdpp.struct<"main_metadata_t", (i32)>>, i32, i32 -> <i32>
    %val  = vdpp.load %field_ref : <i32> -> i32
    %inc  = vdpp.constant(1 : i32) : i32
    // vdpp.add — both operands are i32 (AnyInteger), perfectly legal.
    %sum  = vdpp.add %val, %inc : i32
    vdpp.store %sum, %field_ref : i32, <i32>
    vdpp.return {successor = 1 : i32}
  }
}

// ============================================================================
// 4. BF3DPA — BF3 DPA/ARM dialect (hardware-specific)
//    bf3dpa.add : integer add (AnyInteger operands — floats blocked)
//    bf3dpa.constant / bf3dpa.load / bf3dpa.store also reject float types
//      (verified by BF3DPAOps.cpp verifiers added in this session)
// ============================================================================

module @bf3dpa_example {
  func.func @nc_block0_arm(
      %hdr  : !vdpp.ptr<!vdpp.struct<"main_headers_t", (i32)>>,
      %meta : !vdpp.ptr<!vdpp.struct<"main_metadata_t", (i32)>>,
      %std  : !vdpp.ptr<!vdpp.struct<"nc_standard_metadata_t", (i9, i9, i32, i1)>>)
      -> i32 {
    %c0  = bf3dpa.constant(0 : i32) : i32
    %ptr = bf3dpa.getelementptr %meta[%c0, %c0]
        : !vdpp.ptr<!vdpp.struct<"main_metadata_t", (i32)>>, i32, i32
        -> !vdpp.ptr<i32>
    %val = bf3dpa.load %ptr : !vdpp.ptr<i32> -> i32
    %c1  = bf3dpa.constant(1 : i32) : i32
    // bf3dpa.add — both operands are i32 (AnyInteger), legal.
    %sum = bf3dpa.add %val, %c1 : i32
    bf3dpa.store %sum, %ptr : i32, !vdpp.ptr<i32>
    // Float example that the verifier NOW rejects (uncomment to test):
    // %bad = bf3dpa.constant(1.0 : f32) : f32
    //   → error: BF3DPA does not support floating-point types
    %ret = bf3dpa.constant(0 : i32) : i32
    bf3dpa.return %ret : i32
  }
}
