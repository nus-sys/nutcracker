// shared_counter.p4 — Tests cross-block VFFA sharing classification
//
// Counter/meter accessed via direct action calls in the apply block,
// separated by table applies (which create block boundaries).
//
// flow_counter: called with srcAddr in block A, then srcAddr again in block B
//               → same key both times → SharedPartitionable
//
// asym_counter: called with srcAddr in block A, then dstAddr in block B
//               → different keys → SharedNonPartitionable
//
// Expected memory_analysis.json:
//   flow_counter  → shared_partitionable,     shardingKeyConsistent=true
//   asym_counter  → shared_non_partitionable, shardingKeyConsistent=false

#include <core.p4>
#include "nc.p4"

typedef bit<48> macAddr_t;
typedef bit<32> ip4Addr_t;

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

struct headers_t {
    ethernet_t ethernet;
    ipv4_t     ipv4;
}

struct metadata_t {
    bit<9> egress_port;
}

parser MainParser(
    packet_in pkt,
    out headers_t hdr,
    inout metadata_t meta,
    in nc_standard_metadata_t std_meta
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
        transition accept;
    }
}

control MainControl(
    inout headers_t hdr,
    inout metadata_t meta,
    inout nc_standard_metadata_t std_meta
) {
    // Accessed twice with the same key (srcAddr) → SharedPartitionable
    Counter<bit<32>>(1024) flow_counter;

    // Accessed with srcAddr then dstAddr → SharedNonPartitionable
    Counter<bit<32>>(1024) asym_counter;

    // Direct action calls that embed the VFFA accesses.
    // These are called from apply{} directly (not via table_apply),
    // so the p4hir-to-vdrmt pass inlines them into the vdrmt block.

    action count_stage1() {
        // Both counters indexed by srcAddr
        flow_counter.count(hdr.ipv4.srcAddr);
        asym_counter.count(hdr.ipv4.srcAddr);
    }

    action count_stage2() {
        // flow_counter: same key (srcAddr) → consistent
        flow_counter.count(hdr.ipv4.srcAddr);
        // asym_counter: different key (dstAddr) → inconsistent
        asym_counter.count(hdr.ipv4.dstAddr);
    }

    // Forwarding table (creates the block boundary between stage1 and stage2)
    action ipv4_forward(bit<9> port) {
        std_meta.egress_port = port;
        hdr.ipv4.ttl = hdr.ipv4.ttl - 1;
    }

    action drop() {
        std_meta.drop = 1;
    }

    table ipv4_lpm {
        key = { hdr.ipv4.dstAddr: exact; }
        actions = { ipv4_forward; drop; NoAction; }
        size = 256;
        default_action = NoAction();
    }

    apply {
        if (hdr.ipv4.isValid()) {
            // Stage 1: count before forwarding table
            count_stage1();

            // Table apply creates a block boundary
            ipv4_lpm.apply();

            // Stage 2: count again after forwarding table (different block)
            count_stage2();
        }
    }
}

control MainDeparser(
    packet_out pkt,
    in headers_t hdr,
    in metadata_t meta,
    in nc_standard_metadata_t std_meta
) {
    apply {
        pkt.emit(hdr.ethernet);
        pkt.emit(hdr.ipv4);
    }
}

NC_PIPELINE(MainParser(), MainControl(), MainDeparser()) main;
