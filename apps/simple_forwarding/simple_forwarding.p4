// simple_forwarding.p4 - Minimal example for testing the partition pass

#include <core.p4>
#include "nc.p4"

header ipv4_t {
    bit<8>  protocol;
    bit<32> src_addr;
}

struct main_headers_t {
    ipv4_t ipv4;
}

struct main_metadata_t {
    bit<32> temp;
}

parser MainParser(
    packet_in pkt,
    out main_headers_t hdr,
    inout main_metadata_t meta,
    in nc_standard_metadata_t standard_meta
) {
    state start {
        pkt.extract(hdr.ipv4);
        transition accept;
    }
}

control MainControl(
    inout main_headers_t hdr,
    inout main_metadata_t meta,
    inout nc_standard_metadata_t standard_meta
) {
    apply {
        if (hdr.ipv4.protocol == 4) {
            meta.temp = hdr.ipv4.src_addr & 0xFF;
        } else {
            meta.temp = 0;
        }
    }
}

control MainDeparser(
    packet_out pkt,
    in main_headers_t hdr,
    in main_metadata_t meta,
    in nc_standard_metadata_t standard_meta
) {
    apply {
        pkt.emit(hdr.ipv4);
    }
}

NC_PIPELINE(
    MainParser(),
    MainControl(),
    MainDeparser()
) main;