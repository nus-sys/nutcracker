/* nc_dpa_stub.c — Stub DPA symbols for all-DRMT deployments.
 *
 * When no blocks are mapped to the DPA (NC_NUM_ARM_QUEUES == 0 and no
 * NC_HW_DPA entries in nc_block_hw[]), nc_flexio_host.c is still compiled
 * in but nc_flexio_init() is never called at runtime (guarded by
 * nc_has_hw(NC_HW_DPA) in nc_runtime_main.c).
 *
 * The linker still needs the DPACC-generated symbols. Provide stubs so the
 * binary links without a .dpa device program.
 */
#include <stddef.h>

/* DPACC-generated symbols — stubbed when no DPA blocks are used */
void *nc_dpa_app = NULL;
void  nc_dpa_event_handler(void) {}
