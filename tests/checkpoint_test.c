#include "app/checkpoint.h"
#include "app_internal.h"
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

x68k_app_t s_app;
static int s_paused = 1;
int dbg_is_paused(void) { return s_paused; }
void dbg_wp_rearm_all(void) {}
void dbg_enter_snapshot_stop(void) { s_paused = 1; }

int main(void) {
    x68k_bus_t *bus = x68k_bus_create();
    assert(bus != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(bus);
    s_app.cpu = mc68k_create(&hooks);
    assert(s_app.cpu != NULL);
    x68k_bus_set_cpu(bus, s_app.cpu);
    x68k_bus_guest_clock_set_paused(bus, 1);
    s_app.audio_buffers.fifo_frames = 4;
    s_app.audio_buffers.adpcm_fifo = calloc(8, sizeof(int32_t));
    assert(s_app.audio_buffers.adpcm_fifo != NULL);
    s_app.audio.adpcm_fifo_len = 2;
    s_app.audio_buffers.adpcm_fifo[0] = 11;
    s_app.audio_buffers.adpcm_fifo[3] = 22;
    bus->ram[0x1234] = 0x5A;
    checkpoint_init(NULL);
    assert(checkpoint_save(bus) == CHECKPOINT_OK);
    bus->ram[0x1234] = 0;
    s_app.audio_buffers.adpcm_fifo[0] = 0;
    assert(checkpoint_load(bus) == CHECKPOINT_OK);
    assert(bus->ram[0x1234] == 0x5A);
    assert(s_app.audio_buffers.adpcm_fifo[0] == 11);
    assert(checkpoint_slot_state(bus, NULL) == CHECKPOINT_SLOT_VALID);
    free(s_app.audio_buffers.adpcm_fifo);
    mc68k_destroy(s_app.cpu);
    x68k_bus_destroy(bus);
    puts("checkpoint_test: OK");
    return 0;
}
