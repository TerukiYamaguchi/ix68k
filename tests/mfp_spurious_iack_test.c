/*
 * MFP(IRQ6) の IACK にベクタを渡せないときスプリアス割り込み(ベクタ24)へ倒れる
 * ことの単体テスト。
 *
 * CPUのIRQ線は x68k_cycle_catchup()（MFP_TICK_CYCLES粒度）でしか更新されない
 * 一方、IMRA/IMRB書き込みはマスクしたチャンネルの mfp_ch_pending をその場で
 * クリアする。このため「線はレベル6のまま、pendingは空」という古い線が生じ、
 * ゲストがIPLを下げた瞬間にベクタの無いレベル6割り込みが取られる（PCM8A.Xの
 * 割り込みハンドラが冒頭でMFPを部分マスクした直後にIPLを7から5へ下げるため
 * 実際に発生する）。ここでオートベクタ($78 = ベクタ30、IPLROM未設定)へ飛ぶと
 * Human68kの例外ダイアログになるため、他レベルと同じくスプリアス割り込みとして
 * 扱わなければならない。
 *
 * bus_iack() は static なので、mfp_isr_eoi_test.c と同じく
 * x68k_bus_get_hooks() 経由で叩く。ベクタ選択自体はCPUコアが行うため、
 * 実CPUを1命令ずつ走らせて着地先ベクタを確認する。
 */
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>

#define VEC_SPURIOUS   24 /* スプリアス割り込み */
#define VEC_AUTOVEC_L6 30 /* レベル6オートベクタ($78)。ここへ落ちるのが不具合 */

#define RESET_SSP     0x00008000u
#define CODE_ADDR     0x00001000u
#define SPURIOUS_ADDR 0x00002000u
#define AUTOVEC_ADDR  0x00003000u

static void write_long(x68k_bus_t *b, uint32_t addr, uint32_t val) {
    x68k_bus_write8(b, addr, (uint8_t)(val >> 24));
    x68k_bus_write8(b, addr + 1, (uint8_t)(val >> 16));
    x68k_bus_write8(b, addr + 2, (uint8_t)(val >> 8));
    x68k_bus_write8(b, addr + 3, (uint8_t)val);
}

static void write_word(x68k_bus_t *b, uint32_t addr, uint16_t val) {
    x68k_bus_write8(b, addr, (uint8_t)(val >> 8));
    x68k_bus_write8(b, addr + 1, (uint8_t)val);
}

/* リセットベクタ・例外ベクタ・テストコードを配置してCPUを起動する。
 * コードは「MOVE #$2000,SR」(IPLマスクを0へ下げる) + NOP。 */
static mc68k_state_t *setup_cpu(x68k_bus_t *b) {
    write_long(b, 0x000000u, RESET_SSP);
    write_long(b, 0x000004u, CODE_ADDR);
    write_long(b, (uint32_t)VEC_SPURIOUS * 4u, SPURIOUS_ADDR);
    write_long(b, (uint32_t)VEC_AUTOVEC_L6 * 4u, AUTOVEC_ADDR);

    write_word(b, CODE_ADDR, 0x46FCu); /* MOVE #imm,SR */
    write_word(b, CODE_ADDR + 2, 0x2000u);
    write_word(b, CODE_ADDR + 4, 0x4E71u); /* NOP */
    write_word(b, CODE_ADDR + 6, 0x4E71u); /* NOP */

    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu); /* iack_spurious()がCPUへ通知するために必須 */
    mc68k_reset(cpu);
    assert(mc68k_get_pc(cpu) == CODE_ADDR);

    mc68k_execute(cpu, 1); /* MOVE #$2000,SR: IPLマスク0・スーパーバイザ */
    assert(mc68k_is_supervisor(cpu));
    return cpu;
}

/* 1. pendingが空のままレベル6割り込みを受け付けたら、オートベクタ(ベクタ30)
 *    ではなくスプリアス割り込み(ベクタ24)へ飛ぶこと。 */
static void test_stale_irq6_line_takes_spurious_vector(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_state_t *cpu = setup_cpu(b);

    assert(b->mfp.ch_pending == 0u); /* 古い線: 線は立つがpendingは空 */
    mc68k_set_irq(cpu, 6);
    mc68k_execute(cpu, 1);

    assert(mc68k_get_pc(cpu) == SPURIOUS_ADDR);
    printf("test_stale_irq6_line_takes_spurious_vector: PC=0x%06X (vector %d)\n",
           mc68k_get_pc(cpu), VEC_SPURIOUS);
    mc68k_destroy(cpu);
}

/* 2. pendingが立っているときは従来どおりMFPの正規ベクタ(VR|チャンネル番号)へ
 *    飛ぶこと（1.の変更が正常経路を壊していないことの回帰確認）。 */
static void test_pending_irq6_takes_mfp_vector(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_state_t *cpu = setup_cpu(b);

    uint32_t handler = 0x00004000u;
    uint8_t vec = (uint8_t)(b->mfp.vr | (unsigned)MFP_CH_TIMER_C);
    write_long(b, (uint32_t)vec * 4u, handler);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);

    mc68k_set_irq(cpu, 6);
    mc68k_execute(cpu, 1);

    assert(mc68k_get_pc(cpu) == handler);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C)); /* IACKで消費される */
    printf("test_pending_irq6_takes_mfp_vector: PC=0x%06X (vector 0x%02X)\n", mc68k_get_pc(cpu),
           vec);
    mc68k_destroy(cpu);
}

/* 3. IMRA/IMRB書き込みが古い線を作る実際の経路の再現。Timer C(ch5)をpendingに
 *    してからIMRBでマスクするとch_pendingが消え、線だけが残る状態になる。 */
static void test_imr_write_clears_pending_leaving_stale_line(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_state_t *cpu = setup_cpu(b);

    mfp_reg_write8(b, 0xE88015u, 0x20u); /* IMRB bit5 = Timer C アンマスク */
    b->mfp.iprb |= 0x20u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    assert(x68k_bus_mfp_pending_irq(b));
    mc68k_set_irq(cpu, 6); /* ここでIRQ線が立つ */

    /* ゲストがIMRBでTimer Cをマスクする。線は次のcatchupまで下がらない。 */
    mfp_reg_write8(b, 0xE88015u, 0x00u);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
    assert(!x68k_bus_mfp_pending_irq(b));

    mc68k_execute(cpu, 1);
    assert(mc68k_get_pc(cpu) == SPURIOUS_ADDR);
    printf("test_imr_write_clears_pending_leaving_stale_line: PC=0x%06X\n", mc68k_get_pc(cpu));
    mc68k_destroy(cpu);
}

#define RTE_STUB_ROM_ADDR  0x0000FE006Cu /* IPLROM内nmi_handler末尾の単独rte */
/* IPLROMのベクタ一括フィル値。上位バイトにベクタ番号、下位24bitに共有例外
 * ハンドラ($FF0770)を置く形式（実測値）。 */
#define IPLROM_VECTOR_FILL 0x18FF0770u
/* ゲスト（NetBSD/x68kカーネル）が物理$0へロードされたときの$60の中身の例。
 * locore.s の `movc %d0,%cacr` の後半＋`movl #DC_FREEZE,%d0` の先頭2バイト。 */
#define GUEST_CODE_AT_VEC  0x0002203Cu

/* 4. スプリアス割り込み対策: x68k_bus_pin_spurious_vector() を呼んでおくと、ベクタ24が
 *    IPLROM内の単独rte命令($FE006C)を指すよう固定され、スプリアス割り込みが
 *    実際にRTEで静かに元の位置へ復帰すること。
 *    RAM[0x0100]（install_rte_stubs()のRTEスタブ）はBIOS実行前だけの一時的な
 *    安全地帯で、Human68k起動後はワークエリアとして再利用され得る（動作確認で
 *    実際に上書きされ「おかしな命令」例外に化けた）ため、恒久的な固定先には
 *    使えない。ここではROM領域を模すため iocs_rom バッファへ直接rteを書く。 */
static void test_pinned_spurious_vector_returns_via_rte(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_state_t *cpu = setup_cpu(b);

    /* bus_read_rom8()は$FE0000-$FFFFFFを iocs_rom[0x0C0000..] へ直接マップする
     * （x68k_bus_load_iocs_romが128KB ROMをこのオフセットへ丸ごとfreadする）。
     * addr-IOCS_ROM_BASEではIPLROMの折り返し(iocs_rom_offset)を経ないため
     * 一致しない。 */
    uint32_t rom_off = 0x0C0000u + (RTE_STUB_ROM_ADDR - 0x00FE0000u);
    b->iocs_rom[rom_off] = 0x4E; /* RTE: 0x4E73 */
    b->iocs_rom[rom_off + 1] = 0x73;

    /* IPLROMのベクタ一括フィル直後の状態を模す。 */
    write_long(b, (uint32_t)VEC_SPURIOUS * 4u, IPLROM_VECTOR_FILL);
    x68k_bus_pin_spurious_vector(b);
    assert(x68k_bus_read32(b, (uint32_t)VEC_SPURIOUS * 4u) == RTE_STUB_ROM_ADDR);
    x68k_bus_pin_spurious_vector(b); /* 冪等: 自分が書いた値は書き直してよい */
    assert(x68k_bus_read32(b, (uint32_t)VEC_SPURIOUS * 4u) == RTE_STUB_ROM_ADDR);

    uint32_t return_pc = mc68k_get_pc(cpu); /* まだ実行されていないNOPの位置 */
    assert(b->mfp.ch_pending == 0u);
    mc68k_set_irq(cpu, 6);
    mc68k_execute(cpu, 1); /* IACK→スプリアス→ベクタフェッチ */
    assert(mc68k_get_pc(cpu) == RTE_STUB_ROM_ADDR);

    mc68k_execute(cpu, 1); /* RTE実行 */
    assert(mc68k_get_pc(cpu) == return_pc);
    printf("test_pinned_spurious_vector_returns_via_rte: PC=0x%06X\n", mc68k_get_pc(cpu));
    mc68k_destroy(cpu);
}

/* 5. ベクタ24をゲストが自前で使っている場合は固定しないこと。NetBSD/x68k は
 *    カーネルを物理$0へロードするため$60はベクタではなくカーネルコードであり、
 *    無条件に書き込むと locore.s のCPU判別部を壊して不当命令例外になる。 */
static void test_pin_leaves_guest_owned_vector_alone(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    write_long(b, (uint32_t)VEC_SPURIOUS * 4u, GUEST_CODE_AT_VEC);
    x68k_bus_pin_spurious_vector(b);
    assert(x68k_bus_read32(b, (uint32_t)VEC_SPURIOUS * 4u) == GUEST_CODE_AT_VEC);

    /* RAM上の自前ハンドラを置いた場合も同じ（ROM領域を指していない）。 */
    write_long(b, (uint32_t)VEC_SPURIOUS * 4u, CODE_ADDR);
    x68k_bus_pin_spurious_vector(b);
    assert(x68k_bus_read32(b, (uint32_t)VEC_SPURIOUS * 4u) == CODE_ADDR);

    printf("test_pin_leaves_guest_owned_vector_alone: ok\n");
    x68k_bus_destroy(b);
}

int main(void) {
    test_stale_irq6_line_takes_spurious_vector();
    test_pending_irq6_takes_mfp_vector();
    test_imr_write_clears_pending_leaving_stale_line();
    test_pinned_spurious_vector_returns_via_rte();
    test_pin_leaves_guest_owned_vector_alone();
    printf("mfp_spurious_iack_test: all tests passed\n");
    return 0;
}
