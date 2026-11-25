#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <limits.h>
#include <string.h>

/* ---- DMAC(HD63450) 4ch共通のレジスタオフセット定数 ----
 * X68000には同一のHD63450が4ch分搭載されており、各チャンネルは$40バイト
 * 間隔でレジスタが並ぶ（『Inside X68000』1992年 p33 図3）。
 * ch0=FDC、ch1=内蔵SCSI・HDC、ch2=汎用メモリ間転送(IOCS _DMAMOVE系)、
 * ch3=ADPCM（同 p31 図2）。chベースアドレス + レジスタオフセットで
 * 各chの実オフセットを組み立てる。 */
#define DMAC_CH0_BASE 0x4000u
#define DMAC_CH1_BASE 0x4040u
#define DMAC_CH2_BASE 0x4080u
#define DMAC_CH3_BASE 0x40C0u /* ADPCM用 */

/* ch2メモリ間転送の所要時間モデル。dual address転送の1アクセス（read/write）は5クロックとし、
 * 1転送単位（byte=1バイト、word=2バイト）あたり read5 + write5 の計10クロックとする近似。
 * 5クロックは参照実装XEiJの値（XEiJ.java の dmaReadCycles/dmaWriteCycles、PRO/Compact以外の
 * 機種）を借りたもので、データシートでは照合していない。
 * DMACのクロックもXEiJに合わせ、X68000系（68000/68010/68020）は10MHz、X68030系
 * （68030/68EC030と、それを土台にする68040/68060）は12.5MHzとする。ix68kには機種設定が
 * なくCPUモデルだけなので、CPUモデルで切り替える。実機では未較正。
 * XEiJ自体はIPLROMの_DMAMOVE設定（DCR=$08 16bitポート、OCR.SIZE=00 パックありバイト）では
 * 2バイトに1回だけ read5/write5
 * をかけて操作ごとに追加クロックを足す（約9〜14クロック/バイト）。
 * 10クロック/バイトになるのはXEiJではSIZE=11（パックなし）のときだけで、ここでは近似として
 * byte/wordとも10クロック/単位を使う。バス調停・リフレッシュ・CPUとのバス競合による延びは
 * 含めない（基準時間）。 */
#define DMAC_CLOCK_HZ_X68000 10000000u
#define DMAC_CLOCK_HZ_X68030 12500000u
#define DMAC_CLOCKS_PER_UNIT 10u

/* CPU未配線（単体試験等）は10MHz。 */
static uint32_t dmac_clock_hz(const x68k_bus_t *b) {
    if (!b->cpu)
        return DMAC_CLOCK_HZ_X68000;
    mc68k_model_t model = mc68k_get_model(b->cpu);
    return model >= MC68K_MODEL_68EC030 ? DMAC_CLOCK_HZ_X68030 : DMAC_CLOCK_HZ_X68000;
}

#define DMAC_REG_CSR     0x00u /* 1B, write-1-to-clear */
#define DMAC_REG_CER     0x01u /* 1B, error code */
#define DMAC_REG_OCR     0x05u /* 1B */
#define DMAC_REG_SCR     0x06u /* 1B */
#define DMAC_REG_CCR     0x07u /* 1B, bit7=START */
#define DMAC_REG_MTC     0x0Au /* 2B */
#define DMAC_REG_MAR     0x0Cu /* 4B（ch2）または+1した3B/24bitアドレス（ch0/ch1） */
#define DMAC_REG_DAR     0x14u /* 4B、全ch共通 */
#define DMAC_REG_BTC     0x1Au /* 2B、アレイチェーンのみ */
#define DMAC_REG_BAR     0x1Cu /* 4B、チェーンのみ */
/* RESET後は$0F（未初期化割り込みベクタ番号15相当、XEiJ HD63450.java dmaReset()準拠）。
 * IACK時は要求チャネルのNIVをそのまま返す（dmac_iack()参照）。 */
#define DMAC_REG_NIV     0x25u /* 1B, Normal Interrupt Vector */
#define DMAC_REG_EIV     0x27u /* 1B, Error Interrupt Vector。RESET後は$0F、意味はNIVと同様 */
/* GCR($40FF)はch3ブロックの末尾に含まれる。 */
#define DMAC_CH_REG_SPAN 0x40u /* 1ch分のレジスタ領域幅 */

/* CSR(+$00)のビット。
 * bit6のBTCは Block Transfer Complete（CNT継続動作でブロックを1つ転送し
 * 終えたときに立つ）であり、レジスタのBTC(+$1A, Base Transfer Counter)とは
 * 名前が同じだけの別物なので注意する。
 * bit3のACTはチャンネルが動作中であることを示すライブステータスで、
 * write-1-to-clearの対象外（DMAC_CSR_WRITE_CLEARABLE参照）。 */
#define DMAC_CSR_COC 0x80u
#define DMAC_CSR_ERR 0x10u
#define DMAC_CSR_BTC 0x40u
#define DMAC_CSR_ACT 0x08u

/* CCR(+$07)のビット（『Inside X68000』1992年 p45 図11相当）。 */
#define DMAC_CCR_STR 0x80u /* STaRt operation */
#define DMAC_CCR_CNT 0x40u /* CoNTinue operation */
#define DMAC_CCR_HLT 0x20u /* HaLT operation */
#define DMAC_CCR_SAB 0x10u /* Software ABort */
#define DMAC_CCR_ITE 0x08u /* InTerrupt Enable */

/* write-1-to-clearの対象ビット: COC(bit7)/BTC(bit6)/NDT(bit5)/ERR(bit4)/
 * DIT(bit2)/PCT(bit1)。ACT(bit3)は転送の進行状態そのもの、PCS(bit0)は~PCL端子の
 * 現在値であり、どちらも書き込みでは変化しない（HD63450データシート/
 * XEiJ HD63450.java dmaWriteByte()のDMA_CSR分岐）。 */
#define DMAC_CSR_WRITE_CLEARABLE 0xF6u

/* CSR write-1-to-clear（全ch共通、『Inside X68000』1992年 p43 図11）。
 * 上記の各ビットは、そのビットに1を書き込んだときだけクリアされる。単純な
 * 上書き代入にすると、ROMが転送前後に書く#$FFで全ビットが常に1（＝ERRビットも
 * 常時セット）に見えてしまい、正規の完了/エラー判定が誤動作する。
 * ACT/PCSまでクリアしてしまうのも同様に誤りで、ADPCM再生ドライバが
 * #$FFでCSRをクリアした直後にACTを読んで転送継続中かを判定できなくなる
 * （dmac_write8()のch3 CCR分岐のコメント参照）。 */
static void dmac_csr_clear(x68k_bus_t *b, uint32_t ch_base, uint8_t val) {
    b->crtc_regs[ch_base + DMAC_REG_CSR] &= (uint8_t)~(val & DMAC_CSR_WRITE_CLEARABLE);
}

/* ch3のCCR.CNT(bit6)武装状態を、内部状態とゲストが読み返すCCRレジスタの両方へ
 * 反映する。PCM8A.XはCCRへCNTを書いた直後に `BTST #6,7(A5)` で読み返し、
 * 立っていなければ「チャンネルが既に停止していた」と判断してフルリスタート
 * (SAB→MAR/MTC再設定→START)経路へ落ちるため、書いた値をそのまま保持するのでは
 * なく実際に武装できたかどうかを返す必要がある。 */
static void adpcm_dma_set_cnt(x68k_bus_t *b, int on) {
    b->dmac.ch3.cnt = on;
    if (on)
        b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CCR] |= DMAC_CCR_CNT;
    else
        b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CCR] &= (uint8_t)~DMAC_CCR_CNT;
}

/* 4バイト(32bit)幅レジスタの読み書き。OCR/SCR/MTC/MAR/DAR/BTC/BARの各バイトは
 * CSR/CCR以外dmac_write8()で横取りせず、既存のCRTC領域汎用フォールバック
 * （生値ストア）にそのまま任せる。offはch_base + DMAC_REG_*で組み立てる。 */
static uint32_t dmac_reg_l(const x68k_bus_t *b, uint32_t off) {
    return ((uint32_t)b->crtc_regs[off] << 24) | ((uint32_t)b->crtc_regs[off + 1] << 16) |
           ((uint32_t)b->crtc_regs[off + 2] << 8) | (uint32_t)b->crtc_regs[off + 3];
}

static void dmac_set_reg_l(x68k_bus_t *b, uint32_t off, uint32_t val) {
    b->crtc_regs[off] = (uint8_t)(val >> 24);
    b->crtc_regs[off + 1] = (uint8_t)(val >> 16);
    b->crtc_regs[off + 2] = (uint8_t)(val >> 8);
    b->crtc_regs[off + 3] = (uint8_t)val;
}

/* 2バイト(16bit)幅レジスタ（MTC/BTC）の読み書き。 */
static uint32_t dmac_reg_w(const x68k_bus_t *b, uint32_t off) {
    return ((uint32_t)b->crtc_regs[off] << 8) | (uint32_t)b->crtc_regs[off + 1];
}

static void dmac_set_reg_w(x68k_bus_t *b, uint32_t off, uint32_t val) {
    b->crtc_regs[off] = (uint8_t)(val >> 8);
    b->crtc_regs[off + 1] = (uint8_t)val;
}

/* MAR専用: 68000のアドレス空間は24bitのため、ch0/ch1は実機でもMARの
 * 最上位バイト(off+0)を使わず下位24bit(off+1〜off+3)だけを読み書きする
 * （実機計装で確認）。ch2のMARは`dmac_reg_l`で4バイト全体を読む。この
 * 非対称性は実機の挙動を忠実に再現するための意図的なもの。offはch_base +
 * DMAC_REG_MARを渡す（内部でoff+1〜off+3を読む）。 */
static uint32_t dmac_reg_addr24(const x68k_bus_t *b, uint32_t off) {
    return ((uint32_t)b->crtc_regs[off + 1] << 16) | ((uint32_t)b->crtc_regs[off + 2] << 8) |
           (uint32_t)b->crtc_regs[off + 3];
}

static void dmac_set_reg_addr24(x68k_bus_t *b, uint32_t off, uint32_t val) {
    b->crtc_regs[off + 1] = (uint8_t)(val >> 16);
    b->crtc_regs[off + 2] = (uint8_t)(val >> 8);
    b->crtc_regs[off + 3] = (uint8_t)val;
}

/* 1ブロック分の転送。dir=0: MAR→DAR、dir=1: DAR→MAR
 * （『Inside X68000』1992年 p47 図12 OCR.DIR）。mac/dacはMAR/DAR
 * それぞれの1バイトごとの増減（0:固定 1:++ 2:--、同 p55 図16 SCR.MAC/DAC。
 * 3=指定禁止はIOCS呼び出し規約上発生しない前提）。バス経由read/writeなので
 * RAM以外（GVRAM等）宛でも動作する。全chで共通に使う汎用エンジン。 */
static void dmac_transfer_block(x68k_bus_t *b, uint32_t *mar, uint32_t *dar, uint32_t count,
                                int dir, int mac, int dac) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t v = dir ? x68k_bus_read8(b, *dar) : x68k_bus_read8(b, *mar);
        if (dir)
            x68k_bus_write8(b, *mar, v);
        else
            x68k_bus_write8(b, *dar, v);
        if (mac == 1)
            (*mar)++;
        else if (mac == 2)
            (*mar)--;
        if (dac == 1)
            (*dar)++;
        else if (dac == 2)
            (*dar)--;
    }
}

/* 正常完了。ACTを落としCOCを立て、ITEラッチ済みならNIV IRQを要求する。 */
static void dmac2_complete(x68k_bus_t *b) {
    b->dmac.ch2_active = 0;
    x68k_bus_page_map_update_mask(b);
    b->dmac.ch2_credit_num = 0;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] &= (uint8_t)~DMAC_CSR_ACT;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] |= DMAC_CSR_COC;
    if (b->dmac.ch2_ite)
        b->dmac.ch2_irq_pending = 1; /* レベル3。IACK時にch2 NIVで通知する。 */
}

static void dmac2_error(x68k_bus_t *b, uint8_t cer, int ite) {
    b->dmac.ch2_active = 0;
    x68k_bus_page_map_update_mask(b);
    b->dmac.ch2_credit_num = 0;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] &= (uint8_t)~DMAC_CSR_ACT;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] |= DMAC_CSR_ERR;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CER] = cer;
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CCR] &= (uint8_t)~DMAC_CCR_SAB;
    if (ite)
        b->dmac.ch2_error_pending = 1;
}

/* CPU Hzが変わると、creditの単位(DMAC clock x CPU Hz)も変わる。経過時間を保つよう換算する。 */
void x68k_bus_dmac_cpu_hz_changed(x68k_bus_t *b, uint32_t old_hz, uint32_t new_hz) {
    if (old_hz == 0 || old_hz == new_hz)
        return;
    /* credit * new_hz / old_hz。128bit整数に頼らず商と余りに分けて、int64内で計算する。 */
    int64_t credit = b->dmac.ch2_credit_num;
    b->dmac.ch2_credit_num = (credit / old_hz) * new_hz + (credit % old_hz) * new_hz / old_hz;
}

int x68k_bus_dmac_cycles_to_next_event(const x68k_bus_t *b) {
    if (!b->dmac.ch2_active)
        return INT_MAX;
    int64_t cost = (int64_t)DMAC_CLOCKS_PER_UNIT * b->cpu_hz;
    int64_t need = cost - b->dmac.ch2_credit_num;
    int64_t hz = dmac_clock_hz(b);
    int64_t cycles = (need + hz - 1) / hz;
    if (cycles < 1)
        cycles = 1;
    return cycles > INT_MAX ? INT_MAX : (int)cycles;
}

/* チェーン表の読出し時間（XEiJ read値の初期近似。実機は未較正）。 */
#define DMAC_TABLE_BYTE_CLOCKS 5u
/* ix68k固有の安全停止。リンクを辿るエントリ数の上限（実チップの仕様ではない）。 */
#define DMAC_CHAIN_ENTRY_LIMIT 65536u

/* 戻り値: 1=ロード成功、0=アレイの表を使い切った（正常完了）、-1=ロードしたMTCが0（CER=0x0D）。
 */
static int dmac2_load_chain_entry(x68k_bus_t *b) {
    if (b->dmac.ch2_chain == 2) {
        if (b->dmac.ch2_btc == 0)
            return 0;
    }
    uint32_t bar = b->dmac.ch2_bar;
    b->dmac.ch2_mar = ((uint32_t)x68k_bus_read8(b, bar) << 24) |
                      ((uint32_t)x68k_bus_read8(b, bar + 1) << 16) |
                      ((uint32_t)x68k_bus_read8(b, bar + 2) << 8) |
                      (uint32_t)x68k_bus_read8(b, bar + 3);
    b->dmac.ch2_mtc =
        ((uint32_t)x68k_bus_read8(b, bar + 4) << 8) | (uint32_t)x68k_bus_read8(b, bar + 5);
    if (b->dmac.ch2_chain == 3) {
        b->dmac.ch2_next_bar = ((uint32_t)x68k_bus_read8(b, bar + 6) << 24) |
                               ((uint32_t)x68k_bus_read8(b, bar + 7) << 16) |
                               ((uint32_t)x68k_bus_read8(b, bar + 8) << 8) |
                               (uint32_t)x68k_bus_read8(b, bar + 9);
    }
    dmac_set_reg_l(b, DMAC_CH2_BASE + DMAC_REG_MAR, b->dmac.ch2_mar);
    dmac_set_reg_w(b, DMAC_CH2_BASE + DMAC_REG_MTC, b->dmac.ch2_mtc);
    /* 表読出し時間は借り（負のcredit）として積み、次のデータ転送を遅らせる。 */
    int64_t table_bytes = b->dmac.ch2_chain == 3 ? 10 : 6;
    b->dmac.ch2_credit_num -= table_bytes * DMAC_TABLE_BYTE_CLOCKS * (int64_t)b->cpu_hz;
    b->dmac.ch2_entries++;
    return b->dmac.ch2_mtc != 0 ? 1 : -1;
}

static void dmac2_chain_loaded(x68k_bus_t *b, int loaded) {
    if (loaded == 0)
        dmac2_complete(b);
    else if (loaded < 0)
        dmac2_error(b, 0x0Du, b->dmac.ch2_ite);
}

/* ch2の1転送単位（byte=1、word=2 byte）。wordは偶数アドレスのみ（奇数はCER=0x05(MAR)/
 * 0x06(DAR)のaddress error。データには触れない）。MAR/DARはMAC/DACに従い単位幅で増減する。 */
static int dmac2_transfer_unit(x68k_bus_t *b) {
    dmac_t *d = &b->dmac;
    if (d->ch2_unit == 2u) {
        if (d->ch2_mar & 1u) {
            dmac2_error(b, 0x05u, d->ch2_ite);
            return 0;
        }
        if (d->ch2_dar & 1u) {
            dmac2_error(b, 0x06u, d->ch2_ite);
            return 0;
        }
    }
    uint32_t from = d->ch2_dir ? d->ch2_dar : d->ch2_mar;
    uint32_t to = d->ch2_dir ? d->ch2_mar : d->ch2_dar;
    if (d->ch2_unit == 2u)
        x68k_bus_write16(b, to, x68k_bus_read16(b, from));
    else
        x68k_bus_write8(b, to, x68k_bus_read8(b, from));
    uint32_t step = d->ch2_unit;
    d->ch2_mar += d->ch2_mac == 1 ? step : d->ch2_mac == 2 ? -step : 0u;
    d->ch2_dar += d->ch2_dac == 1 ? step : d->ch2_dac == 2 ? -step : 0u;
    return 1;
}

static void dmac2_advance(x68k_bus_t *b, int cpu_cycles) {
    if (cpu_cycles <= 0 || !b->dmac.ch2_active)
        return;
    b->dmac.ch2_credit_num += (int64_t)cpu_cycles * dmac_clock_hz(b);
    int64_t cost = (int64_t)DMAC_CLOCKS_PER_UNIT * b->cpu_hz;
    while (b->dmac.ch2_active && b->dmac.ch2_credit_num >= cost) {
        b->dmac.ch2_credit_num -= cost;
        if (!dmac2_transfer_unit(b) || !b->dmac.ch2_active)
            break; /* DMA自身の書込みがch2レジスタに再入して止めた場合も含む */
        b->dmac.ch2_mtc--;
        dmac_set_reg_l(b, DMAC_CH2_BASE + DMAC_REG_MAR, b->dmac.ch2_mar);
        dmac_set_reg_l(b, DMAC_CH2_BASE + DMAC_REG_DAR, b->dmac.ch2_dar);
        dmac_set_reg_w(b, DMAC_CH2_BASE + DMAC_REG_MTC, b->dmac.ch2_mtc);
        if (b->dmac.ch2_mtc != 0)
            continue;
        if (b->dmac.ch2_chain == 0) {
            dmac2_complete(b);
        } else if (b->dmac.ch2_chain == 2) {
            b->dmac.ch2_bar += 6;
            b->dmac.ch2_btc--;
            dmac_set_reg_l(b, DMAC_CH2_BASE + DMAC_REG_BAR, b->dmac.ch2_bar);
            dmac_set_reg_w(b, DMAC_CH2_BASE + DMAC_REG_BTC, b->dmac.ch2_btc);
            dmac2_chain_loaded(b, dmac2_load_chain_entry(b));
        } else {
            uint32_t next = b->dmac.ch2_next_bar;
            if (next == 0) {
                dmac2_complete(b);
            } else if (next == b->dmac.ch2_bar ||
                       b->dmac.ch2_entries >= DMAC_CHAIN_ENTRY_LIMIT) {
                dmac2_error(b, 0x01u, b->dmac.ch2_ite); /* 自己参照・過長リンク */
            } else {
                b->dmac.ch2_bar = next;
                dmac_set_reg_l(b, DMAC_CH2_BASE + DMAC_REG_BAR, next);
                dmac2_chain_loaded(b, dmac2_load_chain_entry(b));
            }
        }
    }
}

/* DMA自身のバスアクセス中はsyncingを立て、CPU側callbackからの再帰同期を抑止する。 */
void x68k_bus_dmac_tick(x68k_bus_t *b, int cpu_cycles) {
    b->dmac.syncing = 1;
    dmac2_advance(b, cpu_cycles);
    b->dmac.syncing = 0;
}

void x68k_bus_dmac_set_cycle_source(x68k_bus_t *b, bool (*fn)(void *ctx, uint64_t *out),
                                    void *ctx) {
    b->dmac.cycle_source = fn;
    b->dmac.cycle_ctx = ctx;
    b->dmac.synced_cycles = 0;
}

void x68k_bus_dmac_chunk_begin(x68k_bus_t *b) {
    b->dmac.synced_cycles = 0;
    b->dmac.chunk_active = 1;
}

void x68k_bus_dmac_chunk_tick(x68k_bus_t *b, int consumed) {
    int64_t rest = (int64_t)consumed - (int64_t)b->dmac.synced_cycles;
    b->dmac.synced_cycles = 0;
    b->dmac.chunk_active = 0;
    if (rest > 0)
        x68k_bus_dmac_tick(b, rest > INT_MAX ? INT_MAX : (int)rest);
}

void x68k_bus_dmac_sync_to_now(x68k_bus_t *b) {
    /* CPUアクセスの境界。ロック書込みの破棄(dmac_write8)は同一アクセス内に限る。 */
    b->dmac.ch2_discard_left = 0;
    uint64_t now;
    if (!b->dmac.ch2_active || !b->dmac.chunk_active || b->dmac.syncing ||
        !b->dmac.cycle_source || !b->dmac.cycle_source(b->dmac.cycle_ctx, &now))
        return;
    if (now <= b->dmac.synced_cycles)
        return;
    uint64_t delta = now - b->dmac.synced_cycles;
    b->dmac.synced_cycles = now;
    x68k_bus_dmac_tick(b, delta > INT_MAX ? INT_MAX : (int)delta);
}

/* CCR(+7)へSTART($80)が書かれたときの開始処理。CHAIN(OCRビット3-2)で
 * 単純転送(0)/アレイチェーン(2)/リンクアレイチェーン(3)を分岐する
 * （IOCS $8A/$8B/$8CがそれぞれOCRにこの値を設定する。src/iocs.cではなく
 * ROM本体が設定するため、ここではOCRの値だけを見て判定すればよい）。 */
static void dmac2_start(x68k_bus_t *b) {
    uint8_t ocr = b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_OCR];
    uint8_t scr = b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_SCR];
    int dir = (ocr >> 7) & 1;
    int chain = (ocr >> 2) & 3;
    int mac = (scr >> 2) & 3;
    int dac = scr & 3;
    uint8_t dcr = b->crtc_regs[DMAC_CH2_BASE + 0x04u];
    int ite = (b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CCR] & DMAC_CCR_ITE) != 0;

    if (b->dmac.ch2_active || (b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] &
                               (DMAC_CSR_COC | DMAC_CSR_BTC | 0x20u | DMAC_CSR_ERR))) {
        /* 動作中の再STARTは実行中の転送を中断するので、ITEは新しいCCRでなくラッチ済みを使う。
         */
        dmac2_error(b, 0x02u, b->dmac.ch2_active ? b->dmac.ch2_ite : ite);
        return;
    }
    /* 受理するのはDCR=$08のSIZE=byte/wordのみ。long・pack・8-bitポートなどは
     * データ試験と実機観測がそろうまでconfiguration errorで拒否する。 */
    if (dcr != 0x08u || ((ocr >> 4) & 3) > 1 || chain == 1 || mac == 3 || dac == 3) {
        dmac2_error(b, 0x01u, ite);
        return;
    }

    b->dmac.ch2_dir = dir;
    b->dmac.ch2_unit = (uint8_t)(1u + ((ocr >> 4) & 1u));
    b->dmac.ch2_mac = mac;
    b->dmac.ch2_dac = dac;
    b->dmac.ch2_chain = (uint8_t)chain;
    b->dmac.ch2_ite = ite;
    b->dmac.ch2_mar = dmac_reg_l(b, DMAC_CH2_BASE + DMAC_REG_MAR);
    b->dmac.ch2_dar = dmac_reg_l(b, DMAC_CH2_BASE + DMAC_REG_DAR);
    b->dmac.ch2_bar = dmac_reg_l(b, DMAC_CH2_BASE + DMAC_REG_BAR);
    b->dmac.ch2_btc = dmac_reg_w(b, DMAC_CH2_BASE + DMAC_REG_BTC);
    b->dmac.ch2_credit_num = 0;
    b->dmac.ch2_entries = 0;
    if (chain == 0) {
        /* _DMAMOVE: 単純転送。MTC本数ぶんをそのまま1ブロックとして転送する
         * （$FF00バイト以上の分割はROM側のソフトウェアループが担当し、
         * 都度このCCR起動を繰り返すだけなのでここでは常に単発でよい）。 */
        uint32_t mtc = dmac_reg_w(b, DMAC_CH2_BASE + DMAC_REG_MTC);
        b->dmac.ch2_mtc = mtc;
        if (mtc == 0) {
            dmac2_error(b, 0x0Du, ite);
            return;
        }
    } else {
        if (chain == 2 && b->dmac.ch2_btc == 0) {
            dmac2_error(b, 0x0Fu, ite);
            return;
        }
        /* _DMAMOV_A(chain=2)/_DMAMOV_L(chain=3): BARが指すチェーンテーブルを
         * たどって複数ブロックを転送する。エントリ形式（doscall.txt/IOCS一覧
         * $8b/$8c仕様）: +0=long 転送元アドレス, +4=word 転送バイト数
         * （リンクモードのみ +6=long 次テーブルアドレス、0で終端）。
         * 転送元(MAR役)はエントリ毎に差し替わるが、転送先(DAR)は全ブロックを
         * 通じて永続し、dacの増減設定に従い蓄積していく。 */
        if (dmac2_load_chain_entry(b) <= 0) {
            dmac2_error(b, 0x0Du, ite);
            return;
        }
    }
    /* データはここでは動かさない。チャンネルをACTにし、x68k_bus_dmac_tick()が転送時間に
     * 応じて1単位ずつ進め、最後にCOCと割り込み要求を立てる。IPLROMは起動直後にch2の
     * メモリ間転送とMFP Timer-Dで転送速度を測り、CPUクロック表示に使う。
     * 即時完了だと経過カウントが1になり除算が0割りで異常終了する。
     * 既知の誤差: cycle source(060turboのbus callback)が使えない経路では、START を含む
     * CPUチャンクの経過時間が丸ごと転送時間に数えられ、最大1チャンク
     * （MFP_TICK_CYCLES=200 CPUサイクル）早く進む。 */
    /* START以前のCPU時間はDMAに数えない。CPU bus callback中なら基準をいまに合わせる。 */
    uint64_t now;
    if (b->dmac.chunk_active && b->dmac.cycle_source &&
        b->dmac.cycle_source(b->dmac.cycle_ctx, &now))
        b->dmac.synced_cycles = now;
    b->dmac.ch2_active = 1;
    x68k_bus_page_map_update_mask(b);
    b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CSR] |= DMAC_CSR_ACT;
}

/* RESET命令相当のDMAC(HD63450)初期化。回路図でIC23 HD63450-10の
 * 27番ピン(BEC2)がネットRESETに直結していることを確認済み
 * （回路図 p5）。レジスタ実体は
 * b->crtc_regs[$4000-$40FF]の生バイトで、4chが$40バイトずつ並ぶ
 * （ch0=$4000/ch1=$4040/ch2=$4080/ch3=$40C0、GCRはch3ブロック末尾の$40FF）。
 * この256バイト範囲を使うのはdmac.c/fdc.cのみ（CRTC/パレット等の他レジスタは
 * 範囲外）なので、chごとの$40バイト一括ゼロクリアで安全に全レジスタを
 * 電源投入相当へ戻せる。NIV/EIVだけは実チップのRESET後の値$0F（未初期化
 * 割り込みベクタ番号15相当。XEiJ HD63450.java dmaReset()準拠）へ書き戻す
 * （IACK時は要求チャネルのNIVを返すため、リセット直後は$0Fを返す）。
 * IPLROMはRESET命令の後にDMACを初期化する（例: IPLROMXV.DAT $FF0E1C〜）ため、
 * 全クリアしてもブート経路への影響は無い。 */
void dmac_reset(x68k_bus_t *b) {
    static const uint32_t ch_base[4] = {DMAC_CH0_BASE, DMAC_CH1_BASE, DMAC_CH2_BASE,
                                        DMAC_CH3_BASE};
    for (int i = 0; i < 4; i++) {
        memset(&b->crtc_regs[ch_base[i]], 0, DMAC_CH_REG_SPAN);
        b->crtc_regs[ch_base[i] + DMAC_REG_NIV] = 0x0Fu;
        b->crtc_regs[ch_base[i] + DMAC_REG_EIV] = 0x0Fu;
    }
    /* dmac_t/adpcm_dma_tはホスト側設定・外部ピン相当の状態を持たないため、
     * 構造体全体のゼロクリアでよい（ch0_armed武装フラグ、ch2/ch3のirq_pending、
     * ch3の転送途中状態dir/mac/dac/mar/dar/remaining/chain/bar/btc/
     * chain_done/budget_acc、いずれも0が初期値と一致する）。フィールド追加時は
     * ここを再確認すること。 */
    bool (*cycle_source)(void *, uint64_t *) = b->dmac.cycle_source;
    void *cycle_ctx = b->dmac.cycle_ctx;
    int chunk_active = b->dmac.chunk_active;
    uint64_t synced_cycles = b->dmac.synced_cycles;
    memset(&b->dmac, 0, sizeof(b->dmac));
    /* 同期配線と実行中チャンクの状態はホスト側の事情なので、リセットで失わない。
     * CPUのバスアクセス中(reset命令等)に呼ばれても反映済みサイクルを巻き戻さない。 */
    b->dmac.cycle_source = cycle_source;
    b->dmac.cycle_ctx = cycle_ctx;
    b->dmac.chunk_active = chunk_active;
    b->dmac.synced_cycles = synced_cycles;
    x68k_bus_page_map_update_mask(b); /* ch2_active が 0 に戻る */
}

int dmac_iack(x68k_bus_t *b, uint8_t *vector) {
    if (b->dmac.ch2_error_pending) {
        b->dmac.ch2_error_pending = 0;
        *vector = b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_EIV];
        return 1;
    }
    if (b->dmac.ch2_irq_pending) {
        b->dmac.ch2_irq_pending = 0;
        *vector = b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_NIV];
        return 1;
    }
    if (b->dmac.ch3_irq_pending) {
        b->dmac.ch3_irq_pending = 0;
        *vector = b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_NIV];
        return 1;
    }
    return 0;
}

/* ch2 active中に書くと動作タイミングエラー(CER=0x02)になるレジスタ。
 * XEiJ HD63450.java dmaWriteByte()準拠: DCR/SCR/MTC/MAR/DAR/MFC/DFC。
 * OCR/CCR/BTC/BAR/NIV/EIVはactive中も書ける（CCRはSAB/ITE、BTC/BARはチェーン用）。 */
#define DMAC_REG_DCR 0x04u
#define DMAC_REG_MFC 0x29u
#define DMAC_REG_DFC 0x31u

static int dmac2_reg_locked_while_active(uint32_t off) {
    return off == DMAC_REG_DCR || off == DMAC_REG_SCR ||
           (off >= DMAC_REG_MTC && off < DMAC_REG_MTC + 2u) ||
           (off >= DMAC_REG_MAR && off < DMAC_REG_MAR + 4u) ||
           (off >= DMAC_REG_DAR && off < DMAC_REG_DAR + 4u) || off == DMAC_REG_MFC ||
           off == DMAC_REG_DFC;
}

int dmac_write8(x68k_bus_t *b, uint32_t addr, uint8_t val) {
    int locked = addr >= 0xE84080u && addr < 0xE84080u + DMAC_CH_REG_SPAN &&
                 dmac2_reg_locked_while_active(addr - 0xE84080u);
    if (locked && b->dmac.ch2_active) {
        /* 書込みは破棄し、転送中のラッチ状態とレジスタ読戻しを乖離させない。
         * word/longはバイトに分割されて届くため、同じCPUアクセス内の続く最大3バイトも破棄する
         * （次のアクセスの開始でx68k_bus_dmac_sync_to_now()が解除する）。 */
        dmac2_error(b, 0x02u, b->dmac.ch2_ite);
        b->dmac.ch2_discard_addr = addr + 1u;
        b->dmac.ch2_discard_left = 3;
        return 1;
    }
    if (locked && b->dmac.ch2_discard_left > 0 && addr == b->dmac.ch2_discard_addr) {
        b->dmac.ch2_discard_addr = addr + 1u;
        b->dmac.ch2_discard_left--;
        return 1;
    }
    b->dmac.ch2_discard_left = 0;
    if (addr == 0xE84000u) {
        /* DMAC ch0 CSR: チャンネルステータスレジスタ。ACT/PCSはライブ
         * ステータスでwrite-1-to-clear対象外。ROMはFDC転送前後に必ず
         * #$FFを書いてクリアする（$FF92A0/$FF92B2/$FF92EC）。単純な上書き
         * 代入だとERRビット(bit4)が常時セットに見えてしまい、_B_READの
         * $FF938Aゲートが毎回「DMACエラー」と誤認して正規のFDC ready待ちを
         * スキップし、割り込みハンドラがまだ更新し終えていない結果テーブル
         * ($C90+)から前回分の古いステータスを読んでしまう（write-1-to-clear
         * の一般的な理由は`dmac_csr_clear`のコメント参照）。 */
        dmac_csr_clear(b, DMAC_CH0_BASE, val);
        return 1;
    }
    if (addr == 0xE84007u) {
        /* DMAC ch0 CCR: bit7=START。FDC用。ch1と異なり、CCR STARTはFDCが
         * READ/WRITE DATAコマンドを受け取るより先に発行される（実機計装で
         * 確認、何百セクタも例外なく一貫した順序）。この時点ではFDCがどの
         * セクタを転送するか未確定（コマンドのcyl/head/secをまだ受け取って
         * いない）ため、ここでは武装フラグを立てるだけで転送しない。実転送は
         * fdc_exec()がREAD/WRITE DATAコマンド実行完了後にdmac_ch0_fdc_
         * transfer()を呼んで行う（fdc.c参照）。OCR/SCR/MTC/MARの生値ストア
         * は既存のCRTC領域汎用フォールバックに任せる（ch2と同じ方式、ここ
         * ではCCR自体のみ扱う）。 */
        b->crtc_regs[0x4007u] = val;
        if (val & 0x80u)
            b->dmac.ch0_armed = 1;
        /* bit4 = SAB(ソフトウェアアボート)。ROMはFDCがNot Ready等で実行データ
         * フェーズに入らずCOCが立たなかったとき、$FF92D8からここへ#$10を書いて
         * 転送要求を取り消す。武装したまま残すと、次のREAD/WRITE/SCAN DATAが
         * 「前回のSTART」を消費する形になり、CCR STARTを書かない異常系で
         * 意図しない転送が走り得る。 */
        if (val & 0x10u)
            b->dmac.ch0_armed = 0;
        return 1;
    }
    if (addr == 0xE84040u) {
        /* DMAC ch1 CSR: 内蔵SCSI(SPC)割り付け。ch0同様write-1-to-clear。
         * ROMはSCSI転送前後に#$FFでCOC等をクリアする。 */
        dmac_csr_clear(b, DMAC_CH1_BASE, val);
        return 1;
    }
    if (addr == 0xE84047u) {
        /* DMAC ch1 CCR: bit7=START。実SCSI転送はDMAC ch1が駆動し、ROMは
         * DAR=$E96035(SPC DREG)・MTC=チャンク長を設定してここへ#$80を書く
         * ことで起動する。DAR=$E96001(HDC DATAポート)の場合はsasi
         * (SASI互換)が同じ仕組みで駆動する。ioi_init($FF0F16)がDMAC ch1の
         * デバイスアドレスに$E96001を設定するため、SCSI優先度$8?00での
         * ブート時READコマンドのデータ転送はこちらを通る。
         *
         * READ(device→memory)/WRITE(memory→device、SPCのみ。HDC/SASIは
         * WRITEコマンド自体が未実装)とも、ch2と同じ汎用バイトループ
         * (dmac_transfer_block)で処理する。$E96035/$E96001へのx68k_bus_
         * read8/write8はscsi/sasiのDREG/DATAパスに届き、1バイト
         * アクセスごとにSPC/HDC側のtc/FIFOが自前で前進する（tc==0で
         * spc_transfer_complete()等も自動発火）。OCR.DIRが方向を表すため
         * READ/WRITEとも同一のロジックで処理できる（実機採取値:
         * READ=OCR $B1(SPC)/$B2(HDC)・WRITE=OCR $31、いずれもSCR=$04で
         * 共通、DIRのみ異なる）。 */
        b->crtc_regs[0x4047u] = val;
        if (val & 0x80u) {
            uint32_t dar = dmac_reg_l(b, DMAC_CH1_BASE + DMAC_REG_DAR);
            int is_scsi = dar == 0xE96035u;
            int is_sasi = dar == 0xE96001u;
            if ((is_scsi &&
                 (scsi_dma_active(&b->spc_in) || scsi_dma_write_active(&b->spc_in))) ||
                (is_sasi && sasi_dma_active(&b->hdc_in))) {
                uint8_t ocr = b->crtc_regs[DMAC_CH1_BASE + DMAC_REG_OCR];
                uint8_t scr = b->crtc_regs[DMAC_CH1_BASE + DMAC_REG_SCR];
                int dir = (ocr >> 7) & 1;
                int mac = (scr >> 2) & 3;
                int dac = scr & 3;
                uint32_t mtc = dmac_reg_w(b, DMAC_CH1_BASE + DMAC_REG_MTC);
                uint32_t mar = dmac_reg_addr24(b, DMAC_CH1_BASE + DMAC_REG_MAR);
                uint32_t dar_cur = dar;
                uint32_t transferred = 0;
                /* COMMANDの先頭バイトでSPC側のbuf_limitが1→CDB長へ変わる。
                 * そのため残量を開始前に固定せず、各バイトでゲートを再評価する。
                 * DATA INの短転送でターゲットがREQを下げた場合もここで止め、
                 * 実際に動かした分だけMTC/MARを進める。 */
                while (transferred < mtc) {
                    int active = is_scsi ? (scsi_dma_active(&b->spc_in) ||
                                            scsi_dma_write_active(&b->spc_in))
                                         : sasi_dma_active(&b->hdc_in);
                    if (!active)
                        break;
                    dmac_transfer_block(b, &mar, &dar_cur, 1, dir, mac, dac);
                    transferred++;
                }
                dmac_set_reg_w(b, DMAC_CH1_BASE + DMAC_REG_MTC, mtc - transferred);
                dmac_set_reg_addr24(b, DMAC_CH1_BASE + DMAC_REG_MAR, mar);
                if (transferred == mtc)
                    b->crtc_regs[DMAC_CH1_BASE + DMAC_REG_CSR] |= DMAC_CSR_COC;
            }
        }
        return 1;
    }
    if (addr == 0xE84080u) {
        /* DMAC ch2 CSR: 汎用メモリ間転送(_DMAMOVE系)用。ch0/ch1同様
         * write-1-to-clear。IOCS $8D _DMAMODEはCSRではなく$C34(ROM側ワーク)を
         * 見るため実際にはIOCS層からは参照されないが、実機仕様に忠実にする。 */
        dmac_csr_clear(b, DMAC_CH2_BASE, val);
        return 1;
    }
    if (addr == 0xE84087u) {
        /* DMAC ch2 CCR: bit7=START。IOCS _DMAMOVE/_DMAMOV_A/_DMAMOV_L
         * (0x8A-0x8C)がOCR/SCR/MTC/MAR/DAR/BTC/BARを設定した上でここへ#$88
         * を書いて起動する（Human68kのROM委譲、src/iocs.c参照）。dmac2_start()は
         * ACTにするだけで、転送はx68k_bus_dmac_tick()が時間に応じて進め、完了後に
         * レベル3の割り込みを要求する。ROM側は転送開始後すぐrtsで戻り、この割り込みハンドラ
         * ($C34クリア)でのみ次呼び出しの待ちが解けるため、割り込みなしでは
         * 2回目以降の_DMAMOVE系呼び出しが永久ビジーウェイトする。 */
        b->crtc_regs[0x4087u] = val & (uint8_t)~0x80u; /* STRは読戻しで0。RMWで再STARTしない */
        if (val & DMAC_CCR_SAB) {
            if (b->dmac.ch2_active)
                dmac2_error(b, 0x11u, b->dmac.ch2_ite);
            else
                b->crtc_regs[DMAC_CH2_BASE + DMAC_REG_CCR] &= (uint8_t)~DMAC_CCR_SAB;
        } else if (val & 0x80u) {
            dmac2_start(b);
        }
        return 1;
    }
    if (addr == 0xE840C0u) {
        /* DMAC ch3 CSR: ADPCM(MSM6258)専用。ch0-2同様write-1-to-clear。 */
        dmac_csr_clear(b, DMAC_CH3_BASE, val);
        return 1;
    }
    if (addr == 0xE840C7u) {
        /* DMAC ch3 CCR: bit7=START。IOCS _ADPCMOUT/INP/AOT/AIN/LOT/LIN
         * (0x60-0x65)がOCR/SCR/MTC(またはBTC)/MAR(またはBAR)/DAR($E92003固定)
         * を設定した上でここへ#$88を書いて起動する（ROM委譲、src/iocs.c参照）。
         * ch0-2と異なりMSM6258のサンプリング周波数(3.9k-15.6kHz)に同期した
         * 実時間転送が前提のため、ここでは瞬時転送せずadpcm_dma状態へラッチ
         * するのみ。実転送はx68k_bus_adpcm_tick()がCPUチャンク単位で漸進的に行う
         * （main.c参照）。chain(OCRビット3-2)!=0(_ADPCMAOT/AIN/LOT/LINが使う
         * アレイ/リンクアレイチェーン)は最初のエントリをtick()側でロードする
         * ため、ここではBAR/BTCをラッチするのみでMAR/remainingは0のまま。
         * bit6(CNTコンティニュー動作)はPCM8A.X等がダブルバッファ再生に使う
         * （STARTと同時にも、動作中の個別書き込みでも来る。下記）。
         * bit4(SAB停止)/bit5(HLT一時停止)/bit3(INT enableのみ=再開)は
         * _ADPCMMOD(0x67)がCCRへ個別に書く（下記）。
         *
         * STARTではCSR.ACT(bit3)も立てる。PCM8A.Xは割り込みハンドラで
         * `BTST #3,(A5)` によりチャンネルが動作中かを判定し、動作中なら
         * BAR/BTCを差し替えてCNTを再セットするだけの継続経路、停止していれば
         * SAB→MAR/MTC再設定→STARTのフルリスタート経路へ分岐する。後者では
         * MARに無音バッファを設定するため、ACTを立てないと実PCMデータを
         * 積んだBAR側のバッファが一度も再生されない。 */
        b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CCR] = val;
        if (val & DMAC_CCR_STR) {
            uint8_t ocr = b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_OCR];
            uint8_t scr = b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_SCR];
            int chain = (ocr >> 2) & 3;
            b->dmac.ch3.dir = (ocr >> 7) & 1;
            b->dmac.ch3.mac = (scr >> 2) & 3;
            b->dmac.ch3.dac = scr & 3;
            b->dmac.ch3.dar = dmac_reg_l(b, DMAC_CH3_BASE + DMAC_REG_DAR);
            b->dmac.ch3.chain = chain;
            b->dmac.ch3.chain_done = 0;
            b->dmac.ch3.paused = 0;
            adpcm_dma_set_cnt(b, 0);
            b->dmac.ch3.budget_acc = 0;
            if (chain == 0) {
                b->dmac.ch3.mar = dmac_reg_addr24(b, DMAC_CH3_BASE + DMAC_REG_MAR);
                b->dmac.ch3.remaining = dmac_reg_w(b, DMAC_CH3_BASE + DMAC_REG_MTC);
            } else {
                b->dmac.ch3.bar = dmac_reg_l(b, DMAC_CH3_BASE + DMAC_REG_BAR);
                b->dmac.ch3.btc =
                    dmac_reg_w(b, DMAC_CH3_BASE + DMAC_REG_BTC); /* chain==2のみ使用 */
                b->dmac.ch3.remaining = 0; /* 最初のエントリはtick()側でロードする */
            }
            b->dmac.ch3.active = 1;
            b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CSR] |= DMAC_CSR_ACT;
        }
        /* CNT(bit6): コンティニュー動作を武装する。ブロック完了時にハードウェアが
         * BAR→MAR / BTC→MTC を取り込んで転送を継続する機構で、PCM8A.X等の
         * ADPCM連続再生ドライバはこれでダブルバッファを回す（詳細は
         * adpcm_dma_continue_block()のコメント参照）。単純転送(chain==0)専用の
         * 機能であり、チェーンモードでのセットはコンフィギュレーションエラー、
         * 動作中でもSTART同時書き込みでもないときのセットはタイミングエラーの
         * ため、どちらも武装せず無視する（読み返しも0になる）。CNTへ0を
         * 書いてもクリアされない（ブロック完了時にハードウェアが自動クリアする）
         * ため、その場合は現在の武装状態をそのまま読み返せるようにする。 */
        if (val & DMAC_CCR_CNT)
            adpcm_dma_set_cnt(b, b->dmac.ch3.chain == 0 &&
                                     (b->dmac.ch3.active || (val & DMAC_CCR_STR)));
        else
            adpcm_dma_set_cnt(b, b->dmac.ch3.cnt);
        /* _ADPCMMOD(0x67): d1=0(SAB停止)/1(HLT一時停止)/2(INT enableのみ=再開)。
         * ROMハンドラはOCR/SCR等を再設定せずCCRへ個別ビットのみを書く想定
         * （STARTと同時に来る想定はしていない）。 */
        if (val & DMAC_CCR_SAB) {
            /* SAB: 転送を中断する。既存hardware仕様は完了割り込みを発生させるか
             * 不明のため、安全側として発火させない（明示的な中断でROM側の
             * ウェイトループを誤って解いてしまう方が実害が大きいため）。 */
            b->dmac.ch3.active = 0;
            b->dmac.ch3.paused = 0;
            adpcm_dma_set_cnt(b, 0);
            b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CSR] &= (uint8_t)~DMAC_CSR_ACT;
        }
        if (val & DMAC_CCR_HLT) {
            /* HLT: 一時停止。状態(mar/remaining/bar等)は保持し転送のみ止める。 */
            b->dmac.ch3.paused = 1;
        }
        if (val & DMAC_CCR_ITE) {
            /* INT enableのみ: 再開。STARTと同時に来た場合はここで無条件に
             * paused=0へ落ちるが、STARTの武装処理で既に0のため無害。 */
            b->dmac.ch3.paused = 0;
        }
        return 1;
    }
    return 0;
}

/* ch3チェーン(アレイ/リンクアレイ)の次エントリを読み込む。ch2のdmac2_start()
 * と同じテーブル形式（+0=long転送元アドレス, +4=word転送バイト数、リンクの
 * みさらに+6=long次テーブルアドレス・0で終端）をフレームまたぎで漸進的に
 * 走査するため、ここでは「次の1エントリを読む」だけを行う。呼び出し元は
 * remaining==0（前エントリを使い切った、または初回）のときだけ呼ぶこと。 */
static void adpcm_dma_load_next_entry(x68k_bus_t *b) {
    uint32_t bar = b->dmac.ch3.bar;
    b->dmac.ch3.mar = ((uint32_t)x68k_bus_read8(b, bar) << 24) |
                      ((uint32_t)x68k_bus_read8(b, bar + 1) << 16) |
                      ((uint32_t)x68k_bus_read8(b, bar + 2) << 8) |
                      (uint32_t)x68k_bus_read8(b, bar + 3);
    b->dmac.ch3.remaining =
        ((uint32_t)x68k_bus_read8(b, bar + 4) << 8) | (uint32_t)x68k_bus_read8(b, bar + 5);
    if (b->dmac.ch3.chain == 3) {
        uint32_t next = ((uint32_t)x68k_bus_read8(b, bar + 6) << 24) |
                        ((uint32_t)x68k_bus_read8(b, bar + 7) << 16) |
                        ((uint32_t)x68k_bus_read8(b, bar + 8) << 8) |
                        (uint32_t)x68k_bus_read8(b, bar + 9);
        if (next == 0)
            b->dmac.ch3.chain_done = 1; /* このエントリの処理完了後に終了 */
        else
            b->dmac.ch3.bar = next;
    } else {
        b->dmac.ch3.bar = bar + 6;
        b->dmac.ch3.btc--;
    }
}

/* ch3転送の完了処理（単純転送・チェーン共通）。CSR.COCを立てch3_irq_pending
 * で通知し、bus_iackのベクタ$6A割り込みでROMの$C32ウェイトループが解ける。 */
static void adpcm_dma_complete(x68k_bus_t *b) {
    b->dmac.ch3.active = 0;
    adpcm_dma_set_cnt(b, 0);
    b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CSR] &= (uint8_t)~DMAC_CSR_ACT;
    b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CSR] |= DMAC_CSR_COC;
    b->dmac.ch3_irq_pending = 1;
}

/* ch3単純転送(chain==0)のCNTコンティニュー継続処理。MTCが0になった時点で
 * CCR.CNTが武装されていれば、実機のHD63450は転送を終了せず次の動作を行う:
 *   1. CSR.BTC(bit6, Block Transfer Complete)を立てる
 *   2. CCR.CNTを自動クリアする（次ブロックの継続はソフトが再セットして指示する）
 *   3. ITE(CCR bit3)が有効なら正常割り込み(ベクタ$6A)を要求する
 *   4. BAR→MAR / BTC→MTC を取り込み、そのまま転送を継続する
 * ソフトウェア側はこの割り込みでCSR.BTCをwrite-1-to-clearし、次々ブロックの
 * BAR/BTCを設定してCNTを再セットする。PCM8A.XのADPCM連続再生はこの機構で
 * 48バイトのダブルバッファを途切れなく回しており、COC(転送終了)を立てて
 * しまうとドライバは演奏終了と判断してMSM6258を停止する。
 * BTC(Base Transfer Counter)が0のときは実機ではカウントエラー終了となるため、
 * remaining==0のまま戻して呼び出し元の完了処理に委ねる。
 * （XEiJ HD63450.java dmaTransfer() のコンティニューモード分岐に準拠） */
static void adpcm_dma_continue_block(x68k_bus_t *b) {
    adpcm_dma_set_cnt(b, 0);
    b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CSR] |= DMAC_CSR_BTC;
    if (b->crtc_regs[DMAC_CH3_BASE + DMAC_REG_CCR] & DMAC_CCR_ITE)
        b->dmac.ch3_irq_pending = 1;
    /* MARは24bitアドレス（dmac_reg_addr24と同じくBARの最上位バイトは使わない）。 */
    b->dmac.ch3.mar = dmac_reg_l(b, DMAC_CH3_BASE + DMAC_REG_BAR) & 0x00FFFFFFu;
    b->dmac.ch3.remaining = dmac_reg_w(b, DMAC_CH3_BASE + DMAC_REG_BTC);
}

/* main.cのCPUチャンクループからMFP/OPMと同じ粒度で呼ばれる、DMAC ch3(ADPCM)の
 * 実時間ペースド転送本体。ADPCMサンプリング周波数(adpcm_get_sample_rate_hz)から
 * 「1バイト=2サンプルあたりのCPUサイクル数」を逆算し、経過サイクル分たまった
 * バイト数だけdmac_transfer_blockで漸進転送する（例: 15.6kHz→1280サイクル=128µs
 * ごとに1バイト＝130バイト/フレーム相当）。chain!=0(アレイ/リンクアレイ)は
 * エントリを使い切るたびに次エントリをロードし、チェーン終端で完了処理を行う。 */
void x68k_bus_adpcm_tick(x68k_bus_t *b, int cpu_cycles) {
    if (cpu_cycles <= 0)
        return;
    if (!b->dmac.ch3.active || b->dmac.ch3.paused)
        return;

    /* このチャンクで経過したCPUサイクル分の転送バイト数
     * (fs * cpu_cycles / (2 * CPU_HZ)、通常割り切れない)を誤差蓄積(Bresenham型)
     * で正確に配分する。素朴に切り捨てると端数が毎回失われ、実際の配送レートが
     * 真のサンプリング周波数よりわずかに遅くなり、長時間再生でSDL音声キューを
     * 徐々に痩せさせてアンダーランを誘発する。budget_accはCCR STARTで0に
     * リセットされる（dmac_write8()のch3 CCR START分岐）。
     *
     * オーバーフロー: budget_accは剰余なので常にbudget_den未満。加算分は
     * fs(最大15625)*cpu_cyclesなので、cpu_cyclesが数十万オーダーまで
     * uint32_tに収まる。main.cが渡すのはMFP_TICK_CYCLES(200)+命令オーバー
     * シュート分（高々数百）なので十分な余裕がある。budget_den(2*cpu_hz)も
     * 50MHz上限で1億程度でありuint32_t範囲内。 */
    uint32_t fs = adpcm_get_sample_rate_hz(&b->adpcm);
    uint32_t budget_den = 2u * b->cpu_hz;
    b->dmac.ch3.budget_acc += fs * (uint32_t)cpu_cycles;
    uint32_t budget = b->dmac.ch3.budget_acc / budget_den;
    b->dmac.ch3.budget_acc -= budget * budget_den;
    /* チャンク粒度では budget==0 が正常に起こる。まだ 1 バイト分に満たなければ
     * MAR/MTC/DAR を変更せずに抜ける。 */
    if (budget == 0u)
        return;

    /* HD63450 はブロック完了時に次のチェーン要素を連続してロードする。
     * エントリ境界をまたいで転送予算を使い切るまで処理する。 */
    while (budget > 0u) {
        if (b->dmac.ch3.remaining == 0u) {
            if (b->dmac.ch3.chain == 0)
                break; /* 単純転送は使い切ったら終端（下の完了処理へ） */
            if (b->dmac.ch3.chain_done || (b->dmac.ch3.chain == 2 && b->dmac.ch3.btc == 0u)) {
                adpcm_dma_complete(b);
                return;
            }
            adpcm_dma_load_next_entry(b);
            if (b->dmac.ch3.remaining == 0u)
                break; /* 長さ0エントリ等の異常時に無限ループを避ける */
        }
        uint32_t n = budget < b->dmac.ch3.remaining ? budget : b->dmac.ch3.remaining;
        dmac_transfer_block(b, &b->dmac.ch3.mar, &b->dmac.ch3.dar, n, b->dmac.ch3.dir,
                            b->dmac.ch3.mac, b->dmac.ch3.dac);
        b->dmac.ch3.remaining -= n;
        budget -= n;
        /* 単純転送でブロックを使い切った時点でCNTが武装されていれば、実機と同じく
         * その場でBAR/BTCを取り込んで継続する。予算を使い切ってループを抜けた後に
         * 回すと、下の完了処理がCOCを立ててしまい継続の機会を失う。 */
        if (b->dmac.ch3.remaining == 0u && b->dmac.ch3.chain == 0 && b->dmac.ch3.cnt) {
            adpcm_dma_continue_block(b);
            if (b->dmac.ch3.remaining == 0u)
                break; /* BTC==0(カウントエラー相当): 下の完了処理へ */
        }
    }

    /* レジスタ書き戻し: chain==0のみMAR/MTCを反映する。chain!=0はMAR/BAR/BTC
     * がエントリごとに切り替わり、かつROM側もこれらをポーリングしない設計
     * （ch2同様、完了は割り込みでのみ通知）のため書き戻し不要。 */
    if (b->dmac.ch3.chain == 0) {
        dmac_set_reg_addr24(b, DMAC_CH3_BASE + DMAC_REG_MAR, b->dmac.ch3.mar);
        dmac_set_reg_w(b, DMAC_CH3_BASE + DMAC_REG_MTC, b->dmac.ch3.remaining);
    }
    dmac_set_reg_l(b, DMAC_CH3_BASE + DMAC_REG_DAR, b->dmac.ch3.dar);

    if (b->dmac.ch3.remaining == 0u && b->dmac.ch3.chain == 0)
        adpcm_dma_complete(b);
    /* chain!=0はここでremaining==0になっても即完了とせず、次tickの冒頭
     * （本関数先頭のchain分岐）でチェーン終端判定・次エントリロードへ回す。 */
}

/* fdc_exec()がREAD/WRITE DATAコマンド実行完了時に呼ぶ。ch0 CCR STARTで
 * 武装済み(dmac_ch0_armed)であれば、その時点で生値ストア済みのOCR/SCR/
 * MTC/MARを読み出し、fdc.cが設定した実行データフェーズ
 * (b->fdc.dma_ptr/dma_remaining/dma_dir)を汎用エンジンで消費する。
 * DARは$E94003固定のはず（実機計装で確認、例外なし）だが、想定外の値なら
 * フォールバックに委ねる（ch1のDAR値ゲート判定と同じ考え方）。 */
int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    if (!b->dmac.ch0_armed)
        return 0;
    b->dmac.ch0_armed = 0;

    uint32_t dar = dmac_reg_l(b, DMAC_CH0_BASE + DMAC_REG_DAR);
    if (dar != 0xE94003u)
        return 0;

    uint8_t ocr = b->crtc_regs[DMAC_CH0_BASE + DMAC_REG_OCR];
    uint8_t scr = b->crtc_regs[DMAC_CH0_BASE + DMAC_REG_SCR];
    int dir = (ocr >> 7) & 1;
    int mac = (scr >> 2) & 3;
    int dac = scr & 3;
    uint32_t mtc = dmac_reg_w(b, DMAC_CH0_BASE + DMAC_REG_MTC);
    uint32_t mar = dmac_reg_addr24(b, DMAC_CH0_BASE + DMAC_REG_MAR);

    uint32_t remaining = b->fdc.dma_remaining;
    uint32_t n = mtc < remaining ? mtc : remaining;
    if (n > 0)
        dmac_transfer_block(b, &mar, &dar, n, dir, mac, dac);

    b->crtc_regs[DMAC_CH0_BASE + DMAC_REG_CSR] |= 0x80u; /* CSR COC */
    dmac_set_reg_w(b, DMAC_CH0_BASE + DMAC_REG_MTC, 0);
    dmac_set_reg_addr24(b, DMAC_CH0_BASE + DMAC_REG_MAR, mar);
    return 1;
}
