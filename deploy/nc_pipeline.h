/* nc_pipeline.h — Generated for: shared_counter
 * Block → hardware mapping produced by nutcracker.
 * nc_hw_target_t is defined in nc_runtime.h before this file is included.
 */
#pragma once

#define NC_NUM_BLOCKS     4    /* blocks 0-3, all DRMT */
#define NC_FIRST_BLOCK    0
#define NC_NUM_ARM_QUEUES 0    /* no ARM blocks */

static const nc_hw_target_t nc_block_hw[NC_NUM_BLOCKS] = {
    NC_HW_DRMT,   /* block 0 */
    NC_HW_DRMT,   /* block 1 */
    NC_HW_DRMT,   /* block 2 */
    NC_HW_DRMT,   /* block 3 */
};

/* ARM queue → entrypoint block mapping (empty: no ARM blocks) */
static const int nc_arm_queue_to_block[1] = { -1 };
