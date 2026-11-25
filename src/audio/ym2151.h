#ifndef X68K_YM2151_H
#define X68K_YM2151_H

#include <stdint.h>

/*
 * OPM(YM2151) FM音源合成コア。ymfm(Copyright (c) 2021, Aaron Giles、
 * BSD 3-Clause License, https://github.com/aaronsgiles/ymfm)を、XEiJ
 * (xeij/YM2151.java, https://stdkmd.net/xeij/)のJava移植を経由してC言語へ
 * 再移植したもの。ライセンス全文はLICENSES/ymfm-BSD-3-Clause.txtを参照。
 * バス/CPU/MFPに非依存の純ロジック。BUSY・クロック供給・MFP割り込みは
 * peripheral/opm.c、CT1/CT2はperipheral/adpcm.cが扱う。
 * CSM(レジスタ$14 bit7)によるキーオントリガは未実装。
 */

#define YM2151_CHANNELS  8
#define YM2151_OPERATORS 32 /* YM2151_CHANNELS * 4 */
#define YM2151_REGISTERS 256
#define YM2151_REG_MODE  0x14u /* opm.cがBUSY判定除外(YM2151.java L1434-1438)に使う */

/* fm_operator::prepare()が毎サンプル前に計算しキャッシュする値
 * （YM2151.java opdata_cache, L610-624相当）。 */
typedef struct {
    uint32_t phase_step;  /* PHASE_STEP_DYNAMIC(1)ならPM有効時に毎サンプル再計算 */
    uint32_t total_level; /* トータルレベル×8 */
    uint32_t block_freq;
    int32_t detune;
    uint32_t multiple;
    uint32_t eg_sustain;
    /* index 1..4 = EG_ATTACK/DECAY/SUSTAIN/RELEASE使用。0/5はJava同様未使用
     * （EG_STATES=6のまま配列サイズをそろえ、添字を直接対応させる）。
     */
    uint8_t eg_rate[6];
} ym2151_opdata_cache_t;

/* fm_operator（YM2151.java L640-970相当）。OPMの1オペレータ(スロット)。 */
typedef struct {
    int choffs;     /* 所属チャンネルのレジスタオフセット(0-7、chnumと同値) */
    int opoffs;     /* オペレータのレジスタオフセット(0-31、s_fixed_map由来) */
    uint32_t phase; /* 10.10形式の現在位相 */
    uint16_t env_attenuation;
    int env_state; /* EG_ATTACK..EG_RELEASE(ym2151.c内部定義) */
    uint8_t key_state;
    uint8_t keyon_live; /* bit0=通常keyon, bit1=リズム(未使用), bit2=CSM(未使用) */
    ym2151_opdata_cache_t cache;
} ym2151_operator_t;

/* fm_channel（YM2151.java L976-1169相当）。4オペレータをまとめる。 */
typedef struct {
    int choffs;
    int16_t feedback_0, feedback_1, feedback_in;
    ym2151_operator_t *op[4]; /* s_fixed_mapで割り当てたoperatorsへのポインタ */
} ym2151_channel_t;

/* YM2151エンジン全体の状態（ym2151, fm_engine_base, opm_registers相当）。 */
typedef struct {
    uint8_t address; /* $E90001でラッチしたレジスタ番号(m_address) */
    uint32_t env_counter;
    uint8_t status;       /* bit0=TimerA, bit1=TimerB */
    uint8_t total_clocks; /* 下位8bitのみ保持(Java uint8キャストに対応) */
    uint32_t active_channels;
    uint32_t modified_channels;
    uint32_t prepare_count;

    ym2151_channel_t channel[YM2151_CHANNELS];
    ym2151_operator_t operators[YM2151_OPERATORS];

    uint32_t lfo_counter;
    uint32_t noise_lfsr;
    uint8_t noise_counter;
    uint8_t noise_state;
    uint8_t lfo_am;

    /* OPM内蔵タイマA/B(YM2151.java engine_timer_expired/update_timer,
     * L1594-1719相当)。MC68901(MFP)内蔵Timer A/B/Dとは完全に別物
     * （注意喚起済み）。
     * running: LOAD(レジスタ$14 bit0/1)によるエッジトリガのstart/stop状態。
     * running中はオーバーフロー毎にIRQENの状態に関わらず自動リロードする。
     */
    uint32_t timer_a_counter;
    uint32_t timer_b_counter;
    uint8_t timer_a_running;
    uint8_t timer_b_running;

    uint8_t regdata[YM2151_REGISTERS];
    int16_t lfo_waveform[256 * 4]; /* LFO波形256エントリ×4種、AM=下位8bit/PM=上位8bit */
    uint16_t waveform[1024];       /* WAVEFORM_LENGTH(0x400)。OPMは波形1種のみ */
} ym2151_t;

void ym2151_reset(ym2151_t *c);

/* $E90001(アドレスラッチ)/$E90003(データ)をまとめて1回の呼び出しで扱う。
 * addr=直前にラッチされたレジスタ番号、data=書き込みバイト。 */
void ym2151_write(ym2151_t *c, uint8_t addr, uint8_t data);

/* 62500Hz固定のステレオint32をframesサンプル生成する。各チャンネルを加算後、
 * YM3012相当の量子化と-32768〜32767のクリップを行う。buf[2*i]=L, buf[2*i+1]=R。 */
void ym2151_generate(ym2151_t *c, int32_t *buf, int frames);

/* bit1=TimerBフラグ, bit0=TimerAフラグ。BUSYビットは含まない
 * (opm.cのopm_status_read8が別途合成する)。 */
uint8_t ym2151_status(const ym2151_t *c);

/* OPM内蔵タイマA/Bをopm_clocks分進める。オーバーフロー時、
 * IRQEN(レジスタ$14 bit2/3)が
 * 立っていればステータスフラグをセットし、IRQENの状態に関わらず自動的に
 * 周期値からリロードする(YM2151.java engine_timer_expired相当)。
 * CSM(bit7)によるキーオントリガは未実装。呼び出し元はopm_tick()。 */
void ym2151_run_timers(ym2151_t *c, uint32_t opm_clocks);

#endif /* X68K_YM2151_H */
