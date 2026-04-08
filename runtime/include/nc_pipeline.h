/* nc_pipeline.h — TEMPLATE (overridden by generated deploy/nc_pipeline.h)
 * When building the runtime standalone, this provides default values.
 * The deploy/ project overrides this with the actual generated mapping.
 * nc_hw_target_t is defined in nc_runtime.h before this file is included.
 */
#pragma once

#define NC_NUM_BLOCKS     1
#define NC_FIRST_BLOCK    0
#define NC_NUM_ARM_QUEUES 0

static const nc_hw_target_t nc_block_hw[NC_NUM_BLOCKS] = {
    NC_HW_DRMT,
};

/* ARM queue → entrypoint block mapping (empty when no ARM blocks) */
static const int nc_arm_queue_to_block[1] = { -1 };
