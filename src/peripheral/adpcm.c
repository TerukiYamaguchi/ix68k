#include "peripheral/adpcm.h"

#define PCM_OFF_STATUS_CMD 0x01u /* $E92001 */
#define PCM_OFF_DATA       0x03u /* $E92003 */

#define PPI_OFF_PORTC 0x05u /* $E9A005 */
#define PPI_OFF_CTRL  0x07u /* $E9A007 */

#define OPM_OFF_REG_SEL 0x01u /* $E90001 */
#define OPM_OFF_DATA    0x03u /* $E90003 */
#define OPM_REG_CT1     0x1Bu

void adpcm_reset(adpcm_t *a) {
    a->mode = ADPCM_MODE_IDLE;
    adpcm_codec_reset(&a->codec);
    a->last_pcm[0] = 0;
    a->last_pcm[1] = 0;
    a->ppi_c = 0;
    a->opm_reg_sel = 0;
    a->opm_reg1b = 0;
    a->audio_head = 0;
    a->audio_tail = 0;
    a->rec_head = 0;
    a->rec_tail = 0;
}

/* 出力リングへ1サンプルpush（満杯時は黙って破棄）。 */
static void adpcm_audio_ring_push(adpcm_t *a, int16_t sample) {
    int next = (a->audio_head + 1) % ADPCM_AUDIO_RING_SIZE;
    if (next == a->audio_tail)
        return;
    a->audio_ring[a->audio_head] = sample;
    a->audio_head = next;
}

/* 録音入力リングから1サンプルpop（空なら0=無音）。 */
static int16_t adpcm_rec_ring_pop(adpcm_t *a) {
    if (a->rec_tail == a->rec_head)
        return 0;
    int16_t v = a->rec_ring[a->rec_tail];
    a->rec_tail = (a->rec_tail + 1) % ADPCM_REC_RING_SIZE;
    return v;
}

uint8_t adpcm_pcm_read8(adpcm_t *a, uint32_t off) {
    if (off == PCM_OFF_STATUS_CMD) {
        /*
         * 『Inside X68000』 図32/33: bit6='1'固定、bit7=PLAY/REC
         * （0=再生中、1=録音中またはスタンバイ(アイドル)中）。bit5-0='0'固定。
         */
        uint8_t status = 0x40u;
        if (a->mode != ADPCM_MODE_PLAY)
            status |= 0x80u;
        return status;
    }
    if (off == PCM_OFF_DATA) {
        /*
         * 録音方向。録音入力リングからマイクサンプルをpopしてencode()に
         * 与える。リングが空（マイク未接続・TCC権限拒否・単に無音区間）なら
         * pop側が0を返すため、常に無音をencodeする動作に自然にフォール
         * バックする。下位nibbleが先のサンプル。
         */
        int16_t s0 = adpcm_rec_ring_pop(a);
        int16_t s1 = adpcm_rec_ring_pop(a);
        uint8_t lo = adpcm_codec_encode(&a->codec, s0);
        uint8_t hi = adpcm_codec_encode(&a->codec, s1);
        return (uint8_t)(lo | (hi << 4));
    }
    return 0x00u;
}

void adpcm_pcm_write8(adpcm_t *a, uint32_t off, uint8_t val) {
    if (off == PCM_OFF_STATUS_CMD) {
        /*
         * 『Inside X68000』 図34: bit2=REC ST・bit1=PLAY ST・bit0=SP(停止)。
         * 複数ビット同時設定は想定外だが、安全側としてSTOP>PLAY>RECの優先順で扱う。
         *
         * PLAY STでは予測器(codec)状態をリセットしない: XEiJ
         * MemoryMappedDevice.java MMD_PCM mmdWb（動作開始分岐、pcmActive=trueに
         * するのみ）で確認済み。ROM側_ADPCMOUTは$ff00バイトごとのチャンク転送
         * のたびにPLAY STを再送信するため、ここでリセットすると数チャンク跨ぎの
         * 長時間再生でチャンク境界ごとに予測器が0に飛び、その直後だけ音量が
         * 小さく聞こえる（動作検証で確認済み）。
         */
        if (val & 0x01u) {
            a->mode = ADPCM_MODE_IDLE;
        } else if (val & 0x02u) {
            a->mode = ADPCM_MODE_PLAY;
        } else if (val & 0x04u) {
            a->mode = ADPCM_MODE_REC;
            adpcm_codec_reset(&a->codec);
        }
        return;
    }
    if (off == PCM_OFF_DATA) {
        /* 再生方向。下位nibbleが先のサンプル（$E92003仕様）。デコードした各
         * サンプルは出力リングへも積む（main.cが毎フレーム吸い出し
         * PAL層の音声出力へ渡す）。
         *
         * ゼロデータ補正: $00/$88は両ニブルとも量子化幅最小の
         * 「詰めコード」(magnitude=0)。無音・減衰区間でこれが連続すると
         * predictor(signal)が詰めコードの符号側へ単調に振り切れ、そのサン
         * プルの継続時間だけ大きなDCオフセット(実測: 16bitレンジの1/3以上)
         * として持続し、次サンプルとの境界でステップ状のクリックノイズに
         * なる不具合が実機BGMで確認された。predictorの符号と同方向の詰め
         * コードが来たときだけ符号ニブルを反転し0側へ引き戻す(XEiJ
         * ADPCM.javaのゼロデータ補正に倣う。量子化幅の更新は変えず符号のみ
         * 補正するため、$00/$88以外の実際の音声トランジェントには影響
         * しない)。判定はバイト先頭(このバイトのデコード開始前)のsignalの
         * 符号を1回だけ用い、両ニブルへ同じ補正を適用する。 */
        uint8_t lo = (uint8_t)(val & 0x0Fu);
        uint8_t hi = (uint8_t)((val >> 4) & 0x0Fu);
        if (val == 0x00u && a->codec.signal >= 0) {
            lo = hi = 0x08u;
        } else if (val == 0x88u && a->codec.signal < 0) {
            lo = hi = 0x00u;
        }
        a->last_pcm[0] = adpcm_codec_decode(&a->codec, lo);
        a->last_pcm[1] = adpcm_codec_decode(&a->codec, hi);
        adpcm_audio_ring_push(a, a->last_pcm[0]);
        adpcm_audio_ring_push(a, a->last_pcm[1]);
        return;
    }
}

uint8_t adpcm_ppi_read8(adpcm_t *a, uint32_t off) {
    /* 8255は出力ポートの読み出しで直前の書き込み値がそのまま返る
     * （『Inside X68000』 3-2-4節）。$E9A007は書き込み専用のため未対応。 */
    if (off == PPI_OFF_PORTC)
        return a->ppi_c;
    return 0x00u;
}

void adpcm_ppi_write8(adpcm_t *a, uint32_t off, uint8_t val) {
    if (off == PPI_OFF_PORTC) {
        a->ppi_c = val;
        return;
    }
    if (off == PPI_OFF_CTRL) {
        /*
         * 『Inside X68000』 3-2-5節/図37: bit7=0のときビットセット/
         * リセットコマンド。bit3-1=対象ビット位置、bit0=セットするデータ。
         * bit7=1のモード設定コマンドは8255全体の再設定でX68000では起動時に
         * 固定済みのため対象外（no-op）。
         */
        if ((val & 0x80u) == 0) {
            uint32_t bit_pos = (val >> 1) & 0x07u;
            if (val & 0x01u)
                a->ppi_c |= (uint8_t)(1u << bit_pos);
            else
                a->ppi_c &= (uint8_t)~(1u << bit_pos);
        }
        return;
    }
}

void adpcm_opm_write8(adpcm_t *a, uint32_t off, uint8_t val) {
    if (off == OPM_OFF_REG_SEL) {
        a->opm_reg_sel = val;
        return;
    }
    if (off == OPM_OFF_DATA) {
        /* $1B(bit7=CT1:ADPCM基本クロック、bit6=CT2:FDC READY強制)のみ追跡。
         * 他はFM音源合成(opm.c)側の担当でここでは追跡しない。 */
        if (a->opm_reg_sel == OPM_REG_CT1)
            a->opm_reg1b = val;
        return;
    }
}

uint32_t adpcm_get_sample_rate_hz(const adpcm_t *a) {
    /*
     * 『Inside X68000』 3-2-4節/図36: bit3-2=分周選択。実測では
     * 11(未使用)は01(1/768)と同じ結果になると明記されている
     * （MAME src/mame/sharp/x68k.cppのadpcm_div[]は1/512の複製として実装
     * しており本書の実測記述と食い違うため、本書側を採用した）。
     */
    static const uint32_t k_rate_divider[4] = {1024u, 768u, 512u, 768u};
    uint32_t rate_bits = (a->ppi_c >> 2) & 0x03u;

    /* 3-2-6節/図38: OPM $1B bit7=CT1、1=4MHz・0=8MHz */
    uint32_t base_clock = (a->opm_reg1b & 0x80u) ? 4000000u : 8000000u;

    return base_clock / k_rate_divider[rate_bits];
}

void adpcm_get_pan(const adpcm_t *a, int *left_on, int *right_on) {
    /* 3-2-5節/図36: bit0=左bit1=右、アクティブロー(1=OFF,0=ON)。 */
    *left_on = (a->ppi_c & 0x01u) == 0u;
    *right_on = (a->ppi_c & 0x02u) == 0u;
}

int adpcm_get_ct2(const adpcm_t *a) {
    return (a->opm_reg1b & 0x40u) ? 1 : 0;
}

int adpcm_is_recording(const adpcm_t *a) {
    return a->mode == ADPCM_MODE_REC;
}

int adpcm_take_samples(adpcm_t *a, int16_t *out, int max_count) {
    int n = 0;
    while (n < max_count && a->audio_tail != a->audio_head) {
        out[n++] = a->audio_ring[a->audio_tail];
        a->audio_tail = (a->audio_tail + 1) % ADPCM_AUDIO_RING_SIZE;
    }
    return n;
}

void adpcm_rec_push_samples(adpcm_t *a, const int16_t *in, int count) {
    for (int i = 0; i < count; i++) {
        int next = (a->rec_head + 1) % ADPCM_REC_RING_SIZE;
        if (next == a->rec_tail)
            return; /* 満杯: 以降は黙って破棄 */
        a->rec_ring[a->rec_head] = in[i];
        a->rec_head = next;
    }
}
