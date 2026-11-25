/*
 * DMAC ch2(汎用メモリ間転送)が転送時間を持つことの単体テスト。
 * IPLROM 1.6はch2のメモリ間転送とMFP Timer-Dで転送速度を測るため、即時完了だと
 * 経過カウントが1になり0割りで異常終了していた。CCR START直後はACTのみ立ち、
 * COC・割り込み要求は転送バイト数に応じたCPUサイクル経過後に立つことを確認する。
 * x68k_bus_createはROM不要（dmac_reset_test.cと同じ流儀）。
 */
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#define CH2_CSR  0xE84080u
#define CH2_DCR  0xE84084u
#define CH2_OCR  0xE84085u
#define CH2_SCR  0xE84086u
#define CH2_CCR  0xE84087u
#define CH2_MTC  0xE8408Au
#define CH2_MAR  0xE8408Cu
#define CH2_DAR  0xE84094u
#define SRC_ADDR 0x1000u
#define DST_ADDR 0x2000u
#define CSR_COC  0x80u
#define CSR_ACT  0x08u

static void write_long(x68k_bus_t *b, uint32_t addr, uint32_t v) {
    for (int i = 0; i < 4; i++)
        x68k_bus_write8(b, addr + (uint32_t)i, (uint8_t)(v >> (24 - 8 * i)));
}

static void start_copy_ccr(x68k_bus_t *b, uint32_t len, uint8_t ccr) {
    for (uint32_t i = 0; i < len; i++)
        x68k_bus_write8(b, SRC_ADDR + i, (uint8_t)(i + 1));
    x68k_bus_write8(b, CH2_DCR, 0x08u); /* 16-bit port。IPLROMの_DMAMOVE設定。 */
    x68k_bus_write8(b, CH2_OCR, 0x00u);
    x68k_bus_write8(b, CH2_SCR, 0x05u); /* MAC=1, DAC=1 */
    x68k_bus_write8(b, CH2_MTC, (uint8_t)(len >> 8));
    x68k_bus_write8(b, CH2_MTC + 1, (uint8_t)len);
    write_long(b, CH2_MAR, SRC_ADDR);
    write_long(b, CH2_DAR, DST_ADDR);
    x68k_bus_write8(b, CH2_CCR, ccr);
}

static void start_copy(x68k_bus_t *b, uint32_t len) {
    start_copy_ccr(b, len, 0x88u); /* START + ITE */
}

/* 1. START直後はACTのみ。データはまだ移動せず、10 DMACクロックごとに1 byteだけ
 *    進む。COC・割り込みは最後のbyteの後にだけ発生する。 */
static void test_busy_until_deadline(uint32_t cpu_hz) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, cpu_hz);
    assert(x68k_bus_dmac_cycles_to_next_event(b) == INT_MAX);

    start_copy(b, 100);
    assert(x68k_bus_read8(b, DST_ADDR) == 0u);
    assert(x68k_bus_read8(b, DST_ADDR + 99) == 0u);
    uint8_t csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & CSR_ACT) && !(csr & CSR_COC));
    assert(!x68k_bus_dmac2_pending_irq(b));

    int per_byte = (int)(10u * (cpu_hz / 10000000u));
    assert(x68k_bus_dmac_cycles_to_next_event(b) == per_byte);

    x68k_bus_dmac_tick(b, per_byte - 1);
    csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & CSR_ACT) && !(csr & CSR_COC));
    assert(x68k_bus_dmac_cycles_to_next_event(b) == 1);

    x68k_bus_dmac_tick(b, 1);
    assert(x68k_bus_read8(b, DST_ADDR) == 1u);
    csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & CSR_ACT) && !(csr & CSR_COC));
    assert(!x68k_bus_dmac2_pending_irq(b));

    x68k_bus_dmac_tick(b, per_byte * 99);
    assert(x68k_bus_read8(b, DST_ADDR + 99) == 100u);
    csr = x68k_bus_read8(b, CH2_CSR);
    assert(!(csr & CSR_ACT) && (csr & CSR_COC));
    assert(x68k_bus_dmac2_pending_irq(b));
    assert(x68k_bus_dmac_cycles_to_next_event(b) == INT_MAX);
    printf("test_busy_until_deadline(%u): OK\n", cpu_hz);
}

/* 2. 転送中のリセットでACTと残り時間が残留しないこと。 */
static void test_reset_clears_busy(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    start_copy(b, 100);
    x68k_bus_reset_peripheral(b);
    assert(x68k_bus_dmac_cycles_to_next_event(b) == INT_MAX);
    assert(!(x68k_bus_read8(b, CH2_CSR) & (CSR_ACT | CSR_COC)));
    x68k_bus_dmac_tick(b, 100000);
    assert(!x68k_bus_dmac2_pending_irq(b));
    printf("test_reset_clears_busy: OK\n");
}

/* ---- CPU bus callbackとの時刻同期 ---- */
typedef struct {
    bool available;
    uint64_t now;
    int calls;
} fake_cpu_clock_t;

static bool fake_cycle_source(void *ctx, uint64_t *out) {
    fake_cpu_clock_t *c = (fake_cpu_clock_t *)ctx;
    c->calls++;
    if (!c->available)
        return false;
    *out = c->now;
    return true;
}

static x68k_bus_t *start_synced(fake_cpu_clock_t *clk, uint32_t len) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    x68k_bus_dmac_set_cycle_source(b, fake_cycle_source, clk);
    start_copy(b, len);
    x68k_bus_dmac_chunk_begin(b);
    return b;
}

/* 2. CPUのstore直前に同期する。DMAが先に読んだbyteは旧値、後に読むbyteは新値になる。 */
static void test_sync_orders_cpu_and_dma(void) {
    const int per_byte = 50; /* 10 DMACクロック @ 50MHz CPU / 10MHz DMAC */
    fake_cpu_clock_t clk = {true, 0, 0};
    x68k_bus_t *b = start_synced(&clk, 4);

    /* 2 byte進んだ時刻でCPUが転送元[2]を書く。callbackが書込み前にDMAを進める。 */
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    clk.now = (uint64_t)(per_byte * 2);
    assert(hooks.write_byte(hooks.ctx, SRC_ADDR + 2u, 0x77u));
    assert(x68k_bus_read8(b, DST_ADDR + 0) == 1u);
    assert(x68k_bus_read8(b, DST_ADDR + 1) == 2u);
    assert(x68k_bus_read8(b, DST_ADDR + 2) == 0u);

    /* store後の同期ではbyte[2]を新値で読む。残りはchunk_tickが進める。 */
    clk.now = (uint64_t)(per_byte * 4);
    x68k_bus_dmac_sync_to_now(b);
    assert(x68k_bus_read8(b, DST_ADDR + 2) == 0x77u);
    x68k_bus_dmac_chunk_tick(b, per_byte * 4);
    assert(!(x68k_bus_read8(b, CH2_CSR) & CSR_ACT));
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_COC);
    x68k_bus_destroy(b);
}

/* 3. 同期済みの分はchunk_tickで二重に進めない。 */
static void test_chunk_tick_excludes_synced(void) {
    fake_cpu_clock_t clk = {true, 0, 0};
    x68k_bus_t *b = start_synced(&clk, 100);
    clk.now = 500; /* 10 byte */
    x68k_bus_dmac_sync_to_now(b);
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 90u);
    x68k_bus_dmac_chunk_tick(b, 600); /* 全体600 → 残り100サイクル=2 byte */
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 88u);
    x68k_bus_destroy(b);
}

/* 4. DMA自身のメモリアクセスでは再帰同期しない（sourceを呼ばない）。 */
static void test_no_recursive_sync(void) {
    fake_cpu_clock_t clk = {true, 0, 0};
    x68k_bus_t *b = start_synced(&clk, 4);
    clk.now = 200;
    x68k_bus_dmac_sync_to_now(b);
    assert(clk.calls == 1); /* 転送中のDMAアクセスからsyncを呼ばない */
    b->dmac.syncing = 1;
    clk.now = 400;
    x68k_bus_dmac_sync_to_now(b);
    assert(clk.calls == 1);
    b->dmac.syncing = 0;
    x68k_bus_destroy(b);
}

/* 5. sourceがfalse（非68060・callback外）なら従来どおり進めない。チャンク外も同様。 */
static void test_no_sync_without_source(void) {
    fake_cpu_clock_t clk = {false, 1000, 0};
    x68k_bus_t *b = start_synced(&clk, 4);
    x68k_bus_dmac_sync_to_now(b);
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 4u);
    clk.available = true;
    x68k_bus_dmac_chunk_tick(b, 0); /* チャンクを閉じる */
    x68k_bus_dmac_sync_to_now(b);
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 4u);
    x68k_bus_destroy(b);
}

/* word転送（IPLROM 1.6の速度測定はOCR=$11: SIZE=word, REQG=limited auto）。MTCは単位数、
 * MAR/DARは2ずつ進み、1単位の時間はbyteと同じ10クロック。奇数アドレスはデータに触れずaddress error。 */
static void start_word_copy(x68k_bus_t *b, uint32_t words, uint32_t src, uint32_t dst) {
    for (uint32_t i = 0; i < words * 2u; i++)
        x68k_bus_write8(b, src + i, (uint8_t)(i + 1));
    x68k_bus_write8(b, CH2_DCR, 0x08u);
    x68k_bus_write8(b, CH2_OCR, 0x11u);
    x68k_bus_write8(b, CH2_SCR, 0x05u);
    x68k_bus_write8(b, CH2_MTC, (uint8_t)(words >> 8));
    x68k_bus_write8(b, CH2_MTC + 1, (uint8_t)words);
    write_long(b, CH2_MAR, src);
    write_long(b, CH2_DAR, dst);
    x68k_bus_write8(b, CH2_CCR, 0x88u);
}

static void test_word_transfer(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_word_copy(b, 4, SRC_ADDR, DST_ADDR);
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_ACT);
    const int per_word = 50; /* 10 DMACクロック @ 50MHz CPU / 10MHz DMAC（CPU未配線） */
    assert(x68k_bus_dmac_cycles_to_next_event(b) == per_word);
    x68k_bus_dmac_tick(b, per_word);
    assert(x68k_bus_read8(b, DST_ADDR) == 1u && x68k_bus_read8(b, DST_ADDR + 1) == 2u);
    assert(x68k_bus_read8(b, DST_ADDR + 2) == 0u);
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 3u);
    assert(x68k_bus_read8(b, CH2_MAR + 3) == (uint8_t)(SRC_ADDR + 2u));
    x68k_bus_dmac_tick(b, per_word * 3);
    assert(x68k_bus_read8(b, DST_ADDR + 7) == 8u);
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_COC);
    x68k_bus_destroy(b);
}

/* DMACクロックはCPUモデルで決まる。68000は10MHz、68060（X68030系）は12.5MHz。 */
static int cycles_per_unit(mc68k_model_t model) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create_model(&hooks, model);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_word_copy(b, 4, SRC_ADDR, DST_ADDR);
    int cycles = x68k_bus_dmac_cycles_to_next_event(b);
    b->cpu = NULL; /* destroy後にbusが参照しないようにする */
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
    return cycles;
}

static void test_dmac_clock_by_cpu_model(void) {
    assert(cycles_per_unit(MC68K_MODEL_68000) == 50); /* 10クロック / 10MHz */
    assert(cycles_per_unit(MC68K_MODEL_68030) == 40); /* 10クロック / 12.5MHz */
    assert(cycles_per_unit(MC68K_MODEL_68060) == 40);
}

static void test_word_odd_address_error(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_word_copy(b, 2, SRC_ADDR + 1u, DST_ADDR);
    x68k_bus_dmac_tick(b, 1000);
    assert(x68k_bus_read8(b, DST_ADDR) == 0u);
    assert(x68k_bus_read8(b, CH2_CSR) & 0x10u); /* ERR */
    assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x05u);
    assert(!(x68k_bus_read8(b, CH2_CSR) & CSR_ACT));
    x68k_bus_destroy(b);
}

/* active中のDCR/SCR/MTC/MAR/DAR書込みはCER=0x02で転送を止め、書込みを破棄する。
 * OCR/BTC/BARはactive中も書ける。ERR後はsticky CSRを消せば再STARTできる。 */
static void test_write_while_active(void) {
    static const uint32_t locked[] = {CH2_DCR, CH2_SCR, CH2_MTC, CH2_MTC + 1u, CH2_MAR,
                                      CH2_MAR + 3u, CH2_DAR, CH2_DAR + 3u};
    for (size_t i = 0; i < sizeof(locked) / sizeof(locked[0]); i++) {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        x68k_bus_set_cpu_hz(b, 50000000u);
        start_copy(b, 100);
        uint8_t before = x68k_bus_read8(b, locked[i]);
        x68k_bus_write8(b, locked[i], (uint8_t)(before ^ 0x40u));
        uint8_t csr = x68k_bus_read8(b, CH2_CSR);
        assert((csr & 0x10u) && !(csr & CSR_ACT)); /* ERR, ACT=0 */
        assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x02u);
        assert(x68k_bus_read8(b, locked[i]) == before);
        assert(x68k_bus_dmac2_pending_irq(b)); /* start_copyはITE=1: エラーIRQ */
        x68k_bus_write8(b, CH2_CSR, 0xFFu);
        start_copy(b, 4);
        assert(x68k_bus_read8(b, CH2_CSR) & CSR_ACT);
        x68k_bus_destroy(b);
    }
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100);
    x68k_bus_write8(b, CH2_OCR, 0x00u);
    x68k_bus_write8(b, 0xE8409Bu, 0x12u); /* BTC+1 */
    x68k_bus_write8(b, 0xE8409Du, 0x34u); /* BAR+1 */
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_ACT);
    assert(!(x68k_bus_read8(b, CH2_CSR) & 0x10u));
    x68k_bus_destroy(b);
    printf("test_write_while_active: OK\n");
}

#define CH2_BTC  0xE8409Au
#define CH2_BAR  0xE8409Cu
#define TABLE_ADDR 0x3000u

static void start_chain(x68k_bus_t *b, uint8_t ocr, uint32_t bar, uint16_t btc) {
    x68k_bus_write8(b, CH2_DCR, 0x08u);
    x68k_bus_write8(b, CH2_OCR, ocr);
    x68k_bus_write8(b, CH2_SCR, 0x00u); /* DAR固定。長いリンクがチェーン表を上書きしない */
    write_long(b, CH2_DAR, DST_ADDR);
    write_long(b, CH2_BAR, bar);
    x68k_bus_write8(b, CH2_BTC, (uint8_t)(btc >> 8));
    x68k_bus_write8(b, CH2_BTC + 1, (uint8_t)btc);
    x68k_bus_write8(b, CH2_CCR, 0x88u);
}

static void put_entry(x68k_bus_t *b, uint32_t at, uint32_t mar, uint16_t len, int link, uint32_t next) {
    write_long(b, at, mar);
    x68k_bus_write8(b, at + 4, (uint8_t)(len >> 8));
    x68k_bus_write8(b, at + 5, (uint8_t)len);
    if (link)
        write_long(b, at + 6, next);
}

/* チェーン表の読出しは1 byte 5 DMACクロック（50MHz CPU/10MHz DMACで1クロック5 CPUサイクル）。
 * array(6 byte)=150サイクル、link(10 byte)=250サイクル後にデータ転送が始まり、次の表も同様。 */
static void test_table_fetch_cost(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    x68k_bus_write8(b, SRC_ADDR, 0xAAu);
    x68k_bus_write8(b, SRC_ADDR + 8, 0xBBu);
    put_entry(b, TABLE_ADDR, SRC_ADDR, 1, 0, 0);
    put_entry(b, TABLE_ADDR + 6, SRC_ADDR + 8, 1, 0, 0);
    start_chain(b, 0x08u, TABLE_ADDR, 2);
    assert(x68k_bus_dmac_cycles_to_next_event(b) == 200);
    x68k_bus_dmac_tick(b, 199);
    assert(x68k_bus_read8(b, DST_ADDR) == 0u);
    x68k_bus_dmac_tick(b, 1);
    assert(x68k_bus_read8(b, DST_ADDR) == 0xAAu);
    x68k_bus_dmac_tick(b, 199); /* 次の表150 + データ50 の手前 */
    assert(x68k_bus_read8(b, DST_ADDR) == 0xAAu);
    x68k_bus_dmac_tick(b, 1);
    assert(x68k_bus_read8(b, DST_ADDR) == 0xBBu);
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_COC);
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    put_entry(b, TABLE_ADDR, SRC_ADDR, 1, 1, 0);
    start_chain(b, 0x0Cu, TABLE_ADDR, 0);
    assert(x68k_bus_dmac_cycles_to_next_event(b) == 300); /* 表250 + データ50 */
    x68k_bus_destroy(b);
    printf("test_table_fetch_cost: OK\n");
}

/* 自己参照リンクと循環リンクはCER=0x01で安全停止する（ix68k固有の拡張）。 */
static void test_link_loop_stops(void) {
    for (int ring = 0; ring < 2; ring++) {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        x68k_bus_set_cpu_hz(b, 50000000u);
        uint32_t second = TABLE_ADDR + 16u;
        put_entry(b, TABLE_ADDR, SRC_ADDR, 1, 1, ring ? second : TABLE_ADDR);
        if (ring)
            put_entry(b, second, SRC_ADDR, 1, 1, TABLE_ADDR);
        start_chain(b, 0x0Cu, TABLE_ADDR, 0);
        for (int i = 0; i < 200 && (x68k_bus_read8(b, CH2_CSR) & CSR_ACT); i++)
            x68k_bus_dmac_tick(b, 1000000);
        uint8_t csr = x68k_bus_read8(b, CH2_CSR);
        assert((csr & 0x10u) && !(csr & CSR_ACT) && !(csr & CSR_COC));
        assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x01u);
        x68k_bus_destroy(b);
    }
    printf("test_link_loop_stops: OK\n");
}

/* 未対応のDCR/OCR/SCR組合せはデータに触れずCER=0x01で拒否する。 */
static void test_unsupported_config_rejected(void) {
    static const struct { uint8_t dcr, ocr, scr; } cfg[] = {
        {0x00u, 0x00u, 0x05u}, /* 8-bitポート */
        {0x09u, 0x00u, 0x05u}, /* DCR予約ビット */
        {0x08u, 0x20u, 0x05u}, /* SIZE=long */
        {0x08u, 0x30u, 0x05u}, /* SIZE=11 (8-bitのunpacked) */
        {0x08u, 0x04u, 0x05u}, /* CHAIN=01 予約 */
        {0x08u, 0x00u, 0x0Du}, /* MAC=11 予約 */
        {0x08u, 0x00u, 0x07u}, /* DAC=11 予約 */
    };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        x68k_bus_set_cpu_hz(b, 50000000u);
        x68k_bus_write8(b, SRC_ADDR, 0x5Au);
        x68k_bus_write8(b, CH2_DCR, cfg[i].dcr);
        x68k_bus_write8(b, CH2_OCR, cfg[i].ocr);
        x68k_bus_write8(b, CH2_SCR, cfg[i].scr);
        x68k_bus_write8(b, CH2_MTC + 1, 4u);
        write_long(b, CH2_MAR, SRC_ADDR);
        write_long(b, CH2_DAR, DST_ADDR);
        x68k_bus_write8(b, CH2_CCR, 0x88u);
        uint8_t csr = x68k_bus_read8(b, CH2_CSR);
        assert((csr & 0x10u) && !(csr & CSR_ACT) && !(csr & CSR_COC));
        assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x01u);
        x68k_bus_dmac_tick(b, 10000);
        assert(x68k_bus_read8(b, DST_ADDR) == 0u);
        x68k_bus_destroy(b);
    }
    printf("test_unsupported_config_rejected: OK\n");
}

static uint32_t read_long(x68k_bus_t *b, uint32_t addr) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4u; i++)
        v = (v << 8) | x68k_bus_read8(b, addr + i);
    return v;
}

/* エラー完了はITE=1ならレベル3を要求し、IACKでEIVを返す。ITE=0なら要求しない。 */
static void test_error_irq(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    x68k_bus_write8(b, 0xE840A5u, 0x45u); /* NIV */
    x68k_bus_write8(b, 0xE840A7u, 0x44u); /* EIV */
    start_copy(b, 100);
    x68k_bus_write8(b, CH2_MTC + 1, 1u);
    assert(x68k_bus_dmac2_pending_irq(b));
    uint8_t vec = 0;
    assert(dmac_iack(b, &vec) && vec == 0x44u);
    assert(!x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy_ccr(b, 100, 0x80u); /* ITE=0 */
    x68k_bus_write8(b, CH2_MTC + 1, 1u);
    assert(x68k_bus_read8(b, CH2_CSR) & 0x10u);
    assert(!x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);
    printf("test_error_irq: OK\n");
}

/* チャンク途中のSTARTは、START以前のCPU時間をDMAに数えない。 */
static void test_start_mid_chunk(void) {
    fake_cpu_clock_t clk = {true, 0, 0};
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    x68k_bus_dmac_set_cycle_source(b, fake_cycle_source, &clk);
    x68k_bus_dmac_chunk_begin(b);
    clk.now = 150;
    start_copy(b, 4);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    clk.now = 160; /* START後10サイクル。1単位(50)に満たない */
    assert(hooks.write_byte(hooks.ctx, 0x4000u, 0u));
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 4u);
    clk.now = 200; /* START後50サイクル */
    assert(hooks.write_byte(hooks.ctx, 0x4000u, 0u));
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 3u);
    x68k_bus_destroy(b);
    printf("test_start_mid_chunk: OK\n");
}

/* CCRはSTRを読戻しで0にする。active中のread-modify-writeが転送を中断しない。 */
static void test_ccr_str_readback(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100);
    uint8_t ccr = x68k_bus_read8(b, CH2_CCR);
    assert(!(ccr & 0x80u) && (ccr & 0x08u));
    x68k_bus_write8(b, CH2_CCR, ccr);
    uint8_t csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & CSR_ACT) && !(csr & 0x10u));
    x68k_bus_destroy(b);
    printf("test_ccr_str_readback: OK\n");
}

/* active中のlong書込み（バイト分割で届く）は4バイトとも破棄される。 */
static void test_long_write_discarded(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100);
    write_long(b, CH2_MAR, 0x00005000u);
    assert(read_long(b, CH2_MAR) == SRC_ADDR);
    x68k_bus_write8(b, CH2_CSR, 0xFFu);
    write_long(b, CH2_MAR, 0x00005000u); /* 非activeでは書ける */
    assert(read_long(b, CH2_MAR) == 0x00005000u);
    x68k_bus_destroy(b);
    printf("test_long_write_discarded: OK\n");
}

/* チェーンからロードしたMTC=0はCER=0x0D。表を使い切ったアレイは正常完了。 */
static void test_chain_zero_mtc(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    put_entry(b, TABLE_ADDR, SRC_ADDR, 1, 0, 0);
    put_entry(b, TABLE_ADDR + 6, SRC_ADDR + 8, 0, 0, 0);
    start_chain(b, 0x08u, TABLE_ADDR, 2);
    x68k_bus_dmac_tick(b, 100000);
    uint8_t csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & 0x10u) && !(csr & CSR_COC) && !(csr & CSR_ACT));
    assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x0Du);
    x68k_bus_destroy(b);
    printf("test_chain_zero_mtc: OK\n");
}

/* 完了IRQはIACKでのみ消費する。COCを消して再STARTしてもpendingは消えない。 */
static void test_pending_survives_restart(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 2);
    x68k_bus_dmac_tick(b, 1000);
    assert(x68k_bus_dmac2_pending_irq(b));
    x68k_bus_write8(b, CH2_CSR, 0xFFu);
    start_copy(b, 2);
    assert(x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);
    printf("test_pending_survives_restart: OK\n");
}

/* SAB・active中の再STARTのITEは、START時にラッチした値で決まる。 */
static void test_latched_ite(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy_ccr(b, 100, 0x80u); /* ITE=0 */
    x68k_bus_write8(b, CH2_CCR, 0x18u); /* SAB + ITE=1 */
    assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x11u);
    assert(!x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100); /* ITE=1 */
    x68k_bus_write8(b, CH2_CCR, 0x80u); /* active中の再START（ITE=0） */
    assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x02u);
    assert(x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);
    printf("test_latched_ite: OK\n");
}

/* active中にCPU Hzを変えても、残り時間（経過割合）が保たれる。 */
static void test_cpu_hz_change_keeps_progress(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100);
    x68k_bus_dmac_tick(b, 25); /* 1単位(50サイクル)の半分 */
    x68k_bus_set_cpu_hz(b, 25000000u);
    assert(x68k_bus_dmac_cycles_to_next_event(b) == 13); /* 残り半分 = 12.5 → 13 */
    x68k_bus_destroy(b);
    printf("test_cpu_hz_change_keeps_progress: OK\n");
}

/* DMA自身の書込みがch2のロック対象レジスタに当たったら、そこで止まる（COCを立てない）。 */
static void test_dma_write_into_ch2_regs(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    x68k_bus_write8(b, SRC_ADDR, 0x11u);
    x68k_bus_write8(b, CH2_DCR, 0x08u);
    x68k_bus_write8(b, CH2_OCR, 0x00u);
    x68k_bus_write8(b, CH2_SCR, 0x05u);
    x68k_bus_write8(b, CH2_MTC + 1, 1u);
    write_long(b, CH2_MAR, SRC_ADDR);
    write_long(b, CH2_DAR, CH2_MTC);
    x68k_bus_write8(b, CH2_CCR, 0x80u);
    x68k_bus_dmac_tick(b, 1000);
    uint8_t csr = x68k_bus_read8(b, CH2_CSR);
    assert((csr & 0x10u) && !(csr & CSR_COC) && !(csr & CSR_ACT));
    x68k_bus_destroy(b);
    printf("test_dma_write_into_ch2_regs: OK\n");
}

static uint8_t cer_of(x68k_bus_t *b) {
    return x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu;
}

/* 異常系: COC残留の再START、MTC=0、array START時のBTC=0、非activeのSAB。 */
static void test_error_codes(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 2);
    x68k_bus_dmac_tick(b, 1000);
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_COC);
    start_copy(b, 2); /* COCを消さずに再START */
    assert((x68k_bus_read8(b, CH2_CSR) & 0x10u) && cer_of(b) == 0x02u);
    assert(x68k_bus_read8(b, CH2_CSR) & CSR_COC); /* COCは自動で消さない */
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 0);
    assert((x68k_bus_read8(b, CH2_CSR) & 0x10u) && cer_of(b) == 0x0Du);
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_chain(b, 0x08u, TABLE_ADDR, 0);
    assert((x68k_bus_read8(b, CH2_CSR) & 0x10u) && cer_of(b) == 0x0Fu);
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_write8(b, CH2_CCR, 0x10u); /* 非activeのSAB */
    assert(!(x68k_bus_read8(b, CH2_CSR) & 0x10u));
    assert(!(x68k_bus_read8(b, CH2_CCR) & 0x10u));
    assert(!x68k_bus_dmac2_pending_irq(b));
    x68k_bus_destroy(b);
    printf("test_error_codes: OK\n");
}

/* 端数比(33MHz CPU / 12.5MHz DMAC)でも、一括tickと1サイクル刻みで同じ状態になる。 */
static void test_fractional_tick_split(void) {
    uint8_t dst[2][8];
    for (int run = 0; run < 2; run++) {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        mc68k_bus_t hooks = x68k_bus_get_hooks(b);
        mc68k_state_t *cpu = mc68k_create_model(&hooks, MC68K_MODEL_68060);
        assert(cpu != NULL);
        x68k_bus_set_cpu(b, cpu);
        x68k_bus_set_cpu_hz(b, 33000000u);
        start_copy(b, 8);
        if (run == 0) {
            x68k_bus_dmac_tick(b, 1000);
        } else {
            for (int i = 0; i < 1000; i++)
                x68k_bus_dmac_tick(b, 1);
        }
        for (uint32_t i = 0; i < 8; i++)
            dst[run][i] = x68k_bus_read8(b, DST_ADDR + i);
        assert(x68k_bus_read8(b, CH2_MTC + 1) == 0u);
        b->cpu = NULL;
        mc68k_destroy(cpu);
        x68k_bus_destroy(b);
    }
    for (int i = 0; i < 8; i++)
        assert(dst[0][i] == dst[1][i]);
    printf("test_fractional_tick_split: OK\n");
}

/* 破棄は同一CPUアクセス内だけ。エラー後の次のアクセスは破棄しない。 */
static void test_discard_limited_to_one_access(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, 50000000u);
    start_copy(b, 100);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    assert(hooks.write_word(hooks.ctx, CH2_MTC, 0x0001u)); /* active中: 2バイトとも破棄 */
    assert(x68k_bus_read8(b, CH2_MTC + 1) == 100u);
    assert((x68k_bus_read8(b, CH2_CSR + 1) & 0x1Fu) == 0x02u);
    assert(hooks.write_long(hooks.ctx, CH2_MAR, 0x00005000u)); /* エラー後の別アクセス */
    assert(read_long(b, CH2_MAR) == 0x00005000u);
    x68k_bus_destroy(b);
    printf("test_discard_limited_to_one_access: OK\n");
}

int main(void) {
    test_busy_until_deadline(10000000u);
    test_busy_until_deadline(50000000u);
    test_reset_clears_busy();
    test_sync_orders_cpu_and_dma();
    test_chunk_tick_excludes_synced();
    test_no_recursive_sync();
    test_no_sync_without_source();
    test_word_transfer();
    test_word_odd_address_error();
    test_dmac_clock_by_cpu_model();
    test_write_while_active();
    test_table_fetch_cost();
    test_link_loop_stops();
    test_unsupported_config_rejected();
    test_error_irq();
    test_start_mid_chunk();
    test_ccr_str_readback();
    test_long_write_discarded();
    test_chain_zero_mtc();
    test_pending_survives_restart();
    test_latched_ite();
    test_cpu_hz_change_keeps_progress();
    test_dma_write_into_ch2_regs();
    test_error_codes();
    test_fractional_tick_split();
    test_discard_limited_to_one_access();
    printf("dmac_ch2_timing_test: all tests passed\n");
    return 0;
}
