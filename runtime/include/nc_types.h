/* nc_types.h — TEMPLATE (overridden by generated deploy/nc_types.h)
 * Provides stub packet header/metadata types for building the runtime.
 * The deploy/ project overrides this with actual P4-derived types.
 */
#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t _placeholder;
} main_headers_t;

typedef struct __attribute__((packed)) {
    uint8_t _placeholder;
} main_metadata_t;

typedef struct __attribute__((packed)) {
    uint32_t ingress_port;
    uint32_t egress_port;
    uint32_t drop;
} nc_standard_metadata_t;
