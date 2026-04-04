/* nc_runtime.h — nutcracker pipeline runtime (static template)
 *
 * This header is included by both the static runtime and the generated deploy/
 * directory.  It defines the dispatch interface between the generated block
 * functions and the hardware-specific execution paths.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Block → hardware mapping ─────────────────────────────────────────────── */

typedef enum {
    NC_HW_DRMT = 0,   /* NIC match-action (DOCA Flow)  */
    NC_HW_DPA  = 1,   /* Data Path Accelerator (FlexIO) */
    NC_HW_ARM  = 2,   /* ARM/host CPU cores (DPDK)     */
} nc_hw_target_t;

/* Provided by the generated nc_pipeline.h */
#include "nc_pipeline.h"

/* ── ARM worker argument (passed via rte_eal_remote_launch arg) ───────────── */

struct nc_arm_worker_arg {
    uint16_t port_id;
    uint16_t queue_id;
};

/* ── ARM block dispatch table ─────────────────────────────────────────────── */

/* Each ARM block function: returns next block ID (-1 = pipeline done) */
typedef int (*nc_arm_block_fn)(void *headers, void *metadata, void *std_meta);

/* Populated by arm_handler.c; NULL for non-ARM blocks. */
extern nc_arm_block_fn nc_arm_dispatch[NC_NUM_BLOCKS];

/* ── pkt_meta: DRMT→ARM intermediate result channel ──────────────────────── */
/* pkt_meta (32-bit, the only field that survives the NIC→CPU RSS boundary)
 * carries DRMT-computed intermediate results for ARM block handlers to read.
 * ARM dispatch is static (all NC_HW_ARM blocks run in order); no block ID
 * encoding is needed here. */
#include <rte_flow.h>
static inline uint32_t nc_meta_get(struct rte_mbuf *mbuf) {
    if (!rte_flow_dynf_metadata_avail()) return 0;
    return *RTE_FLOW_DYNF_METADATA(mbuf);
}

/* ── Runtime lifecycle ────────────────────────────────────────────────────── */

struct nc_runtime_cfg {
    uint16_t nb_ports;      /* number of DPDK/DOCA ports         */
    uint16_t nb_queues;     /* queues per port                   */
    const char *device;     /* IBV device name for FlexIO        */
    int      nb_arm_cores;  /* ARM worker lcores (0 = auto)      */
};

int  nc_runtime_init(const struct nc_runtime_cfg *cfg);
void nc_runtime_run(void);   /* blocks until signal               */
void nc_runtime_destroy(void);
