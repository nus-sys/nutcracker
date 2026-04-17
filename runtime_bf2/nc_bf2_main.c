/* nc_bf2_main.c — nutcracker BF2 pipeline runtime (DOCA 2.2).
 *
 * Mirrors the NF-testkit/doca2.2 init pattern:
 *   doca_argp → dpdk_queues_and_ports_init → init_doca_flow → init_doca_flow_ports
 *   → nc_setup_pipeline (generated) → doca_flow_entries_process → idle loop.
 *
 * Usage: sudo ./nc_pipeline -l 0-3 -n 4 -a 03:00.0,dv_flow_en=2 -a 03:00.1,dv_flow_en=2
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <dpdk_utils.h>

#include "flow_common.h"

DOCA_LOG_REGISTER(NC_BF2_PIPELINE);

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

extern doca_error_t nc_setup_pipeline(struct doca_flow_port *port, int port_id,
                                      struct entries_status *status);

int
main(int argc, char **argv)
{
    doca_error_t result;
    int exit_status = EXIT_FAILURE;
    int nb_ports = 2;
    struct doca_flow_port *ports[2] = {NULL, NULL};
    struct application_dpdk_config dpdk_config = {
        .port_config.nb_ports    = nb_ports,
        .port_config.nb_queues   = 1,
        .port_config.nb_hairpin_q = 2,
        .sft_config = {0},
    };

    result = doca_log_create_standard_backend();
    if (result != DOCA_SUCCESS)
        goto out;

    result = doca_argp_init("nc_pipeline", NULL);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init ARGP: %s", doca_get_error_string(result));
        goto out;
    }
    doca_argp_set_dpdk_program(dpdk_init);
    result = doca_argp_start(argc, argv);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start ARGP: %s", doca_get_error_string(result));
        goto argp_cleanup;
    }

    result = dpdk_queues_and_ports_init(&dpdk_config);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("DPDK queues/ports init failed");
        goto dpdk_cleanup;
    }

    struct doca_flow_resources resource = { .nb_counters = 4096 };
    uint32_t nr_shared[DOCA_FLOW_SHARED_RESOURCE_MAX] = {0};
    result = init_doca_flow(dpdk_config.port_config.nb_queues, "vnf,hws",
                            resource, nr_shared);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow: %s", doca_get_error_string(result));
        goto ports_queues_cleanup;
    }

    result = init_doca_flow_ports(nb_ports, ports, true);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("init_doca_flow_ports: %s", doca_get_error_string(result));
        goto flow_cleanup;
    }

    struct entries_status status = {0};
    result = nc_setup_pipeline(ports[0], 0, &status);
    if (result != DOCA_SUCCESS || status.failure) {
        DOCA_LOG_ERR("nc_setup_pipeline: %s (processed=%d, failure=%d)",
                     doca_get_error_string(result), status.nb_processed, status.failure);
        goto ports_stop;
    }
    if (status.nb_processed > 0) {
        result = doca_flow_entries_process(ports[0], 0, DEFAULT_TIMEOUT_US,
                                          status.nb_processed);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("entries_process: %s", doca_get_error_string(result));
            goto ports_stop;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    DOCA_LOG_INFO("nutcracker BF2 pipeline running (Ctrl-C to stop)");
    while (g_running)
        sleep(1);

    exit_status = EXIT_SUCCESS;

ports_stop:
    stop_doca_flow_ports(nb_ports, ports);
flow_cleanup:
    doca_flow_destroy();
ports_queues_cleanup:
    dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
    dpdk_fini();
argp_cleanup:
    doca_argp_destroy();
out:
    if (exit_status == EXIT_SUCCESS)
        DOCA_LOG_INFO("Pipeline shut down cleanly");
    else
        DOCA_LOG_ERR("Pipeline exited with errors");
    return exit_status;
}
