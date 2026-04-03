#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

struct ethernet {
    uint8_t dstAddr_0;
    uint8_t dstAddr_1;
    uint8_t dstAddr_2;
    uint8_t dstAddr_3;
    uint8_t dstAddr_4;
    uint8_t dstAddr_5;
    uint8_t srcAddr_0;
    uint8_t srcAddr_1;
    uint8_t srcAddr_2;
    uint8_t srcAddr_3;
    uint8_t srcAddr_4;
    uint8_t srcAddr_5;
    uint16_t etherType;
};

struct ipv4 {
	uint8_t 	ihl;
	uint8_t		version;
    uint8_t	    tos;
	uint8_t	    totalLen;
	uint8_t	    identification;
	uint8_t     frag;
	uint8_t	    ttl;
	uint8_t     protocol;
	uint16_t	hdrChecksum;
	uint32_t	srcAddr;
	uint32_t    dstAddr;
	uint32_t    reserved1;
	uint32_t    reserved2;
};

struct tcp {
	uint16_t	srcPort;
	uint16_t	dstPort;
	uint32_t	seqNo;
	uint32_t	ackNo;
	uint8_t	    dataOff;
	uint8_t	    flags;
	uint16_t	window;
	uint16_t	checksum;
	uint16_t	urgentPtr;
	uint32_t    reserved1;
	uint32_t    reserved2;
	uint32_t    reserved3;
	uint32_t    reserved4;
};

struct packet {
    struct ethernet ethernet;
    struct ipv4 ipv4;
    struct tcp tcp;
	uint32_t    reserved1;
	uint32_t    reserved2;
	uint32_t    reserved3;
	uint32_t    reserved4;
	uint32_t    reserved5;
	uint32_t    reserved6;
	uint32_t    reserved7;
	uint32_t    reserved8;
	uint32_t    reserved9;
	uint32_t    reserved10;
};

enum {
    DROP = -1,
};

#define IPV4_ADDR(a, b, c, d) \
    (htonl(((a) & 0xff) << 24 | ((b) & 0xff) << 16 | ((c) & 0xff) << 8 | ((d) & 0xff)))

int __attribute__((section(".entry"))) process_inbound(struct packet * pkt) {
    struct ipv4 *ipv4 = &pkt->ipv4;
    struct tcp *tcp = &pkt->tcp;
    
    uint32_t dst_addr = ipv4->dstAddr;
    uint16_t dst_port = tcp->dstPort;
    uint8_t protocol = ipv4->protocol;

    if (dst_addr == IPV4_ADDR(192, 168, 1, 1)) {
        if (protocol == 6 && (dst_port == htons(443) || dst_port == htons(80))) {
            ipv4->dstAddr = IPV4_ADDR(10, 0, 0, 10);
        }
    }
    
    // Update Ethernet headers for internal network
    pkt->ethernet.srcAddr_0 = 0x3c;
	pkt->ethernet.srcAddr_1 = 0xfd;
	pkt->ethernet.srcAddr_2 = 0xfe;
	pkt->ethernet.srcAddr_3 = 0x9e;
	pkt->ethernet.srcAddr_4 = 0x7d;
	pkt->ethernet.srcAddr_5 = 0x21;

    pkt->ethernet.dstAddr_0 = 0x3c;
	pkt->ethernet.dstAddr_1 = 0xfd;
	pkt->ethernet.dstAddr_2 = 0xfe;
	pkt->ethernet.dstAddr_3 = 0x9e;
	pkt->ethernet.dstAddr_4 = 0x5d;
	pkt->ethernet.dstAddr_5 = 0x01;

    pkt->ethernet.etherType = htons(0x0800);

    return 0;
}

int main() {
    process_inbound(NULL);
    return 0;
}