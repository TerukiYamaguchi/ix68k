#pragma once
#include <stdint.h>

/*
 * ADPCM(MSM6258、モノラル・可変レート)やOPM(YM2151、固定62500Hz)のサンプル列を
 * 固定出力レートへ変換する。バス/CPU/PALいずれにも依存しない純ロジック
 * （src/render/配下の各レンダラと同じ設計方針、main.cが各ソースの生成結果を渡して呼ぶ）。
 *
 * リサンプル方式は audio_resample_mode_t で切替可能（電脳音楽
 * クラシック1実地検証で最近傍のエイリアスノイズを確認、比較検証用に複数方式を
 * 選択可能にした）。ix68k.cfg の resample_mode で選ぶ。
 */

typedef enum {
    AUDIO_RESAMPLE_NEAREST = 0, /* 最近傍。最速だが高調波にエイリアスが乗る */
    AUDIO_RESAMPLE_LINEAR = 1,  /* 線形補間（XEiJ SoundSource.java 準拠） */
    AUDIO_RESAMPLE_CUBIC = 2,   /* Catmull-Rom 3次補間。前後2点ずつ計4点参照 */
} audio_resample_mode_t;

/*
 * リサンプル状態（呼び出しをまたいで持ち越す。0初期化して開始）。
 *   phase     : Q16.16固定小数点の入力側サンプル位置（次コール先頭からの
 *               相対値）。フレーム境界でリセットすると位相がずれて
 *               クリックノイズが生じるため、呼び出し元は同じ変数を
 *               使い回すこと。
 *   prev[ch]  : 直前コール最後の生サンプル（ch=0がモノラル/L、ch=1がR。
 *               モノラル関数はch=1を使わない）。CUBICが今コール先頭
 *               (idx==0)で参照するp(-1)用。LINEAR/NEARESTは未使用。
 *   have_prev : prevが有効か（ストリーム先頭ではまだ無いため0、以後1）。
 *
 * 単体テスト等、コールごとに独立して丸めたい場合はゼロ初期化した変数を渡す。
 */
typedef struct {
    uint64_t phase;
    int32_t prev[2];
    int have_prev;
} audio_resample_state_t;

/*
 * in/in_count        : 入力モノラルサンプル列とその要素数
 * native_rate_hz     : 入力の実効サンプリング周波数(x68k_bus_adpcm_take_samples()で取得)
 * out_rate_hz        : 出力先固定レート（例: 44100）
 * left_on/right_on   : パンポット制御（adpcm_get_pan()の論理値、bit0=左bit1=右を
 *                      アクティブローから変換済み）
 * out                : 出力先(interleaved L,R,L,R,...)。呼び出し側が確保する
 * out_capacity_frames: outが受け入れ可能な最大フレーム数（1フレーム=L+R）
 * mode               : リサンプル方式
 * state              : リサンプル状態（呼び出しをまたいで持ち越す。0初期化して開始）
 *
 * 戻り値: 生成したフレーム数（out_capacity_framesで頭打ち）
 */
int audio_mix_resample_pan(const int16_t *in, int in_count, uint32_t native_rate_hz,
                           uint32_t out_rate_hz, int left_on, int right_on, int16_t *out,
                           int out_capacity_frames, audio_resample_mode_t mode,
                           audio_resample_state_t *state);

/*
 * モノラルサンプル列をあるレートから別のレートへ変換する。
 * audio_mix_resample_pan()と同じアルゴリズムで、アップ/ダウンサンプル
 * いずれの方向にも対応する（パン合成は行わない）。
 *
 * in/in_count : 入力モノラルサンプル列とその要素数
 * in_rate_hz  : 入力レート
 * out_rate_hz : 出力レート
 * out         : 出力先（モノラル）。呼び出し側が確保
 * out_capacity: outが受け入れ可能な最大サンプル数
 * mode/state  : audio_mix_resample_pan()参照
 *
 * 戻り値: 生成したサンプル数（out_capacityで頭打ち）
 */
int audio_resample_mono(const int16_t *in, int in_count, uint32_t in_rate_hz,
                        uint32_t out_rate_hz, int16_t *out, int out_capacity,
                        audio_resample_mode_t mode, audio_resample_state_t *state);

/*
 * OPM(YM2151)出力のミキシング用 int32 版。中間は int32 で保持しクリップは
 * 最終段(audio_mix_sum_clip_stereo)の1箇所のみで行う（中間クリップは
 * 音の歪みを誘発するため避ける）。
 */

/*
 * ステレオint32(@in_rate_hz)を別レートへリサンプルする（加算・パン・クリップ
 * は行わない）。OPMの62500Hz固定出力を出力レート(44100Hz)へ変換する用途。
 *
 * in/in_frames  : 入力ステレオフレーム列(interleaved L,R)とそのフレーム数
 *                 （位相の停止境界。このフレーム分として生成する出力数を
 *                 決める）。
 * in_valid_frames: in[]バッファ中、実際に読み出して安全なフレーム数
 *                 （in_frames以上）。LINEAR/CUBICが位相停止境界を跨いで
 *                 先読み(p1/p2)する際、in_framesまでしか無いと次コールの
 *                 データが来るまで末尾サンプルを複製する近似が入り、
 *                 フレーム境界(1/60秒周期)で波形が破壊される
 *                 （実地検証で判明）。呼び出し側が次フレーム分の
 *                 先読みサンプルを既にバッファへ生成済みなら、その分を
 *                 含めた長さを渡すことでこの近似を回避できる。
 *                 先読みを用意できない場合はin_framesと同じ値を渡す
 *                 （旧来通りの末尾複製近似にフォールバックする）。
 * 他の引数はaudio_mix_resample_pan()と同様。
 * 戻り値: 生成したフレーム数（out_capacity_framesで頭打ち）
 */
int audio_resample_stereo_i32(const int32_t *in, int in_frames, int in_valid_frames,
                              uint32_t in_rate_hz, uint32_t out_rate_hz, int32_t *out,
                              int out_capacity_frames, audio_resample_mode_t mode,
                              audio_resample_state_t *state);

/*
 * audio_mix_resample_pan()のint32出力版。ADPCM(モノラルint16)を出力レートへ
 * リサンプルしパンを適用してint32ステレオへ書く（クリップは行わない）。
 *
 * in_valid_count: audio_resample_stereo_i32()のin_valid_framesと同義
 * （in[]バッファ中、実際に読み出して安全な要素数。in_count以上）。
 * 先読みを用意できない場合はin_countと同じ値を渡す。
 */
int audio_mix_resample_pan_i32(const int16_t *in, int in_count, int in_valid_count,
                               uint32_t native_rate_hz, uint32_t out_rate_hz, int left_on,
                               int right_on, int32_t *out, int out_capacity_frames,
                               audio_resample_mode_t mode, audio_resample_state_t *state);

/*
 * 2つのint32ステレオ源(frames分)を加算し、16bit範囲にクリップしてint16
 * ステレオへ書く。加算・クリップを行う唯一の箇所。
 *
 * src_a/src_b: 加算する2つのステレオ源(interleaved L,R)。frames分の長さがあること
 * out        : 出力先(interleaved L,R)。呼び出し側がframes*2要素分確保する
 * 戻り値     : frames（呼び出し元の頭打ち計算を簡潔にするため、そのまま返す）
 */
int audio_mix_sum_clip_stereo(const int32_t *src_a, const int32_t *src_b, int frames,
                              int16_t *out);

/*
 * ADPCM出力のDCブロック(ハイパス)フィルタ状態。MSM6258
 * コーデックのpredictorはリーク項を持たない純粋な積分器のため、特定の
 * ADPCMサンプルの組み合わせでDCオフセットが蓄積し、ノート境界のステップ
 * 変化として可聴レベルのクリックノイズになる。実機アナログ出力段のAC
 * 結合を模した1次ハイパスフィルタで除去する。L/R独立、0初期化して開始。
 */
typedef struct {
    int32_t x1[2];     /* 直前入力(ch0=L, ch1=R、整数フルスケール) */
    int64_t y1_q16[2]; /* 直前出力(Q16固定小数点で保持。整数へ丸めるのは出力時のみ
                        * とし、フィードバック側の丸め誤差累積によるDC残留
                        * (quantization limit cycle)を防ぐ） */
} audio_dc_block_state_t;

/*
 * y[n] = x[n] - x[n-1] + R*y[n-1] の1次ハイパスフィルタをステレオint32
 * バッファへin-placeで適用する。R≈0.999、遮断周波数fc≈7Hz
 * @44100Hz（1-R = 2π・fc/fs相当の近似）。ADPCM出力にのみ適用し、OPMは
 * DCフリーなため対象外（audio_mixer.h冒頭コメント・main.c参照）。
 *
 * buf   : interleaved L,R。in-placeで上書きされる
 * frames: フレーム数
 * state : フィルタ状態(呼び出しをまたいで持ち越す。0初期化して開始)
 */
void audio_dc_block_stereo(int32_t *buf, int frames, audio_dc_block_state_t *state);
