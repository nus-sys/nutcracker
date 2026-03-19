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

/*
 * nc_doca_flow_init — no-op after DOCA Flow and port are started by the
 * caller (nc_runtime_main.c).  The generated doca_flow_pipeline.c creates
 * all pipes via nc_setup_pipeline().
 */
doca_error_t nc_doca_flow_init(struct doca_flow_port *port,
                               int nb_queues,
                               struct doca_flow_pipe **drmt_pipes)
{
    (void)port;
    (void)nb_queues;
    (void)drmt_pipes;
    DOCA_LOG_INFO("nc_doca_flow_init: dispatch handled by generated pipeline");
    return DOCA_SUCCESS;
}

void nc_doca_flow_destroy(void)
{
    doca_flow_destroy();
}
