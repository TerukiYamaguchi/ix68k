#include "peripheral/opm.h"
#include "bus/bus_internal.h"

/* YM2151.java writeData() L1465-1467: 32*clock_prescale()。clock_prescaleは
 * DEFAULT_PRESCALE=2固定(YM2151.java L205,L1278、set_clock_prescale()を
 * 呼ぶ実装は無い)。よって32*2=64 OPMクロック。 */
#define OPM_BUSY_CLOCKS 64u
#define OPM_HZ          4000000u /* OPM(YM2151)クロックは実機で常時4MHz固定 */

static void opm_refresh_fmirq_line(opm_t *o) {
    o->fmirq_line = (ym2151_status(&o->core) != 0) ? 1 : 0;
}

static uint32_t opm_gcd_u32(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

void opm_set_cpu_hz(opm_t *o, uint32_t cpu_hz) {
    o->cpu_hz = cpu_hz;
    uint32_t g = opm_gcd_u32(OPM_HZ, cpu_hz);
    o->clk_ratio_num = OPM_HZ / g;
    o->clk_ratio_den = cpu_hz / g;
}

void opm_reset(opm_t *o) {
    ym2151_reset(&o->core);
    o->reg_addr = 0;
    o->busy_until_clk = 0;
    o->clk_accum = 0;
    o->clk_frac = 0;
    o->fmirq_line = 0;
    opm_set_cpu_hz(o, 10000000u); /* 既定10MHz(実機標準) */
}

void opm_reg_write8(opm_t *o, uint32_t off, uint8_t val) {
    if (off == 0x01u) {
        o->reg_addr = val; /* アドレスラッチ */
        return;
    }
    if (off == 0x03u) {
        ym2151_write(&o->core, o->reg_addr, val);
        /* REG_MODE(0x14)書き込みはBUSYをセットしない
         * (YM2151.java writeData L1434-1438で早期return)。 */
        if (o->reg_addr != YM2151_REG_MODE)
            o->busy_until_clk = o->clk_accum + OPM_BUSY_CLOCKS;
        opm_refresh_fmirq_line(o); /* FRESET等による即時変化を反映 */
        return;
    }
}

uint8_t opm_status_read8(const opm_t *o) {
    uint8_t busy = (o->clk_accum < o->busy_until_clk) ? 0x80u : 0u;
    return (uint8_t)(busy | ym2151_status(&o->core));
}

void opm_tick(opm_t *o, int cpu_cycles) {
    /* OPMクロック=4MHz固定。CPUクロックはo->cpu_hz(opm_set_cpu_hzでキャッシュ済みの
     * 既約分数clk_ratio_num/den)に応じて可変。mfp.cのmfp_tc_frac(Timer C)と同じ
     * 整数端数累積方式。 */
    o->clk_frac += (uint32_t)cpu_cycles * o->clk_ratio_num;
    uint32_t opm_clocks = o->clk_frac / o->clk_ratio_den;
    o->clk_frac %= o->clk_ratio_den;
    o->clk_accum += opm_clocks;

    ym2151_run_timers(&o->core, opm_clocks);
    opm_refresh_fmirq_line(o);
}

/* bus非依存のopm_tick()を呼び、FMIRQ線の変化をMFPへ通知する。 */
void x68k_bus_opm_tick(x68k_bus_t *b, int cpu_cycles) {
    int before = b->opm.fmirq_line;
    opm_tick(&b->opm, cpu_cycles);
    if (b->opm.fmirq_line != before)
        x68k_bus_mfp_opmirq(b, b->opm.fmirq_line);
}

/* OPMへ書き込み、FMIRQ線の変化をMFPへ通知する。 */
void x68k_bus_opm_reg_write8(x68k_bus_t *b, uint32_t off, uint8_t val) {
    int before = b->opm.fmirq_line;
    opm_reg_write8(&b->opm, off, val);
    if (b->opm.fmirq_line != before)
        x68k_bus_mfp_opmirq(b, b->opm.fmirq_line);
}

/* 62500Hz固定のステレオint32波形を生成する。 */
void x68k_bus_opm_generate(x68k_bus_t *b, int32_t *buf, int frames) {
    ym2151_generate(&b->opm.core, buf, frames);
}
