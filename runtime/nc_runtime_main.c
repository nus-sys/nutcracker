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
volatile int g_keep_running = 1;   /* extern'd by nc_arm_worker.c */
static void sig_handler(int sig) { (void)sig; g_keep_running = 0; }

/* ── Configuration ────────────────────────────────────────────────────────── */
static char g_device[64] = "mlx5_0";
static int  g_nb_queues  = 1;

/* ── External declarations ────────────────────────────────────────────────── */
/* From nc_doca_flow.c */
extern doca_error_t nc_doca_flow_init(struct doca_flow_port *port,
                                       int nb_queues,
                                       struct doca_flow_pipe **drmt_pipes);
extern void nc_doca_flow_set_peer_port(struct doca_flow_port *port1);
extern void nc_doca_flow_destroy(void);

/* From nc_flexio_host.c */
extern struct flexio_rq *nc_flexio_init(const char *dev);
extern int  nc_flexio_start(void);
extern void nc_flexio_destroy(void);
extern struct flexio_rq *nc_flexio_get_rq(void);

/* From nc_arm_worker.c */
extern int  nc_arm_worker_loop(void *arg);

/* From deploy/doca_flow_pipeline.c (generated) */
struct entries_status { _Bool failure; int nb_processed; };
extern doca_error_t nc_setup_pipeline(struct doca_flow_port *port, int port_id,
                                       struct entries_status *status);

/* ARM dispatch table (populated by arm_handler.c) */
nc_arm_block_fn nc_arm_dispatch[NC_NUM_BLOCKS];

/* ── DOCA Flow entry-process callback ────────────────────────────────────── */
static void nc_entry_process_cb(struct doca_flow_pipe_entry *entry,
                                uint16_t pipe_queue,
                                enum doca_flow_entry_status status,
                                enum doca_flow_entry_op op,
                                void *user_ctx)
{
    (void)entry; (void)pipe_queue; (void)op;
    struct entries_status *s = (struct entries_status *)user_ctx;
    if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
        s->failure = 1;
    s->nb_processed++;
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static int nc_has_hw(nc_hw_target_t hw) {
    for (int i = 0; i < NC_NUM_BLOCKS; i++)
        if (nc_block_hw[i] == hw) return 1;
    return 0;
}

/* ── DPDK port setup ──────────────────────────────────────────────────────── */
#define NC_RX_RING_SIZE 1024
#define NC_TX_RING_SIZE 1024
#define NC_NUM_MBUFS    8192
#define NC_MBUF_CACHE   256

static struct rte_mempool *g_mbuf_pool = NULL;

static int nc_dpdk_ports_init(uint16_t nb_queues)
{
    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        DOCA_LOG_ERR("No DPDK ports found");
        return -1;
    }

    g_mbuf_pool = rte_pktmbuf_pool_create("NC_MBUF_POOL",
        NC_NUM_MBUFS * nb_ports, NC_MBUF_CACHE, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!g_mbuf_pool) {
        DOCA_LOG_ERR("Failed to create mbuf pool: %s", rte_strerror(rte_errno));
        return -1;
    }

    /* Minimal port conf — DOCA Flow HWS manages all steering internally */
    struct rte_eth_conf port_conf = {
        .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
    };

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info dev_info;
        rte_eth_dev_info_get(port_id, &dev_info);

        if (rte_eth_dev_configure(port_id, nb_queues, nb_queues, &port_conf) != 0) {
            DOCA_LOG_ERR("rte_eth_dev_configure failed on port %u", port_id);
            return -1;
        }
        for (uint16_t q = 0; q < nb_queues; q++) {
            if (rte_eth_rx_queue_setup(port_id, q, NC_RX_RING_SIZE,
                    rte_eth_dev_socket_id(port_id), NULL, g_mbuf_pool) < 0) {
                DOCA_LOG_ERR("rx_queue_setup failed: port %u q %u", port_id, q);
                return -1;
            }
            if (rte_eth_tx_queue_setup(port_id, q, NC_TX_RING_SIZE,
                    rte_eth_dev_socket_id(port_id), NULL) < 0) {
                DOCA_LOG_ERR("tx_queue_setup failed: port %u q %u", port_id, q);
                return -1;
            }
        }
        if (rte_eth_dev_start(port_id) < 0) {
            DOCA_LOG_ERR("rte_eth_dev_start failed on port %u", port_id);
            return -1;
        }
        DOCA_LOG_INFO("DPDK port %u started (%u RX/TX queues)", port_id, nb_queues);
    }
    return 0;
}

static void nc_dpdk_ports_fini(void)
{
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Enable DOCA logging (app + SDK) to stderr before anything else */
    struct doca_log_backend *log_backend;
    doca_log_backend_create_with_fd(2, &log_backend);
    doca_log_backend_set_sdk_level(log_backend, DOCA_LOG_LEVEL_DEBUG);
    doca_log_level_set_global_lower_limit(DOCA_LOG_LEVEL_DEBUG);

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

    /* ── DPDK port init ─────────────────────────────────────────────────────── */
    if (nc_dpdk_ports_init((uint16_t)g_nb_queues) != 0) {
        DOCA_LOG_ERR("DPDK port init failed");
        rte_eal_cleanup();
        return 1;
    }

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
        doca_flow_cfg_set_nr_counters(flow_cfg, 4096) != DOCA_SUCCESS ||
        doca_flow_cfg_set_cb_entry_process(flow_cfg, nc_entry_process_cb) != DOCA_SUCCESS ||
        doca_flow_init(flow_cfg) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init DOCA Flow");
        if (flow_cfg) doca_flow_cfg_destroy(flow_cfg);
        goto cleanup;
    }
    doca_flow_cfg_destroy(flow_cfg);

    /* Open both DPDK ports with DOCA Flow and pair them for VNF forwarding */
    struct doca_flow_port *ports[2] = {NULL, NULL};
    for (int pid = 0; pid < 2; pid++) {
        struct doca_flow_port_cfg *port_cfg = NULL;
        char port_id_str[8];
        snprintf(port_id_str, sizeof(port_id_str), "%d", pid);
        if (doca_flow_port_cfg_create(&port_cfg) != DOCA_SUCCESS ||
            doca_flow_port_cfg_set_devargs(port_cfg, port_id_str) != DOCA_SUCCESS ||
            doca_flow_port_start(port_cfg, &ports[pid]) != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to start DOCA Flow port %d", pid);
            if (port_cfg) doca_flow_port_cfg_destroy(port_cfg);
            goto cleanup;
        }
        doca_flow_port_cfg_destroy(port_cfg);
    }
    if (doca_flow_port_pair(ports[0], ports[1]) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to pair DOCA Flow ports");
        goto cleanup;
    }
    struct doca_flow_port *port = ports[0];
    nc_doca_flow_set_peer_port(ports[1]);

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
        /* One static arg struct per queue: port/queue passed via launch arg
         * so each worker reads from its own lcore stack, not shared TLS. */
        static struct nc_arm_worker_arg arm_args[NC_NUM_ARM_QUEUES > 0 ? NC_NUM_ARM_QUEUES : 1];
        uint16_t queue = 0;
        unsigned lcore;
        RTE_LCORE_FOREACH_WORKER(lcore) {
            if (queue >= NC_NUM_ARM_QUEUES) break;
            arm_args[queue].port_id  = 0;
            arm_args[queue].queue_id = queue;
            rte_eal_remote_launch(nc_arm_worker_loop, &arm_args[queue], lcore);
            queue++;
        }
        DOCA_LOG_INFO("ARM workers launched (%u queues)", (unsigned)NC_NUM_ARM_QUEUES);
    }

    DOCA_LOG_INFO("nutcracker runtime running (Ctrl-C to stop)");
    while (g_keep_running) rte_delay_ms(100);
    rte_eal_mp_wait_lcore();   /* wait for ARM workers to exit cleanly */

cleanup:
    nc_doca_flow_destroy();
    if (nc_has_hw(NC_HW_DPA)) nc_flexio_destroy();
    nc_dpdk_ports_fini();
    rte_eal_cleanup();
    return 0;
}
