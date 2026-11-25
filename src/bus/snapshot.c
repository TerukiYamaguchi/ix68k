#include "bus/snapshot.h"

#include "bus/bus_internal.h"
#include "peripheral/hostdrv_remote.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool fd_present[4];
    bool fd_write_protect[4];
    const fdc_media_profile_t *fd_profile[4];
    media_fp_t fd_fp[4];
    bool scsi_present[SCSI_NUM_UNITS];
    scsi_unit_kind_t scsi_kind[SCSI_NUM_UNITS];
    int scsi_removable[SCSI_NUM_UNITS];
    int scsi_write_protected[SCSI_NUM_UNITS];
    uint32_t scsi_bytes_per_record[SCSI_NUM_UNITS];
    uint32_t scsi_disk_end_record[SCSI_NUM_UNITS];
    media_fp_t scsi_fp[SCSI_NUM_UNITS];
} media_set_fp_t;

struct x68k_bus_snapshot {
    x68k_bus_t *image;
    uint8_t *ram, *local_memory, *gvram, *tvram, *crtc_regs, *sram, *sp_ram;
    mc68k_fpu_snapshot_t *fpu_cir;
    uint64_t guest_mono_us;
    int64_t rtc_now;
    uint32_t cpu_hz, local_memory_size;
    x68k_media_generation_t media_generation;
    x68k_timing_profile_t timing_profile;
    int fdc_num_drives;
    media_set_fp_t media;
    scsi_overlay_t *scsi_overlays[SCSI_NUM_UNITS];
};

struct x68k_bus_restore_plan {
    /* prepare 時に確保し、commit は所有権を移すだけにする。これにより
     * commit 後にメモリ不足で部分復元となることを防ぐ。 */
    scsi_overlay_t *scsi_overlays[SCSI_NUM_UNITS];
};

static void set_status(x68k_snapshot_status_t *status, x68k_snapshot_status_t value) {
    if (status)
        *status = value;
}

static void media_set_free(media_set_fp_t *set) {
    for (int i = 0; i < 4; i++)
        media_fp_free(&set->fd_fp[i]);
    for (int i = 0; i < SCSI_NUM_UNITS; i++)
        media_fp_free(&set->scsi_fp[i]);
}

static bool media_set_capture(media_set_fp_t *set, const x68k_bus_t *bus) {
    memset(set, 0, sizeof(*set));
    for (int i = 0; i < 4; i++) {
        set->fd_present[i] = bus->fdc.disk[i] != NULL;
        set->fd_write_protect[i] = bus->fdc.write_protect[i] != 0;
        set->fd_profile[i] = bus->fdc.profile[i];
        if (set->fd_present[i] && !media_fp_clone(&set->fd_fp[i], &bus->fdc.media_fp[i]))
            goto fail;
    }
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        const scsi_unit_t *unit = &bus->spc_in.units[i];
        set->scsi_present[i] = unit->present != 0;
        set->scsi_kind[i] = unit->kind;
        set->scsi_removable[i] = unit->removable;
        set->scsi_write_protected[i] = unit->write_protected;
        set->scsi_bytes_per_record[i] = unit->bytes_per_record;
        set->scsi_disk_end_record[i] = unit->disk_end_record;
        if (set->scsi_present[i] && unit->fp &&
            !media_fp_clone(&set->scsi_fp[i], &unit->media_fp))
            goto fail;
    }
    return true;
fail:
    media_set_free(set);
    return false;
}

static const char *const FD_WRITABLE_DETAIL[4] = {"FD0_WRITABLE", "FD1_WRITABLE",
                                                  "FD2_WRITABLE", "FD3_WRITABLE"};
static const char *const FD_NO_FP_DETAIL[4] = {"FD0_NO_FINGERPRINT", "FD1_NO_FINGERPRINT",
                                               "FD2_NO_FINGERPRINT", "FD3_NO_FINGERPRINT"};
static const char *const SCSI_WRITABLE_DETAIL[SCSI_NUM_UNITS] = {
    "SCSI0_WRITABLE_NO_OVERLAY", "SCSI1_WRITABLE_NO_OVERLAY", "SCSI2_WRITABLE_NO_OVERLAY",
    "SCSI3_WRITABLE_NO_OVERLAY", "SCSI4_WRITABLE_NO_OVERLAY", "SCSI5_WRITABLE_NO_OVERLAY",
    "SCSI6_WRITABLE_NO_OVERLAY"};
static const char *const SCSI_NO_FP_DETAIL[SCSI_NUM_UNITS] = {
    "SCSI0_NO_FINGERPRINT", "SCSI1_NO_FINGERPRINT", "SCSI2_NO_FINGERPRINT",
    "SCSI3_NO_FINGERPRINT", "SCSI4_NO_FINGERPRINT", "SCSI5_NO_FINGERPRINT",
    "SCSI6_NO_FINGERPRINT"};

const char *x68k_bus_snapshot_external_resource_detail(const x68k_bus_t *bus) {
    if (bus->hostfile.state != 0 || bus->hostfile.get_src || bus->hostfile.put_tmp_fp ||
        bus->hostfile.put_tmp_valid)
        return "HOSTFILE_TRANSFER";
    if (hostdrv_remote_is_configured())
        return "HOSTDRV";
    if (adpcm_is_recording(&bus->adpcm))
        return "ADPCM_RECORDING";
    for (int i = 0; i < 4; i++) {
        if (!bus->fdc.disk[i])
            continue;
        if (!bus->fdc.write_protect[i])
            return FD_WRITABLE_DETAIL[i];
        if (!bus->fdc.media_fp[i].valid)
            return FD_NO_FP_DETAIL[i];
    }
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        const scsi_unit_t *unit = &bus->spc_in.units[i];
        if (!unit->fp)
            continue;
        if (!unit->write_protected && !unit->overlay)
            return SCSI_WRITABLE_DETAIL[i];
        if (!unit->media_fp.valid)
            return SCSI_NO_FP_DETAIL[i];
    }
    return NULL;
}

static bool bus_check_external_resources(const x68k_bus_t *bus) {
    return x68k_bus_snapshot_external_resource_detail(bus) == NULL;
}

static bool media_set_matches(const media_set_fp_t *saved, const x68k_bus_t *bus) {
    for (int i = 0; i < 4; i++) {
        if (saved->fd_present[i] != (bus->fdc.disk[i] != NULL) ||
            saved->fd_write_protect[i] != (bus->fdc.write_protect[i] != 0) ||
            saved->fd_profile[i] != bus->fdc.profile[i] ||
            (saved->fd_present[i] && !media_fp_equal(&saved->fd_fp[i], &bus->fdc.media_fp[i])))
            return false;
        if (saved->fd_present[i] && !media_fp_matches_path(&saved->fd_fp[i], 0))
            return false;
    }
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        const scsi_unit_t *unit = &bus->spc_in.units[i];
        if (saved->scsi_present[i] != (unit->present != 0) ||
            saved->scsi_kind[i] != unit->kind || saved->scsi_removable[i] != unit->removable ||
            saved->scsi_write_protected[i] != unit->write_protected ||
            saved->scsi_bytes_per_record[i] != unit->bytes_per_record ||
            saved->scsi_disk_end_record[i] != unit->disk_end_record ||
            (unit->fp && !media_fp_equal(&saved->scsi_fp[i], &unit->media_fp)))
            return false;
        if (unit->fp && (!media_fp_matches_path(&saved->scsi_fp[i], 0) ||
                         !media_fp_matches_open_file(&saved->scsi_fp[i], unit->img_path ? 1 : 0,
                                                     unit->fp) ||
                         (unit->img_path && !media_fp_matches_path(&saved->scsi_fp[i], 1))))
            return false;
    }
    return true;
}

void x68k_bus_snapshot_destroy(x68k_bus_snapshot_t *snap) {
    if (!snap)
        return;
    free(snap->image);
    free(snap->ram);
    free(snap->local_memory);
    free(snap->gvram);
    free(snap->tvram);
    free(snap->crtc_regs);
    free(snap->sram);
    free(snap->sp_ram);
    mc68k_fpu_snapshot_destroy(snap->fpu_cir);
    for (int i = 0; i < SCSI_NUM_UNITS; i++)
        scsi_overlay_destroy(snap->scsi_overlays[i]);
    media_set_free(&snap->media);
    free(snap);
}

x68k_bus_snapshot_t *x68k_bus_snapshot_create(const x68k_bus_t *bus,
                                              x68k_snapshot_status_t *status) {
    if (!bus_check_external_resources(bus)) {
        set_status(status, X68K_SNAPSHOT_EXTERNAL_RESOURCE);
        return NULL;
    }
    x68k_bus_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap)
        goto nomem;
    snap->image = malloc(sizeof(*snap->image));
    snap->ram = malloc(RAM_BUF_SIZE);
    snap->local_memory = bus->local_memory_size ? malloc(bus->local_memory_size) : NULL;
    snap->gvram = malloc(GVRAM_SIZE);
    snap->tvram = malloc(TVRAM_SIZE);
    snap->crtc_regs = malloc(CRTC_SIZE);
    snap->sram = malloc(SRAM_SIZE);
    snap->sp_ram = malloc(SPRITE_SIZE);
    if (!snap->image || !snap->ram || (bus->local_memory_size && !snap->local_memory) ||
        !snap->gvram || !snap->tvram || !snap->crtc_regs || !snap->sram || !snap->sp_ram)
        goto nomem_destroy;
    memcpy(snap->image, bus, sizeof(*snap->image));
    memcpy(snap->ram, bus->ram, RAM_BUF_SIZE);
    if (bus->local_memory_size)
        memcpy(snap->local_memory, bus->local_memory, bus->local_memory_size);
    memcpy(snap->gvram, bus->gvram, GVRAM_SIZE);
    memcpy(snap->tvram, bus->tvram, TVRAM_SIZE);
    memcpy(snap->crtc_regs, bus->crtc_regs, CRTC_SIZE);
    memcpy(snap->sram, bus->sram, SRAM_SIZE);
    memcpy(snap->sp_ram, bus->sp_ram, SPRITE_SIZE);
    if (bus->fpu_cir && !(snap->fpu_cir = mc68k_fpu_snapshot_create(bus->fpu_cir)))
        goto nomem_destroy;
    if (!media_set_capture(&snap->media, bus))
        goto nomem_destroy;
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        if (!bus->spc_in.units[i].overlay)
            continue;
        snap->scsi_overlays[i] = scsi_overlay_clone(bus->spc_in.units[i].overlay);
        if (!snap->scsi_overlays[i])
            goto nomem_destroy;
    }
    snap->guest_mono_us = guest_clock_mono_us(&bus->guest_clock);
    snap->rtc_now = rtc_now(&bus->rtc);
    snap->cpu_hz = bus->cpu_hz;
    snap->local_memory_size = bus->local_memory_size;
    snap->timing_profile = bus->timing_profile;
    snap->fdc_num_drives = bus->fdc.num_drives;
    snap->media_generation = x68k_bus_media_generation(bus);
    set_status(status, X68K_SNAPSHOT_OK);
    return snap;
nomem_destroy:
    x68k_bus_snapshot_destroy(snap);
nomem:
    set_status(status, X68K_SNAPSHOT_NOMEM);
    return NULL;
}

x68k_bus_restore_plan_t *x68k_bus_snapshot_prepare(const x68k_bus_t *bus,
                                                   const x68k_bus_snapshot_t *snap,
                                                   x68k_snapshot_status_t *status) {
    if (!snap || bus->cpu_hz != snap->cpu_hz) {
        set_status(status, X68K_SNAPSHOT_CPU_CLOCK_MISMATCH);
        return NULL;
    }
    if (bus->timing_profile != snap->timing_profile ||
        bus->local_memory_size != snap->local_memory_size ||
        bus->fdc.num_drives != snap->fdc_num_drives ||
        (bus->fpu_cir != NULL) != (snap->fpu_cir != NULL) ||
        (bus->fpu_cir && bus->fpu_cir_base != snap->image->fpu_cir_base)) {
        set_status(status, X68K_SNAPSHOT_CONFIG_MISMATCH);
        return NULL;
    }
    if (!bus_check_external_resources(bus)) {
        set_status(status, X68K_SNAPSHOT_EXTERNAL_RESOURCE);
        return NULL;
    }
    if (!media_set_matches(&snap->media, bus)) {
        set_status(status, X68K_SNAPSHOT_MEDIA_MISMATCH);
        return NULL;
    }
    if (bus->fpu_cir && !mc68k_fpu_snapshot_check(bus->fpu_cir, snap->fpu_cir)) {
        set_status(status, X68K_SNAPSHOT_CONFIG_MISMATCH);
        return NULL;
    }
    x68k_bus_restore_plan_t *plan = calloc(1, sizeof(*plan));
    if (!plan) {
        set_status(status, X68K_SNAPSHOT_NOMEM);
        return NULL;
    }
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        if (!snap->scsi_overlays[i])
            continue;
        plan->scsi_overlays[i] = scsi_overlay_clone(snap->scsi_overlays[i]);
        if (!plan->scsi_overlays[i]) {
            x68k_bus_restore_plan_destroy(plan);
            set_status(status, X68K_SNAPSHOT_NOMEM);
            return NULL;
        }
    }
    set_status(status, X68K_SNAPSHOT_OK);
    return plan;
}

void x68k_bus_restore_plan_destroy(x68k_bus_restore_plan_t *plan) {
    if (!plan)
        return;
    for (int i = 0; i < SCSI_NUM_UNITS; i++)
        scsi_overlay_destroy(plan->scsi_overlays[i]);
    free(plan);
}

void x68k_bus_snapshot_commit(x68k_bus_t *bus, const x68k_bus_snapshot_t *snap,
                              x68k_bus_restore_plan_t *plan) {
    scsi_overlay_t *old_overlays[SCSI_NUM_UNITS];
    for (int i = 0; i < SCSI_NUM_UNITS; i++)
        old_overlays[i] = bus->spc_in.units[i].overlay;
    x68k_page_map_t *pmap = bus->pmap; /* 表の実体はスナップショットに含めない */
    struct {
        uint32_t ram_watch_addr, ram_watch_len;
        int boot_dev_watch_armed, boot_dev_read_seen, super_protect_enabled, nmi_pending;
        uint8_t joy_port[2];
        guest_clock_t guest_clock;
        int host_x, host_y;
        uint8_t btn_left, btn_right;
    } keep = {.ram_watch_addr = bus->ram_watch_addr,
              .ram_watch_len = bus->ram_watch_len,
              .boot_dev_watch_armed = bus->boot_dev_watch_armed,
              .boot_dev_read_seen = bus->boot_dev_read_seen,
              .super_protect_enabled = bus->super_protect_enabled,
              .nmi_pending = bus->nmi_pending,
              .joy_port = {bus->joy_port[0], bus->joy_port[1]},
              .guest_clock = bus->guest_clock,
              .host_x = bus->scc.host_x,
              .host_y = bus->scc.host_y,
              .btn_left = bus->scc.btn_left,
              .btn_right = bus->scc.btn_right};
    memcpy(bus, snap->image, sizeof(*bus));
    bus->pmap = pmap;
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        scsi_overlay_destroy(old_overlays[i]);
        bus->spc_in.units[i].overlay = plan->scsi_overlays[i];
        plan->scsi_overlays[i] = NULL;
    }
    bus->ram_watch_addr = keep.ram_watch_addr;
    bus->ram_watch_len = keep.ram_watch_len;
    bus->boot_dev_watch_armed = keep.boot_dev_watch_armed;
    bus->boot_dev_read_seen = keep.boot_dev_read_seen;
    bus->super_protect_enabled = keep.super_protect_enabled;
    bus->nmi_pending = keep.nmi_pending;
    memcpy(bus->joy_port, keep.joy_port, sizeof(keep.joy_port));
    bus->guest_clock = keep.guest_clock;
    bus->scc.host_x = keep.host_x;
    bus->scc.host_y = keep.host_y;
    bus->scc.btn_left = keep.btn_left;
    bus->scc.btn_right = keep.btn_right;
    memcpy(bus->ram, snap->ram, RAM_BUF_SIZE);
    if (bus->local_memory_size)
        memcpy(bus->local_memory, snap->local_memory, bus->local_memory_size);
    memcpy(bus->gvram, snap->gvram, GVRAM_SIZE);
    memcpy(bus->tvram, snap->tvram, TVRAM_SIZE);
    memcpy(bus->crtc_regs, snap->crtc_regs, CRTC_SIZE);
    memcpy(bus->sram, snap->sram, SRAM_SIZE);
    memcpy(bus->sp_ram, snap->sp_ram, SPRITE_SIZE);
    if (bus->fpu_cir)
        mc68k_fpu_snapshot_restore(bus->fpu_cir, snap->fpu_cir);
    guest_clock_rebase_paused(&bus->guest_clock, snap->guest_mono_us);
    rtc_snapshot_rebase(&bus->rtc, snap->rtc_now);
    scc_rebase_host_pos(&bus->scc);
    bus->mfp.chunk_active = 0;
    bus->mfp.synced_cycles = 0;
    bus->dmac.chunk_active = 0;
    bus->dmac.syncing = 0;
    bus->dmac.synced_cycles = 0;
    x68k_bus_page_map_refresh(bus);
    x68k_bus_restore_plan_destroy(plan);
}

x68k_media_generation_t x68k_bus_snapshot_media_generation(const x68k_bus_snapshot_t *snap) {
    return snap ? snap->media_generation : (x68k_media_generation_t){0};
}

uint32_t x68k_bus_snapshot_cpu_hz(const x68k_bus_snapshot_t *snap) {
    return snap ? snap->cpu_hz : 0;
}
