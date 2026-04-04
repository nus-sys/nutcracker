/* nc_arm_worker.c — DPDK worker thread for ARM-mapped blocks.
 *
 * Each worker core runs nc_arm_worker_loop(), which:
 *   1. Dequeues packets from its dedicated RSS queue (one per ARM entrypoint).
 *   2. Looks up the entrypoint block via nc_arm_queue_to_block[g_queue_id].
 *   3. Calls nc_arm_dispatch[block](headers, metadata, std_meta).
 *   4. Chains consecutive ARM blocks directly via return value.
 *   5. Forwards to egress when the chain returns -1.
 *
 * pkt_meta is NOT used for dispatch — it carries DRMT-computed intermediate
 * results, readable via nc_meta_get(m) inside block handler functions.
 */
#include <stdint.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_flow.h>
#include <doca_log.h>

#include "nc_runtime.h"

DOCA_LOG_REGISTER(NC_ARM_WORKER);

#define NC_ARM_BURST_SIZE  64

/* Shutdown flag shared with nc_runtime_main.c */
extern volatile int g_keep_running;

/* Extract nc_types.h pointers from raw packet buffer.
 *
 * Layout assumed: [ main_headers_t ][ main_metadata_t ][ nc_standard_metadata_t ]
 */
#include "nc_types.h"
static inline void nc_pkt_unpack(uint8_t *data,
                                  void **hdr, void **meta, void **std_meta) {
    *hdr      = data;
    *meta     = data + sizeof(main_headers_t);
    *std_meta = data + sizeof(main_headers_t) + sizeof(main_metadata_t);
}

/* Main ARM worker loop — runs on a dedicated DPDK lcore. */
int nc_arm_worker_loop(void *arg) {
    const struct nc_arm_worker_arg *warg = (const struct nc_arm_worker_arg *)arg;
    uint16_t port_id  = warg->port_id;
    uint16_t queue_id = warg->queue_id;
    struct rte_mbuf *pkts[NC_ARM_BURST_SIZE];
    uint16_t nb_rx;

    /* Look up this queue's entrypoint block ID once at startup. */
    int start_block = -1;
    if (queue_id < NC_NUM_ARM_QUEUES)
        start_block = nc_arm_queue_to_block[queue_id];

    if (start_block < 0 || start_block >= NC_NUM_BLOCKS) {
        DOCA_LOG_ERR("ARM worker queue %u: no valid entrypoint block", queue_id);
        return -1;
    }

    DOCA_LOG_INFO("ARM worker started: lcore %u, port %u, queue %u, block %d",
                  rte_lcore_id(), port_id, queue_id, start_block);

    while (g_keep_running) {
        nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, NC_ARM_BURST_SIZE);
        if (nb_rx == 0) continue;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = pkts[i];

            uint8_t *pkt_data = rte_pktmbuf_mtod(m, uint8_t *);
            void *hdr, *meta, *std_meta;
            nc_pkt_unpack(pkt_data, &hdr, &meta, &std_meta);

            /* Run this block then chain any consecutive ARM blocks directly. */
            int next = start_block;
            while (next >= 0 && next < NC_NUM_BLOCKS &&
                   nc_block_hw[next] == NC_HW_ARM &&
                   nc_arm_dispatch[next] != NULL) {
                next = nc_arm_dispatch[next](hdr, meta, std_meta);
            }

            /* Forward to egress port. */
            uint16_t egress = port_id ^ 1;
            uint16_t sent = rte_eth_tx_burst(egress, queue_id, &m, 1);
            if (sent == 0) rte_pktmbuf_free(m);
        }
    }
    return 0;
}
