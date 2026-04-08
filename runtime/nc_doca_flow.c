/* nc_doca_flow.c — DOCA Flow lifecycle management.
 *
 * Dispatch is handled directly by the generated doca_flow_pipeline.c:
 *   DRMT → DRMT : DOCA_FLOW_FWD_PIPE to next DRMT pipe
 *   DRMT → ARM  : DOCA_FLOW_FWD_RSS  to ARM entrypoint queue
 *   DRMT → DPA  : DOCA_FLOW_FWD_RSS  to DPA dedicated queue
 *   DRMT → egress: DOCA_FLOW_FWD_PORT
 *
 * This file only manages init/destroy of the DOCA Flow subsystem.
 */
#include <doca_log.h>
#include <doca_flow.h>
#include "nc_runtime.h"

DOCA_LOG_REGISTER(NC_DOCA_FLOW);

static struct doca_flow_port *g_flow_ports[2] = {NULL, NULL};

/*
 * nc_doca_flow_init — saves the primary port handle for cleanup; actual pipe
 * creation is done by the generated nc_setup_pipeline() in doca_flow_pipeline.c.
 */
doca_error_t nc_doca_flow_init(struct doca_flow_port *port,
                               int nb_queues,
                               struct doca_flow_pipe **drmt_pipes)
{
    (void)nb_queues;
    (void)drmt_pipes;
    g_flow_ports[0] = port;
    DOCA_LOG_INFO("nc_doca_flow_init: dispatch handled by generated pipeline");
    return DOCA_SUCCESS;
}

/* Called once the peer port has been started and paired. */
void nc_doca_flow_set_peer_port(struct doca_flow_port *port1)
{
    g_flow_ports[1] = port1;
}

void nc_doca_flow_destroy(void)
{
    for (int i = 0; i < 2; i++) {
        if (g_flow_ports[i]) {
            doca_flow_port_stop(g_flow_ports[i]);
            g_flow_ports[i] = NULL;
        }
    }
    doca_flow_destroy();
}
