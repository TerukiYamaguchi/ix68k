#include "bus/x68k_bus.h"
#include "peripheral/hostdrv_remote.h"
#include "peripheral/scsi.h"
#include "peripheral/scsi_rom_data.h"
#include "peripheral/sasi.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"   /* mc68k_debug_translate()（HOSTDRVの論理→物理変換） */
#include "render/renderer.h" /* grb555_to_argb()（パレットARGBキャッシュ用） */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h> /* EXDEV（HOSTFILE rename→コピーのフォールバック判定） */
#ifdef _WIN32
#include "platform/win_compat.h" /* unlink()/strcasecmp()/access()/chmod()/S_IWUSR 相当 */
#else
#include <unistd.h>   /* unlink()（HOSTFILE 一時ファイル破棄用）/access() */
#include <strings.h>  /* strcasecmp()（FD イメージ拡張子判定用） */
#include <sys/stat.h> /* chmod()/mode_t/S_IWUSR（WPトグル用） */
#endif
#include "bus/bus_internal.h"

/* ---- IOI 信号管理ヘルパー (XEiJ IOInterrupt.java 準拠) ---- */

static void ioi_fdd_fall(x68k_bus_t *b) {
    if (b->ioi_signal & IOI_FDD_SIGNAL) {
        b->ioi_signal &= ~IOI_FDD_SIGNAL;
        b->ioi_request &= ~IOI_FDD_REQUEST;
    }
}

static void ioi_fdd_rise(x68k_bus_t *b) {
    if (!(b->ioi_signal & IOI_FDD_SIGNAL)) {
        b->ioi_signal |= IOI_FDD_SIGNAL;
        if ((b->ioi_enable & IOI_FDD_REQUEST) && !(b->ioi_request & IOI_FDD_REQUEST))
            b->ioi_request |= IOI_FDD_REQUEST;
    }
}

/* ディスクを排出し、抜去割り込みを発生させる（実機の $E94005 イジェクト書き込み・
 * IOCS _B_EJECT・MCP EJECT_DISK いずれからもここを通す）。
 * 『Inside X68000』: 「ディスクの抜き挿しが行われると割り込みが発生する」 */
static void fdc_eject_drive(x68k_bus_t *b, int drive) {
    if (drive < 0 || drive >= 4 || !b->fdc.disk[drive])
        return;
    free(b->fdc.disk[drive]);
    b->fdc.disk[drive] = NULL;
    b->fdc.disk_bytes[drive] = 0;
    b->fdc.profile[drive] = NULL;
    /* disk_pathも一緒に解放しないと、イジェクト後もx68k_bus_fdc_disk_path()が
     * 古いファイル名を返し続け、ディスク選択ウィンドウの表示が更新されない。 */
    free(b->fdc.disk_path[drive]);
    b->fdc.disk_path[drive] = NULL;
    media_fp_free(&b->fdc.media_fp[drive]);
    fdc_media_changed(&b->fdc);
    b->fdc_ejected_flag[drive] = 1;
    /* DDAM状態は永続化しないため排出時にクリアする。 */
    fdc_ddam_clear_drive(&b->fdc, drive);
    /* WP状態も同様に非永続化。ホストのファイル属性が
     * 単一の真実であり、次回挿入時に再判定する。 */
    b->fdc.write_protect[drive] = 0;
    ioi_fdd_rise(b);
}

static void ioi_fdc_fall(x68k_bus_t *b) {
    if (b->ioi_signal & IOI_FDC_SIGNAL) {
        b->ioi_signal &= ~IOI_FDC_SIGNAL;
        b->ioi_request &= ~IOI_FDC_REQUEST;
    }
}

static void ioi_fdc_rise(x68k_bus_t *b) {
    if (!(b->ioi_signal & IOI_FDC_SIGNAL)) {
        b->ioi_signal |= IOI_FDC_SIGNAL;
        if ((b->ioi_enable & IOI_FDC_REQUEST) && !(b->ioi_request & IOI_FDC_REQUEST))
            b->ioi_request |= IOI_FDC_REQUEST;
    }
}

/* ---- 内部ヘルパー ---- */

/*
 * 現在「搭載済み」とみなすメインメモリの上限アドレス+1を返す。
 * SRAM $ED0008（4B, BE）= SWITCH.X MEMORY が書き込む「メモリのリミット
 * アドレス+1」（XCプログラマーズマニュアル）をそのまま使う。未初期化/
 * 異常値（0 または GVRAM_BASE 超）は安全側 RAM_SIZE（1MB）にフォールバック。
 * この値を超えるアクセスは bus_error_gap 扱いになる。
 */
static inline uint32_t ram_limit(const x68k_bus_t *b) {
    const uint8_t *p = &b->sram[SRAM_REAL_OFFSET + 0x08u];
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                 (uint32_t)p[3];
    if (v == 0u || v > GVRAM_BASE)
        v = RAM_SIZE;
    return v;
}

#define LOCAL_MEMORY_BASE 0x10000000u
static inline bool local_memory_contains(const x68k_bus_t *b, uint32_t addr) {
    return b->local_memory && addr >= LOCAL_MEMORY_BASE &&
           addr - LOCAL_MEMORY_BASE < b->local_memory_size;
}
static inline uint32_t cpu_bus_addr(const x68k_bus_t *b, uint32_t addr) {
    return local_memory_contains(b, addr) ? addr : (addr & 0xFFFFFFu);
}

/* ワード/ロングの高速パスは単一の低位24-bitアドレスを直接参照する。
 * アクセス範囲が実装済みローカルSIMMへ触れる場合だけ、各バイトを個別に
 * デコードする。SIMM外の高位アドレスは従来どおり24-bitミラーであり、RTCや
 * FPU-CIRのword/long特殊経路を維持する。 */
static inline bool cpu_fast_bus_addr(const x68k_bus_t *b, uint32_t raw_addr, unsigned size,
                                     uint32_t *mapped_addr) {
    *mapped_addr = cpu_bus_addr(b, raw_addr);
    /* ローカルメモリは連続した範囲なので、アクセスの先頭と末尾バイトだけ調べれば足りる。 */
    return !local_memory_contains(b, raw_addr) &&
           !local_memory_contains(b, raw_addr + size - 1u);
}

/*
 * 生ウェイト数（システムポート#5から求めた「設定値+2」等）を、較正係数
 * X68K_WAIT_SCALE_NUM/DEN で「有効ペナルティ」へ変換する（
 * cpu_design.md §2）。X68K_WAIT_MAX で上限クランプ
 * する。
 */
static inline uint8_t wait_penalty(unsigned raw) {
    unsigned scaled = (raw * X68K_WAIT_SCALE_NUM) / X68K_WAIT_SCALE_DEN;
    return (uint8_t)(scaled > X68K_WAIT_MAX ? X68K_WAIT_MAX : scaled);
}

/*
 * システムポート#5 ($E8E009, X68030) の書込みを解釈し、RAM/ROMの有効
 * ペナルティを更新する。bit7-4=ROM wait、bit3-0=DRAM wait、いずれも
 * 設定値+2ウェイト。DRAMの$0だけはスタティックカラム動作の近似として
 * X68K_RAM_WAIT_STATIC_COLUMNを使う（cpu_design.md §2）。
 */
static void x68k_bus_apply_wait_port(x68k_bus_t *b, uint8_t val) {
    /* バッキングストア（crtc_regs[]）もここで揃える。write8() のCRTC分岐が
     * 別途同じ値を書くが冪等なので害はない。x68k_bus_create()/
     * bus_reset_peripheral() のようにwrite8()を経由しない初期化経路でも
     * 読み戻しが書いた値と一致するようにするため。 */
    b->crtc_regs[0xE8E009u - CRTC_BASE] = val;
    unsigned rom_n = (val >> 4) & 0x0Fu, ram_n = val & 0x0Fu;
    b->wait.rom = wait_penalty(rom_n + 2u);
    b->wait.ram = wait_penalty(ram_n == 0u ? X68K_RAM_WAIT_STATIC_COLUMN : ram_n + 2u);
    b->wait.mmio = 0u; /* MMIO は課金しない */
    x68k_bus_page_map_refresh(b);
}

/*
 * mc68k_bus_t.access_timing フック実装。RAM/ROMだけを
 * cacheable=trueとして課金し、それ以外（VRAM/SRAM/MMIO/未マップ）は
 * cacheable=false・wait=0とする。境界式は既存の ram_limit()/IOCS_ROM_BASE
 * をそのまま使い、独自の境界を書き直さない
 * （cpu_design.md §2）。
 */
#define WAIT_PAIR(read, write)     {(read), (write)}
#define WAIT_SAME(value)           WAIT_PAIR((value), (value))
#define WAIT_REGION(region, value) [X68K_WAIT_REGION_##region] = value
static const x68k_wait_table_t s_wait_tables[X68K_TIMING_PROFILE_COUNT] =
    {
        [X68K_TIMING_PROFILE_X68000] =
            {
                .region = {WAIT_REGION(ROM, WAIT_SAME(1)),
                           WAIT_REGION(SRAM, WAIT_SAME(1)),
                           WAIT_REGION(GVRAM, WAIT_SAME(1)),
                           WAIT_REGION(TVRAM, WAIT_SAME(2)),
                           WAIT_REGION(CRTC, WAIT_SAME(1)),
                           WAIT_REGION(VC_PALETTE, WAIT_SAME(3)),
                           WAIT_REGION(VC_MODE, WAIT_SAME(1)),
                           WAIT_REGION(DMAC, WAIT_SAME(15)),
                           WAIT_REGION(MFP, WAIT_SAME(4)),
                           WAIT_REGION(RTC, WAIT_SAME(2)),
                           WAIT_REGION(PRN, WAIT_SAME(1)),
                           WAIT_REGION(SYS, WAIT_SAME(1)),
                           WAIT_REGION(OPM, WAIT_SAME(2)),
                           WAIT_REGION(ADPCM, WAIT_SAME(2)),
                           WAIT_REGION(FDC, WAIT_SAME(2)),
                           WAIT_REGION(HDC, WAIT_SAME(2)),
                           WAIT_REGION(SCC, WAIT_SAME(6)),
                           WAIT_REGION(PPI, WAIT_SAME(2)),
                           WAIT_REGION(IOI, WAIT_SAME(2)),
                           WAIT_REGION(SPRITE_REG, WAIT_SAME(1)),
                           WAIT_REGION(SPRITE_VRAM, WAIT_PAIR(4, 5))},
                .long_factor = 2,
            },
        [X68K_TIMING_PROFILE_XVI] =
            {
                .region = {WAIT_REGION(ROM, WAIT_SAME(4)),
                           WAIT_REGION(SRAM, WAIT_SAME(4)),
                           WAIT_REGION(GVRAM, WAIT_SAME(4)),
                           WAIT_REGION(TVRAM, WAIT_SAME(6)),
                           WAIT_REGION(CRTC, WAIT_SAME(4)),
                           WAIT_REGION(VC_PALETTE, WAIT_SAME(7)),
                           WAIT_REGION(VC_MODE, WAIT_SAME(3)),
                           WAIT_REGION(DMAC, WAIT_SAME(26)),
                           WAIT_REGION(MFP, WAIT_SAME(9)),
                           WAIT_REGION(RTC, WAIT_SAME(5)),
                           WAIT_REGION(PRN, WAIT_SAME(4)),
                           WAIT_REGION(SYS, WAIT_SAME(4)),
                           WAIT_REGION(OPM, WAIT_SAME(5)),
                           WAIT_REGION(ADPCM, WAIT_SAME(5)),
                           WAIT_REGION(FDC, WAIT_SAME(5)),
                           WAIT_REGION(HDC, WAIT_SAME(5)),
                           WAIT_REGION(SCC, WAIT_SAME(12)),
                           WAIT_REGION(PPI, WAIT_SAME(5)),
                           WAIT_REGION(IOI, WAIT_SAME(5)),
                           WAIT_REGION(SPRITE_REG, WAIT_SAME(4)),
                           WAIT_REGION(SPRITE_VRAM, WAIT_PAIR(9, 10))},
                .long_factor = 2,
            },
        [X68K_TIMING_PROFILE_X68030] =
            {
                .region = {WAIT_REGION(SRAM, WAIT_SAME(5)),
                           WAIT_REGION(GVRAM, WAIT_SAME(9)),
                           WAIT_REGION(TVRAM, WAIT_SAME(9)),
                           WAIT_REGION(CRTC, WAIT_SAME(9)),
                           WAIT_REGION(VC_PALETTE, WAIT_SAME(14)),
                           WAIT_REGION(VC_MODE, WAIT_PAIR(9, 11)),
                           WAIT_REGION(DMAC, WAIT_PAIR(37, 31)),
                           WAIT_REGION(MFP, WAIT_SAME(24)),
                           WAIT_REGION(RTC, WAIT_SAME(31)),
                           WAIT_REGION(PRN, WAIT_SAME(7)),
                           WAIT_REGION(SYS, WAIT_SAME(7)),
                           WAIT_REGION(OPM, WAIT_PAIR(19, 53)),
                           WAIT_REGION(ADPCM, WAIT_SAME(19)),
                           WAIT_REGION(FDC, WAIT_SAME(19)),
                           WAIT_REGION(HDC, WAIT_SAME(19)),
                           WAIT_REGION(SCC, WAIT_PAIR(41, 49)),
                           WAIT_REGION(PPI, WAIT_SAME(19)),
                           WAIT_REGION(IOI, WAIT_SAME(19)),
                           WAIT_REGION(SPRITE_REG, WAIT_SAME(9)),
                           WAIT_REGION(SPRITE_VRAM, WAIT_PAIR(17, 18))},
                .long_factor = 1,
                .ram_rom_from_port5 = true,
            },
        [X68K_TIMING_PROFILE_060TURBO] =
            {
                .region = {WAIT_REGION(SRAM, WAIT_SAME(13)),
                           WAIT_REGION(GVRAM, WAIT_SAME(21)),
                           WAIT_REGION(TVRAM, WAIT_SAME(22)),
                           WAIT_REGION(CRTC, WAIT_SAME(21)),
                           WAIT_REGION(VC_PALETTE, WAIT_SAME(33)),
                           WAIT_REGION(VC_MODE, WAIT_SAME(21)),
                           WAIT_REGION(DMAC, WAIT_SAME(73)),
                           WAIT_REGION(MFP, WAIT_SAME(47)),
                           WAIT_REGION(RTC, WAIT_SAME(77)),
                           WAIT_REGION(PRN, WAIT_SAME(17)),
                           WAIT_REGION(SYS, WAIT_SAME(17)),
                           WAIT_REGION(OPM, WAIT_SAME(37)),
                           WAIT_REGION(ADPCM, WAIT_SAME(37)),
                           WAIT_REGION(FDC, WAIT_SAME(37)),
                           WAIT_REGION(HDC, WAIT_SAME(37)),
                           WAIT_REGION(SCC, WAIT_SAME(97)),
                           WAIT_REGION(PPI, WAIT_SAME(37)),
                           WAIT_REGION(IOI, WAIT_SAME(37)),
                           WAIT_REGION(SPRITE_REG, WAIT_SAME(21)),
                           WAIT_REGION(SPRITE_VRAM, WAIT_PAIR(37, 39))},
                .long_factor = 1,
                .ram_rom_from_port5 = true,
            },
};
#undef WAIT_REGION
#undef WAIT_SAME
#undef WAIT_PAIR

static x68k_wait_region_t classify_wait_region(const x68k_bus_t *b, uint32_t addr) {
    if (addr < RAM_BASE + ram_limit(b))
        return X68K_WAIT_REGION_RAM;
    if (addr >= IOCS_ROM_BASE)
        return X68K_WAIT_REGION_ROM;
    if (addr >= 0xC00000u && addr <= 0xDFFFFFu)
        return X68K_WAIT_REGION_GVRAM;
    if (addr >= 0xE00000u && addr <= 0xE7FFFFu)
        return X68K_WAIT_REGION_TVRAM;
    if (addr >= 0xE80000u && addr <= 0xE81FFFu)
        return X68K_WAIT_REGION_CRTC;
    if (addr >= 0xE82000u && addr <= 0xE823FFu)
        return X68K_WAIT_REGION_VC_PALETTE;
    if (addr >= 0xE82400u && addr <= 0xE83FFFu)
        return X68K_WAIT_REGION_VC_MODE;
    if (addr >= 0xE84000u && addr <= 0xE85FFFu)
        return X68K_WAIT_REGION_DMAC;
    if (addr >= 0xE88000u && addr <= 0xE89FFFu)
        return X68K_WAIT_REGION_MFP;
    if (addr >= 0xE8A000u && addr <= 0xE8BFFFu)
        return X68K_WAIT_REGION_RTC;
    if (addr >= 0xE8C000u && addr <= 0xE8DFFFu)
        return X68K_WAIT_REGION_PRN;
    if (addr >= 0xE8E000u && addr <= 0xE8FFFFu)
        return X68K_WAIT_REGION_SYS;
    if (addr >= 0xE90000u && addr <= 0xE91FFFu)
        return X68K_WAIT_REGION_OPM;
    if (addr >= 0xE92000u && addr <= 0xE93FFFu)
        return X68K_WAIT_REGION_ADPCM;
    if (addr >= 0xE94000u && addr <= 0xE95FFFu)
        return X68K_WAIT_REGION_FDC;
    if (addr >= 0xE96000u && addr <= 0xE97FFFu)
        return X68K_WAIT_REGION_HDC;
    if (addr >= 0xE98000u && addr <= 0xE99FFFu)
        return X68K_WAIT_REGION_SCC;
    if (addr >= 0xE9A000u && addr <= 0xE9BFFFu)
        return X68K_WAIT_REGION_PPI;
    if (addr >= 0xE9C000u && addr <= 0xE9DFFFu)
        return X68K_WAIT_REGION_IOI;
    if (addr >= 0xEB0000u && addr <= 0xEB7FFFu)
        return X68K_WAIT_REGION_SPRITE_REG;
    if (addr >= 0xEB8000u && addr <= 0xEBFFFFu)
        return X68K_WAIT_REGION_SPRITE_VRAM;
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE)
        return X68K_WAIT_REGION_SRAM;
    return X68K_WAIT_REGION_NONE;
}

static void bus_access_timing(void *ctx, const mc68k_bus_access_t *access,
                              mc68k_bus_timing_t *out) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    out->cacheable = false;
    out->wait_cycles = 0;
    if (access->fc == MC68K_FC_CPU_SPACE)
        return;
    if (!b->cpu || b->timing_profile == X68K_TIMING_PROFILE_NONE)
        return;
    if (local_memory_contains(b, access->physical))
        return;
    uint32_t addr = access->physical & 0xFFFFFFu;
    x68k_wait_region_t r = classify_wait_region(b, addr);
    /* 040turbo は実測値がないため、X68030 母機の表を暫定で流用する（bus_timing_design.md 3.5
     * 節）。 */
    const x68k_wait_table_t *t =
        &s_wait_tables[b->timing_profile == X68K_TIMING_PROFILE_040TURBO
                           ? X68K_TIMING_PROFILE_X68030
                           : b->timing_profile];
    if (t->ram_rom_from_port5 && (r == X68K_WAIT_REGION_RAM || r == X68K_WAIT_REGION_ROM)) {
        out->cacheable = true;
        out->wait_cycles = r == X68K_WAIT_REGION_RAM ? b->wait.ram : b->wait.rom;
        return;
    }
    out->wait_cycles = access->read ? t->region[r].read : t->region[r].write;
    if (access->size == 4)
        out->wait_cycles *= t->long_factor;
}

/* hostdrv_remote_reg_write8() へ渡すゲストメモリアクセサ。リクエストヘッダは
 * スーパーバイザスタック上だが転送バッファは GVRAM 等を指しうるため、RAM 直参照ではなく
 * バス経由にする。read8/write8 本体より前方で使うため、ここで前方宣言する。 */
static uint8_t hostdrv_rd_mem_read8(void *ctx, uint32_t addr);
static void hostdrv_rd_mem_write8(void *ctx, uint32_t addr, uint8_t val);

/*
 * アドレス空間ごとのデコードはここに集め、CPU/デバッグのアクセス幅ラッパーから
 * 分離する。read8()/write8() は優先順位だけを決め、各ハンドラはその領域固有の
 * read-clear やレジスタ副作用を保持する。
 */
static uint8_t bus_read_ram8(const x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_gvram8(const x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_tvram8(const x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_crtc8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_mmio8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_opm_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_pcm_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_ppi_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_fdc_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_sasi_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_scsi_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_scc_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_mmio_ioi_read8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_expansion8(x68k_bus_t *b, uint32_t addr);
static uint8_t bus_read_rom8(const x68k_bus_t *b, uint32_t addr);

/* 有効な FPU-CIR の32-byte window 内か。無効時は常に偽（XB1 全域がバスエラー）。 */
static inline bool fpu_cir_in_window(const x68k_bus_t *b, uint32_t addr) {
    return b->fpu_cir && addr >= b->fpu_cir_base && addr < b->fpu_cir_base + FPU_CIR_SIZE;
}

static inline uint8_t read8(x68k_bus_t *b, uint32_t addr) {
    if (addr < RAM_BASE + ram_limit(b))
        return bus_read_ram8(b, addr);
    if (addr >= GVRAM_BASE && addr < GVRAM_BASE + GVRAM_SIZE)
        return bus_read_gvram8(b, addr);
    if (addr >= TVRAM_BASE && addr < TVRAM_BASE + TVRAM_SIZE)
        return bus_read_tvram8(b, addr);
    if (addr >= CRTC_BASE && addr < CRTC_BASE + CRTC_SIZE)
        return bus_read_crtc8(b, addr);
    /* 0xE90000–0xE9FFFF: I/O デバイス群（XEiJ MMD 配置準拠） */
    if (addr >= OPM_BASE && addr < DEV_BASE)
        return bus_read_mmio8(b, addr);
    /* $EA0000-$EAFFFF: 拡張スロット領域。HOSTDRV($EA0200-)/HOSTFILE($EA0100-)の
     * 裏口ウィンドウとRTCテストバイト$EAFFEBだけが実体を持ち、残りはフォールバック。
     * 範囲は DEV_BASE-DEV_END で判定する（個別ウィンドウの定数で範囲を組むと
     * HOSTFILE_BASE < HOSTDRV_RD_BASE のため始点>終点の空範囲になりうる）。 */
    if (addr >= DEV_BASE && addr < DEV_END)
        return bus_read_expansion8(b, addr);
    /* $EB0000-$EBFFFF: スプライトスクロールレジスタ+BGコントロール+画面モードレジスタ
     * +PCGエリア+BGデータエリア。 */
    if (addr >= SPRITE_BASE && addr < SPRITE_END)
        return b->sp_ram[addr - SPRITE_BASE];
    /* XB3 への CPU アクセスは先にバスエラーになる。0x00 は MCP の非 CPU
     * アクセスに対する副作用のない戻り値。 */
    if (addr >= SPRITE_END && addr < SRAM_BASE)
        return 0x00u;
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE) {
        if (b->boot_dev_watch_armed && addr == 0xED0018u)
            b->boot_dev_read_seen = 1;
        return b->sram[addr - SRAM_BASE];
    }
    if (addr >= IOCS_ROM_BASE)
        return bus_read_rom8(b, addr);
    return 0xFF;
}

static uint8_t bus_read_ram8(const x68k_bus_t *b, uint32_t addr) {
    return b->ram[addr - RAM_BASE];
}

static uint8_t bus_read_gvram8(const x68k_bus_t *b, uint32_t addr) {
    return b->gvram[addr - GVRAM_BASE];
}

static uint8_t bus_read_tvram8(const x68k_bus_t *b, uint32_t addr) {
    return b->tvram[addr - TVRAM_BASE];
}

static uint8_t bus_read_mmio8(x68k_bus_t *b, uint32_t addr) {
    /* OPM $E90001/$E90003（A0を無視し常にステータスを返す） */
    if (addr >= OPM_BASE && addr < PCM_BASE)
        return bus_mmio_opm_read8(b, addr);
    /* PCM ADPCM(MSM6258) $E92001/$E92003 */
    if (addr >= PCM_BASE && addr < FDC_BASE)
        return bus_mmio_pcm_read8(b, addr);
    /* PPI ポートA/B $E9A001/$E9A003（ジョイスティック#1/#2）。ROM _JOYGET は
     * ここを無加工でパススルーするだけなので、バス側で保持する状態をそのまま
     * 返す。ポートCより先に判定する。 */
    if (addr == PPI_BASE + 0x0001u || addr == PPI_BASE + 0x0003u ||
        (addr >= PPI_BASE && addr < IOI_BASE))
        return bus_mmio_ppi_read8(b, addr);
    /* FDC (0xE94000–0xE95FFF) */
    if (addr >= FDC_BASE && addr < HDC_BASE)
        return bus_mmio_fdc_read8(b, addr);
    /* HDC (0xE96001-0xE96007, SASI互換): IPLROM起動時ブート専用経路 */
    if (addr >= HDC_BASE && addr < SPC_BASE)
        return bus_mmio_sasi_read8(b, addr);
    /* SPC (0xE96021-0xE9603F, 内蔵SCSI) */
    if (addr >= SPC_BASE && addr < SPC_BASE + SPC_SIZE)
        return bus_mmio_scsi_read8(b, addr);
    /* SCC (0xE98000-0xE98007, ch B=マウス/ch A=RS-232C) */
    if (addr >= SCC_BASE && addr < SCC_BASE + SCC_SIZE)
        return bus_mmio_scc_read8(b, addr);
    /* IOI (0xE9C000–0xE9DFFF) */
    if (addr >= IOI_BASE && addr < XB1_BASE)
        return bus_mmio_ioi_read8(b, addr);
    /* $E9E000-$E9FFFF (XB1 拡張ボード領域1): addr_in_bus_error_gap() 側でバス
     * エラーになるため、CPU 命令経由のアクセスはここへ到達する前に
     * mc68k_signal_bus_error() 済み。ここでの 0x00 固定は MCP 裏口アクセス向け。
     * 有効な FPU-CIR window だけは gap から外れ、未実装 CIR は読出し全1。 */
    if (fpu_cir_in_window(b, addr))
        return 0xFFu;
    return 0x00u;
}

static uint8_t bus_read_crtc8(x68k_bus_t *b, uint32_t addr) {
    if (addr >= 0xE88001u && addr <= 0xE8802Fu && (addr & 1u))
        return mfp_reg_read8(b, addr);
    if (addr >= RTC_BASE && addr < RTC_BASE + RTC_SIZE && (addr & 1u))
        return rtc_reg_read8(&b->rtc, addr);
    /* 高速クリアの完了ビットは、最初の読出しで1を返してから消費する。 */
    if (addr == 0xE80481u && b->crtc.rapid_clear_pending) {
        b->crtc.rapid_clear_pending = 0;
        uint8_t v = b->crtc_regs[addr - CRTC_BASE];
        b->crtc_regs[addr - CRTC_BASE] = (uint8_t)(v & ~0x02u);
        return v;
    }
    if (addr >= 0xE8E000u && addr <= 0xE8FFFFu &&
        (addr & SYSPORT_MIRROR_MASK) == SYSPORT4_OFFSET) {
        switch (b->timing_profile) {
        case X68K_TIMING_PROFILE_X68030:
        case X68K_TIMING_PROFILE_060TURBO:
        case X68K_TIMING_PROFILE_040TURBO:
            return SYSPORT4_ID_X68030;
        default:
            return b->cpu_hz <= SYSPORT4_SLOW_MAX_HZ ? SYSPORT4_ID_SLOW : SYSPORT4_ID_FAST;
        }
    }
    return b->crtc_regs[addr - CRTC_BASE];
}

static uint8_t bus_mmio_opm_read8(x68k_bus_t *b, uint32_t addr) {
    (void)addr;
    return opm_status_read8(&b->opm);
}

static uint8_t bus_mmio_pcm_read8(x68k_bus_t *b, uint32_t addr) {
    return adpcm_pcm_read8(&b->adpcm, addr - PCM_BASE);
}

static uint8_t bus_mmio_ppi_read8(x68k_bus_t *b, uint32_t addr) {
    /* ポートC bit4/bit5=1でジョイスティック#1/#2の操作状態を通知しなくなる
     * （『Inside X68000』ジョイスティック章2-2節）。禁止中は未接続と同じ全bit1。 */
    if (addr == PPI_BASE + 0x0001u) {
        uint8_t v = (b->adpcm.ppi_c & PPI_PC_JOY1_DISABLE) ? JOY_PORT_RELEASED : b->joy_port[0];
        /* IOA5/IOA6=1で#1のトリガA/B信号線をLowにする（同章2-2節）。
         * 操作禁止との優先関係は資料に記述が無く、信号線を直接引くものとして
         * 禁止中でも押下扱いにする（実機/XEiJ未確認）。 */
        if (b->adpcm.ppi_c & PPI_PC_JOY1_TRG_A_FORCE)
            v &= (uint8_t)~JOY_BIT_TRG_A;
        if (b->adpcm.ppi_c & PPI_PC_JOY1_TRG_B_FORCE)
            v &= (uint8_t)~JOY_BIT_TRG_B;
        return v;
    }
    if (addr == PPI_BASE + 0x0003u)
        return (b->adpcm.ppi_c & PPI_PC_JOY2_DISABLE) ? JOY_PORT_RELEASED : b->joy_port[1];
    return adpcm_ppi_read8(&b->adpcm, addr - PPI_BASE);
}

static uint8_t bus_mmio_fdc_read8(x68k_bus_t *b, uint32_t addr) {
    uint32_t off = addr - FDC_BASE;
    if (off == 0x0001u)
        return fdc_msr(&b->fdc);
    if (off == 0x0003u)
        return fdc_read_data(&b->fdc);
    if (off == 0x0005u) {
        /* ドライブコントロールの選択ビットに対応する DISK IN を論理和で返す。
         * アクセスドライブ選択($E94007)とは別のレジスタであるため参照しない。 */
        uint8_t result = 0;
        for (int i = 0; i < 4; i++) {
            if ((b->drv_ctrl_latch & (uint8_t)(1u << i)) && b->fdc.disk[i])
                result |= 0x80u;
        }
        return result;
    }
    return off == 0x0007u ? 0xFFu : 0x00u;
}

static uint8_t bus_mmio_sasi_read8(x68k_bus_t *b, uint32_t addr) {
    return sasi_read8(&b->hdc_in, addr - HDC_BASE);
}

static uint8_t bus_mmio_scsi_read8(x68k_bus_t *b, uint32_t addr) {
    return scsi_read8(&b->spc_in, addr - SPC_BASE);
}

static uint8_t bus_mmio_scc_read8(x68k_bus_t *b, uint32_t addr) {
    return scc_read8(&b->scc, addr);
}

static uint8_t bus_mmio_ioi_read8(x68k_bus_t *b, uint32_t addr) {
    uint32_t off = addr - IOI_BASE;
    if (off == 0x0001u)
        return (uint8_t)((b->ioi_signal | b->ioi_enable) & 0xFFu);
    if (off == 0x0003u)
        return b->ioi_vector;
    return 0x00u;
}

static uint8_t bus_read_expansion8(x68k_bus_t *b, uint32_t addr) {
    /*
     * DEV 領域 (0xEA0000–0xEAFFFF): 拡張スロット部分（HOSTDRV/HOSTFILE裏口ウィンドウと
     * 0xEAFFEB を除く）は addr_in_bus_error_gap() 側でバスエラーになるため、CPU 命令
     * 経由のアクセスはここへ到達する前に mc68k_signal_bus_error() 済み（戻り値は破棄
     * される）。ここでの 0x00 固定は MCP read_memory/write_memory 等の裏口アクセス
     * （cpu_read8/write8 を経由せず read8/write8 を直接呼ぶ）向けのフォールバック。
     * 0xEAFFEB (RTC テストバイト) のみ書き込みを保持して読み返す。
     */
    if (addr >= HOSTDRV_RD_BASE && addr < HOSTDRV_RD_END)
        return hostdrv_remote_reg_read8(&b->hostdrv_rd, addr - HOSTDRV_RD_BASE);
    if (addr >= HOSTFILE_BASE && addr < HOSTFILE_END)
        return hostfile_reg_read8(&b->hostfile, addr - HOSTFILE_BASE);
    if (addr >= DEV_BASE && addr < DEV_END) {
        if (addr >= 0xEAFF81u && addr <= 0xEAFF89u && (addr & 1u))
            return b->extended_area_set[(addr - 0xEAFF81u) >> 1];
        return (addr == 0xEAFFEBu) ? b->dev_rtc_eb : 0x00u;
    }
    return 0x00u;
}

static uint8_t bus_read_rom8(const x68k_bus_t *b, uint32_t addr) {
    /*
     * $FC0000–$FC1FFF: 内蔵 SCSI BIOS ROM（実ROM）。
     * 実機 XVI では ROM の $FF02D8 が
     *   movea.l $ed000c.l, a0   ; A0 = $FC0000 (SRAM デフォルト)
     *   movea.l (a0), a3         ; A3 = [$FC0000] = SCSI BIOS エントリポインタ
     *   jsr (a3)
     * として呼び出す。$FC2000–$FDFFFF は未使用領域として $FF を返す
     * （『Inside X68000』、実機の未実装ROMソケット相当）。
     */
    if (addr >= 0xFC0000u && addr < 0xFC0000u + SCSI_ROM_SIZE)
        return b->scsi_rom[addr - 0xFC0000u];
    if (addr >= 0xFC0000u + SCSI_ROM_SIZE && addr < 0xFE0000u)
        return 0xFFu;
    /*
     * $FE0000–$FFFFFF: 128KB IPLROM（2 × 64KB）
     *   $FE0000–$FEFFFF → iocs_rom[0x0C0000..0x0CFFFF]（ROM Human）
     *   $FF0000–$FFFFFF → iocs_rom[0x0D0000..0x0DFFFF]（IPLROM）
     *
     * $F00000–$FBFFFF: CGROM 768KB（24x24 漢字は $F80000 以降にもある）
     */
    return b->iocs_rom[iocs_rom_offset(addr)];
}

/*
 * 非侵襲peek: I/Oレジスタのread-clear等の副作用を一切起こさない。デバッガの
 * メモリダンプ・逆アセンブル専用パス（mc68k_set_peek_hooks経由）。read8()と異なり
 * メモリで裏打ちされた領域だけを直接読み、I/O領域・未マップは副作用なしで
 * 0x00固定を返す。const x68k_bus_t* にできることが「副作用がない」ことの
 * コンパイラによる担保になる（x68k_bus_read_ram_byte()に既存の前例あり）。
 */
static inline uint8_t peek8(const x68k_bus_t *b, uint32_t addr) {
    if (addr < RAM_BASE + ram_limit(b))
        return b->ram[addr - RAM_BASE];
    if (addr >= GVRAM_BASE && addr < GVRAM_BASE + GVRAM_SIZE)
        return b->gvram[addr - GVRAM_BASE];
    if (addr >= TVRAM_BASE && addr < TVRAM_BASE + TVRAM_SIZE)
        return b->tvram[addr - TVRAM_BASE];
    /* MFPミラー($E88001-$E8802F奇数)はUSART/タイマレジスタの読み出しに副作用が
     * あるため除外し、RTC($E8A001-$E8A01F奇数)はrtc_reg_peek8()（副作用なし）
     * を専用に呼ぶため除外する。それ以外は末尾のI/O領域と同じ0x00固定へ落とす。 */
    if (addr >= CRTC_BASE && addr < CRTC_BASE + CRTC_SIZE &&
        !(addr >= 0xE88001u && addr <= 0xE8802Fu && (addr & 1u)) &&
        !(addr >= RTC_BASE && addr < RTC_BASE + RTC_SIZE && (addr & 1u)))
        return b->crtc_regs[addr - CRTC_BASE];
    if (addr >= RTC_BASE && addr < RTC_BASE + RTC_SIZE && (addr & 1u))
        return rtc_reg_peek8(&b->rtc, addr);
    if (addr >= SPRITE_BASE && addr < SPRITE_END)
        return b->sp_ram[addr - SPRITE_BASE];
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE)
        return b->sram[addr - SRAM_BASE];
    if (addr >= IOCS_ROM_BASE) {
        if (addr >= 0xFC0000u && addr < 0xFC0000u + SCSI_ROM_SIZE)
            return b->scsi_rom[addr - 0xFC0000u];
        if (addr >= 0xFC0000u + SCSI_ROM_SIZE && addr < 0xFE0000u)
            return 0xFFu;
        return b->iocs_rom[iocs_rom_offset(addr)];
    }
    /* FPU-CIR window は API を呼ばず全1（デバッガ読出しで状態を変えない） */
    if (fpu_cir_in_window(b, addr))
        return 0xFFu;
    return 0x00u; /* I/O・未マップ・CRTC内MFPミラー: 副作用なしで固定値を返す */
}

/*
 * RAM書き込みデバッグウォッチ。ram_watch_len が指定範囲内の addr への書き込みを
 * pal_log() へ記録する。既定 OFF（ram_watch_len==0）時は比較1回のみで即return。
 * write8() の RAM 分岐と bus_write_word/long() の RAM 高速パス（write8 を経由しない）
 * の両方から呼ぶ必要がある。
 */
static inline void ram_watch_check(x68k_bus_t *b, uint32_t addr, uint8_t old_val,
                                   uint8_t new_val) {
    if (!b->ram_watch_len || addr < b->ram_watch_addr ||
        addr >= b->ram_watch_addr + b->ram_watch_len)
        return;
    uint32_t pc = b->cpu ? mc68k_get_pc(b->cpu) : 0;
    /* pal_log() は platform 層依存のため使わない（本ファイルは platform 抜きで
     * 複数のユニットテストからリンクされる）。出力先はstderrで揃える。 */
    fprintf(stderr, "[RAMWATCH] PC=%06X addr=%06X %02X -> %02X\n", pc, addr, old_val, new_val);
}

/* システムポート#6 ($E8E00F) の電源断シーケンス検出。
 * 前面スイッチOFF（gpip2_level==1）の間だけ $00,$0F,$0F を数える。各書込み
 * 時点でスイッチ状態を見るため、ONの間の書込みはステートを待機へ戻す。 */
static void sysport6_poweroff_write(x68k_bus_t *b, uint8_t val) {
    if (!b->mfp.gpip2_level) {
        b->poweroff_seq = SYSPORT6_POWEROFF_SEQ0;
        return;
    }
    switch (b->poweroff_seq) {
    case SYSPORT6_POWEROFF_SEQ1:
        if (val == SYSPORT6_VAL_NEXT) {
            b->poweroff_seq = SYSPORT6_POWEROFF_SEQ2;
            return;
        }
        break;
    case SYSPORT6_POWEROFF_SEQ2:
        if (val == SYSPORT6_VAL_NEXT) {
            b->poweroff_seq = SYSPORT6_POWEROFF_SEQ0;
            b->power_off_requested = 1;
            return;
        }
        break;
    default:
        break;
    }
    b->poweroff_seq =
        (val == SYSPORT6_VAL_FIRST) ? SYSPORT6_POWEROFF_SEQ1 : SYSPORT6_POWEROFF_SEQ0;
}

static inline void write8(x68k_bus_t *b, uint32_t addr, uint8_t val) {
    if (addr < RAM_BASE + ram_limit(b)) {
        if (b->ram_watch_len)
            ram_watch_check(b, addr, b->ram[addr - RAM_BASE], val);
        b->ram[addr - RAM_BASE] = val;
        return;
    }
    if (addr >= GVRAM_BASE && addr < GVRAM_BASE + GVRAM_SIZE) {
        /* 素の配列書き込み。副作用を足すなら pmap_page_host の GVRAM 分岐を外すこと */
        b->gvram[addr - GVRAM_BASE] = val;
        return;
    }
    if (addr >= TVRAM_BASE && addr < TVRAM_BASE + TVRAM_SIZE) {
        uint32_t off = addr - TVRAM_BASE;
        /* CRTC R23 アクセスマスク（$E8002E）。『Inside X68000』 図29/p.236:
         *   16bit マスク。**マスク=0 のビットだけが変更される**（=1 は保護）。
         *   1 ワード=水平16px。高バイト crtc_regs[0x2E]=偶数アドレス(px0-7)、
         *   低バイト crtc_regs[0x2F]=奇数アドレス(px8-15)。
         *   _TXYLINE/_TXLINE はこのマスクで 1px だけ立てて垂直/斜め線を描く。
         *   普段は R23=0（全ビット変更可）なので通常描画は無影響。
         *   XEiJ 準拠: ビットマスクは R21 bit9（$2A bit1）が ON のときだけ適用する。
         *   OFF のときは R23 に残骸があっても全ビット書き換え可（消去取りこぼし防止）。 */
        uint8_t mask = (b->crtc_regs[0x2A] & 0x02u)
                           ? ((addr & 1u) ? b->crtc_regs[0x2Fu] : b->crtc_regs[0x2Eu])
                           : 0x00u;
        /* CRTC R21 同時アクセス（SA）。『Inside X68000』 図29/p.236:
         *   R21 bit8     = SA ON/OFF      → crtc_regs[0x2A] bit0
         *   R21 bits 7-4 = AP（同時アクセス対象プレーン T0-T3 の選択）→ crtc_regs[0x2B] bits
         * 7-4 AP ビット=1 のプレーンに 1 回の書き込みで同時書き込みされる（アクティブ・ハイ）。
         * 例: ROM のテキスト描画は R21=$0133 → 低バイト$33 → AP=$3 →
         * プレーン0,1（白＝コード3）。 SA 有効時はアドレスのプレーン部を無視し
         * within（プレーン内オフセット）で AP 選択プレーンにのみ書く。 */
        if (b->crtc_regs[0x2A] & 0x01u) {
            uint8_t ap = (uint8_t)((b->crtc_regs[0x2B] >> 4) & 0x0Fu);
            uint32_t within = off & 0x1FFFFu;
            for (int pl = 0; pl < 4; pl++) {
                if ((ap >> pl) & 1u) { /* AP_p=1 → このプレーンを選択 */
                    uint8_t *cell = &b->tvram[pl * 0x20000u + within];
                    *cell = (uint8_t)((*cell & mask) | (val & (uint8_t)~mask));
                }
            }
        } else {
            uint8_t *cell = &b->tvram[off];
            *cell = (uint8_t)((*cell & mask) | (val & (uint8_t)~mask));
        }
        return;
    }
    if (addr >= CRTC_BASE && addr < CRTC_BASE + CRTC_SIZE) {
        if (addr >= 0xE88001u && addr <= 0xE8802Fu && (addr & 1u)) {
            mfp_reg_write8(b, addr, val);
            return;
        }
        if (addr >= RTC_BASE && addr < RTC_BASE + RTC_SIZE && (addr & 1u)) {
            rtc_reg_write8(&b->rtc, addr, val);
            return;
        }
        if (dmac_write8(b, addr, val))
            return;
        /* エリアセット ($E86001、奇数バイトのみ有効): ユーザモードから
         * アクセス可能なメインメモリ上限を再設定する。
         * 境界式は実機/XEiJ準拠: ((val & 0xFF) + 1) << 13 （8KB粒度、$2000-$200000）。 */
        if (addr == 0xE86001u) {
            b->super_area_limit = (uint32_t)((val + 1u) << 13);
            x68k_bus_page_map_refresh(b);
        }
        b->crtc_regs[addr - CRTC_BASE] = val;
        if (addr == SYSPORT6_ADDR)
            sysport6_poweroff_write(b, val);
        /* システムポート#5 ($E8E009, X68030): ROM/DRAM ウェイト設定。
         *bit7-4=ROM wait、bit3-0=DRAM wait、いずれも
         * 設定値+2ウェイト。DRAMの$0はスタティックカラム動作。 */
        if (addr == 0xE8E009u)
            x68k_bus_apply_wait_port(b, val);
        /* パレットARGBキャッシュの更新。該当1エントリだけをeagerに再変換する。 */
        {
            uint32_t off = addr - CRTC_BASE;
            if (off >= 0x2000u && off < 0x2200u) {
                unsigned idx = (off - 0x2000u) / 2u;
                uint16_t grb = ((uint16_t)b->crtc_regs[0x2000u + idx * 2u] << 8) |
                               b->crtc_regs[0x2000u + idx * 2u + 1u];
                b->crtc.gfx_pal_raw[idx] = grb;
                b->crtc.gfx_pal_argb[idx] = grb555_to_argb(grb);
                b->crtc.pal_dirty = 1;
                if (b->crtc.raster.valid && b->crtc.raster.line >= b->crtc.raster.vdisp_start &&
                    b->crtc.raster.line <
                        b->crtc.raster.vdisp_start + b->crtc.raster.vdisp_lines)
                    b->crtc.pal_vdisp_gfx_writes++;
            } else if (off >= 0x2200u && off < 0x2400u) {
                unsigned idx = (off - 0x2200u) / 2u;
                uint16_t grb = ((uint16_t)b->crtc_regs[0x2200u + idx * 2u] << 8) |
                               b->crtc_regs[0x2200u + idx * 2u + 1u];
                b->crtc.text_pal_raw[idx] = grb;
                b->crtc.text_pal_argb[idx] = grb555_to_argb(grb);
                b->crtc.pal_dirty = 1;
                if (b->crtc.raster.valid && b->crtc.raster.line >= b->crtc.raster.vdisp_start &&
                    b->crtc.raster.line <
                        b->crtc.raster.vdisp_start + b->crtc.raster.vdisp_lines)
                    b->crtc.pal_vdisp_text_writes++;
            }
        }
        /* アクションポート ($E80481) bit3 = テキストラスタコピー。
         * XEiJ CRTC.java（crtRasterCopyOn/crtDoRasterCopy）・Technical
         * Guideによれば、bit3はMPUがラスタコピー機能をON/OFFする単なるスイッチで、
         * 0→1のトリガーでもCRTCの動作終了フラグでもなく、0を書くまで1のまま保持
         * される。ここでの書き込み時即時コピーはMPX68K commit f59c07dが残した
         * 互換経路と同じもので、実際の継続実行はbit3が1の間の毎水平フロント
         * ポーチ（crtc.c crtc_raster_emit_line()）が担う。 */
        if (addr == 0xE80481u && (val & 0x08u))
            x68k_bus_crtc_raster_copy(b);
        /* アクションポート ($E80481) bit1 = グラフィック画面高速クリアトリガ。
         * 実機は1フレーム（インターレース時2フレーム）かけて実行し、完了すると bit1 が
         * 自動的に0へ復帰する。ix68k は bit3 と同じく対象行のクリア自体は
         * 同期即時実行するが、bit1 の読み戻しはここでは落とさず「保留」フラグだけ
         * 立てる。ソフトによっては「トリガ後まずbit1が1になったことを確認してから
         * 0を待つ」という2段階ポーリングを行う（電脳倶楽部Vol.37のブートローダ等）ため、
         * 書き込み直後に即0へ落とすと1が一度も観測できず別の無限ループになる。
         * 実際の0への復帰はread8() CRTC分岐（次にこのポートが読まれた瞬間）で行う。 */
        if (addr == 0xE80481u && (val & 0x02u)) {
            x68k_bus_crtc_rapid_clear(b);
            b->crtc.rapid_clear_pending = 1;
        }
        return;
    }
    /* 0xE90000–0xE9FFFF: I/O デバイス群 */
    if (addr >= OPM_BASE && addr < DEV_BASE) {
        /* OPM $E90001(レジスタ選択)/$E90003(データ)。$1BのCT1/CT2は従来通り
         * adpcm.cが独立追跡、合成コアへはopm.cが供給する（意図的な二重管理）。
         * x68k_bus_opm_reg_write8() はFMIRQ線の変化を検知しMFPへ通知する。 */
        if (addr >= OPM_BASE && addr < PCM_BASE) {
            adpcm_opm_write8(&b->adpcm, addr - OPM_BASE, val);
            x68k_bus_opm_reg_write8(b, addr - OPM_BASE, val);
            return;
        }
        /* PCM ADPCM(MSM6258) $E92001/$E92003 */
        if (addr >= PCM_BASE && addr < FDC_BASE) {
            adpcm_pcm_write8(&b->adpcm, addr - PCM_BASE, val);
            return;
        }
        /* PPI ポートC $E9A005/$E9A007 */
        if (addr >= PPI_BASE && addr < IOI_BASE) {
            adpcm_ppi_write8(&b->adpcm, addr - PPI_BASE, val);
            return;
        }
        /* FDC (0xE94000–0xE95FFF) */
        if (addr >= FDC_BASE && addr < HDC_BASE) {
            uint32_t off = addr - FDC_BASE;
            if (off == 0x0001u || off == 0x0003u) {
                int prev_irq = b->fdc.irq;
                fdc_write_data(&b->fdc, b->ram, b->crtc_regs, val);
                if (!prev_irq && b->fdc.irq)
                    ioi_fdc_rise(b);
                else if (prev_irq && !b->fdc.irq)
                    ioi_fdc_fall(b);
                return;
            }
            /* FDD 割り込み信号クリア (XEiJ fdcWriteDriveControl/DriveSelect 準拠) */
            if (off == 0x0005u) {
                ioi_fdd_fall(b);
                /* イジェクトプロトコル (『Inside X68000』):
                 * bit5=EJECT ON/OFF, bit3-0=対象ドライブ。ドライブビットが
                 * 1→0 に変化した瞬間、EJECT ON/OFFが立っていれば実際に排出する。
                 * 例: $23 書込後に $20 を書込むとドライブ0・1が同時排出される。 */
                uint8_t prev = b->drv_ctrl_latch;
                uint8_t fell = prev & (uint8_t)~val & 0x0Fu;
                if ((val & 0x20u) && fell) {
                    for (int i = 0; i < 4; i++)
                        if (fell & (uint8_t)(1u << i))
                            fdc_eject_drive(b, i);
                }
                /* EJECT MASK(bit6): 対象ドライブの物理イジェクトボタン無効化状態を
                 * bit6の値で確定する（『Inside X68000』 図5）。bit5(EJECT ON/OFF)
                 * と同じくドライブ選択ビットの1→0エッジで動作。 */
                if (fell) {
                    for (int i = 0; i < 4; i++) {
                        if (fell & (uint8_t)(1u << i)) {
                            if (val & 0x40u)
                                b->fdc_eject_masked_bits |= (uint8_t)(1u << i);
                            else
                                b->fdc_eject_masked_bits &= (uint8_t)~(1u << i);
                        }
                    }
                }
                fdc_set_option(&b->fdc, val);
                b->drv_ctrl_latch = val;
                return;
            }
            if (off == 0x0007u) {
                ioi_fdd_fall(b);
                b->fdc_drive_sel = val & 3;
                fdc_set_density(&b->fdc, (val & FDC_DRVSEL_2DD_BIT) != 0);
                /* MOTOR ON(bit7)。アクセスランプ用
                 * （『Inside X68000』 図6: 1=モータON→赤、0=モータOFF→緑）。 */
                b->fdc_motor_on = (val & 0x80u) != 0;
                return;
            }
            return;
        }
        /* HDC (0xE96001-0xE96007, SASI互換): IPLROM起動時ブート専用経路 */
        if (addr >= HDC_BASE && addr < SPC_BASE) {
            sasi_write8(&b->hdc_in, addr - HDC_BASE, val);
            return;
        }
        /* SPC (0xE96021-0xE9603F, 内蔵SCSI) */
        if (addr >= SPC_BASE && addr < SPC_BASE + SPC_SIZE) {
            scsi_write8(&b->spc_in, addr - SPC_BASE, val);
            return;
        }
        /* SCC (0xE98000-0xE98007, ch B=マウス/ch A=RS-232C) */
        if (addr >= SCC_BASE && addr < SCC_BASE + SCC_SIZE) {
            scc_write8(&b->scc, addr, val);
            return;
        }
        /* IOI (0xE9C000–0xE9DFFF) */
        if (addr >= IOI_BASE && addr < XB1_BASE) {
            uint32_t off = addr - IOI_BASE;
            if (off == 0x0001u) {
                uint8_t new_en = val & 0x0Fu;
                uint8_t falled = b->ioi_enable & ~new_en;
                uint8_t raised = (uint8_t)(~b->ioi_enable) & new_en;
                b->ioi_enable = new_en;
                if (falled & IOI_FDC_REQUEST)
                    b->ioi_request &= ~IOI_FDC_REQUEST;
                else if ((raised & IOI_FDC_REQUEST) && (b->ioi_signal & IOI_FDC_SIGNAL))
                    b->ioi_request |= IOI_FDC_REQUEST;
                if (falled & IOI_FDD_REQUEST)
                    b->ioi_request &= ~IOI_FDD_REQUEST;
                else if ((raised & IOI_FDD_REQUEST) && (b->ioi_signal & IOI_FDD_SIGNAL))
                    b->ioi_request |= IOI_FDD_REQUEST;
                return;
            }
            if (off == 0x0003u) {
                b->ioi_vector = val & 0xFCu;
                return;
            }
            return;
        }
        return; /* その他: 書き込み無視 */
    }
    if (addr >= HOSTDRV_RD_BASE && addr < HOSTDRV_RD_END) {
        hostdrv_rd_mem_t mem = {b, hostdrv_rd_mem_read8, hostdrv_rd_mem_write8};
        hostdrv_remote_reg_write8(&b->hostdrv_rd, &mem, addr - HOSTDRV_RD_BASE, val);
        return;
    }
    if (addr >= HOSTFILE_BASE && addr < HOSTFILE_END) {
        hostfile_reg_write8(&b->hostfile, b->ram, RAM_BUF_SIZE, addr - HOSTFILE_BASE, val);
        return;
    }
    if (addr >= DEV_BASE && addr < DEV_END) {
        if (addr >= 0xEAFF81u && addr <= 0xEAFF89u && (addr & 1u)) {
            b->extended_area_set[(addr - 0xEAFF81u) >> 1] = val;
            x68k_bus_page_map_refresh(b);
        } else if (addr == 0xEAFFEBu) {
            b->dev_rtc_eb = val;
        }
        return;
    }
    /* $EB0000-$EBFFFF: スプライトスクロールレジスタ+BGコントロール+画面モードレジスタ
     * +PCGエリア+BGデータエリア。 */
    if (addr >= SPRITE_BASE && addr < SPRITE_END) {
        b->sp_ram[addr - SPRITE_BASE] = val;
        return;
    }
    /* $EC0000-$ECFFFF (XB3 拡張ボード領域3): CPU 経由は addr_in_bus_error_gap() で
     * バスエラー済み。ここでの書き込み無視は MCP 裏口アクセス向け。 */
    if (addr >= SPRITE_END && addr < SRAM_BASE)
        return;
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE) {
        b->sram[addr - SRAM_BASE] = val;
        /* $ED0008-$ED000B は ram_limit()。変わると RAM の範囲（表）が変わる */
        if (addr >= SRAM_BASE + SRAM_REAL_OFFSET + 0x08u &&
            addr < SRAM_BASE + SRAM_REAL_OFFSET + 0x0Cu)
            x68k_bus_page_map_refresh(b);
        return;
    }
    if (addr >= IOCS_ROM_BASE)
        return; /* ROM: 書き込み無視 */
}

/*
 * HOSTDRVが受け取るアドレスはゲストの論理アドレス。060turbo.sysの-lsなどが
 * リクエストヘッダのあるページをローカルメモリへ移すと論理と物理が食い違うため、
 * スーパーバイザデータ(FC=5)として変換してから物理アクセスする。
 * MMU無効やCPU未配線のときは恒等変換になる。
 */
#define HOSTDRV_FC_SUPER_DATA 5u
static uint32_t hostdrv_guest_to_physical(x68k_bus_t *b, uint32_t addr) {
    mc68k_debug_translate_result_t tr;
    if (b->cpu && mc68k_debug_translate(b->cpu, addr, HOSTDRV_FC_SUPER_DATA, 1, &tr) == 0)
        return tr.physical;
    return addr;
}

static uint8_t hostdrv_rd_mem_read8(void *ctx, uint32_t addr) {
    x68k_bus_t *b = ctx;
    return x68k_bus_read8_full(b, hostdrv_guest_to_physical(b, addr));
}

static void hostdrv_rd_mem_write8(void *ctx, uint32_t addr, uint8_t val) {
    x68k_bus_t *b = ctx;
    x68k_bus_write8_full(b, hostdrv_guest_to_physical(b, addr), val);
}

/*
 * 実機で本当に何も繋がっていない範囲か判定する:
 *   ram_limit()-$BFFFFF: 未増設メモリ
 *   $EE0000-$EFFFFF    : 未使用（『Inside X68000』 図1参照）
 *   $E9E000-$E9FFFF（XB1 拡張ボード領域1）・DEV領域の拡張スロット部分（$EA0000-
 *     $EAFFFF、HOSTDRV/HOSTFILE裏口ウィンドウとRTCテストバイト$EAFFEBを除く）・
 *     $EC0000-$ECFFFF（XB3 拡張ボード領域3）: 実機では拡張ボード未装着時にバス
 *     エラーになる。未接続領域を 0x00 固定にすると、デバイス検出がレディ待ちへ
 *     進んで停止するため、同期例外を返す必要がある。
 * $EB0000-$EBFFFF（スプライトコントローラ/PCG/BGエリア）は実機ではチップが実在する
 * ため対象外（IPLROM が起動時に $EB080C 等の画面モードレジスタへ実際に書き込むことを
 * 確認済み）。read8/write8 側で $EB0000-$EBFFFF は sp_ram 実配列として扱う。
 * 実在する I/O チップや意図的にマップした領域は対象外とする。
 */
static inline int addr_in_bus_error_gap(x68k_bus_t *b, uint32_t addr) {
    return (addr >= (RAM_BASE + ram_limit(b)) && addr < GVRAM_BASE) ||
           (addr >= (SRAM_BASE + SRAM_SIZE) && addr < IOCS_ROM_BASE) ||
           (addr >= XB1_BASE && addr < DEV_BASE && !fpu_cir_in_window(b, addr)) ||
           (addr >= DEV_BASE && addr < DEV_END &&
            !(addr >= HOSTDRV_RD_BASE && addr < HOSTDRV_RD_END) &&
            !(addr >= HOSTFILE_BASE && addr < HOSTFILE_END) &&
            !(addr >= 0xEAFF81u && addr <= 0xEAFF89u && (addr & 1u)) && addr != 0xEAFFEBu) ||
           (addr >= SPRITE_END && addr < SRAM_BASE);
}

/*
 * ユーザモードからアクセス不可の領域か判定する（『Inside X68000』
 * 5-5節「MFC, DFC, BFC」）。実機/XEiJ準拠のモデル: ユーザモードでアクセス可能
 * なのは「メインメモリのうち super_area_limit 以上、GVRAM_BASE未満」の範囲だけ。
 * それ以外（低位 [0, super_area_limit) と GVRAM_BASE 以降の VRAM/I/O/SRAM/ROM
 * 全域）はスーパーバイザ専用で、ユーザアクセスするとバスエラーになる。
 * エリアセット($E86001)に依存するのは低位境界のみで、VRAM/I/O側は固定。
 */
static inline int addr_super_protected(x68k_bus_t *b, uint32_t addr) {
    if (addr >= 0x200000u && addr < 0xC00000u) {
        unsigned block = (addr - 0x200000u) >> 18;
        return (b->extended_area_set[block >> 3] >> (block & 7)) & 1u;
    }
    return addr < b->super_area_limit || addr >= GVRAM_BASE;
}

/*
 * 未マップ・ユーザモード保護領域アクセスを検出したら false（バスエラー）を返す
 * 共通ヘルパー。cpu_read8/cpu_write8 だけでなく、read_word/read_long/
 * write_word/write_long の RAM/GVRAM 高速パス（read8/write8 を経由せず配列に
 * 直接アクセスする）からも呼ぶ必要がある。高速パスは「未マップの隙間」には
 * 元々入らない領域だが、ユーザモード保護は RAM 内・GVRAM 内どちらの高速パスにも
 * かかりうるため、この関数を先頭で呼んでから高速アクセスを行うこと。
 */
static inline bool super_protect_check_limit(x68k_bus_t *b, uint32_t addr, uint32_t limit) {
    if (local_memory_contains(b, addr))
        return true;
    /* 隙間の各条件は ram_limit() 以上のアドレスでしか成立しないため、
     * メインメモリ内なら隙間判定を省く（最頻の経路）。 */
    if (addr >= RAM_BASE + limit && addr_in_bus_error_gap(b, addr))
        return false;
    if (b->super_protect_enabled && !mc68k_is_supervisor(b->cpu) &&
        addr_super_protected(b, addr))
        return false;
    return true;
}

/* ram_limit() を呼び出し側で 1 回だけ求めて使い回す版が super_protect_check_limit()。 */
static inline bool super_protect_check(x68k_bus_t *b, uint32_t addr) {
    return super_protect_check_limit(b, addr, ram_limit(b));
}

/* RTC($E8A000-$E8A01F)の偶数アドレスへのバイトアクセスはバスエラー
 * （XEiJ MemoryMappedDevice.MMD_RTC_FIRST/NEXT mmdRbs/mmdRbz/mmdWb 準拠。
 * Xellent30の030セカンドモード相当はix68kに無いため常時適用する）。
 * ワード/ロングアクセスは bus_read_word/long 等の専用分岐が先に処理するため
 * ここには到達しない。 */
static inline bool rtc_even_byte_fault(uint32_t addr) {
    return addr >= RTC_BASE && addr < RTC_BASE + RTC_SIZE && !(addr & 1u);
}

/* cpu_read8/cpu_write8 は CPU バスコールバック専用: read8/write8 に加えて
 * super_protect_check() を呼ぶ。x68k_bus_readN/writeN（デバッグ/IOCS _B_PEEK 等の
 * 裏口アクセス）は read8/write8 を直接呼び、これを経由しないようにする
 * （CPU 実行と無関係のアクセスを誤ってバスエラー扱いしないため）。 */

static inline bool cpu_read8(x68k_bus_t *b, uint32_t addr, uint8_t *out) {
    addr = cpu_bus_addr(b, addr);
    if (local_memory_contains(b, addr)) {
        *out = b->local_memory[addr - LOCAL_MEMORY_BASE];
        return true;
    }
    bool ok = super_protect_check(b, addr) && !rtc_even_byte_fault(addr);
    *out = read8(b, addr);
    return ok;
}

static inline bool cpu_write8(x68k_bus_t *b, uint32_t addr, uint8_t val) {
    addr = cpu_bus_addr(b, addr);
    if (local_memory_contains(b, addr)) {
        b->local_memory[addr - LOCAL_MEMORY_BASE] = val;
        return true;
    }
    bool ok = super_protect_check(b, addr) && !rtc_even_byte_fault(addr);
    write8(b, addr, val);
    return ok;
}

/* ---- 物理ページ表 ----
 * 表のエントリは、ホストのコールバック（bus_read_*、bus_access_timing）と完全に等価な
 * ページだけを載せる。ウェイトは bus_access_timing() 自身をページの両端で呼んで求め、結果が
 * ページ内で一様なときだけ載せる（ram_limit
 * の境界ページなどは載らず、従来どおりコールバック）。 */

typedef struct {
    mc68k_page_wait_t wait;
    bool cacheable;
} pmap_timing_t;

#define PMAP_FC_SUPER_DATA 5u

static bool pmap_page_timing(x68k_bus_t *b, uint32_t phys, pmap_timing_t *out) {
    static const uint32_t offsets[2] = {0u, MC68K_PAGE_MASK};
    bool have = false;
    pmap_timing_t first = {0};
    for (unsigned o = 0; o < 2u; o++) {
        uint16_t w[2][2]; /* [read=1 / write=0][size 4=1 / size 1,2=0] */
        bool cacheable[2][2];
        for (unsigned rd = 0; rd < 2u; rd++) {
            for (unsigned lg = 0; lg < 2u; lg++) {
                const mc68k_bus_access_t a = {.physical = phys + offsets[o],
                                              .size = lg ? 4u : 2u,
                                              .fc = PMAP_FC_SUPER_DATA,
                                              .read = rd != 0u};
                mc68k_bus_timing_t t = {0, false};
                bus_access_timing(b, &a, &t);
                w[rd][lg] = t.wait_cycles;
                cacheable[rd][lg] = t.cacheable;
            }
        }
        /* cacheable は読み書き・size によらず一定でなければ載せない。 */
        if (cacheable[0][0] != cacheable[0][1] || cacheable[0][0] != cacheable[1][0] ||
            cacheable[0][0] != cacheable[1][1])
            return false;
        const pmap_timing_t cur = {.wait = {.read = w[1][0],
                                            .write = w[0][0],
                                            .read_long = w[1][1],
                                            .write_long = w[0][1]},
                                   .cacheable = cacheable[0][0]};
        if (!have) {
            first = cur;
            have = true;
        } else if (memcmp(&first.wait, &cur.wait, sizeof cur.wait) != 0 ||
                   first.cacheable != cur.cacheable) {
            return false;
        }
    }
    *out = first;
    return true;
}

/* ページの先頭から直接読める配列ポインタ。読み出しに副作用がなく、バスエラーにならない
 * （ユーザモード保護を除く）ページだけ返す。保護がページ内で一様でなければ NULL。 */
typedef enum {
    PMAP_KIND_NONE,
    PMAP_KIND_RAM,   /* メイン RAM・ローカルメモリ（ram_watch の対象になりうる） */
    PMAP_KIND_GVRAM, /* 副作用のない素の配列。書き込みも直接 */
    PMAP_KIND_ROM,   /* 読み出しのみ */
} pmap_kind_t;

static const uint8_t *pmap_page_host(x68k_bus_t *b, uint32_t phys, uint32_t rl,
                                     bool *super_only, pmap_kind_t *kind) {
    *super_only = false;
    *kind = PMAP_KIND_NONE;
    if (b->local_memory && phys >= LOCAL_MEMORY_BASE &&
        phys - LOCAL_MEMORY_BASE + MC68K_PAGE_MASK < b->local_memory_size) {
        *kind = PMAP_KIND_RAM;
        return b->local_memory + (phys - LOCAL_MEMORY_BASE);
    }
    const uint32_t a = phys & 0xFFFFFFu;
    const uint8_t *host = NULL;
    if (a + MC68K_PAGE_MASK < RAM_BASE + rl) {
        host = b->ram + (a - RAM_BASE);
        *kind = PMAP_KIND_RAM;
    } else if (b->gvram && a >= GVRAM_BASE && a + MC68K_PAGE_MASK < GVRAM_BASE + GVRAM_SIZE) {
        /* GVRAM の読み書きは配列への素のアクセスだけ（read8/write8/peek8/高速パスとも）。
         * 副作用（プレーン同時書き込み等）を足すなら、ここから外すこと。 */
        host = b->gvram + (a - GVRAM_BASE);
        *kind = PMAP_KIND_GVRAM;
    } else if (a >= IOCS_ROM_BASE && !(a >= 0xFC0000u && a < 0xFE0000u) && b->iocs_rom) {
        host = b->iocs_rom + iocs_rom_offset(a);
        *kind = PMAP_KIND_ROM;
    }
    if (!host)
        return NULL;
    if (b->super_protect_enabled) {
        const int p0 = addr_super_protected(b, a);
        const int p1 = addr_super_protected(b, a + MC68K_PAGE_MASK);
        if (p0 != p1)
            return NULL;
        *super_only = p0 != 0;
    }
    return host;
}

/* ram_watch の範囲と重なるメイン RAM のページ。ローカルメモリは ram_watch の対象外
 * （cpu_write8 のローカル分岐は監視しない）。ウォッチは 24bit アドレスで比べる。 */
static bool pmap_ram_watched(const x68k_bus_t *b, uint32_t phys) {
    if (!b->ram_watch_len || phys >= LOCAL_MEMORY_BASE)
        return false;
    const uint64_t page_end = (uint64_t)phys + MC68K_PAGE_SIZE;
    const uint64_t watch_end = (uint64_t)b->ram_watch_addr + b->ram_watch_len;
    return b->ram_watch_addr < page_end && watch_end > phys;
}

static int pmap_wait_class(mc68k_page_map_t *m, unsigned *classes, const mc68k_page_wait_t *w) {
    for (unsigned k = 0; k < *classes; k++)
        if (memcmp(&m->wait_table[k], w, sizeof *w) == 0)
            return (int)k;
    if (*classes >= MC68K_PAGE_WAIT_CLASSES)
        return -1;
    m->wait_table[*classes] = *w;
    return (int)(*classes)++;
}

static void pmap_fill_entry(x68k_bus_t *b, x68k_page_map_t *pm, unsigned *classes,
                            uint32_t phys, uint32_t rl, mc68k_page_entry_t *e) {
    memset(e, 0, sizeof *e);
    pmap_timing_t t;
    if (pmap_page_timing(b, phys, &t)) {
        const int k = pmap_wait_class(&pm->map, classes, &t.wait);
        if (k >= 0) {
            e->wait_class = (uint8_t)k;
            e->flags |= MC68K_PAGE_TIMING | (t.cacheable ? MC68K_PAGE_CACHEABLE : 0u);
        }
    }
    bool super_only;
    pmap_kind_t kind;
    const uint8_t *host = pmap_page_host(b, phys, rl, &super_only, &kind);
    if (host) {
        e->host = host;
        e->flags |= MC68K_PAGE_READ_DIRECT | (super_only ? MC68K_PAGE_SUPERVISOR_ONLY : 0u);
        const bool writable =
            kind == PMAP_KIND_GVRAM || (kind == PMAP_KIND_RAM && !pmap_ram_watched(b, phys));
        if (writable)
            e->flags |= MC68K_PAGE_WRITE_DIRECT;
    }
}

static bool pmap_local_page(const x68k_bus_t *b, uint32_t phys) {
    return b->local_memory && phys >= LOCAL_MEMORY_BASE &&
           phys - LOCAL_MEMORY_BASE + MC68K_PAGE_MASK < b->local_memory_size;
}

/* 状態から表を pm へ作る。pm->local は必要なら確保し直す。 */
static void pmap_build(x68k_bus_t *b, x68k_page_map_t *pm) {
    memset(pm->map.wait_table, 0, sizeof pm->map.wait_table);
    unsigned classes = 0;
    const uint32_t rl = ram_limit(b);
    for (unsigned i = 0; i < MC68K_PAGE_L2_COUNT; i++)
        pmap_fill_entry(b, pm, &classes, i << MC68K_PAGE_SHIFT, rl, &pm->main[i]);
    const unsigned slots = b->local_memory
                               ? (b->local_memory_size + X68K_PAGE_MAP_LOCAL_SLOT_BYTES - 1u) /
                                     X68K_PAGE_MAP_LOCAL_SLOT_BYTES
                               : 0u;
    if (slots != pm->local_slots) {
        free(pm->local);
        pm->local =
            slots ? calloc((size_t)slots * MC68K_PAGE_L2_COUNT, sizeof *pm->local) : NULL;
        pm->local_slots = pm->local ? slots : 0u;
    }
    for (unsigned k = 0; k < MC68K_PAGE_L1_COUNT; k++)
        pm->map.l1[k] = pm->main;
    for (unsigned sl = 0; sl < pm->local_slots; sl++) {
        mc68k_page_entry_t *tbl = pm->local + (size_t)sl * MC68K_PAGE_L2_COUNT;
        for (unsigned i = 0; i < MC68K_PAGE_L2_COUNT; i++) {
            const uint32_t phys = LOCAL_MEMORY_BASE + sl * X68K_PAGE_MAP_LOCAL_SLOT_BYTES +
                                  (i << MC68K_PAGE_SHIFT);
            if (pmap_local_page(b, phys))
                pmap_fill_entry(b, pm, &classes, phys, rl, &tbl[i]);
            else
                tbl[i] = pm->main[i]; /* スロット内のローカル範囲外は 24bit ミラー */
        }
        pm->map.l1[(LOCAL_MEMORY_BASE >> MC68K_PAGE_L1_SHIFT) + sl] = tbl;
    }
}

void x68k_bus_page_map_update_mask(x68k_bus_t *b) {
    if (!b->pmap)
        return;
    b->pmap->map.disable_mask = b->dmac.ch2_active ? X68K_PAGE_MAP_DISABLE_DMAC : 0u;
}

void x68k_bus_page_map_refresh(x68k_bus_t *b) {
    if (!b->pmap)
        return;
    pmap_build(b, b->pmap);
    x68k_bus_page_map_update_mask(b);
}

bool x68k_bus_page_map_verify(x68k_bus_t *b) {
    if (!b->pmap)
        return true;
    x68k_page_map_t *fresh = calloc(1, sizeof *fresh);
    if (!fresh)
        return false;
    pmap_build(b, fresh);
    const x68k_page_map_t *cur = b->pmap;
    bool ok =
        cur->local_slots == fresh->local_slots &&
        memcmp(cur->map.wait_table, fresh->map.wait_table, sizeof cur->map.wait_table) == 0 &&
        memcmp(cur->main, fresh->main, sizeof cur->main) == 0;
    if (ok && fresh->local_slots)
        ok = memcmp(cur->local, fresh->local,
                    (size_t)fresh->local_slots * MC68K_PAGE_L2_COUNT * sizeof *cur->local) == 0;
    for (unsigned k = 0; ok && k < MC68K_PAGE_L1_COUNT; k++) {
        const bool cur_main = cur->map.l1[k] == cur->main;
        const bool fresh_main = fresh->map.l1[k] == fresh->main;
        ok = cur_main == fresh_main;
    }
    free(fresh->local);
    free(fresh);
    return ok;
}

/* ---- mc68k_bus_t コールバック ---- */

/* x68k_bus_dmac_sync_to_now() の早期 return を呼び出し側へ展開する。ch2 が
 * 非アクティブなら同期は何もしないため、ch2_discard_left のクリアだけで済ませる。 */
static inline void bus_sync_dmac(x68k_bus_t *b) {
    if (!b->dmac.ch2_active) {
        b->dmac.ch2_discard_left = 0;
        return;
    }
    x68k_bus_dmac_sync_to_now(b);
}

static bool bus_read_byte(void *ctx, uint32_t addr, uint8_t *out) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bus_sync_dmac(b);
    return cpu_read8(b, addr, out);
}

/*
 * FPU-CIR の16-bit cycle。CPU アクセスがバスエラーになる cycle（ユーザモード保護違反）では
 * デバイスを呼ばない。CIR の read は副作用を持つため、バスエラー cycle で API を進めない。
 * 偶数アドレスの word/long だけがここへ来る（byte・奇数 word は API を呼ばず read8/write8 の
 * 全1/無視で完了する）。
 */
static bool fpu_cir_cycle_read(x68k_bus_t *b, uint32_t addr, uint16_t *out) {
    *out = FPU_CIR_OPEN_WORD;
    if (!super_protect_check(b, addr))
        return false;
    *out = fpu_cir_read16(&b->fpu_cir_state, b->fpu_cir, addr - b->fpu_cir_base);
    return true;
}

static bool fpu_cir_cycle_write(x68k_bus_t *b, uint32_t addr, uint16_t val) {
    if (!super_protect_check(b, addr))
        return false;
    fpu_cir_write16(&b->fpu_cir_state, b->fpu_cir, addr - b->fpu_cir_base, val);
    return true;
}

static inline bool fpu_cir_word_access(const x68k_bus_t *b, uint32_t addr) {
    return !(addr & 1u) && fpu_cir_in_window(b, addr);
}

/* long は二つの word cycle。2 cycle 目が window 外ならそこだけバスエラー */
static bool fpu_cir_long_read(x68k_bus_t *b, uint32_t addr, uint32_t *out) {
    uint16_t hi, lo = FPU_CIR_OPEN_WORD;
    bool ok = fpu_cir_cycle_read(b, addr, &hi);
    if (ok)
        ok = fpu_cir_in_window(b, addr + 2u) && fpu_cir_cycle_read(b, addr + 2u, &lo);
    *out = ((uint32_t)hi << FPU_CIR_WORD_BITS) | lo;
    return ok;
}

static bool fpu_cir_long_write(x68k_bus_t *b, uint32_t addr, uint32_t val) {
    bool ok = fpu_cir_cycle_write(b, addr, (uint16_t)(val >> FPU_CIR_WORD_BITS));
    if (ok)
        ok =
            fpu_cir_in_window(b, addr + 2u) && fpu_cir_cycle_write(b, addr + 2u, (uint16_t)val);
    return ok;
}

static bool bus_read_word(void *ctx, uint32_t addr, uint16_t *out) {
    x68k_bus_t *b = (x68k_bus_t *)ctx; /* 非const: 読み出しトレースがカウンタを更新する */
    bus_sync_dmac(b);
    uint32_t raw_addr = addr;
    const uint32_t rlimit = ram_limit(b);
    bool fast_path = cpu_fast_bus_addr(b, raw_addr, 2u, &addr);
    /* 高速パスは read8()/cpu_read8() を経由せず配列に直接アクセスするため、
     * ここで先に呼んでおく必要がある。 */
    bool ok = fast_path && super_protect_check_limit(b, addr, rlimit);
    /* 高速パス: RAM / GVRAM の範囲内なら直接配列アクセス */
    if (fast_path && addr + 1u < rlimit) {
        *out = (uint16_t)((b->ram[addr] << 8) | b->ram[addr + 1]);
        return ok;
    }
    if (fast_path && addr >= GVRAM_BASE && addr + 1u < GVRAM_BASE + GVRAM_SIZE) {
        uint32_t off = addr - GVRAM_BASE;
        *out = (uint16_t)((b->gvram[off] << 8) | b->gvram[off + 1]);
        return ok;
    }
    if (fast_path && addr >= IOCS_ROM_BASE && addr + 1u <= 0xFFFFFFu) {
        if (addr >= 0xFC0000u && addr < 0xFE0000u) {
            *out = (uint16_t)(((uint16_t)read8(b, addr) << 8) | read8(b, addr + 1u));
            return ok;
        }
        uint32_t off = iocs_rom_offset(addr);
        *out = (uint16_t)((b->iocs_rom[off] << 8) | b->iocs_rom[off + 1]);
        return ok;
    }
    /* RTCワード読み: 奇数側(addr+1)のレジスタ値のみを返す。偶数バイトは
     * バスエラーにならず単に無視される（XEiJ MMD_RTC mmdRws/mmdRwz準拠）。 */
    if (fast_path && addr >= RTC_BASE && addr + 1u < RTC_BASE + RTC_SIZE) {
        *out = rtc_reg_read8(&b->rtc, addr + 1u);
        return ok;
    }
    if (fast_path && fpu_cir_word_access(b, addr))
        return fpu_cir_cycle_read(b, addr, out);
    uint8_t hi, lo;
    bool ok_hi = cpu_read8(b, raw_addr, &hi);
    bool ok_lo = cpu_read8(b, raw_addr + 1u, &lo);
    *out = (uint16_t)((hi << 8) | lo);
    return ok_hi && ok_lo;
}

static bool bus_read_long(void *ctx, uint32_t addr, uint32_t *out) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bus_sync_dmac(b);
    uint32_t raw_addr = addr;
    const uint32_t rlimit = ram_limit(b);
    bool fast_path = cpu_fast_bus_addr(b, raw_addr, 4u, &addr);
    bool ok =
        fast_path && super_protect_check_limit(
                         b, addr, rlimit); /* 高速パスが read8()/cpu_read8() を経由しないため */
    if (fast_path && addr + 3u < rlimit) {
        *out = ((uint32_t)b->ram[addr] << 24) | ((uint32_t)b->ram[addr + 1] << 16) |
               ((uint32_t)b->ram[addr + 2] << 8) | (uint32_t)b->ram[addr + 3];
        return ok;
    }
    if (fast_path && addr >= GVRAM_BASE && addr + 3u < GVRAM_BASE + GVRAM_SIZE) {
        uint32_t off = addr - GVRAM_BASE;
        *out = ((uint32_t)b->gvram[off] << 24) | ((uint32_t)b->gvram[off + 1] << 16) |
               ((uint32_t)b->gvram[off + 2] << 8) | (uint32_t)b->gvram[off + 3];
        return ok;
    }
    if (fast_path && addr >= IOCS_ROM_BASE && addr + 3u <= 0xFFFFFFu) {
        if (addr >= 0xFC0000u && addr < 0xFE0000u) {
            *out = ((uint32_t)read8(b, addr) << 24) | ((uint32_t)read8(b, addr + 1) << 16) |
                   ((uint32_t)read8(b, addr + 2) << 8) | (uint32_t)read8(b, addr + 3);
            return ok;
        }
        uint32_t off = iocs_rom_offset(addr);
        *out = ((uint32_t)b->iocs_rom[off] << 24) | ((uint32_t)b->iocs_rom[off + 1] << 16) |
               ((uint32_t)b->iocs_rom[off + 2] << 8) | (uint32_t)b->iocs_rom[off + 3];
        return ok;
    }
    /* RTCロング読み: addr+1とaddr+3の2レジスタを束ねて返す
     * （XEiJ MMD_RTC mmdRls: rtcReadByte(a+1)<<16 | rtcReadByte(a+3) 準拠）。 */
    if (fast_path && addr >= RTC_BASE && addr + 3u < RTC_BASE + RTC_SIZE) {
        *out = ((uint32_t)rtc_reg_read8(&b->rtc, addr + 1u) << 16) |
               rtc_reg_read8(&b->rtc, addr + 3u);
        return ok;
    }
    if (fast_path && fpu_cir_word_access(b, addr))
        return fpu_cir_long_read(b, addr, out);
    uint8_t b0, b1, b2, b3;
    bool ok0 = cpu_read8(b, raw_addr, &b0);
    bool ok1 = cpu_read8(b, raw_addr + 1u, &b1);
    bool ok2 = cpu_read8(b, raw_addr + 2u, &b2);
    bool ok3 = cpu_read8(b, raw_addr + 3u, &b3);
    *out = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    return ok0 && ok1 && ok2 && ok3;
}

static bool bus_write_byte_impl(void *ctx, uint32_t addr, uint8_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bus_sync_dmac(b);
    return cpu_write8(b, addr, val);
}

static bool bus_write_word_impl(void *ctx, uint32_t addr, uint16_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bus_sync_dmac(b);
    uint32_t raw_addr = addr;
    const uint32_t rlimit = ram_limit(b);
    bool fast_path = cpu_fast_bus_addr(b, raw_addr, 2u, &addr);
    /* 以下の全分岐（RAM/GVRAM高速パス、CRTC/DEVのwrite8直呼び）が
     * cpu_write8() を経由しないため、ここで先に呼んでおく。 */
    bool ok = fast_path && super_protect_check_limit(b, addr, rlimit);
    if (fast_path && addr + 1u < rlimit) {
        if (b->ram_watch_len) {
            ram_watch_check(b, addr, b->ram[addr], (uint8_t)(val >> 8));
            ram_watch_check(b, addr + 1, b->ram[addr + 1], (uint8_t)val);
        }
        b->ram[addr] = (uint8_t)(val >> 8);
        b->ram[addr + 1] = (uint8_t)val;
        return ok;
    }
    if (fast_path && addr >= GVRAM_BASE && addr + 1u < GVRAM_BASE + GVRAM_SIZE) {
        uint32_t off = addr - GVRAM_BASE;
        b->gvram[off] = (uint8_t)(val >> 8);
        b->gvram[off + 1] = (uint8_t)val;
        return ok;
    }
    /* RTCワード書き: 奇数側(addr+1)のレジスタのみへ書く（XEiJ MMD_RTC mmdWw準拠）。
     * 汎用CRTC分岐より先に判定する必要がある（範囲がCRTC_BASE内に含まれるため）。 */
    if (fast_path && addr >= RTC_BASE && addr + 1u < RTC_BASE + RTC_SIZE) {
        rtc_reg_write8(&b->rtc, addr + 1u, (uint8_t)val);
        return ok;
    }
    if (fast_path && addr >= CRTC_BASE && addr + 1u < CRTC_BASE + CRTC_SIZE) {
        /* MFP 奇数アドレス (0xE88001-0xE8802F) へのバイトは write8 経由でディスパッチ。
         * 偶数バイトは CRTC バッファに書く（実機でも偶数は無視/CRTC に割り当て）。 */
        write8(b, addr, (uint8_t)(val >> 8));
        write8(b, addr + 1, (uint8_t)val);
        return ok;
    }
    if (fast_path && addr >= DEV_BASE && addr + 1u < DEV_END) {
        /* HOSTDRV/HOSTFILE レジスタウィンドウだけは write8 経由で処理する必要がある
         * （それ以外の DEV 領域は書き込み無視のスタブ挙動を維持）。HOSTFILE を
         * このホワイトリストに加え忘れると 0xEA0100 への 16bit 書き込みが
         * 静かに捨てられる。 */
        if ((addr >= HOSTDRV_RD_BASE && addr + 1u < HOSTDRV_RD_END) ||
            (addr >= HOSTFILE_BASE && addr + 1u < HOSTFILE_END)) {
            write8(b, addr, (uint8_t)(val >> 8));
            write8(b, addr + 1, (uint8_t)val);
        }
        return ok;
    }
    if (fast_path && fpu_cir_word_access(b, addr))
        return fpu_cir_cycle_write(b, addr, val);
    bool ok_hi = cpu_write8(b, raw_addr, (uint8_t)(val >> 8));
    bool ok_lo = cpu_write8(b, raw_addr + 1u, (uint8_t)val);
    return ok_hi && ok_lo;
}

static bool bus_write_long_impl(void *ctx, uint32_t addr, uint32_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bus_sync_dmac(b);
    uint32_t raw_addr = addr;
    const uint32_t rlimit = ram_limit(b);
    bool fast_path = cpu_fast_bus_addr(b, raw_addr, 4u, &addr);
    bool ok = fast_path && super_protect_check_limit(
                               b, addr, rlimit); /* 高速パスが cpu_write8() を経由しないため */
    if (fast_path && addr + 3u < rlimit) {
        if (b->ram_watch_len) {
            ram_watch_check(b, addr, b->ram[addr], (uint8_t)(val >> 24));
            ram_watch_check(b, addr + 1, b->ram[addr + 1], (uint8_t)(val >> 16));
            ram_watch_check(b, addr + 2, b->ram[addr + 2], (uint8_t)(val >> 8));
            ram_watch_check(b, addr + 3, b->ram[addr + 3], (uint8_t)val);
        }
        b->ram[addr] = (uint8_t)(val >> 24);
        b->ram[addr + 1] = (uint8_t)(val >> 16);
        b->ram[addr + 2] = (uint8_t)(val >> 8);
        b->ram[addr + 3] = (uint8_t)val;
        return ok;
    }
    if (fast_path && addr >= GVRAM_BASE && addr + 3u < GVRAM_BASE + GVRAM_SIZE) {
        uint32_t off = addr - GVRAM_BASE;
        b->gvram[off] = (uint8_t)(val >> 24);
        b->gvram[off + 1] = (uint8_t)(val >> 16);
        b->gvram[off + 2] = (uint8_t)(val >> 8);
        b->gvram[off + 3] = (uint8_t)val;
        return ok;
    }
    /* RTCロング書き: addr+1/addr+3の2レジスタへ束ねて書く
     * （XEiJ MMD_RTC mmdWl: rtcWriteByte(a+1,d>>16); rtcWriteByte(a+3,d) 準拠）。 */
    if (fast_path && addr >= RTC_BASE && addr + 3u < RTC_BASE + RTC_SIZE) {
        rtc_reg_write8(&b->rtc, addr + 1u, (uint8_t)(val >> 16));
        rtc_reg_write8(&b->rtc, addr + 3u, (uint8_t)val);
        return ok;
    }
    if (fast_path && fpu_cir_word_access(b, addr))
        return fpu_cir_long_write(b, addr, val);
    bool ok0 = cpu_write8(b, raw_addr, (uint8_t)(val >> 24));
    bool ok1 = cpu_write8(b, raw_addr + 1u, (uint8_t)(val >> 16));
    bool ok2 = cpu_write8(b, raw_addr + 2u, (uint8_t)(val >> 8));
    bool ok3 = cpu_write8(b, raw_addr + 3u, (uint8_t)val);
    return ok0 && ok1 && ok2 && ok3;
}

/* ch2 のロック違反で破棄した続きバイトの記憶は、その CPU アクセスの中だけで有効。
 * アクセス終了時に解除する（表の直接経路は次のアクセス開始時の bus_sync_dmac を通らない）。 */
static bool bus_write_byte(void *ctx, uint32_t addr, uint8_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bool ok = bus_write_byte_impl(ctx, addr, val);
    b->dmac.ch2_discard_left = 0;
    return ok;
}
static bool bus_write_word(void *ctx, uint32_t addr, uint16_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bool ok = bus_write_word_impl(ctx, addr, val);
    b->dmac.ch2_discard_left = 0;
    return ok;
}
static bool bus_write_long(void *ctx, uint32_t addr, uint32_t val) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    bool ok = bus_write_long_impl(ctx, addr, val);
    b->dmac.ch2_discard_left = 0;
    return ok;
}

static void bus_sync(void *ctx, int cycles) {
    (void)cycles;
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    x68k_bus_mfp_rtc_alarm(b, rtc_alarm_low(&b->rtc));
}

static void bus_write_hook(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

/* SCC(マウス)がIOCSのマウスカーソル座標ワーク($0ACE)を参照するための
 * bus層への薄いブリッジ（scc.cはbus型を知らないためvoid* ctx経由）。 */
static uint32_t bus_scc_mem_read32(void *ctx, uint32_t addr) {
    return x68k_bus_read32((x68k_bus_t *)ctx, addr);
}

/* ---- 公開 API ---- */

x68k_bus_t *x68k_bus_create(void) {
    x68k_bus_t *b = calloc(1, sizeof(x68k_bus_t));
    if (!b)
        return NULL;

    b->ram = calloc(1, RAM_BUF_SIZE);
    b->gvram = calloc(1, GVRAM_SIZE);
    b->tvram = calloc(1, TVRAM_SIZE);
    b->crtc_regs = calloc(1, CRTC_SIZE);
    b->sram = calloc(1, SRAM_SIZE);
    b->sp_ram = calloc(1, SPRITE_SIZE);
    b->iocs_rom = malloc(IOCS_ROM_SIZE);

    if (!b->ram || !b->gvram || !b->tvram || !b->crtc_regs || !b->sram || !b->sp_ram ||
        !b->iocs_rom) {
        x68k_bus_destroy(b);
        return NULL;
    }

    /* パレットARGBキャッシュの初期値（GRB555=0相当の不透明黒）。crtc_regsはcalloc済み
     * なので実際の変換結果と一致させておく。 */
    for (int i = 0; i < 256; i++)
        b->crtc.gfx_pal_argb[i] = 0xFF000000u;
    for (int i = 0; i < 256; i++)
        b->crtc.text_pal_argb[i] = 0xFF000000u;
    b->crtc.pal_dirty = 1;
    b->crtc.pal_cache_ready = 1;
    b->crtc.contrast_lut_scaled = -1; /* 未構築。x68k_bus_get_contrast_lut()が初回に構築する */

    /* システムポート#1 ($E8E001) コントラスト既定値。crtc_regsはcalloc済みで
     * $00(最暗)のままだと、ROMがSRAM値($ED0028、通常$0E)を復帰する
     * (ff00c0)までの間だけ画面が真っ暗になる。XEiJ初期値の$0F(最明)に
     * 合わせておく（docs/ix68k-internals/design/video_controller_design.md §6）。 */
    b->crtc_regs[0xE001] = 0x0Fu;

    /* システムポート#5 ($E8E009) の既定値$00相当。IPLROMの
     * 通常経路が実際に CLR.B するのと同じ値。SRAM $ED0092 由来の値は
     * IPLROMが実際にポートへ書き込んだ時点でだけ反映する（先取りしない）。 */
    x68k_bus_apply_wait_port(b, 0x00u);
    b->timing_profile = X68K_TIMING_PROFILE_X68000;
    b->cpu_hz = 10000000u;          /* 実機標準10MHz。main()が-cpu-clock/メニューで変更する */
    b->mfp.gpip_raster_enabled = 1; /* 既定on。-mfp-gpip-raster off で逃げ道 */
    /* ix68k は起動時点で前面電源スイッチがONの状態からIPLを実行する。GPIP2を
     * LOWにしておかないと、RTC通常時(GPIP0=HIGH)のIPLROMXVは電源ON待ちへ入る。 */
    b->mfp.gpip2_level = 0u;
    b->super_area_limit = 0x2000u; /* $E86001 エリアセット既定値0相当（XEiJ初期値と一致） */
    b->fdc.num_drives = 2;         /* X68000 標準搭載ドライブ数 */
    b->fdc.bus = b;                /* DMAC ch0汎用エンジン呼び出し用の自己参照 */
    /* FDC(µPD72065)の電源投入時初期化。callocのみだとst0=0の
     * ままで、RESET直後のSENSE INTERRUPT STATUS応答が不正になる（fdc_reset()の
     * コメント参照）。dmac_reset()と同じ理由でここでも明示的に初期化しておく
     * （bus_reset_peripheral()とcreate()の
     * 両方から呼ぶ）。 */
    fdc_reset(&b->fdc);
    /* DMAC(HD63450)の電源投入時初期化。crtc_regsはcallocのみで
     * NIV/EIVレジスタ($E840A5/A7等)も0のままだが、実チップは電源投入時点で
     * $0F（未初期化割り込みベクタ番号15相当）になる（XEiJ HD63450.java
     * dmaInit()もdmaReset()を呼ぶ）。scsi_reset/opm_resetと同じ理由でここでも
     * 明示的に初期化しておく。 */
    dmac_reset(b);
    /* RTC(RP5C15)の初期化はここ(x68k_bus_create())でのみ行う。CPU RESET命令
     * (bus_reset_peripheral())では触らない — 実チップは背面メインスイッチ＋電池で
     * 独立給電されCPU RESET線と無関係で、XEiJ RP5C15.rtcReset()も空実装。 */
    guest_clock_init(&b->guest_clock);
    rtc_init(&b->rtc, &b->guest_clock);
    x68k_bus_mfp_rtc_alarm(b, rtc_alarm_low(&b->rtc));
    scsi_reset(&b->spc_in);
    b->spc_in.bus = b;                      /* IOI割り込み発火用の自己参照 */
    sasi_init(&b->hdc_in, b->spc_in.units); /* SPCとユニット(接続ディスク)を共有 */
    /* opm_reset()はchannel[].op[]ポインタ配列(s_fixed_map由来)をセットアップ
     * する。reset_peripheralフックはCPUのRESET命令実行時にのみ発火するため、
     * IPLROM起動シーケンスの初期段階(RESET命令実行前)でx68k_bus_opm_generate()
     * が呼ばれるとcalloc ゼロクリアのままのNULLポインタを参照してクラッシュする
     * （SIGSEGV @ operator_clock）。scsi_resetと同じパターンでここでも
     * 明示的に初期化しておく。 */
    opm_reset(&b->opm);
    scc_set_mem_read32(&b->scc, bus_scc_mem_read32, b);
    b->joy_port[0] = 0xFFu; /* ジョイスティック#1/#2 未接続（アクティブロー） */
    b->joy_port[1] = 0xFFu;
    memcpy(b->scsi_rom, SCSI16IN, SCSI_ROM_SIZE);
    /* IOCS ROM 未ロード: 0xFF で埋める（line-F 例外を引き起こす） */
    memset(b->iocs_rom, 0xFF, IOCS_ROM_SIZE);

    /*
     * CRTC R20 (0xE80028) デフォルト: 65536色・512×512・31.5kHz
     * （GVRAMの色数判定の一次情報はビデオコントローラR0($E82400)で、起動直後は0=16色。
     *  ここで設定するR20はIPLROMが_CRTMODで書き直すまでの表示サイズ算出用の初期値）
     *   bit 10-8 (crtc_regs[0x28] bits 2-0): crtMemoryModePort = 3 = 512dot 65536色
     *   bit 4    (crtc_regs[0x29] bit 4)   : HF  = 1   = 31.5kHz
     *   bit 3-2  (crtc_regs[0x29] bits 3-2): VD  = 01b = 512ドット
     *   bit 1-0  (crtc_regs[0x29] bits 1-0): HD  = 01b = 512ドット
     * BIOS がロードされればこの値は上書きされる。
     */
    b->crtc_regs[0x28] = 0x03; /* memoryModePort=3 (512dot 65536色) */
    b->crtc_regs[0x29] = 0x15; /* HF=1, VD=01, HD=01 */

    /*
     * テキストパレット (0xE82200–0xE8221F): crtc_regs オフセット 0x2200
     * GRB555 ビットレイアウト: bit15-11=G, bit10-6=R, bit5-1=B, bit0=I
     *
     * BIOS が書く前の段階でも白文字が見えるように標準 16 色パレットを初期化する。
     * エントリ 0 は透過色（描画しない）なので 0x0000 のまま。
     */
    /*
     * テキストパレット設計原則:
     *   plane 0+1 (bit1:0) = テキスト色インデックス (0-3)
     *   plane 2   (bit2)   = ソフトウェアキーボード
     *   plane 3   (bit3)   = マウスカーソル
     *
     * IPLROMXV の実値 ($FF0870, 16 entries × 2 bytes) をそのまま使う。
     *   index 0: 透過 (背景)
     *   index 1: 水色 GRB=(31,0,31)  — plane0 のみ
     *   index 2: 黄色 GRB=(31,31,0)  — plane1 のみ
     *   index 3: 白   GRB=(31,31,31) — plane0+1 (mode=3 デフォルト文字色)
     */
    static const uint16_t def_text_pal[16] = {
        0x0000, 0xF83E, 0xFFC0, 0xFFFE, /*  0- 3 */
        0xDE6C, 0x4022, 0x0302, 0x0008, /*  4- 7 */
        0x0000, 0x0000, 0x0000, 0x0000, /*  8-11 */
        0x0000, 0x00FF, 0xDC00, 0x0400, /* 12-15 */
    };
    for (int i = 0; i < 16; i++) {
        b->crtc_regs[0x2200 + i * 2 + 0] = (uint8_t)(def_text_pal[i] >> 8);
        b->crtc_regs[0x2200 + i * 2 + 1] = (uint8_t)(def_text_pal[i]);
    }

    b->pmap =
        calloc(1, sizeof(*b->pmap)); /* 失敗しても表なし（全アクセスがコールバック）で動く */
    x68k_bus_page_map_refresh(b);
    return b;
}

x68k_media_generation_t x68k_bus_media_generation(const x68k_bus_t *bus) {
    return (x68k_media_generation_t){.fdc = bus->fdc.media_gen,
                                     .scsi = bus->spc_in.media_gen,
                                     .fdc_overflow = bus->fdc.media_gen_overflow,
                                     .scsi_overflow = bus->spc_in.media_gen_overflow};
}

bool x68k_media_generation_equal(x68k_media_generation_t a, x68k_media_generation_t b) {
    return !a.fdc_overflow && !a.scsi_overflow && !b.fdc_overflow && !b.scsi_overflow &&
           a.fdc == b.fdc && a.scsi == b.scsi;
}

void x68k_bus_set_cpu(x68k_bus_t *bus, mc68k_state_t *cpu) {
    bus->cpu = cpu;
    x68k_bus_page_map_refresh(bus);
}

bool x68k_bus_set_timing_profile(x68k_bus_t *bus, x68k_timing_profile_t profile) {
    if (profile >= X68K_TIMING_PROFILE_COUNT)
        return false;
    bus->timing_profile = profile;
    x68k_bus_page_map_refresh(bus);
    return true;
}

bool x68k_bus_set_local_memory(x68k_bus_t *bus, unsigned mb) {
    if (bus->cpu || bus->local_memory ||
        (mb != 0 && mb != 16 && mb != 32 && mb != 64 && mb != 128))
        return false;
    if (mb == 0)
        return true;
    bus->local_memory_size = mb * 1024u * 1024u;
    bus->local_memory = calloc(1, bus->local_memory_size);
    if (bus->local_memory) {
        x68k_bus_page_map_refresh(bus);
        return true;
    }
    bus->local_memory_size = 0;
    return false;
}

/* CPUクロックを変更する（起動オプション -cpu-clock またはメニュー選択から
 * main.cが呼ぶ）。mfp.c/dmac.cはbus->cpu_hzを毎tick直接参照するのでここで
 * 設定するだけでよいが、opm.cはopm_tick()をbus非依存に保つ設計のため、
 * opm_t側にも個別に伝える。 */
void x68k_bus_set_cpu_hz(x68k_bus_t *bus, uint32_t hz) {
    x68k_bus_dmac_cpu_hz_changed(bus, bus->cpu_hz, hz);
    bus->cpu_hz = hz;
    opm_set_cpu_hz(&bus->opm, hz);
}

void x68k_bus_set_mfp_soft_eoi(x68k_bus_t *bus, int enabled) {
    bus->mfp.soft_eoi_enabled = enabled ? 1 : 0;
}

void x68k_bus_set_mfp_gpip_raster(x68k_bus_t *bus, int enabled) {
    bus->mfp.gpip_raster_enabled = enabled ? 1 : 0;
}

void x68k_bus_set_super_protect(x68k_bus_t *bus, int enable) {
    bus->super_protect_enabled = enable;
    x68k_bus_page_map_refresh(bus);
}

void x68k_bus_set_ram_watch(x68k_bus_t *bus, uint32_t addr, uint32_t len) {
    bus->ram_watch_addr = addr;
    bus->ram_watch_len = len;
    x68k_bus_page_map_refresh(bus);
}

bool x68k_bus_set_fpu_cir(x68k_bus_t *bus, int enable, x68k_fpu_cir_address_t address) {
    if (!enable)
        return true;
    if (bus->fpu_cir)
        return true; /* 一度だけ生成する（二重生成でモデルを失わない） */
    return x68k_bus_attach_fpu_cir(bus, mc68k_fpu_create(MC68K_FPU_68881), address);
}

bool x68k_bus_attach_fpu_cir(x68k_bus_t *bus, mc68k_fpu_t *fpu,
                             x68k_fpu_cir_address_t address) {
    if (!fpu)
        return false; /* 生成失敗: 無効のまま（XB1全域がバスエラー） */
    bus->fpu_cir = fpu;
    fpu_cir_init(&bus->fpu_cir_state);
    bus->fpu_cir_base =
        address == X68K_FPU_CIR_SECONDARY ? FPU_CIR_SECONDARY_BASE : FPU_CIR_PRIMARY_BASE;
    return true;
}

void x68k_bus_destroy(x68k_bus_t *bus) {
    if (!bus)
        return;
    if (bus->fpu_cir)
        mc68k_fpu_destroy(bus->fpu_cir);
    free(bus->ram);
    free(bus->local_memory);
    free(bus->gvram);
    free(bus->tvram);
    free(bus->crtc_regs);
    free(bus->sram);
    free(bus->sp_ram);
    free(bus->iocs_rom);
    if (bus->pmap)
        free(bus->pmap->local);
    free(bus->pmap);
    hostfile_reset_transfer_state(
        &bus->hostfile); /* 開いているFILE*・未回収の一時ファイルを片付ける */
    for (int i = 0; i < 4; i++) {
        free(bus->fdc.disk[i]);
        free(bus->fdc.disk_path[i]);
        media_fp_free(&bus->fdc.media_fp[i]);
    }
    for (int i = 0; i < SCSI_NUM_UNITS; i++)
        scsi_detach(&bus->spc_in, i);
    free(bus);
}

void x68k_bus_hostdrv_configure(x68k_bus_t *bus, const char *host_dir) {
    (void)bus; /* hostdrv_remote.c は bus 状態を持たない（レジスタは bus->hostdrv_rd のみ） */
    hostdrv_remote_configure(host_dir);
}

long x68k_bus_hostfile_put(x68k_bus_t *bus, const char *path) {
    return hostfile_put(&bus->hostfile, path);
}

long x68k_bus_hostfile_get(x68k_bus_t *bus, const char *path) {
    return hostfile_get(&bus->hostfile, path);
}

void x68k_bus_hostfile_status(const x68k_bus_t *bus, int *state, uint32_t *len) {
    hostfile_status(&bus->hostfile, state, len);
}

/* 拡張子→媒体プロファイル。イメージはヘッダなしの生セクタ列で、
 * 拡張子が媒体を決める。サイズからの推定はしない（同サイズの媒体があるため）。
 * .xdf/.hdm/.2hd はいずれも標準2HDで同一形式。比較は大小文字を区別しない。 */
static const struct {
    const char *ext;
    const fdc_media_profile_t *profile;
} fdc_ext_table[] = {
    {".xdf", &fdc_profile_2hd}, {".hdm", &fdc_profile_2hd},     {".2hd", &fdc_profile_2hd},
    {".2hs", &fdc_profile_2hs}, {".2hde", &fdc_profile_2hde},   {".2ht", &fdc_profile_2ht},
    {".2hc", &fdc_profile_2hc}, {".2dd9", &fdc_profile_2dd9},   {".2dd8", &fdc_profile_2dd8},
    {".2hq", &fdc_profile_2hq}, {".2hdez", &fdc_profile_2hdez},
};

static const fdc_media_profile_t *fdc_profile_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    for (size_t i = 0; i < sizeof(fdc_ext_table) / sizeof(fdc_ext_table[0]); i++) {
        if (strcasecmp(dot, fdc_ext_table[i].ext) == 0)
            return fdc_ext_table[i].profile;
    }
    return NULL;
}

/* ホストのファイル書き込み権限からWPを判定する（
 * SCSI(MO)にも共用するため汎用名にしている）。stat()+S_IWUSRではなくaccess(W_OK)を
 * 使う: 他人所有ファイルや読み取り専用マウントでも実効的な書き込み可否を
 * 正しく返し、fopen(path,"r+b")が失敗する条件と一致するため。 */
static int host_path_write_protected(const char *path) {
    return (path && access(path, W_OK) != 0) ? 1 : 0;
}

static void fdc_refresh_fingerprint(fdc_t *fdc, int drive) {
    media_fp_free(&fdc->media_fp[drive]);
    if (!fdc->write_protect[drive])
        return;
    FILE *fp = fopen(fdc->disk_path[drive], "rb");
    if (!fp)
        return;
    media_fp_compute_memory(&fdc->media_fp[drive], fdc->disk[drive],
                            (size_t)fdc->disk_bytes[drive], fdc->disk_path[drive], fileno(fp));
    fclose(fp);
}

static void scsi_refresh_fingerprint(scsi_unit_t *unit) {
    media_fp_free(&unit->media_fp);
    if ((!unit->write_protected && !unit->overlay) || !unit->path)
        return;
    const char *paths[MEDIA_FP_MAX_FILES] = {unit->path, unit->img_path};
    int npaths = unit->img_path ? 2 : 1;
    media_fp_compute(&unit->media_fp, paths, npaths);
}

int x68k_bus_load_fdc(x68k_bus_t *bus, int drive, const char *path) {
    if (drive < 0 || drive >= 4)
        return X68K_FDC_ERR_INVALID_DRIVE;
    /* 失敗時は既存の媒体・DDAM・WP・パスを一切変更しない。
     * 状態の更新はイメージの読込みが全て成功した後にまとめて行う。 */
    const fdc_media_profile_t *profile = fdc_profile_for_path(path);
    if (!profile) {
        fprintf(stderr, "error: 未対応の拡張子です: %s\n", path);
        return X68K_FDC_ERR_BAD_EXTENSION;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return X68K_FDC_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return X68K_FDC_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return X68K_FDC_ERR_IO;
    }
    if ((unsigned long)sz != profile->total_bytes) {
        fprintf(stderr,
                "error: %s のサイズが一致しません（%s: 期待 %u バイト, 実際 %ld バイト）\n",
                path, profile->name, profile->total_bytes, sz);
        fclose(fp);
        return X68K_FDC_ERR_SIZE_MISMATCH;
    }
    uint8_t *img = malloc((size_t)sz);
    if (!img) {
        fclose(fp);
        return X68K_FDC_ERR_ALLOC;
    }
    if ((long)fread(img, 1, (size_t)sz, fp) != sz) {
        free(img);
        fclose(fp);
        return X68K_FDC_ERR_IO;
    }
    fclose(fp);
    free(bus->fdc.disk[drive]);
    free(bus->fdc.disk_path[drive]);
    bus->fdc.disk[drive] = img;
    bus->fdc.disk_bytes[drive] = (int)sz;
    bus->fdc.profile[drive] = profile;
    bus->fdc.disk_path[drive] = strdup(path);
    /* DDAM状態は永続化しないため挿入時にクリアする（前の
     * イメージのDDAM状態が別イメージへ持ち越されると通常のREAD DATAが
     * 原因不明のCMエラーを返す事故になる）。 */
    fdc_ddam_clear_drive(&bus->fdc, drive);
    /* WP状態も同様に非永続化し、挿入のたびホストのファイル属性から
     * 再判定する。 */
    bus->fdc.write_protect[drive] = (uint8_t)host_path_write_protected(path);
    fdc_refresh_fingerprint(&bus->fdc, drive);
    fdc_media_changed(&bus->fdc);
    /* ioi_fdd_rise は呼ばない: 起動時にディスク挿入シグナルを出すと
     * BIOS が IOI FDD 割り込みを処理する時点のドライブセレクトで
     * 意図しないドライブをポーリングしてしまう。
     * BIOS は自ら RECALIBRATE / SENSE DRIVE STATUS でディスクを検出する。 */
    return 0;
}

int x68k_bus_insert_fdc_runtime(x68k_bus_t *bus, int drive, const char *path) {
    if (drive < 0 || drive >= 4)
        return X68K_FDC_ERR_INVALID_DRIVE;
    /* 実機は排出しないと次のディスクを挿入できない。eject を経ずに
     * 差し替えると fdc_ejected_flag が立たず、IOCS _B_DRVCHK mode=8を
     * ポーリングするHuman68kのFDDドライバがメディア交換を検知できず
     * 旧ディスクのFAT/ディレクトリキャッシュを読み続けてしまう。
     * 呼び出し側に先に eject_disk を要求する。 */
    if (bus->fdc.disk[drive])
        return X68K_FDC_ERR_ALREADY_INSERTED;
    int err = x68k_bus_load_fdc(bus, drive, path);
    if (err == 0)
        ioi_fdd_rise(bus);
    return err;
}

void x68k_bus_eject_fdc(x68k_bus_t *bus, int drive) {
    /* 物理イジェクトボタン相当の経路（MCP eject_disk・GUIイジェクトボタン）。
     * $E94005 bit6(EJECT MASK)でソフトウェアがこの物理ボタン自体を無効化
     * できるため、マスク中は実機同様に無視する（『Inside X68000』 図5
     * 「1=イジェクトボタンを無効化する」）。IOCS _B_EJECT等のレジスタ書き込み
     * 経由(bit5 EJECT ON/OFF)はソフトウェア自身の制御経路のためマスクの
     * 対象外（fdc_eject_drive()を直接呼ぶ本ファイル内の$E94005ハンドラ参照）。 */
    if (drive < 0 || drive >= 4)
        return;
    if (bus->fdc_eject_masked_bits & (uint8_t)(1u << drive))
        return;
    /* UAF防止ガード: FDCがコマンド実行中(FDC_CMD/FDC_EXEC/FDC_RESULT)は
     * イジェクトを無視する。FDC_EXEC中はdma_ptrがdisk[drive]バッファ内を
     * 指したまま複数回のバス経由read8/write8で漸進消費される
     * （fdc.c の fdc_read_data()/fdc_write_data()）ため、ここでdisk[drive]をfreeするとdma_ptrが
     * ダングリングポインタになりUse-After-Freeになる。どのドライブに
     * 対する実行中かの精密な判定（dma_drvはFDC_EXEC開始時にしか確定しない）
     * より、フェーズ全体で一律ブロックする単純な設計を優先した。 */
    if (bus->fdc.phase != FDC_IDLE)
        return;
    fdc_eject_drive(bus, drive);
}

int x68k_bus_fdc_take_ejected_flag(x68k_bus_t *bus, int drive) {
    if (drive < 0 || drive >= 4)
        return 0;
    int was = bus->fdc_ejected_flag[drive];
    bus->fdc_ejected_flag[drive] = 0;
    return was;
}

int x68k_bus_fdc_pending_irq(const x68k_bus_t *bus) {
    return bus->fdc.irq;
}

int x68k_bus_ioi_pending_irq(const x68k_bus_t *bus) {
    return bus->ioi_request != 0 || bus->spc_irq_pending != 0;
}

int x68k_bus_dmac2_pending_irq(const x68k_bus_t *bus) {
    return bus->dmac.ch2_irq_pending != 0 || bus->dmac.ch2_error_pending != 0;
}

int x68k_bus_dmac3_pending_irq(const x68k_bus_t *bus) {
    return bus->dmac.ch3_irq_pending != 0;
}

int x68k_bus_adpcm_take_samples(x68k_bus_t *bus, int16_t *out, int max_count,
                                uint32_t *native_rate_hz, int *left_on, int *right_on) {
    *native_rate_hz = adpcm_get_sample_rate_hz(&bus->adpcm);
    adpcm_get_pan(&bus->adpcm, left_on, right_on);
    return adpcm_take_samples(&bus->adpcm, out, max_count);
}

uint32_t x68k_bus_adpcm_sample_rate_hz(const x68k_bus_t *bus) {
    return adpcm_get_sample_rate_hz(&bus->adpcm);
}

void x68k_bus_adpcm_push_rec_samples(x68k_bus_t *bus, const int16_t *in, int count) {
    adpcm_rec_push_samples(&bus->adpcm, in, count);
}

int x68k_bus_adpcm_is_recording(const x68k_bus_t *bus) {
    return adpcm_is_recording(&bus->adpcm);
}

void x68k_bus_fdc_ack_irq(x68k_bus_t *bus) {
    bus->fdc.irq = 0;
}

int x68k_bus_fdc_disk_loaded(const x68k_bus_t *bus, int drive) {
    /* disk[4] は外付けを含む4台分を保持する。 */
    if (drive < 0 || drive > 3)
        return 0;
    return bus->fdc.disk[drive] != NULL ? 1 : 0;
}

const char *x68k_bus_fdc_disk_path(const x68k_bus_t *bus, int drive) {
    if (drive < 0 || drive > 3)
        return NULL;
    return bus->fdc.disk_path[drive];
}

int x68k_bus_fdc_write_protected(const x68k_bus_t *bus, int drive) {
    if (!bus || drive < 0 || drive > 3)
        return 0;
    return bus->fdc.write_protect[drive] != 0 ? 1 : 0;
}

int x68k_bus_fdc_set_write_protect(x68k_bus_t *bus, int drive, int protect) {
    if (!bus || drive < 0 || drive >= 4)
        return X68K_FDC_ERR_INVALID_DRIVE;
    if (!bus->fdc.disk[drive] || !bus->fdc.disk_path[drive])
        return X68K_FDC_ERR_NO_DISK;
    /* x68k_bus_eject_fdc()と同じ理由(UAF防止)でFDC実行中は一律拒否する。
     * FDC_EXEC中にWPを立てると、開始済みの1セクタ転送がメモリには書かれて
     * ファイルには書かれない中途半端な状態を作り得る。 */
    if (bus->fdc.phase != FDC_IDLE)
        return X68K_FDC_ERR_BUSY;

    const char *path = bus->fdc.disk_path[drive];
    struct stat st;
    if (stat(path, &st) != 0)
        return X68K_FDC_ERR_IO;
    /* owner writeビット(S_IWUSR)のみをclear/setする。group/otherには
     * 一切触れないため、RW→WP→RWと往復しても元のパーミッションが完全に
     * 保たれる。本ツールがchmodできる対象はほぼ
     * 必ずプロセス自身が所有するファイルなので、owner classの判定で
     * access(W_OK)の結果と実質的に一致する。 */
    mode_t mode = (mode_t)(st.st_mode & 07777);
    mode = protect ? (mode_t)(mode & (mode_t)~S_IWUSR) : (mode_t)(mode | S_IWUSR);
    if (chmod(path, mode) != 0)
        return X68K_FDC_ERR_IO;

    /* 同一ホストファイルを複数ドライブへ挿入している場合、chmod()の効果は
     * 全ドライブへ及ぶ。write_protect[]キャッシュがドライブごとに分かれて
     * いるとWPを迂回できてしまうため、対象ドライブに限らず挿入中の全ドライブ
     * を再判定する。4台のみでコストは無視できる。 */
    for (int i = 0; i < 4; i++) {
        if (bus->fdc.disk_path[i])
            bus->fdc.write_protect[i] =
                (uint8_t)host_path_write_protected(bus->fdc.disk_path[i]);
        if (bus->fdc.disk[i])
            fdc_refresh_fingerprint(&bus->fdc, i);
    }

    /* chmod()が成功してもパーミッションを持たないファイルシステム（既定
     * オプションのexFAT/FAT等）では効果が無いことがあるため、要求値を
     * 鵜呑みにせず再判定結果で食い違いを検出する。 */
    if (bus->fdc.write_protect[drive] != (protect ? 1 : 0))
        return X68K_FDC_ERR_IO;
    fdc_media_changed(&bus->fdc);
    return 0;
}

int x68k_bus_fdc_selected_drive(const x68k_bus_t *bus) {
    return bus->fdc_drive_sel;
}

int x68k_bus_fdc_motor_on(const x68k_bus_t *bus) {
    return bus->fdc_motor_on;
}

uint8_t x68k_bus_read_ram_byte(const x68k_bus_t *bus, uint32_t addr) {
    if (addr >= RAM_BUF_SIZE)
        return 0;
    return bus->ram[addr];
}

void x68k_bus_write_byte(x68k_bus_t *bus, uint32_t addr, uint8_t val) {
    x68k_bus_write8_full(bus, addr, val);
}

/*
 * ローカルSIMM($10000000-)を含む32bitアドレスの裏口アクセス。HOSTDRVのデータ転送と
 * MCP read/write_memoryが使う。DMACは実機同様ローカルメモリへ届かないため、
 * 24bitマスクのx68k_bus_read8/write8を使い続ける。
 */
uint8_t x68k_bus_read8_full(x68k_bus_t *bus, uint32_t addr) {
    if (local_memory_contains(bus, addr))
        return bus->local_memory[addr - LOCAL_MEMORY_BASE];
    return read8(bus, addr & 0xFFFFFFu);
}
void x68k_bus_write8_full(x68k_bus_t *bus, uint32_t addr, uint8_t val) {
    if (local_memory_contains(bus, addr)) {
        bus->local_memory[addr - LOCAL_MEMORY_BASE] = val;
        return;
    }
    write8(bus, addr & 0xFFFFFFu, val);
}

/*
 * x68k_bus_read8/16/32・write8/16/32 は MCP デバッグ backdoor（read_memory 等）と
 * IOCS _B_PEEK/_POKE ショートカット実装が使う「裏口」アクセス。CPU 命令実行
 * (mc68k_execute()) を経由しないため、cpu_read8/cpu_write8 の
 * mc68k_signal_bus_error() 呼び出しを通すと、フラグだけ立って誰も消費せず
 * 次の無関係な CPU 命令で誤ってバスエラー例外が発生してしまう。
 * そのため read8/write8 を直接呼び、バスエラー通知を意図的にスキップする。
 */
uint8_t x68k_bus_read8(x68k_bus_t *bus, uint32_t addr) {
    return read8(bus, addr & 0xFFFFFFu);
}
uint16_t x68k_bus_read16(x68k_bus_t *bus, uint32_t addr) {
    addr &= 0xFFFFFFu;
    return (uint16_t)((read8(bus, addr) << 8) | read8(bus, addr + 1));
}
uint32_t x68k_bus_read32(x68k_bus_t *bus, uint32_t addr) {
    addr &= 0xFFFFFFu;
    return ((uint32_t)read8(bus, addr) << 24) | ((uint32_t)read8(bus, addr + 1) << 16) |
           ((uint32_t)read8(bus, addr + 2) << 8) | (uint32_t)read8(bus, addr + 3);
}
void x68k_bus_write8(x68k_bus_t *bus, uint32_t addr, uint8_t val) {
    write8(bus, addr & 0xFFFFFFu, val);
}
void x68k_bus_write16(x68k_bus_t *bus, uint32_t addr, uint16_t val) {
    addr &= 0xFFFFFFu;
    write8(bus, addr, (uint8_t)(val >> 8));
    write8(bus, addr + 1, (uint8_t)val);
}
void x68k_bus_write32(x68k_bus_t *bus, uint32_t addr, uint32_t val) {
    addr &= 0xFFFFFFu;
    write8(bus, addr, (uint8_t)(val >> 24));
    write8(bus, addr + 1, (uint8_t)(val >> 16));
    write8(bus, addr + 2, (uint8_t)(val >> 8));
    write8(bus, addr + 3, (uint8_t)val);
}

/*
 * x68k_bus_peek8/16/32: mc68k_set_peek_hooks() 経由でmc68000コアのデバッガ
 * （mc68k_debug_read_bytes/mc68k_disassemble等）から呼ばれる非侵襲読み出し。
 * I/Oレジスタの副作用を一切起こさない（peek8()参照）。
 */
uint8_t x68k_bus_peek8(const x68k_bus_t *bus, uint32_t addr) {
    if (local_memory_contains(bus, addr))
        return bus->local_memory[addr - LOCAL_MEMORY_BASE];
    return peek8(bus, addr & 0xFFFFFFu);
}
uint16_t x68k_bus_peek16(const x68k_bus_t *bus, uint32_t addr) {
    return (uint16_t)((x68k_bus_peek8(bus, addr) << 8) | x68k_bus_peek8(bus, addr + 1));
}
uint32_t x68k_bus_peek32(const x68k_bus_t *bus, uint32_t addr) {
    return ((uint32_t)x68k_bus_peek8(bus, addr) << 24) |
           ((uint32_t)x68k_bus_peek8(bus, addr + 1) << 16) |
           ((uint32_t)x68k_bus_peek8(bus, addr + 2) << 8) |
           (uint32_t)x68k_bus_peek8(bus, addr + 3);
}

/*
 * RTC(RP5C15)時刻源の公開ラッパー。iocs.c の _DATESET/_TIMESET($51/$53)・
 * _DATEGET/_TIMEGET($54/$56)がMMIOレジスタと同じ時計を共有するために使う
 * （時刻源をx68k_bus_t側へ集約している）。
 */
int64_t x68k_bus_rtc_now(const x68k_bus_t *bus) {
    return rtc_now(&bus->rtc);
}

uint64_t x68k_bus_guest_mono_us(const x68k_bus_t *bus) {
    return guest_clock_mono_us(&bus->guest_clock);
}

void x68k_bus_guest_clock_set_paused(x68k_bus_t *bus, int paused) {
    guest_clock_set_paused(&bus->guest_clock, paused != 0);
}

void x68k_bus_rtc_set(x68k_bus_t *bus, int64_t target_unix) {
    rtc_set(&bus->rtc, target_unix);
}

void x68k_bus_fdc_set_num_drives(x68k_bus_t *bus, int n) {
    if (n >= 2 && n <= 4)
        bus->fdc.num_drives = n;
}

int x68k_bus_fdc_num_drives(const x68k_bus_t *bus) {
    return bus->fdc.num_drives;
}

/* ---- 内蔵SCSI (SPC) ---- */

void x68k_bus_scsi_irq_raise(x68k_bus_t *bus) {
    bus->spc_irq_pending = 1;
}

int x68k_bus_scsi_insert(x68k_bus_t *bus, int id, const char *path) {
    int rc = scsi_attach(&bus->spc_in, id, path);
    if (rc == 0 && id >= 0 && id < SCSI_NUM_UNITS) {
        scsi_unit_t *u = &bus->spc_in.units[id];
        /* removableなDISK(MO)のみ、ホストのファイル書き込み権限からWPを
         * 確定させる。host_path_write_protected()はbus層
         * static関数のためscsi_media.c(別翻訳単位)からは呼べず、ここに
         * 一本化している。非removable
         * (HDD)はattach_hdd()がfopenの成否(writable)から設定した仮値を
         * そのまま最終値として使う。 */
        if (u->removable && u->kind == SCSI_UNIT_DISK)
            u->write_protected = (uint8_t)host_path_write_protected(path);
        scsi_refresh_fingerprint(u);
    }
    return rc;
}

int x68k_bus_scsi_enable_overlay(x68k_bus_t *bus, int id, const char *path) {
    int rc = scsi_enable_overlay(&bus->spc_in, id, path);
    if (rc == 0 && id >= 0 && id < SCSI_NUM_UNITS)
        scsi_refresh_fingerprint(&bus->spc_in.units[id]);
    return rc;
}

void x68k_bus_scsi_eject(x68k_bus_t *bus, int id) {
    scsi_detach(&bus->spc_in, id);
}

int x68k_bus_scsi_unit_status(const x68k_bus_t *bus, int id, const char **out_path) {
    if (out_path)
        *out_path = NULL;
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    const scsi_unit_t *u = &bus->spc_in.units[id];
    if (!u->present)
        return 0;
    if (out_path)
        *out_path = u->path;
    return 1;
}

int x68k_bus_scsi_insert_empty_removable(x68k_bus_t *bus, int id) {
    return scsi_attach_empty_removable(&bus->spc_in, id);
}

int x68k_bus_scsi_insert_empty_cdrom(x68k_bus_t *bus, int id) {
    return scsi_attach_empty_cdrom(&bus->spc_in, id);
}

int x68k_bus_scsi_unit_attached(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    return bus->spc_in.units[id].fp != NULL;
}

int x68k_bus_scsi_unit_present(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    return bus->spc_in.units[id].present;
}

const char *x68k_bus_scsi_disk_path(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return NULL;
    return bus->spc_in.units[id].path;
}

int x68k_bus_scsi_unit_removable(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    return bus->spc_in.units[id].removable;
}

int x68k_bus_scsi_unit_is_cdrom(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    return bus->spc_in.units[id].kind == SCSI_UNIT_CDROM;
}

int x68k_bus_scsi_write_protected(const x68k_bus_t *bus, int id) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return 0;
    return bus->spc_in.units[id].write_protected != 0 ? 1 : 0;
}

int x68k_bus_scsi_set_write_protect(x68k_bus_t *bus, int id, int protect) {
    if (!bus || id < 0 || id >= SCSI_NUM_UNITS)
        return X68K_SCSI_ERR_INVALID_ID;
    scsi_unit_t *unit = &bus->spc_in.units[id];
    /* 対象はremovableなSCSI_UNIT_DISK(MO)のみ。固定HDDには物理ノッチが無く、
     * CD-ROMはwrite_protected=1固定で変更不可。 */
    if (!unit->removable || unit->kind != SCSI_UNIT_DISK)
        return X68K_SCSI_ERR_NOT_SUPPORTED;
    if (!unit->fp || !unit->path)
        return X68K_SCSI_ERR_NO_MEDIA;
    /* SCSIは単一initiator・単一共有バスのため、同時に選択されうるターゲットは
     * 高々1つ。このIDが現在の選択対象なら、SELECT成立からバスフリーに戻る
     * までの一連の取引の途中である可能性があるため一律拒否する（
     * FDCのphase!=FDC_IDLEと同じ「単純さ優先」の設計だが、SCSIは
     * ターゲットIDごとに独立してブロックできるため対象ID単位で足りる）。 */
    if (bus->spc_in.target_id == id)
        return X68K_SCSI_ERR_BUSY;

    const char *path = unit->path;
    struct stat st;
    if (stat(path, &st) != 0)
        return X68K_SCSI_ERR_IO;
    /* owner writeビット(S_IWUSR)のみをclear/setする。group/otherには
     * 一切触れないため、RW→WP→RWと往復しても元のパーミッションが完全に
     * 保たれる。 */
    mode_t mode = (mode_t)(st.st_mode & 07777);
    mode = protect ? (mode_t)(mode & (mode_t)~S_IWUSR) : (mode_t)(mode | S_IWUSR);
    if (chmod(path, mode) != 0)
        return X68K_SCSI_ERR_IO;

    /* 同一ホストファイルを複数SCSI IDへ挿入している場合、chmod()の効果は
     * 全IDへ及ぶ。write_protectedキャッシュがユニットごとに分かれていると
     * WPを迂回できてしまうため、接続中の全SCSIユニットのうちremovableな
     * DISK(MO)を再判定する。 */
    for (int i = 0; i < SCSI_NUM_UNITS; i++) {
        scsi_unit_t *u = &bus->spc_in.units[i];
        if (u->removable && u->kind == SCSI_UNIT_DISK && u->fp && u->path)
            u->write_protected = (uint8_t)host_path_write_protected(u->path);
        if (u->fp)
            scsi_refresh_fingerprint(u);
    }

    /* chmod()が成功してもパーミッションを持たないファイルシステム（既定
     * オプションのexFAT/FAT等）では効果が無いことがあるため、要求値を
     * 鵜呑みにせず再判定結果で食い違いを検出する。 */
    if (unit->write_protected != (protect ? 1 : 0))
        return X68K_SCSI_ERR_IO;
    scsi_media_changed(&bus->spc_in);
    return 0;
}

int x68k_bus_scsi_target_id(const x68k_bus_t *bus) {
    return bus->spc_in.target_id;
}

int x68k_bus_scsi_transferring(const x68k_bus_t *bus) {
    return scsi_dma_active(&bus->spc_in) || scsi_dma_write_active(&bus->spc_in);
}

void x68k_bus_scc_mouse_position(x68k_bus_t *bus, int x, int y) {
    scc_mouse_set_host_pos(&bus->scc, x, y);
}

void x68k_bus_scc_mouse_button(x68k_bus_t *bus, int left, int right) {
    scc_mouse_button(&bus->scc, left, right);
}

int x68k_bus_scc_pending_irq(const x68k_bus_t *bus) {
    return scc_pending_irq(&bus->scc);
}

/* ホストRS-232C経路(PTY等)からmain.cが毎フレーム受け取ったバイト列を
 * ch A受信リングへ橋渡しする。 */
void x68k_bus_scc_recv_a(x68k_bus_t *bus, const uint8_t *buf, int n) {
    scc_recv_a(&bus->scc, buf, n);
}

/* ch A送信リングからホストへ書き出すべきバイト列をmain.cが毎フレーム
 * 取り出す（main.cがそのままpal_serial_write()へ渡す）。 */
int x68k_bus_scc_send_a_dequeue(x68k_bus_t *bus, uint8_t *out, int max) {
    return scc_send_a_dequeue(&bus->scc, out, max);
}

/* ホストRS-232C経路の接続状態(DCD/CTS相当)をmain.cが起動時に反映する。 */
void x68k_bus_scc_serial_set_carrier(x68k_bus_t *bus, int connected) {
    scc_serial_set_carrier(&bus->scc, connected);
}

void x68k_bus_set_joystick(x68k_bus_t *bus, int index, uint8_t val) {
    if (index < 0 || index > 1)
        return;
    bus->joy_port[index] = val;
}

/* スプリアス割り込み通知（ベクタ24固定）: 該当デバイスなしでIACKに応じる
 * ケース（level 1/3/5 と、level 6(MFP)でベクタを渡せるpendingが無い場合）で使う。
 * level==7(NMI)はX68kで唯一オートベクタが仕様通り使われる経路のため呼ばないこと。 */
static uint8_t iack_spurious(x68k_bus_t *b) {
    mc68k_signal_bus_error(b->cpu);
    return 0u;
}

/*
 * IRQレベル問い合わせコールバック（pull型割り込み受付）。
 * mc68k_set_irq_level_hook()でCPUコアへ登録し、命令境界ごとに呼ばれる。
 * 優先度は1→3→5→6（自動的に高優先度が低優先度を上書きする、旧
 * x68k_cycle_catchup()の判定順を踏襲）。NMIは最優先。
 *
 * 呼び出し規約により副作用禁止・冪等でなければならない（同一命令境界で
 * 複数回呼ばれうる、STOP再開時等）。nmi_pendingも含め、各pendingビットは
 * ここでは読むだけで一切クリアしない。クリアはbus_iack()側（実際に
 * 受け付けられた時点で一度だけ呼ばれる）の責務に統一する。
 *
 * main.cがmc68k_create()直後にmc68k_set_irq_level_hook(cpu, x68k_bus_irq_level, bus)
 * で登録する（bus_hooks.hのフィールドではなくCPUコア側の登録API）
 * ため非staticで公開する。
 */
int x68k_bus_irq_level(void *ctx) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    if (b->nmi_pending)
        return 7;
    if (x68k_bus_ioi_pending_irq(b))
        return 1;
    if (x68k_bus_dmac2_pending_irq(b) || x68k_bus_dmac3_pending_irq(b))
        return 3;
    /* 要求ビットが立っていなければ詳細判定（別翻訳単位の関数）を呼ばない。
     * 詳細判定は各ビットの有無を前提にマスクするだけなので結果は同じ。 */
    if ((b->scc.recv_request || b->scc.recv_request_a) && x68k_bus_scc_pending_irq(b))
        return 5;
    if (b->mfp.ch_pending && x68k_bus_mfp_pending_irq(b))
        return 6;
    return 0;
}

/*
 * IACK コールバック: CPU が IRQ を受け付けるときに呼ばれる。
 * level=6（MFP = IRQ6）のとき保留ベクタを返し、クリアする。
 * level=7（NMI）はnmi_pendingをここでクリアする（x68k_bus_irq_level()側は
 * 冪等性の契約上クリアできないため）。実機同様オートベクタを使う。
 * level 1(IOI)/3(DMAC)/5(SCC)は各デバイスのベクタを返し、渡せるpendingが無ければ
 * スプリアス割り込みにする。それ以外のレベルは 0（オートベクタ）を返す。
 */
static uint8_t bus_iack(void *ctx, int level) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    x68k_bus_dmac_sync_to_now(b);
    if (level == 7) {
        b->nmi_pending = 0;
        return 0; /* オートベクタ（実機NMIの正規経路） */
    }
    if (level == 1) {
        /* IOI — 優先度: SPC > FDC > FDD > HDC > PRN (XEiJ IOInterrupt.ioiAcknowledge 準拠) */
        if (b->spc_irq_pending) {
            b->spc_irq_pending = 0;
            return SPC_IRQ_VECTOR;
        }
        if (b->ioi_request & IOI_FDC_REQUEST) {
            b->ioi_in_service = IOI_FDC_REQUEST;
            b->ioi_request &= ~IOI_FDC_REQUEST;
            return (uint8_t)(b->ioi_vector | IOI_FDC_VECTOR);
        }
        if (b->ioi_request & IOI_FDD_REQUEST) {
            b->ioi_in_service = IOI_FDD_REQUEST;
            b->ioi_request &= ~IOI_FDD_REQUEST;
            return (uint8_t)(b->ioi_vector | IOI_FDD_VECTOR);
        }
        return iack_spurious(b);
    }
    if (level == 3) {
        /* DMAC ch2/ch3の正常割り込み。優先順位・pending消費・NIV参照は
         * dmac_iack()へ集約する。 */
        uint8_t vector;
        if (dmac_iack(b, &vector))
            return vector;
        return iack_spurious(b);
    }
    if (level == 5) {
        /* SCC ch A(RS-232C)/ch B(マウス) Rx割り込み。
         * IACK側はWR1マスクを再チェックしないこと（IRQ線はポーリング粒度
         * でしか更新されず、立った後にマスクが変化した「古い線」があり得る。
         * MFPの同型の割り込み経路と同じ教訓）。recv_request(_a)が立っている限り
         * 必ずベクタを返す。実チップの優先度チェーンではch Aがch Bより
         * 上位のため、ch Aを先にチェックする。 */
        if (b->scc.recv_request_a)
            return scc_iack_a(&b->scc);
        if (b->scc.recv_request)
            return scc_iack(&b->scc);
        return iack_spurious(b);
    }
    if (level != 6)
        return 0;
    /* MFP — 優先度はMC68901のチャンネル番号降順（IERA bit7=ch15が最高、IERB
     * bit0=ch0が最低）: H-SYNC (ch15) > ラスタ一致 (ch14) > Timer A (ch13)
     * > USART RX (ch12) > Timer B (ch8) > GPIP4/V-DISP (ch6) > Timer C (ch5)
     * > Timer D (ch4) > FMIRQ (ch3)。ここは
     * IMRA/IMRB でゲートしないこと。IRQ 線は MFP_TICK_CYCLES 粒度でしか
     * 更新されないため、線が立った後に IMRx が落ちる「古い線」が有り得る。
     * pending が立っている限り必ず正規ベクタを返す。
     *
     * mfp_ch_pending（ビット位置=チャンネル番号）から、in-serviceでブロック
     * されていない最上位ビットを1回のスキャンで取り出す。
     * IPRクリア・ISRセットはmfp_iack_accept()ヘルパに集約している
     * （S=1のときのみISRセット）。
     *
     * isr_top によるIn-Service優先度ゲーティング:
     * S=0のときmfp_isr_top()は常に-1を返すため、block=0となり全ビット許可に
     * 退化する。
     *
     * 渡せるベクタが1つも無いとき（candidates==0）は他レベルと同じく
     * iack_spurious()でスプリアス割り込み(ベクタ24)へ倒す。ここへ到達するのは
     * 「IRQ線を上げてからIACKまでの間にpendingが消える」場合で、IMRA/IMRB
     * 書き込みがマスクしたチャンネルのch_pendingをクリアする（mfp.c）ため
     * 実際に起こる。PCM8A.Xの割り込みハンドラは冒頭でMFPを部分マスクした直後に
     * IPLを7から5へ下げるため、この窓でベクタの無いレベル6割り込みを取る。
     * ここでベクタ0（=オートベクタ$78＝ベクタ30、IPLROM未設定でゴミ）を返すと
     * 未定義ベクタへ飛んでHuman68kの例外ダイアログになる。実機でもMFPレジスタ
     * 操作による割り込み禁止とMPUの割り込み受付が競合するとスプリアス割り込みが
     * 生じうる（X68k I/O Map Data Crystal MFP節）ため、IACKに応答できない
     * この状況はスプリアス割り込みとして扱うのが正しい。 */
    int isr_top = mfp_isr_top(b);
    uint16_t block = (isr_top < 0) ? 0u : (uint16_t)((2u << isr_top) - 1u);
    uint16_t candidates = b->mfp.ch_pending & (uint16_t)~block;
    if (candidates == 0u)
        return iack_spurious(b);
    int ch = 15;
    while (!(candidates & (uint16_t)(1u << ch)))
        ch--;
    mfp_ch_pending_clear(&b->mfp, ch);
    mfp_iack_accept(b, ch);
    return (uint8_t)(b->mfp.vr | (unsigned)ch);
}

/* RESET 命令コールバック: XEiJ ioiReset() 相当 */
static void bus_reset_peripheral(void *ctx) {
    x68k_bus_t *b = (x68k_bus_t *)ctx;
    /* システムポート#5 ($E8E009) を既定値$00相当へ戻す。
     * IPLROMが起動シーケンスで改めて書き込むため、これは「最初の書込み
     * より前の状態」を定義するだけの安全側の既定値。 */
    x68k_bus_apply_wait_port(b, 0x00u);
    /* システムポート#6 の途中ステートはクリアする。電源断要求は取り消さない。 */
    b->poweroff_seq = SYSPORT6_POWEROFF_SEQ0;
    /* RESET 命令実行後、IOI はプリンタ信号のみアサート (XEiJ ioiReset 準拠) */
    b->ioi_signal = IOI_PRN_SIGNAL;
    b->ioi_enable = 0;
    b->ioi_request = 0;
    b->ioi_in_service = 0;
    b->spc_irq_pending = 0;
    /* DMAC(HD63450)のRESET相当。回路図でIC23 HD63450-10の27番ピン
     * (BEC2)がネットRESETに直結していることを確認済み
     * （回路図 p5）。従来はch3(ADPCM)の
     * active/paused/cntとCSR/CCRだけを落としており、ch0-ch2の全レジスタと
     * ch0(FDC)武装フラグ・ch3の転送途中状態がソフトリセットをまたいで残留していた。
     * IPLROMはRESET命令($FF001A、IPLROM30は$FF0040)の後にDMACを初期化する
     * （$FF0E1C等）ため、全クリアしてもブート経路は影響を受けない。 */
    dmac_reset(b);
    /* FDC(µPD72065)のRESET相当。回路図でIC203の1番ピン(RESET)が
     * RESETネットに直結していることを確認済み。
     *DMAC ch0の武装フラグ(ch0_armed)とFDCの実行データフェーズ(phase/
     * dma_*)は一対の状態のため、dmac_reset()の直後にまとめて落とす。disk[]等
     * のメディア挿入状態・cur_cyl[]（機械的ヘッド位置、RESETでは動かない）は
     * 保持する。 */
    fdc_reset(&b->fdc);
    /* FDD制御ラッチ（PEDEC側）。$E94005の前回値を残すと、RESETを
     * またぐ選択bitの1→0エッジで意図しない排出が起き得るため直接クリアする。
     * 書込みハンドラを経由すると同じ副作用を発生させるので、ここでは使わない。
     * EJECT MASKは確定した排出禁止状態、ejected_flagは抜き挿しの記録なので保持する。
     * 詳細は docs/ix68k-internals/design/fdc_design.md §6.4。 */
    b->drv_ctrl_latch = 0;
    b->fdc_drive_sel = 0;
    b->fdc_motor_on = 0;
    scsi_reset(&b->spc_in); /* SCTL.RD相当。RESET未実行だとSCSI16INが古いレジスタ
                             * 状態(前回のSelect/PSNS等)を見て暴走する */
    sasi_reset(&b->hdc_in);
    /* MFP(MC68901)のRESET相当（実験的追加）。従来ここでMFPに
     * 一切触れておらず、Timer C等がソフトリセットをまたいで自走を続けていた。
     * その残留位相（＝リセットした実時刻）がIPLROM30のCPU速度較正結果の
     * ばらつきの原因という仮説を検証するための変更。 */
    mfp_reset(b);
    scc_reset(&b->scc);
    adpcm_reset(&b->adpcm);
    opm_reset(&b->opm);
    opm_set_cpu_hz(&b->opm, b->cpu_hz); /* opm_reset() 後も選択中の CPU クロックを維持 */
    b->joy_port[0] = 0xFFu;             /* ジョイスティック#1/#2 未接続（アクティブロー） */
    b->joy_port[1] = 0xFFu;
    /* RESET 後、FDD READY 信号は継続 HIGH (ディスク挿入中) → FDD 信号を復元 */
    for (int i = 0; i < 4; i++) {
        if (b->fdc.disk[i]) {
            ioi_fdd_rise(b);
            break;
        }
    }
    /* HOSTFILE: 開いているFILE*・未回収の一時ファイルを片付ける
     * （中断されたGET/PUT転送があってもリークしないようにする）。 */
    hostfile_reset_transfer_state(&b->hostfile);
    /* HOSTDRV remote: 開いているホストfd・検索セッションを片付ける */
    hostdrv_remote_reset();
}

/* MCP/ソフトリセット用の公開ラッパ（bus_reset_peripheral は mc68k_bus_t.reset_peripheral
 * フック用に void* シグネチャのため、型付きの公開版を別途用意する） */
void x68k_bus_reset_peripheral(x68k_bus_t *bus) {
    bus_reset_peripheral(bus);
}

void x68k_bus_set_boot_dev_watch(x68k_bus_t *bus, int enable) {
    bus->boot_dev_watch_armed = enable ? 1 : 0;
    bus->boot_dev_read_seen = 0;
}

int x68k_bus_boot_dev_watch_armed(const x68k_bus_t *bus) {
    return bus->boot_dev_watch_armed;
}

int x68k_bus_take_boot_dev_read(x68k_bus_t *bus) {
    int v = bus->boot_dev_read_seen;
    bus->boot_dev_read_seen = 0;
    return v;
}

/* NMIボタン相当の要求を立てる。x68k_cycle_catchup()が
 * pal_nmi_requested()（ホストのNMIボタン）をここへ橋渡しし、
 * mcp_cmd_interrupt()もデバッグ用NMI注入にこれを使う。実際のクリアは
 * bus_iack()がlevel==7を受け付けた時点で行う（bus_irq_level()参照）。 */
void x68k_bus_set_nmi_pending(x68k_bus_t *bus) {
    bus->nmi_pending = 1;
}

int x68k_bus_power_off_requested(const x68k_bus_t *bus) {
    return bus->power_off_requested;
}

mc68k_bus_t x68k_bus_get_hooks(x68k_bus_t *bus) {
    /* access_timing を追加した際、ここが `mc68k_bus_t h;` の
     * 未初期化宣言のままだと新フィールドへ不定値が入る（サイレントUB）。
     * 明示的に {0} で初期化してから代入する
     * （cpu_design.md §2）。 */
    mc68k_bus_t h = {0};
    h.read_byte = bus_read_byte;
    h.read_word = bus_read_word;
    h.read_long = bus_read_long;
    h.write_byte = bus_write_byte;
    h.write_word = bus_write_word;
    h.write_long = bus_write_long;
    h.sync = bus_sync;
    h.write_hook = bus_write_hook;
    h.iack = bus_iack;
    h.reset_peripheral = bus_reset_peripheral;
    h.ctx = bus;
    h.access_timing = bus_access_timing;
    /* X68K_NO_PAGE_MAP が設定されていれば表を使わない（差分確認・切り分け用） */
    if (bus->pmap && !getenv("X68K_NO_PAGE_MAP"))
        h.page_map = &bus->pmap->map;
    return h;
}
