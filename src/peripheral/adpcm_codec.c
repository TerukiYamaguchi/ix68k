#include "adpcm_codec.h"

/*
 * MAME `okim6258_device::index_shift[8]`（`src/devices/sound/okim6258.cpp`）。
 * 4bitコード下位3bit(magnitude)に応じたステップインデックス増減量。
 */
static const int32_t k_index_shift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/*
 * MAME `compute_tables()`が実行時に`floor(16 * (11/10)^step)`で計算する
 * ステップ値テーブル(49エントリ)を事前計算した定数。値は本ファイル実装時に
 * 同一式で1件ずつ検算済み（libm依存を避けるための定数化、値そのものは
 * MAME/古典的OKI ADPCM実装と共通の公知テーブル）。
 */
static const int32_t k_step_table[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,   45,   50,   55,   60,  66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209,  230,  253,  279,  307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};

#define ADPCM_STEP_MAX 48

/*
 * 予測信号(signal)は標準Dialogic/OKI ADPCM仕様通り12bitでクランプする
 * （Wikipedia "Dialogic ADPCM"、および実際にX68000用ADPCMデータを生成する
 * コミュニティツール`wav2adpcm`（https://github.com/tantanGH/wav2adpcm/）の
 * `decode_adpcm()`で確認済み、いずれも[-2048, 2047]）。
 *
 * MAME `okim6258_device`（X68000ドライバ側`set_outbits(OUTPUT_10BITS)`設定）は
 * この予測信号自体を10bitにクランプしているが、同ファイルのコメント
 * 「D/A precision is 10-bits but 12-bit data can be output serially」の通り
 * 12bitはチップ内部予測器の精度、10bitはD/A出力段固有の制限であり、
 * 別レイヤの話を予測器のクランプに混同した実装上のショートカットと判断した
 * （本コーデックでのみ試すと19本中19本が10bitクランプの下限に張り付き、
 * 実信号飽和ではなく単純にクランプ幅不足と分かった）。D/A出力段の10bit精度
 * 落としが必要なら、最終 PCM 化の直前に別途行う。
 */
#define ADPCM_PREDICTOR_BITS 12
#define ADPCM_SIGNAL_MAX     ((1 << (ADPCM_PREDICTOR_BITS - 1)) - 1)
#define ADPCM_SIGNAL_MIN     (-(1 << (ADPCM_PREDICTOR_BITS - 1)))

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void adpcm_codec_reset(adpcm_codec_t *c) {
    /*
     * 初期値は0（`wav2adpcm`の`last_estimate=0`と同一）。MAME
     * `okim6258_device::device_reset()`は`m_signal=-2`だが根拠コメントがなく、
     * 実データ生成に使われている`wav2adpcm`側を優先した（差は最大2/4096の
     * 初回サンプルのみで数サンプルで収束する程度の影響）。
     */
    c->signal = 0;
    c->step = 0;
}

int16_t adpcm_codec_decode(adpcm_codec_t *c, uint8_t nibble) {
    nibble &= 0x0Fu;
    int32_t magnitude = nibble & 0x07;
    int32_t stepval = k_step_table[c->step];

    int32_t diff = stepval >> 3;
    if (magnitude & 4)
        diff += stepval;
    if (magnitude & 2)
        diff += stepval >> 1;
    if (magnitude & 1)
        diff += stepval >> 2;
    if (nibble & 0x08)
        diff = -diff;

    c->signal = clamp_i32(c->signal + diff, ADPCM_SIGNAL_MIN, ADPCM_SIGNAL_MAX);
    c->step = clamp_i32(c->step + k_index_shift[magnitude], 0, ADPCM_STEP_MAX);

    return (int16_t)(c->signal << 4);
}

uint8_t adpcm_codec_encode(adpcm_codec_t *c, int16_t sample) {
    int32_t target = (int32_t)sample >> 4; /* adpcm_codec_decode()のsignal<<4の逆変換 */
    int32_t diff = target - c->signal;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble |= 0x08u;
        diff = -diff;
    }

    int32_t threshold = k_step_table[c->step];
    if (diff >= threshold) {
        nibble |= 0x04u;
        diff -= threshold;
    }
    threshold >>= 1;
    if (diff >= threshold) {
        nibble |= 0x02u;
        diff -= threshold;
    }
    threshold >>= 1;
    if (diff >= threshold) {
        nibble |= 0x01u;
    }

    /* signal/stepの更新はデコーダと同一ロジックで行い、状態を同期させる */
    adpcm_codec_decode(c, nibble);
    return nibble;
}
