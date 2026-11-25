#ifndef X68K_ADPCM_H
#define X68K_ADPCM_H

#include "peripheral/adpcm_codec.h"
#include <stdint.h>

/*
 * MSM6258 ADPCMレジスタ本体 + 依存する周辺レジスタ(PPI ポートC / OPM $1B)。
 * DMAC(ch3)からのフレームペースド転送はdmac.c、SDL音声出力への橋渡しは
 * main.c/audio_mixer.c側で行う。一次資料は『Inside X68000』
 * 図32-38（3-2節、正誤表反映済み）。
 *
 * レジスタ配置（奇数アドレスのみ有効、MFP/SCCと同様のD7-D0下位バイトレーン）:
 *   $E92001 コマンド/ステータス、$E92003 データ（$E92xxx = MSM6258本体）
 *   $E9A005 PPIポートC（ADPCMはbit3-0、bit7-4はジョイスティック用で対象外）
 *   $E9A007 PPIコントロールワード（bit7=0のときポートCビットセット/リセット）
 *   $E90001 OPMレジスタ選択、$E90003 OPMデータ（レジスタ$1Bのみ追跡、
 *   bit7=CT1=ADPCM基本クロック切替、bit6=CT2=FDC READY強制。他レジスタの
 *   FM音源合成はopm.c/audio/ym2151.cが担当し、ここでは追跡しない）
 */

typedef enum {
    ADPCM_MODE_IDLE = 0,
    ADPCM_MODE_PLAY,
    ADPCM_MODE_REC,
} adpcm_mode_t;

/* $E92003 write(再生方向)がデコードした出力サンプルを溜める循環バッファの
 * 容量。フレームペースドDMA(dmac.c)は1フレームあたり最大 約260サンプル
 * (15.6kHz時)しか生成しないため、十分な余裕を持たせる。 */
#define ADPCM_AUDIO_RING_SIZE 1024

/* $E92003 read(録音方向)がencode()に渡す入力サンプルを溜める循環バッファの
 * 容量。main.cが毎フレームダウンサンプル済みマイクサンプルをpushする。
 * 出力側と同じ理由で十分な余裕を持たせる。 */
#define ADPCM_REC_RING_SIZE 1024

typedef struct {
    adpcm_mode_t mode;
    adpcm_codec_t codec;

    /*
     * 直近デコードした2サンプル。単体テスト用の素朴な保持先（$E92003 write が
     * 正しく状態を進めることを直接確認するため）。
     */
    int16_t last_pcm[2];

    /* PPI(8255) ポートC shadow register（$E9A005）。8255は出力ポートの
     * 読み出しで書き込み値がそのまま返る特性を持つため、フルバイトで保持する。 */
    uint8_t ppi_c;

    /* OPM: $E90001でレジスタ番号を選択後、$E90003へ書き込む。$1Bのみ追跡する。 */
    uint8_t opm_reg_sel;
    uint8_t opm_reg1b;

    /* $E92003 write(再生方向)がデコードした各サンプルを積む出力リング。
     * main.cが毎フレームadpcm_take_samples()で吸い出し
     * audio_mixer.c経由でPAL層(pal_audio_queue)へ渡す。head==tailで空。
     * 満杯時のpushは黙って破棄する（他FIFOと同じ方針、mfp.c参照）。 */
    int16_t audio_ring[ADPCM_AUDIO_RING_SIZE];
    int audio_head;
    int audio_tail;

    /* $E92003 read(録音方向)がencode()に渡す入力サンプルのリング。
     * main.cが毎フレームpal_audio_capture_dequeue()で取得した
     * マイクサンプルをadpcm_get_sample_rate_hz()相当へダウンサンプルして
     * push する。空の場合（マイク未接続・TCC権限拒否・単に無音区間）は
     * pop側が0を返し、常に無音をencodeする動作に自然にフォールバックする。 */
    int16_t rec_ring[ADPCM_REC_RING_SIZE];
    int rec_head;
    int rec_tail;
} adpcm_t;

void adpcm_reset(adpcm_t *a);

/* $E92001/$E92003（offはPCM_BASEからのオフセット） */
uint8_t adpcm_pcm_read8(adpcm_t *a, uint32_t off);
void adpcm_pcm_write8(adpcm_t *a, uint32_t off, uint8_t val);

/* $E9A005/$E9A007（offはPPI_BASEからのオフセット） */
uint8_t adpcm_ppi_read8(adpcm_t *a, uint32_t off);
void adpcm_ppi_write8(adpcm_t *a, uint32_t off, uint8_t val);

/* $E90001/$E90003（offはOPM_BASEからのオフセット） */
void adpcm_opm_write8(adpcm_t *a, uint32_t off, uint8_t val);

/*
 * PPI Sampling Rate(bit3-2)・OPM CT1(基本クロック)から実効サンプリング
 * 周波数(Hz)を算出する（フレームペースドDMA・リサンプル処理で使用）。
 */
uint32_t adpcm_get_sample_rate_hz(const adpcm_t *a);

/*
 * PPIポートC bit1-0（パンポット制御、アクティブロー）からL/Rチャンネル出力の
 * 有効/無効を返す（『Inside X68000』 3-2-5節/図36:
 * bit0=左bit1=右、'1'=OFF・'0'=ON）。
 */
void adpcm_get_pan(const adpcm_t *a, int *left_on, int *right_on);

/*
 * OPM $1B bit6=CT2（FDCのREADY端子強制）の状態を返す
 * （『Inside X68000』 図10）。fdc.cのRECALIBRATEがドライブ物理存在
 * プローブモード判定に使う。1=強制レディ状態。
 */
int adpcm_get_ct2(const adpcm_t *a);

/* mode == ADPCM_MODE_REC（$E92001への「REC ST」書き込みで設定、ゲスト側の
 * 録音開始意思表示）かどうかを返す。ホスト側マイクデバイスの起動/停止判定に使う。 */
int adpcm_is_recording(const adpcm_t *a);

/*
 * 出力リングから最大max_count個のサンプルをoutへ取り出す。
 * 戻り値は実際に取り出した個数。main.cが毎フレーム呼び、audio_mixer.c経由で
 * PAL層(pal_audio_queue)へ渡す。
 */
int adpcm_take_samples(adpcm_t *a, int16_t *out, int max_count);

/*
 * 録音入力リングへダウンサンプル済みマイクサンプルをpushする。
 * main.cが毎フレーム呼ぶ。満杯時のpushは黙って破棄する。
 */
void adpcm_rec_push_samples(adpcm_t *a, const int16_t *in, int count);

#endif /* X68K_ADPCM_H */
