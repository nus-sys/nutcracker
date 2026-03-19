//===- BF3DPADialect.td - BF3 DPA cores dialect -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF3DPA_DIALECT_BF3DPA_BF3DPA_DIALECT_TD
#define BF3DPA_DIALECT_BF3DPA_BF3DPA_DIALECT_TD

include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// BF3DPA dialect definition.
//===----------------------------------------------------------------------===//

def BF3DPA_Dialect : Dialect {
    let name = "bf3dpa";
    let summary = "BlueField-3 DPA (Data Path Accelerator) cores dialect.";
    let description = [{
        bf3dpa models the ARM-based DPA (Data Path Accelerator) cores on the NVIDIA
        BlueField-3 SmartNIC. It is a concrete instance of vDPP with the following
        hardware constraints:

        Memory partitioning constraint:
          DPA threads operate on memory buffers provisioned by the control plane.
          All memory accesses beyond thread-local (stack) storage must use addresses
          of the form: provisioned_base + f(packet_fields), where f is a computable
          function of packet header fields (e.g., five-tuple hash, header field value).

          This ensures that each DPA thread's memory footprint is partitionable
          by the input packet, enabling hardware scheduling and isolation.

        The dialect provides two annotation ops to express this constraint:
          bf3dpa.provisioned_ptr — marks a pointer as a control-plane provisioned
                                   memory base (typically a function argument).
          bf3dpa.packet_index    — marks an integer as derived from packet fields.

        Verifier on bf3dpa.load / bf3dpa.store:
          If the pointer originates from a provisioned_ptr (traced through
          getelementptr), all GEP indices must be derivable from packet_index ops.

        DPA-specific intrinsics:
          bf3dpa.thread_fence  — __dpa_thread_fence() memory ordering barrier.
          bf3dpa.thread_id     — returns the DPA thread ID (for index computation).
    }];
    let cppNamespace = "::mlir::bf3dpa";
}

#endif // BF3DPA_DIALECT_BF3DPA_BF3DPA_DIALECT_TD
