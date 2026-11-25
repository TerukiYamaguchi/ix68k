#include "audio/ym2151.h"

/* ymfm (Copyright (c) 2021, Aaron Giles, BSD 3-Clause License) をXEiJの
 * Java移植経由でCへ移植したYM2151タイマ進行部。ライセンス全文は
 * LICENSES/ymfm-BSD-3-Clause.txtを参照。 */

#define REG_MODE      0x14u
#define STATUS_TIMERA 0x01u
#define STATUS_TIMERB 0x02u

static uint32_t timer_a_period(const ym2151_t *c) {
    uint32_t value = ((uint32_t)c->regdata[0x10] << 2) | (c->regdata[0x11] & 0x03u);
    return 64u * (1024u - value);
}

static uint32_t timer_b_period(const ym2151_t *c) {
    return 1024u * (256u - c->regdata[0x12]);
}

void ym2151_run_timers(ym2151_t *c, uint32_t opm_clocks) {
    for (uint32_t i = 0; i < opm_clocks; i++) {
        if (c->timer_a_running && --c->timer_a_counter == 0) {
            if ((c->regdata[REG_MODE] & 0x04u) != 0)
                c->status |= STATUS_TIMERA;
            if ((c->regdata[REG_MODE] & 0x80u) != 0) {
                /* CSMキーオントリガは未実装。タイマのフラグとリロードは行う。 */
            }
            c->timer_a_counter = timer_a_period(c); /* IRQENに関係なくリロードする。 */
        }
        if (c->timer_b_running && --c->timer_b_counter == 0) {
            if ((c->regdata[REG_MODE] & 0x08u) != 0)
                c->status |= STATUS_TIMERB;
            c->timer_b_counter = timer_b_period(c);
        }
    }
}
