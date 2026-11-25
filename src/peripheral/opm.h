#ifndef X68K_OPM_H
#define X68K_OPM_H

#include "audio/ym2151.h"
#include <stdint.h>

/*
 * OPM(YM2151) FM音源 X68000グルー層。$E90000-$E91FFFのレジスタ選択/データポート・
 * ステータスを扱う。波形合成コアはaudio/ym2151.c、CT1/CT2は
 * peripheral/adpcm.cが独立に追跡する。
 */

typedef struct {
    ym2151_t core; /* 波形合成コア */

    uint8_t reg_addr; /* $E90001で選択中のレジスタ番号（YM2151アドレスラッチ） */

    /* BUSY期間の締切とOPMクロック累積値。
     * clk_frac: CPUサイクル→OPMクロック変換の端数。cpu_hz可変対応でCPUクロックに
     * 応じた既約分数(clk_ratio_num/clk_ratio_den)を使う（opm_set_cpu_hz参照）。 */
    uint64_t busy_until_clk;
    uint64_t clk_accum;
    uint32_t clk_frac;
    uint32_t cpu_hz;        /* 実行中CPUクロック(Hz)。既定10,000,000 */
    uint32_t clk_ratio_num; /* OPM_HZ/cpu_hzの既約分数（opm_set_cpu_hzでキャッシュ） */
    uint32_t clk_ratio_den;

    /* OPMのFMIRQ生線(1=アサート)。書き込みとtickのたびに再評価する。 */
    int fmirq_line;
} opm_t;

void opm_reset(opm_t *o);

/* CPUクロック(Hz)を変更する。opm_tick()の変換比を再計算してキャッシュする
 * （CPUクロック可変対応、x68k_bus_set_cpu_hz()から呼ばれる）。opm_resetは
 * 既定10,000,000(10MHz)を設定する。 */
void opm_set_cpu_hz(opm_t *o, uint32_t cpu_hz);

/* $E90001/$E90003（offはOPM_BASEからのオフセット）。$E90001=アドレスラッチ、
 * $E90003=データ書き込み。データ書き込みは合成コアへ供給し、REG_MODE(0x14)
 * 以外はBUSYをセットする(YM2151.java writeData L1465-1467相当)。 */
void opm_reg_write8(opm_t *o, uint32_t off, uint8_t val);

/* OPMはA0を無視し、読み出しは常にステータスレジスタを返す
 * （bit7=BUSY, bit1=TimerBフラグ, bit0=TimerAフラグ）。 */
uint8_t opm_status_read8(const opm_t *o);

/* OPM内蔵タイマA/BをCPUサイクル単位で進める。main.cのチャンクループから
 * x68k_bus_mfp_tick()と並べて呼ぶ。音声生成(ym2151_generate)とは独立した
 * 粒度で進めることでテンポ破綻を防ぐ。 */
void opm_tick(opm_t *o, int cpu_cycles);

#endif /* X68K_OPM_H */
