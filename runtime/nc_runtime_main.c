/* nc_runtime_main.c — nutcracker pipeline runtime entry point.
 *
 * Usage: nc_runtime [EAL options] -- --device mlx5_0 [--queues N]
 *
 * The DPA binary is loaded via the DPACC-generated nc_dpa_app symbol
 * (linked in at compile time from dpa_dev_entry.c).
 */
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_flow.h>

#include <doca_log.h>
#include <doca_flow.h>

#include "nc_runtime.h"

DOCA_LOG_REGISTER(NC_RUNTIME);

/* ── Signal handling ─────────────────────────────────────────────────────── */
static volatile int g_keep_running = 1;
static void sig_handler(int sig) { (void)sig; g_keep_running = 0; }

/* ── Configuration ────────────────────────────────────────────────────────── */
static char g_device[64] = "mlx5_0";
static int  g_nb_queues  = 1;

/* ── External declarations ────────────────────────────────────────────────── */
/* From nc_doca_flow.c */
extern doca_error_t nc_doca_flow_init(struct doca_flow_port *port,
                                       int nb_queues,
                                       struct doca_flow_pipe **drmt_pipes);
extern void nc_doca_flow_destroy(void);

/* From nc_flexio_host.c */
extern struct flexio_rq *nc_flexio_init(const char *dev);
extern int  nc_flexio_start(void);
extern void nc_flexio_destroy(void);
extern struct flexio_rq *nc_flexio_get_rq(void);

/* From nc_arm_worker.c */
extern int  nc_arm_worker_loop(void *arg);
extern void nc_arm_worker_bind(uint16_t port_id, uint16_t queue_id);

/* From deploy/doca_flow_pipeline.c (generated) */
struct entries_status { _Bool failure; int nb_processed; };
extern doca_error_t nc_setup_pipeline(struct doca_flow_port *port, int port_id,
                                       struct entries_status *status);

/* ARM dispatch table (populated by arm_handler.c) */
nc_arm_block_fn nc_arm_dispatch[NC_NUM_BLOCKS];

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static int nc_has_hw(nc_hw_target_t hw) {
    for (int i = 0; i < NC_NUM_BLOCKS; i++)
        if (nc_block_hw[i] == hw) return 1;
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* DPDK EAL init */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "Failed to init DPDK EAL\n");
        return 1;
    }
    argc -= ret; argv += ret;

    /* Parse nutcracker-specific args */
    for (int i = 0; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--device"))
            snprintf(g_device, sizeof(g_device), "%s", argv[i+1]);
        if (!strcmp(argv[i], "--queues"))
            g_nb_queues = atoi(argv[i+1]);
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    struct doca_flow_pipe *drmt_pipes[NC_NUM_BLOCKS] = {NULL};

    /* ── FlexIO / DPA init ────────────────────────────────────────────────── */
    if (nc_has_hw(NC_HW_DPA)) {
        if (!nc_flexio_init(g_device)) {
            DOCA_LOG_ERR("FlexIO init failed");
            goto cleanup;
        }
    }

    /* ── DOCA Flow init + DRMT pipes ─────────────────────────────────────── */
    struct doca_flow_cfg *flow_cfg = NULL;
    if (doca_flow_cfg_create(&flow_cfg) != DOCA_SUCCESS ||
        doca_flow_cfg_set_pipe_queues(flow_cfg, (uint16_t)g_nb_queues) != DOCA_SUCCESS ||
        doca_flow_cfg_set_mode_args(flow_cfg, "vnf,hws") != DOCA_SUCCESS ||
        doca_flow_init(flow_cfg) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init DOCA Flow");
        if (flow_cfg) doca_flow_cfg_destroy(flow_cfg);
        goto cleanup;
    }
    doca_flow_cfg_destroy(flow_cfg);

    /* Open port 0 — caller must have set up the DPDK port already */
    struct doca_flow_port_cfg *port_cfg = NULL;
    struct doca_flow_port    *port = NULL;
    char port_id_str[8];
    snprintf(port_id_str, sizeof(port_id_str), "%u", 0);
    if (doca_flow_port_cfg_create(&port_cfg) != DOCA_SUCCESS ||
        doca_flow_port_cfg_set_devargs(port_cfg, port_id_str) != DOCA_SUCCESS ||
        doca_flow_port_start(port_cfg, &port) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start DOCA Flow port");
        if (port_cfg) doca_flow_port_cfg_destroy(port_cfg);
        goto cleanup;
    }
    doca_flow_port_cfg_destroy(port_cfg);

    /* ── DRMT pipe setup ──────────────────────────────────────────────────── */
    if (nc_has_hw(NC_HW_DRMT)) {
        struct entries_status drmt_status = {0};
        doca_error_t dr = nc_setup_pipeline(port, 0, &drmt_status);
        if (dr != DOCA_SUCCESS || drmt_status.failure) {
            DOCA_LOG_ERR("DRMT pipe setup failed: %s", doca_error_get_descr(dr));
            goto cleanup;
        }
    }

    /* ── DOCA Flow dispatch pipes ─────────────────────────────────────────── */
    {
        doca_error_t dr = nc_doca_flow_init(port, g_nb_queues, drmt_pipes);
        if (dr != DOCA_SUCCESS) {
            DOCA_LOG_ERR("nc_doca_flow_init failed: %s", doca_error_get_descr(dr));
            goto cleanup;
        }
    }

    /* ── Activate DPA event handler ──────────────────────────────────────── */
    if (nc_has_hw(NC_HW_DPA)) {
        if (nc_flexio_start() != 0) {
            DOCA_LOG_ERR("Failed to start DPA event handler");
            goto cleanup;
        }
    }

    /* ── Launch ARM worker threads ────────────────────────────────────────── */
    if (nc_has_hw(NC_HW_ARM)) {
        /* Launch one worker per ARM entrypoint queue (NC_NUM_ARM_QUEUES total).
         * Each worker is bound to its queue and knows its entrypoint block via
         * nc_arm_queue_to_block[queue_id]. */
        uint16_t queue = 0;
        unsigned lcore;
        RTE_LCORE_FOREACH_WORKER(lcore) {
            if (queue >= NC_NUM_ARM_QUEUES) break;
            nc_arm_worker_bind(0, queue);
            rte_eal_remote_launch(nc_arm_worker_loop, NULL, lcore);
            queue++;
        }
        DOCA_LOG_INFO("ARM workers launched (%u queues)", (unsigned)NC_NUM_ARM_QUEUES);
    }

    DOCA_LOG_INFO("nutcracker runtime running (Ctrl-C to stop)");
    while (g_keep_running) rte_delay_ms(100);

cleanup:
    nc_doca_flow_destroy();
    if (nc_has_hw(NC_HW_DPA)) nc_flexio_destroy();
    rte_eal_cleanup();
    return 0;
}
