// weighted_avg.p4 — Per-flow RTT smoothing with metering
//
// Each IPv4/TCP packet:
//   1. Hashed to a flow_id via Hash5Tuple
//   2. Matched in classify_flow by srcAddr; control-plane assigns an id (slot)
//   3. Action calculate_and_meter:
//        curr_rtt  = (new_rtt + 3 * curr_rtt) / 4   -- EWMA alpha=0.25
//        writes smoothed RTT back to rtt_store
//        executes per-slot traffic meter
//   4. Drops RED-metered flows

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
    // TCP Timestamp option fields (carried inline for simplicity).
    // In production these would be parsed from the options region.
    bit<32> ts_val;   // sender's timestamp (used as new_rtt proxy)
    bit<32> ts_ecr;   // echo reply timestamp
}

struct main_headers_t {
    ipv4_t ipv4;
    tcp_t  tcp;
}

struct main_metadata_t {
    bit<32> flow_id;   // hash-based index (also used as default slot)
    bit<32> new_rtt;   // latest RTT sample = ts_val - ts_ecr
    bit<32> curr_rtt;  // EWMA-smoothed RTT (read from / written to rtt_store)
    bit<8>  color;     // meter output: 0=GREEN 1=YELLOW 2=RED
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
    Hash5Tuple()          hash_unit;
    Register<bit<32>>(4096) rtt_store;   // per-slot smoothed RTT
    meter(4096, 0)        flow_meter;    // per-slot packet-rate meter

    // Compute a 5-tuple hash into flow_id so the default action has a slot.
    action compute_flow_id() {
        hash_unit.apply(
            meta.flow_id,
            hdr.ipv4.src_addr,
            hdr.ipv4.dst_addr,
            hdr.tcp.src_port,
            hdr.tcp.dst_port,
            hdr.ipv4.protocol
        );
        // RTT sample = difference of TCP timestamps (sender minus echo).
        meta.new_rtt = hdr.tcp.ts_val - hdr.tcp.ts_ecr;
    }

    // EWMA update + metering.
    // id is assigned by the control plane per-entry (maps srcAddr → slot).
    action calculate_and_meter(bit<32> id) {
        meta.curr_rtt = rtt_store.read(id);
        meta.curr_rtt = (meta.new_rtt + (3 * meta.curr_rtt)) / 4;
        rtt_store.write(id, meta.curr_rtt);
        flow_meter.execute_meter(id, meta.color);
    }

    // Classify by source address; control plane populates entries with slot ids.
    table classify_flow {
        key = { hdr.ipv4.src_addr: exact; }
        actions = { calculate_and_meter; NoAction; }
        default_action = NoAction;
    }

    // Hash-based fallback: use flow_id as the slot when no table entry matches.
    table hash_classify {
        key = { hdr.ipv4.protocol: exact; }
        actions = { compute_flow_id; }
        default_action = compute_flow_id;
    }

    apply {
        if (hdr.ipv4.isValid() && hdr.tcp.isValid()) {
            // Step 1: compute flow_id + new_rtt from packet fields.
            hash_classify.apply();

            // Step 2: look up srcAddr; if matched, run EWMA + meter.
            if (!classify_flow.apply().hit) {
                // No per-source entry: run EWMA + meter on the hash slot.
                meta.curr_rtt = rtt_store.read(meta.flow_id);
                meta.curr_rtt = (meta.new_rtt + (3 * meta.curr_rtt)) / 4;
                rtt_store.write(meta.flow_id, meta.curr_rtt);
                flow_meter.execute_meter(meta.flow_id, meta.color);
            }

            // Step 3: drop RED flows.
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
