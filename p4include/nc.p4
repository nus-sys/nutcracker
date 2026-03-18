// nc.p4 - NutCracker Architecture Definition
// A simplified, compiler-friendly architecture for multi-target packet processing

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
// Programmable Block Type Definitions
// ============================================================================

/// Main parser - extracts headers from incoming packets
parser MainParserT<MH, MM>(
    packet_in pkt,
    out MH main_hdr,
    inout MM main_meta,
    in nc_standard_metadata_t standard_meta
);

/// Main control - packet processing logic (THIS IS WHAT WE ANALYZE)
control MainControlT<MH, MM>(
    inout MH main_hdr,
    inout MM main_meta,
    inout nc_standard_metadata_t standard_meta
);

/// Main deparser - reassembles packet for transmission
control MainDeparserT<MH, MM>(
    packet_out pkt,
    in MH main_hdr,
    in MM main_meta,
    in nc_standard_metadata_t standard_meta
);

//============================================================================
// Extern Functions (Fixed-Function Accelerators)
// ============================================================================

/// Stateful counter array
extern Counter<I> {
    /// Constructor: create counter array with specified size
    Counter(bit<32> size);
    
    /// Increment counter at given index
    void count(in I index);
};

/// Hardware-accelerated 5-tuple hash (CRC32-based)
extern Hash5Tuple {
    /// Constructor: initialize hash unit
    Hash5Tuple();
    
    /// Compute hash over 5-tuple fields
    void apply(
        out bit<32> result,
        in  bit<32> srcAddr,
        in  bit<32> dstAddr,
        in  bit<16> srcPort,
        in  bit<16> dstPort,
        in  bit<8>  proto
    );
};

// ============================================================================
// NC Pipeline Package
// ============================================================================

package NC_PIPELINE<MH, MM>(
    MainParserT<MH, MM> main_parser,
    MainControlT<MH, MM> main_control,
    MainDeparserT<MH, MM> main_deparser
);

#endif  // _NC_P4_