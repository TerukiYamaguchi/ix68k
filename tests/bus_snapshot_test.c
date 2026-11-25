#include "bus/bus_internal.h"
#include "bus/snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* win_get_temp_path() */
#endif

static void write_fake_scsi_image(const char *path) {
    remove(path);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t header[16] = {0};
    memcpy(header, "X68SCSI1", 8);
    header[8] = 0x02;   /* 512-byte block */
    header[13] = 0x10;  /* 16 blocks */
    assert(fwrite(header, 1, sizeof(header), fp) == sizeof(header));
    uint8_t zero[512] = {0};
    for (int i = 0; i < 16; i++)
        assert(fwrite(zero, 1, sizeof(zero), fp) == sizeof(zero));
    assert(fclose(fp) == 0);
}

static void scsi_select(x68k_bus_t *bus) {
    x68k_bus_write8(bus, 0xE96037u, 1u);
    x68k_bus_write8(bus, 0xE96025u, 0x20u);
}

static uint8_t scsi_write6(x68k_bus_t *bus, const uint8_t data[512]) {
    const uint8_t cdb[6] = {0x0A, 0, 0, 0, 1, 0};
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(bus, 0xE96037u, cdb[i]);
        x68k_bus_write8(bus, 0xE96025u, 0xE0u);
        x68k_bus_write8(bus, 0xE96025u, 0xC0u);
    }
    x68k_bus_write8(bus, 0xE96039u, 0);
    x68k_bus_write8(bus, 0xE9603Bu, 2);
    x68k_bus_write8(bus, 0xE9603Du, 0);
    x68k_bus_write8(bus, 0xE96031u, 0x82u);
    x68k_bus_write8(bus, 0xE96025u, 0x80u);
    for (int i = 0; i < 512; i++)
        x68k_bus_write8(bus, 0xE96035u, data[i]);
    return x68k_bus_read8(bus, 0xE96037u);
}

static void scsi_read6(x68k_bus_t *bus, uint8_t data[512]) {
    const uint8_t cdb[6] = {0x08, 0, 0, 0, 1, 0};
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(bus, 0xE96037u, cdb[i]);
        x68k_bus_write8(bus, 0xE96025u, 0xE0u);
        x68k_bus_write8(bus, 0xE96025u, 0xC0u);
    }
    x68k_bus_write8(bus, 0xE96039u, 0);
    x68k_bus_write8(bus, 0xE9603Bu, 2);
    x68k_bus_write8(bus, 0xE9603Du, 0);
    x68k_bus_write8(bus, 0xE96031u, 0x81u);
    x68k_bus_write8(bus, 0xE96025u, 0x80u);
    for (int i = 0; i < 512; i++)
        data[i] = x68k_bus_read8(bus, 0xE96035u);
}

static void test_snapshot_restore(void) {
    x68k_bus_t *bus = x68k_bus_create();
    assert(bus != NULL);
    x68k_bus_guest_clock_set_paused(bus, 1);
    bus->ram[0x1234] = 0x12;
    bus->gvram[0x2345] = 0x34;
    bus->tvram[0x3456] = 0x56;
    bus->crtc_regs[0x28] = 0x78;
    bus->sram[0x10] = 0x9A;
    bus->sp_ram[0x20] = 0xBC;

    x68k_snapshot_status_t status;
    x68k_bus_snapshot_t *snap = x68k_bus_snapshot_create(bus, &status);
    assert(snap != NULL);
    assert(status == X68K_SNAPSHOT_OK);
    x68k_media_generation_t generation = x68k_bus_snapshot_media_generation(snap);
    assert(x68k_media_generation_equal(generation, x68k_bus_media_generation(bus)));

    bus->ram[0x1234] = 0;
    bus->gvram[0x2345] = 0;
    bus->tvram[0x3456] = 0;
    bus->crtc_regs[0x28] = 0;
    bus->sram[0x10] = 0;
    bus->sp_ram[0x20] = 0;
    x68k_bus_restore_plan_t *plan = x68k_bus_snapshot_prepare(bus, snap, &status);
    assert(plan != NULL);
    assert(status == X68K_SNAPSHOT_OK);
    x68k_bus_snapshot_commit(bus, snap, plan);
    assert(bus->ram[0x1234] == 0x12);
    assert(bus->gvram[0x2345] == 0x34);
    assert(bus->tvram[0x3456] == 0x56);
    assert(bus->crtc_regs[0x28] == 0x78);
    assert(bus->sram[0x10] == 0x9A);
    assert(bus->sp_ram[0x20] == 0xBC);

    x68k_bus_snapshot_destroy(snap);
    x68k_bus_destroy(bus);
}

static void test_cpu_clock_mismatch(void) {
    x68k_bus_t *bus = x68k_bus_create();
    assert(bus != NULL);
    x68k_bus_guest_clock_set_paused(bus, 1);
    x68k_snapshot_status_t status;
    x68k_bus_snapshot_t *snap = x68k_bus_snapshot_create(bus, &status);
    assert(snap != NULL);
    x68k_bus_set_cpu_hz(bus, 16000000u);
    assert(x68k_bus_snapshot_prepare(bus, snap, &status) == NULL);
    assert(status == X68K_SNAPSHOT_CPU_CLOCK_MISMATCH);
    x68k_bus_snapshot_destroy(snap);
    x68k_bus_destroy(bus);
}

static void test_snapshot_restores_scsi_overlay(void) {
#ifdef _WIN32
    char image_buf[300], overlay_buf[300];
    win_get_temp_path(image_buf, sizeof(image_buf), "bus_snapshot_overlay_test.HDS");
    win_get_temp_path(overlay_buf, sizeof(overlay_buf), "bus_snapshot_overlay_test.cow");
    const char *image = image_buf;
    const char *overlay = overlay_buf;
#else
    const char *image = "/tmp/bus_snapshot_overlay_test.HDS";
    const char *overlay = "/tmp/bus_snapshot_overlay_test.cow";
#endif
    write_fake_scsi_image(image);
    remove(overlay);
    x68k_bus_t *bus = x68k_bus_create();
    assert(bus != NULL);
    x68k_bus_guest_clock_set_paused(bus, 1);
    assert(x68k_bus_scsi_insert(bus, 0, image) == 0);
    assert(x68k_bus_scsi_enable_overlay(bus, 0, overlay) == 0);
    uint8_t saved[512], changed[512], readback[512];
    memset(saved, 0xA5, sizeof(saved));
    memset(changed, 0x5A, sizeof(changed));
    scsi_select(bus);
    assert(scsi_write6(bus, saved) == 0);
    x68k_snapshot_status_t status;
    x68k_bus_snapshot_t *snap = x68k_bus_snapshot_create(bus, &status);
    assert(snap != NULL && status == X68K_SNAPSHOT_OK);
    scsi_select(bus);
    assert(scsi_write6(bus, changed) == 0);
    scsi_select(bus);
    scsi_read6(bus, readback);
    assert(memcmp(readback, changed, sizeof(readback)) == 0);
    x68k_bus_restore_plan_t *plan = x68k_bus_snapshot_prepare(bus, snap, &status);
    assert(plan != NULL && status == X68K_SNAPSHOT_OK);
    x68k_bus_snapshot_commit(bus, snap, plan);
    scsi_select(bus);
    scsi_read6(bus, readback);
    assert(memcmp(readback, saved, sizeof(readback)) == 0);
    x68k_bus_snapshot_destroy(snap);
    x68k_bus_destroy(bus);
    assert(remove(image) == 0);
    assert(remove(overlay) == 0);
}

int main(void) {
    test_snapshot_restore();
    test_cpu_clock_mismatch();
    test_snapshot_restores_scsi_overlay();
    puts("bus_snapshot_test: OK");
    return 0;
}
