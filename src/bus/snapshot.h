#pragma once

#include "bus/x68k_bus.h"

typedef struct x68k_bus_snapshot x68k_bus_snapshot_t;
typedef struct x68k_bus_restore_plan x68k_bus_restore_plan_t;

typedef enum {
    X68K_SNAPSHOT_OK = 0,
    X68K_SNAPSHOT_NOMEM,
    X68K_SNAPSHOT_EXTERNAL_RESOURCE,
    X68K_SNAPSHOT_MEDIA_MISMATCH,
    X68K_SNAPSHOT_CPU_CLOCK_MISMATCH,
    X68K_SNAPSHOT_CONFIG_MISMATCH,
} x68k_snapshot_status_t;

/* Snapshot を妨げる外部資源の理由コード。妨げが無ければ NULL（静的文字列）。 */
const char *x68k_bus_snapshot_external_resource_detail(const x68k_bus_t *bus);
x68k_bus_snapshot_t *x68k_bus_snapshot_create(const x68k_bus_t *bus,
                                              x68k_snapshot_status_t *status);
x68k_bus_restore_plan_t *x68k_bus_snapshot_prepare(const x68k_bus_t *bus,
                                                   const x68k_bus_snapshot_t *snap,
                                                   x68k_snapshot_status_t *status);
void x68k_bus_snapshot_commit(x68k_bus_t *bus, const x68k_bus_snapshot_t *snap,
                              x68k_bus_restore_plan_t *plan);
void x68k_bus_restore_plan_destroy(x68k_bus_restore_plan_t *plan);
void x68k_bus_snapshot_destroy(x68k_bus_snapshot_t *snap);
x68k_media_generation_t x68k_bus_snapshot_media_generation(const x68k_bus_snapshot_t *snap);
uint32_t x68k_bus_snapshot_cpu_hz(const x68k_bus_snapshot_t *snap);
