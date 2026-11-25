#include "app_internal.h"
#include "mcp/debugger.h"
#include "mcp/perf.h"
#include "mcp/text_event.h"
#include "peripheral/mfp.h"

#include "mc68000/cpu.h"
#include "platform/platform.h"
#include "render/renderer.h"

/* FD0 強制起動中だけ、ROM が読む $ED0018 を一時的に $0000 に置き換える。
 * OPT.1 のキー入力そのものを再現するのではなく、ROM のブートデバイス判定が
 * 1回だけ読む値を置換する方式である。復元は時間やフレーム数ではなく、バス層が
 * 検出した ROM の実読出しイベントで行うため、CPUクロックやホスト負荷に依存しない。 */
static int s_boot_dev_override_pending;
static uint16_t s_boot_dev_saved_value;
static int s_raster_sync_enabled = 1;
static int s_render_enabled = 1;
static uint8_t s_type_bytes[256];
static int s_type_len;
static int s_type_idx;
static uint64_t s_type_start_us;
static uint64_t s_type_next_us;

void app_runtime_set_raster_sync_enabled(int enabled) {
    s_raster_sync_enabled = enabled;
}

int app_runtime_raster_sync_enabled(void) {
    return s_raster_sync_enabled;
}

void app_runtime_set_render_enabled(int enabled) {
    s_render_enabled = enabled ? 1 : 0;
}

int app_runtime_render_enabled(void) {
    return s_render_enabled;
}

void app_type_start(const char *text, int delay_sec) {
    s_type_len = 0;
    s_type_idx = 0;
    for (const char *p = text; *p && s_type_len < (int)sizeof(s_type_bytes); p++) {
        if (*p == '\\' && *(p + 1) == 'n') {
            s_type_bytes[s_type_len++] = '\n';
            p++;
        } else if (*p == '\\' && *(p + 1) == 'r') {
            s_type_bytes[s_type_len++] = '\r';
            p++;
        } else if (*p == '\\' && *(p + 1) == 't') {
            s_type_bytes[s_type_len++] = '\t';
            p++;
        } else if (*p == '\\' && *(p + 1) == 'e') {
            s_type_bytes[s_type_len++] = 0x1B;
            p++;
        } else
            s_type_bytes[s_type_len++] = (uint8_t)*p;
    }
    s_type_start_us = pal_get_ticks_us() + (uint64_t)delay_sec * 1000000ULL;
    s_type_next_us = 0;
}

void type_pump(void) {
    if (s_type_idx >= s_type_len)
        return;
    uint64_t now = pal_get_ticks_us();
    if (now < s_type_start_us || now < s_type_next_us)
        return;
    pal_key_inject_ascii(s_type_bytes[s_type_idx++]);
    s_type_next_us = now + 60000;
}

int app_type_pending(void) {
    return s_type_idx < s_type_len;
}

void app_reset_machine(x68k_bus_t *bus) {
    x68k_bus_reset_vectors(bus);
    x68k_bus_reset_peripheral(bus);
    /* 周辺（OPM/ADPCM）は止まってもSDLの出力キューに積んだ分は残るため、
     * 明示的に捨てないとリセット前の音が滞留分だけ鳴り続ける。 */
    pal_audio_reset();
    if (s_app.cpu)
        mc68k_reset(s_app.cpu);
    dbg_continue();
    mcp_perf_reset(); /* リセットで測定区間は意味を失う。累積は保持する */
    s_app.iocs_ring_cnt = 0;
    s_app.iocs_ring_pos = 0;
    mcp_text_events_reset();
    iocs_key_wait_reset();
    /* リセット前の白帯・ダイアログ文言が read_bitmap_text に残らないよう捨てる。 */
    x68k_bus_putmes_clear(bus);
    /* リセットで常駐ドライバの状態も失われるため、ANKフォント差し替え報告
     *もクリアしてROM固定オフセット参照に戻す。 */
    x68k_bus_clear_ank_font_override(bus);
}

void app_force_fd0_boot_begin(x68k_bus_t *bus) {
    uint8_t hi = x68k_bus_peek8(bus, 0xED0018u);
    uint8_t lo = x68k_bus_peek8(bus, 0xED0019u);
    s_boot_dev_saved_value = (uint16_t)((hi << 8) | lo);
    x68k_bus_write_byte(bus, 0xED0018u, 0x00u);
    x68k_bus_write_byte(bus, 0xED0019u, 0x00u);
    x68k_bus_set_boot_dev_watch(bus, 1);
    s_boot_dev_override_pending = 1;
}

void app_force_fd0_boot_cancel(x68k_bus_t *bus) {
    if (!s_boot_dev_override_pending)
        return;
    x68k_bus_set_boot_dev_watch(bus, 0);
    x68k_bus_write_byte(bus, 0xED0018u, (uint8_t)(s_boot_dev_saved_value >> 8));
    x68k_bus_write_byte(bus, 0xED0019u, (uint8_t)(s_boot_dev_saved_value & 0xFFu));
    s_boot_dev_override_pending = 0;
}

void app_force_fd0_boot_finish_if_read(x68k_bus_t *bus) {
    if (s_boot_dev_override_pending && x68k_bus_take_boot_dev_read(bus))
        app_force_fd0_boot_cancel(bus);
}

int app_force_fd0_boot_active(void) {
    return s_boot_dev_override_pending;
}

void x68k_cycle_catchup(x68k_bus_t *bus, int ran) {
    x68k_bus_pin_spurious_vector(bus); /* 白帯回避、詳細はsram.c参照 */
    app_force_fd0_boot_finish_if_read(bus);
    /* ran == 0（STOP/HALT 中や単発ステップ）でも時間は止めない。MFP タイマを
     * 止めると、hardclock 割り込みで STOP を抜ける OS が永久に復帰できなくなる。 */
    int consumed = ran > 0 ? ran : 1;
    x68k_bus_mfp_chunk_tick(bus,
                            consumed); /* レジスタアクセス時に同期済みの分は除く */
    int raster_lines[64];
    int n = x68k_bus_crtc_raster_advance(bus, consumed, raster_lines, 64);
    if (s_raster_sync_enabled && s_render_enabled) {
        int cap = n < 64 ? n : 64;
        for (int li = 0; li < cap; li++) {
            int row = x68k_bus_crtc_raster_display_row(bus, raster_lines[li]);
            if (row >= 0)
                x68k_compose_line(bus, s_app.framebuf, s_app.native_w, s_app.native_h, row);
        }
    }
    x68k_bus_opm_tick(bus, consumed);
    x68k_bus_adpcm_tick(bus, consumed);
    x68k_bus_dmac_chunk_tick(bus, consumed);
    /* IRQレベルの優先度判定（NMI>IOI>DMAC>SCC>MFP）自体はCPUコアのpull型
     * フック（x68k_bus_irq_level()）が命令境界ごとに行うため、
     * ここでは行わない。NMIボタンだけはbus層に状態を持たないホスト(pal)由来の
     * 入力なので、ここでbus層のstickyフラグ（nmi_pending）へ橋渡しする。 */
    if (pal_nmi_requested())
        x68k_bus_set_nmi_pending(bus);
    /* 前面電源スイッチOFF（File > Power Switch (OFF)）。
     * x68k_bus_mfp_power_switch()は変化がなければ何もしない。 */
    if (pal_power_switch_off_requested())
        x68k_bus_mfp_power_switch(bus, 0);
}
