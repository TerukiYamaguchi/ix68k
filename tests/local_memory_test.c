#include "bus/x68k_bus.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "peripheral/hostdrv_remote.h"
#include <assert.h>
#include <stdio.h>

static void check_rtc_mirror(x68k_bus_t *b) {
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    uint16_t word;
    uint32_t longword;
    const uint32_t mirror = 0x01e8a000u;
    assert(h.read_word(h.ctx, mirror, &word));
    assert(h.read_long(h.ctx, mirror, &longword));
    assert(h.write_word(h.ctx, mirror, 0x1234));
    assert(h.write_long(h.ctx, mirror, 0x56789abcu));
}

static void check_fpu_cir_mirror(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_set_fpu_cir(b, 1, X68K_FPU_CIR_PRIMARY));
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    uint16_t word;
    uint32_t longword;
    const uint32_t mirror = 0x01e9e000u;
    assert(h.read_word(h.ctx, mirror, &word));
    assert(h.read_long(h.ctx, mirror, &longword));
    assert(h.write_word(h.ctx, mirror, 0x1234));
    assert(h.write_long(h.ctx, mirror, 0x56789abcu));
    x68k_bus_destroy(b);
}

static void check_lifetime(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b && x68k_bus_set_local_memory(b, 0));
    assert(x68k_bus_set_local_memory(b, 16));
    assert(!x68k_bus_set_local_memory(b, 0));
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&h);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu);
    assert(!x68k_bus_set_local_memory(b, 32));
    x68k_bus_set_super_protect(b, 1);
    uint8_t value;
    assert(h.write_byte(h.ctx, 0x10000000u, 0x5a));
    assert(h.read_byte(h.ctx, 0x10000000u, &value) && value == 0x5a);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);

    b = x68k_bus_create();
    assert(b != NULL);
    h = x68k_bus_get_hooks(b);
    cpu = mc68k_create(&h);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu);
    assert(!x68k_bus_set_local_memory(b, 0));
    assert(!x68k_bus_set_local_memory(b, 16));
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void check(unsigned mb) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b && x68k_bus_set_local_memory(b, mb));
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    uint8_t v;
    uint32_t base = 0x10000000u, end = base + mb * 1024u * 1024u - 1u;
    assert(h.write_byte(h.ctx, base, 0x12) && h.read_byte(h.ctx, base, &v) && v == 0x12);
    assert(h.write_byte(h.ctx, end, 0x34) && h.read_byte(h.ctx, end, &v) && v == 0x34);
    assert(h.write_word(h.ctx, base + 2u, 0x5678));
    uint16_t word;
    assert(h.read_word(h.ctx, base + 2u, &word) && word == 0x5678);
    assert(h.write_long(h.ctx, base + 4u, 0x9abcdef0u));
    uint32_t longword;
    assert(h.read_long(h.ctx, base + 4u, &longword) && longword == 0x9abcdef0u);
    /* 容量外候補は低位24-bitへ戻る。 */
    assert(h.write_byte(h.ctx, base + mb * 1024u * 1024u, 0x56));
    assert(h.read_byte(h.ctx, 0, &v) && v == 0x56);
    /* 開始・終端跨ぎのword/longは各byteを別々にデコードする。 */
    assert(h.write_word(h.ctx, base - 1u, 0xabcd));
    assert(h.read_byte(h.ctx, base, &v) && v == 0xcd);
    uint8_t start0, start1, start2;
    assert(h.read_byte(h.ctx, base - 3u, &start0));
    assert(h.read_byte(h.ctx, base - 2u, &start1));
    assert(h.read_byte(h.ctx, base - 1u, &start2));
    assert(h.read_long(h.ctx, base - 3u, &longword));
    assert(longword == ((uint32_t)start0 << 24 | (uint32_t)start1 << 16 |
                        (uint32_t)start2 << 8 | 0xcdu));
    assert(h.write_long(h.ctx, end - 1u, 0x12345678u));
    assert(h.read_byte(h.ctx, end - 1u, &v) && v == 0x12);
    assert(h.read_byte(h.ctx, end, &v) && v == 0x34);
    assert(h.read_byte(h.ctx, 0, &v) && v == 0x56);
    assert(h.read_byte(h.ctx, 1, &v) && v == 0x78);
    assert(h.read_long(h.ctx, end - 1u, &longword) && longword == 0x12345678u);
    /* 共通API（HOSTDRV/MCP側）は局所メモリへ到達しない。 */
    x68k_bus_write8(b, base, 0xa5);
    assert(h.read_byte(h.ctx, base, &v) && v == 0xcd);
    check_rtc_mirror(b);
    assert(!x68k_bus_set_local_memory(b, 0));
    x68k_bus_destroy(b);
}
/* HOSTDRVの$57（メディアチェック）でリクエストヘッダを処理させる。
 * 処理済みなら +3/+4/+18 が 0 に書き換わる（事前に非ゼロを入れておく）。 */
#define HDR_COMCOD      2u
#define HDR_ERR_LOW     3u
#define HDR_RESULT      18u
#define HDR_SIZE        26u
#define COMCOD_MEDIACHK 0x57u
#define PRESET_ERR      0x36u

static void hostdrv_prepare_header(x68k_bus_t *b, uint32_t phys) {
    for (uint32_t i = 0; i < HDR_SIZE; i++)
        x68k_bus_write8_full(b, phys + i, 0);
    x68k_bus_write8_full(b, phys + HDR_COMCOD, COMCOD_MEDIACHK);
    x68k_bus_write8_full(b, phys + HDR_ERR_LOW, PRESET_ERR);
    for (uint32_t i = 0; i < 4; i++)
        x68k_bus_write8_full(b, phys + HDR_RESULT + i, 0xff);
}

static void hostdrv_exec(x68k_bus_t *b, uint32_t guest_header) {
    for (uint32_t i = 0; i < 4; i++)
        x68k_bus_write8(b, HOSTDRV_RD_BASE + 4u + i, (uint8_t)(guest_header >> (24u - 8u * i)));
    x68k_bus_write8(b, HOSTDRV_RD_BASE + 2u, HOSTDRV_RD_CMD_EXEC);
}

static int hostdrv_header_handled(x68k_bus_t *b, uint32_t phys) {
    return x68k_bus_read8_full(b, phys + HDR_ERR_LOW) == 0 &&
           x68k_bus_read8_full(b, phys + HDR_RESULT) == 0 &&
           x68k_bus_read8_full(b, phys + HDR_RESULT + 3u) == 0;
}

/* ローカルメモリ上のリクエストヘッダをHOSTDRVが読み書きできる（以前は書き込みが捨てられた）。 */
static void check_hostdrv_local_memory(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b && x68k_bus_set_local_memory(b, 16));
    const uint32_t header = 0x10000100u;
    hostdrv_prepare_header(b, header);
    hostdrv_exec(b, header);
    assert(hostdrv_header_handled(b, header));
    /* MCP の read_memory と同じ peek もローカルメモリを読める。 */
    x68k_bus_write8_full(b, 0x10000200u, 0x6b);
    assert(x68k_bus_peek8(b, 0x10000200u) == 0x6b);
    x68k_bus_destroy(b);
}

static void write_long_be(x68k_bus_t *b, uint32_t addr, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++)
        x68k_bus_write8(b, addr + i, (uint8_t)(value >> (24u - 8u * i)));
}

static void write_word_be(x68k_bus_t *b, uint32_t addr, uint16_t value) {
    x68k_bus_write8(b, addr, (uint8_t)(value >> 8));
    x68k_bus_write8(b, addr + 1u, (uint8_t)value);
}

/* PMMUが論理 $8000 を物理 $9000 へずらしているとき、HOSTDRVは論理側のヘッダを処理する
 * （060turbo.sys の -ls がヘッダのあるページを移すと、以前は物理側を読み書きして
 * Humanに結果が届かなかった）。ページテーブルの組み方は debugger_test.c と同じ。 */
static void check_hostdrv_pmmu(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create_model(&hooks, MC68K_MODEL_68030);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu);

    write_long_be(b, 0x4000u, 0x0000000Au);
    write_long_be(b, 0x4004u, 0x00005000u);
    for (uint32_t page = 0; page < 0x20u; page++)
        write_long_be(b, 0x5000u + page * 4u, (page << 12) | 9u);
    write_long_be(b, 0x5020u, 0x00009009u); /* 論理 $008xxx -> 物理 $009xxx */

    write_long_be(b, 0x0000u, 0x00003000u);
    write_long_be(b, 0x0004u, 0x00001000u);
    write_long_be(b, 0x2000u, 0x80000003u);
    write_long_be(b, 0x2004u, 0x00004000u);
    write_long_be(b, 0x2008u, 0x82C0AA00u);
    write_word_be(b, 0x1000u, 0x207Cu); write_long_be(b, 0x1002u, 0x00002000u);
    write_word_be(b, 0x1006u, 0xF010u); write_word_be(b, 0x1008u, 0x4C00u); /* PMOVE (A0),CRP */
    write_word_be(b, 0x100Au, 0xF010u); write_word_be(b, 0x100Cu, 0x4800u); /* PMOVE (A0),SRP */
    write_word_be(b, 0x100Eu, 0x207Cu); write_long_be(b, 0x1010u, 0x00002008u);
    write_word_be(b, 0x1014u, 0xF010u); write_word_be(b, 0x1016u, 0x4000u); /* PMOVE (A0),TC */
    mc68k_reset(cpu);
    for (int i = 0; i < 5; i++)
        mc68k_execute(cpu, 1);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    assert(regs.pc == 0x00001018u);

    hostdrv_prepare_header(b, 0x9000u); /* 変換後の本物のヘッダ */
    hostdrv_prepare_header(b, 0x8000u); /* 論理を無視して物理を見た場合に処理されてしまう囮 */
    hostdrv_exec(b, 0x8000u);
    assert(hostdrv_header_handled(b, 0x9000u));
    assert(!hostdrv_header_handled(b, 0x8000u));

    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

int main(void) {
    x68k_bus_t *off = x68k_bus_create();
    assert(off != NULL);
    check_rtc_mirror(off);
    x68k_bus_destroy(off);
    check_fpu_cir_mirror();
    check_lifetime();
    check(16);
    check(32);
    check(64);
    check(128);
    check_hostdrv_local_memory();
    check_hostdrv_pmmu();
    puts("local_memory_test: OK");
}
