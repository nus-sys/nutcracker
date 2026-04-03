// bloom_filter_int.p4 - Bloom Filter-based INT using NC architecture

#include <core.p4>
#include "nc.p4"

// ============================================================================
// Type Definitions
// ============================================================================

typedef bit<9>  egressSpec_t;
typedef bit<48> macAddr_t;
typedef bit<32> ip4Addr_t;

typedef bit<32> bloom_filter_index_t;
typedef bit<16> bloom_filter_value_t;

// ============================================================================
// Header Definitions
// ============================================================================

header ethernet_t {
    macAddr_t dstAddr;
    macAddr_t srcAddr;
    bit<16>   etherType;
}

header ipv4_t {
    bit<4>    version;
    bit<4>    ihl;
    bit<6>    dscp;
    bit<2>    ecn;
    bit<16>   totalLen;
    bit<16>   identification;
    bit<3>    flags;
    bit<13>   fragOffset;
    bit<8>    ttl;
    bit<8>    protocol;
    bit<16>   hdrChecksum;
    ip4Addr_t srcAddr;
    ip4Addr_t dstAddr;
}

header tcp_t {
    bit<16> srcPort;
    bit<16> dstPort;
    bit<32> seqNo;
    bit<32> ackNo;
    bit<4>  dataOffset;
    bit<4>  res;
    bit<8>  flags;
    bit<16> window;
    bit<16> checksum;
    bit<16> urgentPtr;
}

header int_header_t {
    bit<4>  version;
    bit<2>  rep;
    bit<1>  c;
    bit<1>  e;
    bit<5>  rsvd1;
    bit<5>  ins_cnt;
    bit<8>  max_hop_cnt;
    bit<8>  total_hop_cnt;
    bit<4>  instruction_mask_0003;
    bit<4>  instruction_mask_0407;
    bit<4>  instruction_mask_0811;
    bit<4>  instruction_mask_1215;
    bit<16> rsvd2;
}

header int_switch_id_t {
    bit<32> switch_id;
}

// ============================================================================
// Main Headers and Metadata
// ============================================================================

struct main_headers_t {
    ethernet_t      ethernet;
    ipv4_t          ipv4;
    tcp_t           tcp;
    int_header_t    int_header;
    int_switch_id_t int_switch_id;
}

struct main_metadata_t {
    // INT metadata
    bit<3>  int_node_type;      // NONE=0, TRANSIT=4
    bit<32> switch_id;
    bit<1>  int_enabled;
    
    // Bloom filter metadata
    bit<32> bloom_hash;
    bit<32> bloom_index;
    bit<16> bloom_counter;
    
    // Forwarding metadata
    bit<9>  egress_port;
    bit<1>  drop_packet;
}

// ============================================================================
// Constants
// ============================================================================

const bit<3> INT_NODE_NONE = 0;
const bit<3> INT_NODE_TRANSIT = 4;

const bit<32> BLOOM_FILTER_ENTRIES = 4096;
const bit<16> BLOOM_FILTER_INDEX_BITS = 12;
const bit<16> REPORT_FREQ = 100;

const bit<32> MULT_HASH_A = 73;

// ============================================================================
// Parser
// ============================================================================

parser MainParser(
    packet_in pkt,
    out main_headers_t hdr,
    inout main_metadata_t meta,
    in nc_standard_metadata_t standard_meta
) {
    state start {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.etherType) {
            0x0800: parse_ipv4;
            default: accept;
        }
    }
    
    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol) {
            6: parse_tcp;
            default: accept;
        }
    }
    
    state parse_tcp {
        pkt.extract(hdr.tcp);
        transition accept;
    }
}

// ============================================================================
// Main Control - Bloom Filter INT Processing
// ============================================================================

control MainControl(
    inout main_headers_t hdr,
    inout main_metadata_t meta,
    inout nc_standard_metadata_t standard_meta
) {
    
    // ========================================================================
    // Register Instantiation
    // ========================================================================
    
    Register<bloom_filter_value_t>(BLOOM_FILTER_ENTRIES) bloom_filter;
    
    // ========================================================================
    // Actions - Bloom Filter Operations
    // ========================================================================
    
    action compute_bloom_hash() {
        // Compute hash of source IP
        bit<32> src_ip = hdr.ipv4.srcAddr;
        
        // Multiplicative hash
        bit<32> hash_val = (src_ip * MULT_HASH_A) >> (32 - BLOOM_FILTER_INDEX_BITS);
        
        // Mask to bloom filter size
        meta.bloom_index = hash_val & (BLOOM_FILTER_ENTRIES - 1);
    }
    
    action read_bloom_counter() {
        // Read counter value from bloom filter
        meta.bloom_counter = bloom_filter.read(meta.bloom_index);
    }
    
    action increment_bloom_counter() {
        // Increment and write back
        bit<16> new_val = meta.bloom_counter + 1;
        bloom_filter.write(meta.bloom_index, new_val);
    }
    
    action reset_bloom_counter() {
        // Reset counter to 0
        bloom_filter.write(meta.bloom_index, 0);
    }
    
    // ========================================================================
    // Actions - INT Operations
    // ========================================================================
    
    action enable_int_transit(bit<32> switch_id) {
        meta.int_node_type = INT_NODE_TRANSIT;
        meta.switch_id = switch_id;
        meta.int_enabled = 1;
    }
    
    action add_int_header() {
        // Add INT header
        hdr.int_header.setValid();
        hdr.int_header.version = 1;
        hdr.int_header.rep = 0;
        hdr.int_header.c = 0;
        hdr.int_header.e = 0;
        hdr.int_header.rsvd1 = 0;
        hdr.int_header.ins_cnt = 1;
        hdr.int_header.max_hop_cnt = 8;
        hdr.int_header.total_hop_cnt = 1;
        hdr.int_header.instruction_mask_0003 = 0x8; // Switch ID instruction
        hdr.int_header.instruction_mask_0407 = 0;
        hdr.int_header.instruction_mask_0811 = 0;
        hdr.int_header.instruction_mask_1215 = 0;
        hdr.int_header.rsvd2 = 0;
        
        // Add switch ID metadata
        hdr.int_switch_id.setValid();
        hdr.int_switch_id.switch_id = meta.switch_id;
    }
    
    // ========================================================================
    // Actions - Forwarding
    // ========================================================================
    
    action ipv4_forward(macAddr_t src_mac, macAddr_t dst_mac, bit<9> port) {
        hdr.ethernet.srcAddr = src_mac;
        hdr.ethernet.dstAddr = dst_mac;
        hdr.ipv4.ttl = hdr.ipv4.ttl - 1;
        standard_meta.egress_port = port;
    }
    
    action drop() {
        standard_meta.drop = 1;
    }
    
    // ========================================================================
    // Tables
    // ========================================================================
    
    table ipv4_lpm {
        key = {
            hdr.ipv4.dstAddr: exact;
        }
        actions = {
            ipv4_forward;
            drop;
            NoAction;
        }
        size = 256;
        default_action = NoAction();
    }
    
    table int_config_transit {
        key = {
            hdr.ipv4.srcAddr: exact;
        }
        actions = {
            enable_int_transit;
            NoAction;
        }
        size = 64;
        default_action = NoAction();
    }
    
    // ========================================================================
    // Apply Block - Main Processing Pipeline
    // ========================================================================
    
    apply {
        // Initialize metadata
        meta.int_node_type = INT_NODE_NONE;
        meta.int_enabled = 0;
        meta.drop_packet = 0;
        
        // Check if IPv4 is valid
        if (hdr.ipv4.isValid()) {
            
            // ================================================================
            // Stage 1: Forwarding
            // ================================================================
            
            ipv4_lpm.apply();
            
            // ================================================================
            // Stage 2: Bloom Filter Tracking
            // ================================================================
            
            // Compute hash for source IP
            compute_bloom_hash();
            
            // Read current counter value
            read_bloom_counter();
            
            // ================================================================
            // Stage 3: INT Decision
            // ================================================================
            
            if (meta.bloom_counter >= REPORT_FREQ) {
                // Counter exceeded threshold - trigger INT
                
                // Reset counter
                reset_bloom_counter();
                
                // Check if INT should be enabled for this flow
                int_config_transit.apply();
                
                if (meta.int_enabled == 1) {
                    // Add INT headers
                    add_int_header();
                }
                
            } else {
                // Counter below threshold - just increment
                increment_bloom_counter();
            }
            
            // ================================================================
            // Stage 4: Update checksums (simplified)
            // ================================================================
            
            hdr.ipv4.hdrChecksum = 0; // Recompute in deparser
        }
    }
}

// ============================================================================
// Deparser
// ============================================================================

control MainDeparser(
    packet_out pkt,
    in main_headers_t hdr,
    in main_metadata_t meta,
    in nc_standard_metadata_t standard_meta
) {
    apply {
        pkt.emit(hdr.ethernet);
        pkt.emit(hdr.ipv4);
        pkt.emit(hdr.tcp);
        pkt.emit(hdr.int_header);
        pkt.emit(hdr.int_switch_id);
    }
}

// ============================================================================
// Main Pipeline Instantiation
// ============================================================================

NC_PIPELINE(
    MainParser(),
    MainControl(),
    MainDeparser()
) main;