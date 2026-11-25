#include "app/checkpoint.h"
#include "app_internal.h"
#include "bus/snapshot.h"
#include "mcp/debugger.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    app_audio_state_t state;
    int fifo_len;
    bool fifo_valid;
    int32_t *fifo;
} app_audio_snapshot_t;
typedef struct {
    bool used;
    mc68k_snapshot_t *cpu;
    x68k_bus_snapshot_t *bus;
    app_audio_snapshot_t *audio;
} checkpoint_slot_t;
static checkpoint_slot_t s_slot;
static checkpoint_env_t s_env;

static void audio_destroy(app_audio_snapshot_t *snap) {
    if (snap) {
        free(snap->fifo);
        free(snap);
    }
}
static app_audio_snapshot_t *audio_create(void) {
    if (s_app.audio.adpcm_fifo_len < 0)
        return NULL;
    app_audio_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap)
        return NULL;
    snap->state = s_app.audio;
    snap->fifo_len = s_app.audio.adpcm_fifo_len;
    snap->fifo_valid = snap->fifo_len <= (int)s_app.audio_buffers.fifo_frames;
    /* 壊れた有効長でもsaveは失敗させない。loadのpreflightで拒否するが、
     * 保存時には割当て済みFIFOの範囲外を読まない。 */
    if (!snap->fifo_valid)
        return snap;
    if (snap->fifo_len > 0) {
        snap->fifo = malloc(sizeof(*snap->fifo) * (size_t)snap->fifo_len * 2u);
        if (!snap->fifo) {
            audio_destroy(snap);
            return NULL;
        }
        memcpy(snap->fifo, s_app.audio_buffers.adpcm_fifo,
               sizeof(*snap->fifo) * (size_t)snap->fifo_len * 2u);
    }
    return snap;
}
static void slot_destroy(checkpoint_slot_t *slot) {
    mc68k_snapshot_destroy(slot->cpu);
    x68k_bus_snapshot_destroy(slot->bus);
    audio_destroy(slot->audio);
    memset(slot, 0, sizeof(*slot));
}
static const char *s_detail;
static checkpoint_result_t guards(x68k_bus_t *bus) {
    s_detail = NULL;
    if (!s_app.cpu)
        return CHECKPOINT_NOCPU;
    if (!dbg_is_paused())
        return CHECKPOINT_RUNNING;
    if (s_env.serial_active && s_env.serial_active()) {
        s_detail = "SERIAL_ACTIVE";
        return CHECKPOINT_EXTERNAL_RESOURCE;
    }
    if (s_env.auto_input_pending && s_env.auto_input_pending()) {
        s_detail = "AUTO_INPUT_PENDING";
        return CHECKPOINT_EXTERNAL_RESOURCE;
    }
    if ((s_env.fd0_override_active && s_env.fd0_override_active()) ||
        x68k_bus_boot_dev_watch_armed(bus))
        return CHECKPOINT_FD0_OVERRIDE;
    return CHECKPOINT_OK;
}
void checkpoint_init(const checkpoint_env_t *env) {
    s_env = env ? *env : (checkpoint_env_t){0};
    slot_destroy(&s_slot);
}
checkpoint_slot_state_t checkpoint_slot_state(const x68k_bus_t *bus, const char **reason_out) {
    if (reason_out)
        *reason_out = NULL;
    if (!s_slot.used)
        return CHECKPOINT_SLOT_EMPTY;
    if (!x68k_media_generation_equal(x68k_bus_snapshot_media_generation(s_slot.bus),
                                     x68k_bus_media_generation(bus))) {
        if (reason_out)
            *reason_out = "MEDIA_CHANGED";
        return CHECKPOINT_SLOT_INVALID_STATE;
    }
    return CHECKPOINT_SLOT_VALID;
}
checkpoint_result_t checkpoint_save(x68k_bus_t *bus) {
    checkpoint_result_t guard = guards(bus);
    if (guard != CHECKPOINT_OK)
        return guard;
    checkpoint_slot_t next = {0};
    mc68k_snapshot_status_t cpu_status;
    x68k_snapshot_status_t bus_status;
    next.cpu = mc68k_snapshot_create(s_app.cpu, &cpu_status);
    if (!next.cpu)
        return CHECKPOINT_OOM;
    next.bus = x68k_bus_snapshot_create(bus, &bus_status);
    if (!next.bus) {
        slot_destroy(&next);
        if (bus_status == X68K_SNAPSHOT_NOMEM)
            return CHECKPOINT_OOM;
        s_detail = x68k_bus_snapshot_external_resource_detail(bus);
        return CHECKPOINT_EXTERNAL_RESOURCE;
    }
    next.audio = audio_create();
    if (!next.audio) {
        slot_destroy(&next);
        return CHECKPOINT_OOM;
    }
    next.used = true;
    slot_destroy(&s_slot);
    s_slot = next;
    return CHECKPOINT_OK;
}
static checkpoint_result_t map_bus(const x68k_bus_t *bus, x68k_snapshot_status_t status) {
    switch (status) {
    case X68K_SNAPSHOT_MEDIA_MISMATCH:
        return CHECKPOINT_MEDIA_MISMATCH;
    case X68K_SNAPSHOT_CPU_CLOCK_MISMATCH:
        return CHECKPOINT_CPU_CLOCK_MISMATCH;
    case X68K_SNAPSHOT_CONFIG_MISMATCH:
        return CHECKPOINT_CONFIG_MISMATCH;
    case X68K_SNAPSHOT_EXTERNAL_RESOURCE:
        s_detail = x68k_bus_snapshot_external_resource_detail(bus);
        return CHECKPOINT_EXTERNAL_RESOURCE;
    default:
        return CHECKPOINT_OOM;
    }
}
checkpoint_result_t checkpoint_load(x68k_bus_t *bus) {
    checkpoint_result_t guard = guards(bus);
    if (guard != CHECKPOINT_OK)
        return guard;
    if (!s_slot.used)
        return CHECKPOINT_NO_SLOT;
    if (checkpoint_slot_state(bus, NULL) == CHECKPOINT_SLOT_INVALID_STATE)
        return CHECKPOINT_SLOT_INVALID;
    mc68k_snapshot_status_t cpu_status;
    mc68k_restore_plan_t *cpu_plan = mc68k_snapshot_prepare(s_app.cpu, s_slot.cpu, &cpu_status);
    if (!cpu_plan)
        return cpu_status == MC68K_SNAPSHOT_MISMATCH ? CHECKPOINT_CONFIG_MISMATCH
                                                     : CHECKPOINT_OOM;
    x68k_snapshot_status_t bus_status;
    x68k_bus_restore_plan_t *bus_plan = x68k_bus_snapshot_prepare(bus, s_slot.bus, &bus_status);
    if (!bus_plan) {
        mc68k_snapshot_plan_destroy(cpu_plan);
        return map_bus(bus, bus_status);
    }
    if (!s_slot.audio->fifo_valid ||
        s_slot.audio->fifo_len > (int)s_app.audio_buffers.fifo_frames) {
        mc68k_snapshot_plan_destroy(cpu_plan);
        x68k_bus_restore_plan_destroy(bus_plan);
        return CHECKPOINT_OOM;
    }
    mc68k_snapshot_commit(s_app.cpu, s_slot.cpu, cpu_plan);
    mc68k_clear_history_rings(s_app.cpu);
    x68k_bus_snapshot_commit(bus, s_slot.bus, bus_plan);
    s_app.audio = s_slot.audio->state;
    s_app.audio.capture_resample_state = (audio_resample_state_t){0};
    if (s_slot.audio->fifo_len)
        memcpy(s_app.audio_buffers.adpcm_fifo, s_slot.audio->fifo,
               sizeof(*s_slot.audio->fifo) * (size_t)s_slot.audio->fifo_len * 2u);
    if (s_env.host_cleanup)
        s_env.host_cleanup();
    dbg_wp_rearm_all();
    dbg_enter_snapshot_stop();
    return CHECKPOINT_OK;
}
const char *checkpoint_result_detail(checkpoint_result_t result) {
    return result == CHECKPOINT_EXTERNAL_RESOURCE ? s_detail : NULL;
}
const char *checkpoint_result_reason(checkpoint_result_t result) {
    switch (result) {
    case CHECKPOINT_RUNNING:
        return "RUNNING";
    case CHECKPOINT_NO_SLOT:
        return "NO_SLOT";
    case CHECKPOINT_SLOT_INVALID:
        return "SLOT_INVALID";
    case CHECKPOINT_MEDIA_MISMATCH:
        return "MEDIA_MISMATCH";
    case CHECKPOINT_CPU_CLOCK_MISMATCH:
        return "CPU_CLOCK_MISMATCH";
    case CHECKPOINT_CONFIG_MISMATCH:
        return "CONFIG_MISMATCH";
    case CHECKPOINT_EXTERNAL_RESOURCE:
        return "EXTERNAL_RESOURCE";
    case CHECKPOINT_FD0_OVERRIDE:
        return "FD0_OVERRIDE";
    default:
        return NULL;
    }
}
