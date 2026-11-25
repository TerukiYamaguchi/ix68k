/*
 * MFP ISRA/ISRB と VR Sビット（ソフトウェアEOI）の単体テスト
 * （docs/ix68k-internals/design/mfp_design.md §9）。mfp_fmirq_test.c と同じ
 * ソース構成・呼び出し流儀（x68k_bus_get_hooks() 経由の iack フック）を踏襲する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    /* 既定はoff（実測でS=1書き込みが観測されなかったため）。
     * S=1関連のテスト(T2-T14)はSビット対応自体の正しさを検証するものなので、
     * ここで明示的に有効化する。off自体の検証はT15で行う。 */
    b->mfp.soft_eoi_enabled = 1;

    /* --- A. VR --- */

    /* T1: bit3=S を保持し、bit2-0は常に0で読める（Inside X68000 L1832） */
    mfp_reg_write8(b, 0xE88017u, 0x48u);
    assert(mfp_reg_read8(b, 0xE88017u) == 0x48u);
    assert(b->mfp.vr == 0x40u); /* ベクタ合成用の値が汚れていないこと */
    mfp_reg_write8(b, 0xE88017u, 0x4Fu);
    assert(mfp_reg_read8(b, 0xE88017u) == 0x48u); /* bit2-0は無視される */

    /* T2: S=1でもゲーティング未導入の段階では既存のIACK動作が変わらない */
    mfp_reg_write8(b, 0xE88017u, 0x40u); /* S=0へ戻す */
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 12u));
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_USART_RX));

    /* T3: S=1→S=0 の書き込みで ISRA/ISRB が 0 になる（mfp_design.md §5.2） */
    mfp_reg_write8(b, 0xE88017u, 0x48u); /* S=1 */
    b->mfp.isra = 0x80u;
    b->mfp.isrb = 0x01u;
    mfp_reg_write8(b, 0xE88017u, 0x40u); /* S=1→0 */
    assert(b->mfp.isra == 0u);
    assert(b->mfp.isrb == 0u);

    /* --- B. ISR 書き込み意味論（ISR） --- */

    /* T4: S=1でch12(USART RX)をIACK → ISRA bit4セット、IPRA bit4クリア、pending解除 */
    mfp_reg_write8(b, 0xE88017u, 0x48u); /* S=1 */
    b->mfp.isra = 0u;
    b->mfp.isrb = 0u;
    b->mfp.ipra |= 0x10u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 12u));
    assert((b->mfp.isra & 0x10u) != 0u);
    assert((b->mfp.ipra & 0x10u) == 0u);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_USART_RX));

    /* T5: AND意味論。$FFを書いても不変、"0"にしたビットだけクリアされる */
    b->mfp.isra = 0x50u;
    mfp_reg_write8(b, 0xE8800Fu, 0xFFu);
    assert(b->mfp.isra == 0x50u);
    mfp_reg_write8(b, 0xE8800Fu, 0xEFu);
    assert(b->mfp.isra == 0x40u);
    mfp_reg_write8(b, 0xE8800Fu, 0x00u);
    assert(b->mfp.isra == 0x00u);

    /* T6: bclr.b #4,($00E8800F) のRMWシミュレーション。bit4のみ落ち他ビットは保たれる */
    b->mfp.isra = 0xFFu;
    {
        uint8_t v = mfp_reg_read8(b, 0xE8800Fu);
        v &= (uint8_t)~0x10u;
        mfp_reg_write8(b, 0xE8800Fu, v);
    }
    assert(b->mfp.isra == 0xEFu);

    /* T7: ソフトウェアからISRをセットすることはできない */
    b->mfp.isra = 0u;
    mfp_reg_write8(b, 0xE8800Fu, 0xFFu);
    assert(b->mfp.isra == 0u);
    b->mfp.isrb = 0u;
    mfp_reg_write8(b, 0xE88011u, 0xFFu);
    assert(b->mfp.isrb == 0u);

    /* T8: S=0ではIACKがISRを一切セットしない（実装済み8チャンネル全部で確認） */
    mfp_reg_write8(b, 0xE88017u, 0x40u); /* S=0 */
    b->mfp.isra = 0u;
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_HSYNC);
    mfp_ch_pending_set(&b->mfp, MFP_CH_RASTER);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_D);
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    for (int i = 0; i < 8; i++)
        hooks.iack(b, 6);
    assert(b->mfp.isra == 0u);
    assert(b->mfp.isrb == 0u);

    /* --- C. ゲーティング --- */

    /* T9: S=1、ISRA bit4(ch12)セット中はTimer C(ch5、下位)がブロックされる。
     * ラスタ一致(ch14、上位)は通り、通った結果ISRA bit6がセットされる。 */
    mfp_reg_write8(b, 0xE88017u, 0x48u); /* S=1 */
    b->mfp.isra = 0x10u;                 /* ch12 in-service */
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    assert(x68k_bus_mfp_pending_irq(b) == 0);
    assert(hooks.iack(b, 6) == 0);
    mfp_ch_pending_set(&b->mfp, MFP_CH_RASTER);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 14u));
    assert((b->mfp.isra & 0x40u) != 0u); /* ch14 = ISRA bit6 */

    /* T10: 同一チャンネルはISR[n]=1の間、再要求できない（L1966） */
    b->mfp.isra = 0x10u; /* ch12のみ in-service */
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    assert(hooks.iack(b, 6) == 0);
    mfp_ch_pending_clear(
        &b->mfp, MFP_CH_USART_RX); /* ブロックされ消費されないまま残留するため後片付け */

    /* T11: EOIで解放。ch12のみin-service中はTimer C(ch5)がブロックされ、
     * ISRAをEOI（$EF書き込みでbit4クリア）すると取れるようになる。 */
    b->mfp.isra = 0x10u;
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    assert(hooks.iack(b, 6) == 0);
    mfp_reg_write8(b, 0xE8800Fu, 0xEFu); /* bit4のみクリア */
    assert(b->mfp.isra == 0u);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 5u));

    /* T12: ネスト。ch5 ack -> ch13 ack可 -> ch4は両方にブロックされる ->
     * ch13 EOI -> ch4はまだch5にブロック -> ch5 EOI -> ch4が通る */
    b->mfp.isra = 0u;
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 5u)); /* ISRB bit5 セット */
    assert((b->mfp.isrb & 0x20u) != 0u);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 13u)); /* ISRA bit5 セット */
    assert((b->mfp.isra & 0x20u) != 0u);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_D);
    assert(hooks.iack(b, 6) == 0);                         /* ch4はch13にブロックされる */
    mfp_reg_write8(b, 0xE8800Fu, (uint8_t)~0x20u);         /* ch13 EOI（ISRA bit5クリア） */
    assert(hooks.iack(b, 6) == 0);                         /* ch4はまだch5にブロック */
    mfp_reg_write8(b, 0xE88011u, (uint8_t)~0x20u);         /* ch5 EOI（ISRB bit5クリア） */
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 4u)); /* ch4が通る */

    /* T13: 後方互換はmfp_fmirq_test.c（別ファイル、CTestで独立実行）が
     * S=0のまま既存シーケンスA〜Gを回帰確認する。ここではmfp_isr_top()が
     * S=0で常に-1（全許可）を返すことだけ直接確認する。 */
    b->mfp.vr_s = 0u;
    assert(mfp_isr_top(b) == -1);
    b->mfp.vr_s = 1u;

    /* T14: S=0に戻したら残留ISRで止まらない */
    b->mfp.isra = 0u;
    b->mfp.isrb = 0u;
    mfp_ch_pending_set(&b->mfp, MFP_CH_HSYNC);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 15u));
    assert((b->mfp.isra & 0x80u) != 0u);
    mfp_reg_write8(b, 0xE88017u, 0x40u); /* S=1→0 */
    assert(b->mfp.isra == 0u);
    mfp_ch_pending_set(&b->mfp, MFP_CH_RASTER);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_D);
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    for (int i = 0; i < 7; i++)
        assert(hooks.iack(b, 6) != 0); /* 全チャンネルが通る */

    /* T15: -mfp-soft-eoi off。Sビット書き込みを無視して常に自動EOI */
    b->mfp.soft_eoi_enabled = 0;
    mfp_reg_write8(b, 0xE88017u, 0x48u);
    assert(b->mfp.vr_s == 0u);
    assert(mfp_reg_read8(b, 0xE88017u) == 0x40u);
    assert(mfp_isr_top(b) == -1);
    b->mfp.soft_eoi_enabled = 1;

    /* T16: 未実装チャンネル(ch7)はゲスト書き込みからは立てられないが、
     * （ヘルパで無理やり）立っていればch6以下は正しくブロックされる */
    mfp_reg_write8(b, 0xE88017u, 0x48u); /* S=1 */
    b->mfp.isrb = 0u;
    mfp_reg_write8(b, 0xE88011u, 0xFFu);
    assert(b->mfp.isrb == 0u); /* ゲストからはセットできない */
    mfp_iack_accept(b, 7);     /* IACK経由なら立つ */
    assert((b->mfp.isrb & 0x80u) != 0u);
    mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4); /* ch6 */
    assert(hooks.iack(b, 6) == 0);

    /* --- D. IPR 書き込み --- */

    /* T17: IPRA/IPRBもISR同様AND意味論 */
    b->mfp.ipra = 0x18u;
    mfp_reg_write8(b, 0xE8800Bu, 0xFFu);
    assert(b->mfp.ipra == 0x18u);
    mfp_reg_write8(b, 0xE8800Bu, 0xEFu);
    assert(b->mfp.ipra == 0x08u);

    x68k_bus_destroy(b);
    printf("mfp_isr_eoi_test: OK\n");
    return 0;
}
