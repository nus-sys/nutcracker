// meter.p4 — Traffic policing using meter + hash-based flow index
//
// Each arriving IPv4/TCP packet is:
//   1. Hashed to a flow index via Hash5Tuple
//   2. Checked against a per-flow packet-rate meter
//   3. Dropped if the meter returns RED (color==2)

#include <core.p4>
#include "nc.p4"

// ============================================================================
// Headers
// ============================================================================

header ipv4_t {
    bit<4>  version;
    bit<4>  ihl;
    bit<8>  diffserv;
    bit<16> total_len;
    bit<16> identification;
    bit<3>  flags;
    bit<13> frag_offset;
    bit<8>  ttl;
    bit<8>  protocol;
    bit<16> hdr_checksum;
    bit<32> src_addr;
    bit<32> dst_addr;
}

header tcp_t {
    bit<16> src_port;
    bit<16> dst_port;
    bit<32> seq_no;
    bit<32> ack_no;
    bit<4>  data_offset;
    bit<3>  res;
    bit<3>  ecn;
    bit<1>  urg;
    bit<1>  ack;
    bit<1>  psh;
    bit<1>  rst;
    bit<1>  syn;
    bit<1>  fin;
    bit<16> window;
    bit<16> checksum;
    bit<16> urgent_ptr;
}

struct main_headers_t {
    ipv4_t ipv4;
    tcp_t  tcp;
}

struct main_metadata_t {
    bit<32> flow_id;
    bit<8>  color;   // 0=green, 1=yellow, 2=red
}

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
// Control
// ============================================================================

control MainControl(
    inout main_headers_t hdr,
    inout main_metadata_t meta,
    inout nc_standard_metadata_t standard_meta
) {
    Hash5Tuple() hash_unit;
    meter(1024, 0) flow_meter;    // 1024 slots, packets-based

    // Compute flow ID from 5-tuple
    action compute_flow_id() {
        hash_unit.apply(
            meta.flow_id,
            hdr.ipv4.src_addr,
            hdr.ipv4.dst_addr,
            hdr.tcp.src_port,
            hdr.tcp.dst_port,
            hdr.ipv4.protocol
        );
    }

    // Simple validity check table (triggers partitioner block split)
    table proto_check {
        key = { hdr.ipv4.protocol: exact; }
        actions = { compute_flow_id; }
        default_action = compute_flow_id;
    }

    apply {
        if (hdr.ipv4.isValid() && hdr.tcp.isValid()) {
            proto_check.apply();
            // Execute per-flow meter
            flow_meter.execute_meter(meta.flow_id, meta.color);
            // Drop if RED
            if (meta.color == 2) {
                standard_meta.drop = 1;
            }
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
        pkt.emit(hdr.ipv4);
        pkt.emit(hdr.tcp);
    }
}

// ============================================================================
// Main Package
// ============================================================================

NC_PIPELINE(
    MainParser(),
    MainControl(),
    MainDeparser()
) main;
