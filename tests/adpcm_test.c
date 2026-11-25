/*
 * ADPCM(MSM6258)レジスタ + PPIポートC + OPM $1B の単体テスト。
 * DMAC(ch3)・SDL音声出力はまだ繋がっていない（bus/CPU非依存の直接read8/write8
 * 呼び出しのみで検証する）。
 */
#include "peripheral/adpcm.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    adpcm_t a;
    adpcm_reset(&a);

    /* リセット直後: アイドル状態、ステータス=0xC0 (bit6='1'固定 + bit7=1) */
    assert(adpcm_pcm_read8(&a, 0x01u) == 0xC0u);

    /* PLAY ST(bit1) → ステータスのbit7が落ちる（再生中=0） */
    adpcm_pcm_write8(&a, 0x01u, 0x02u);
    assert(adpcm_pcm_read8(&a, 0x01u) == 0x40u);

    /* $E92003 write がコーデック状態を1バイト進めることを直接確認
     * （リセット直後のコーデックへ同じnibble列をdecodeした期待値と突き合わせる） */
    adpcm_codec_t expect;
    adpcm_codec_reset(&expect);
    int16_t exp0 = adpcm_codec_decode(&expect, 0x03u & 0x0Fu);
    int16_t exp1 = adpcm_codec_decode(&expect, (0x03u >> 4) & 0x0Fu);
    adpcm_pcm_write8(&a, 0x03u, 0x03u);
    assert(a.last_pcm[0] == exp0);
    assert(a.last_pcm[1] == exp1);

    /* 続けて別バイトを書き込むと状態が引き継がれ、さらに進む */
    int16_t exp2 = adpcm_codec_decode(&expect, 0x0Au & 0x0Fu);
    int16_t exp3 = adpcm_codec_decode(&expect, (0x0Au >> 4) & 0x0Fu);
    adpcm_pcm_write8(&a, 0x03u, 0x0Au);
    assert(a.last_pcm[0] == exp2);
    assert(a.last_pcm[1] == exp3);

    /* STOP(bit0) → アイドルに戻りステータスのbit7が立つ */
    adpcm_pcm_write8(&a, 0x01u, 0x01u);
    assert(adpcm_pcm_read8(&a, 0x01u) == 0xC0u);

    /* REC ST(bit2) → 図32の仕様通り、bit7は録音中もスタンバイと同じ1のまま
     * （bit7は「再生中か否か」のみを示す）。 */
    adpcm_pcm_write8(&a, 0x01u, 0x04u);
    assert(adpcm_pcm_read8(&a, 0x01u) == 0xC0u);

    /* $E92003 read (録音方向) もコーデック状態を1バイト進める
     * （マイク入力が無いため無音(0)をencode()する）。 */
    adpcm_codec_t expect_rec;
    adpcm_codec_reset(&expect_rec);
    uint8_t exp_lo = adpcm_codec_encode(&expect_rec, 0);
    uint8_t exp_hi = adpcm_codec_encode(&expect_rec, 0);
    uint8_t expect_byte = (uint8_t)(exp_lo | (exp_hi << 4));
    /* REC STでコーデックはリセットされているので、上のexpect_recと同じ初期状態から始まる */
    uint8_t got = adpcm_pcm_read8(&a, 0x03u);
    assert(got == expect_byte);

    /* PPI ポートC ($E9A005): 直接書き込み→読み出しでそのまま返る */
    adpcm_ppi_write8(&a, 0x05u, 0x0Fu);
    assert(adpcm_ppi_read8(&a, 0x05u) == 0x0Fu);

    /* PPI コントロールワード ($E9A007) ビットセット/リセット:
     * bit3-1=対象ビット位置、bit0=セットする値。まずbit2をクリア。 */
    adpcm_ppi_write8(&a, 0x07u, (uint8_t)((2u << 1) | 0u)); /* bit2を0に */
    assert((adpcm_ppi_read8(&a, 0x05u) & 0x04u) == 0u);
    adpcm_ppi_write8(&a, 0x07u, (uint8_t)((2u << 1) | 1u)); /* bit2を1に */
    assert((adpcm_ppi_read8(&a, 0x05u) & 0x04u) == 0x04u);

    /* OPM $1B(CT1): レジスタ選択→データ書き込みの2段アクセス */
    adpcm_opm_write8(&a, 0x01u, 0x1Bu); /* レジスタ選択 */
    adpcm_opm_write8(&a, 0x03u, 0x80u); /* CT1=1(4MHz) */
    assert(a.opm_reg1b == 0x80u);

    /* 対象外レジスタ選択時は無視される（FM音源合成自体は対象外） */
    adpcm_opm_write8(&a, 0x01u, 0x08u);
    adpcm_opm_write8(&a, 0x03u, 0xFFu);
    assert(a.opm_reg1b == 0x80u); /* 変化しない */

    /* サンプリング周波数算出: CT1=1(4MHz)・rate=10(1/512) → 4MHz/512 */
    adpcm_ppi_write8(&a, 0x05u, 0x08u); /* bit3-2=10 */
    assert(adpcm_get_sample_rate_hz(&a) == 4000000u / 512u);

    /* rate=11(未使用)は01(1/768)と同じ結果になる(inside_x68000_1992.md実測記述) */
    adpcm_ppi_write8(&a, 0x05u, 0x0Cu); /* bit3-2=11 */
    assert(adpcm_get_sample_rate_hz(&a) == 4000000u / 768u);

    /* OPM $1B(CT2): bit6=FDC READY強制。CT1(bit7)とは独立 */
    adpcm_opm_write8(&a, 0x01u, 0x1Bu);
    adpcm_opm_write8(&a, 0x03u, 0x80u); /* CT1=1のみ、CT2=0 */
    assert(adpcm_get_ct2(&a) == 0);
    adpcm_opm_write8(&a, 0x01u, 0x1Bu);
    adpcm_opm_write8(&a, 0x03u, 0xC0u); /* CT1=1・CT2=1 同時 */
    assert(adpcm_get_ct2(&a) == 1);
    assert((a.opm_reg1b & 0x80u) == 0x80u); /* CT1は変化しない */

    printf("adpcm_test: OK\n");
    return 0;
}
