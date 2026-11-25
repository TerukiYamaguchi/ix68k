#pragma once

#include "bus/x68k_bus.h"

typedef enum {
    CHECKPOINT_OK = 0,
    CHECKPOINT_NOCPU,
    CHECKPOINT_RUNNING,
    CHECKPOINT_NO_SLOT,
    CHECKPOINT_SLOT_INVALID,
    CHECKPOINT_MEDIA_MISMATCH,
    CHECKPOINT_CPU_CLOCK_MISMATCH,
    CHECKPOINT_CONFIG_MISMATCH,
    CHECKPOINT_EXTERNAL_RESOURCE,
    CHECKPOINT_FD0_OVERRIDE,
    CHECKPOINT_OOM
} checkpoint_result_t;
typedef enum {
    CHECKPOINT_SLOT_EMPTY,
    CHECKPOINT_SLOT_VALID,
    CHECKPOINT_SLOT_INVALID_STATE
} checkpoint_slot_state_t;
typedef struct {
    int (*serial_active)(void);
    int (*auto_input_pending)(void);
    int (*fd0_override_active)(void);
    void (*host_cleanup)(void);
} checkpoint_env_t;
void checkpoint_init(const checkpoint_env_t *env);
checkpoint_result_t checkpoint_save(x68k_bus_t *bus);
checkpoint_result_t checkpoint_load(x68k_bus_t *bus);
checkpoint_slot_state_t checkpoint_slot_state(const x68k_bus_t *bus, const char **reason_out);
const char *checkpoint_result_reason(checkpoint_result_t result);
/* 直近の checkpoint_save/load が EXTERNAL_RESOURCE で失敗した原因コード。無ければ NULL。 */
const char *checkpoint_result_detail(checkpoint_result_t result);
