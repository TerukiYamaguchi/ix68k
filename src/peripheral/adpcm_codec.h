#ifndef X68K_ADPCM_CODEC_H
#define X68K_ADPCM_CODEC_H

#include <stdint.h>

/*
 * OKI MSM6258 4bit ADPCMコーデック。
 *
 * 標準Dialogic/OKI ADPCM仕様（Wikipedia "Dialogic ADPCM"、実際にX68000用
 * ADPCMデータを生成するコミュニティツール`wav2adpcm`
 * https://github.com/tantanGH/wav2adpcm/ の`decode_adpcm()`/`encode_adpcm()`
 * と突き合わせ済み）に基づき実装。予測信号は12bitでクランプする
 * （MAME `okim6258_device`のX68000ドライバ側設定=10bitクランプとは異なる。
 * `adpcm_codec.c`のコメント参照）。
 * ハードウェア(DMAC/レジスタ)からは独立した純ロジック
 * （MSM6258レジスタ本体`adpcm.c`から呼ばれる）。
 *
 * データレイアウト: 1バイト=2サンプル、下位4bitが時間的に先
 * （`$E92003`レジスタ仕様、『Inside X68000』図32と一致）。
 */

typedef struct {
    int32_t signal; /* 予測信号（12bitクランプ範囲、出力時に<<4） */
    int32_t step;   /* ステップインデックス(0-48) */
} adpcm_codec_t;

void adpcm_codec_reset(adpcm_codec_t *c);

/* 4bitコード(下位4bitのみ有効)をデコードし、16bitスケールのPCMサンプルを返す */
int16_t adpcm_codec_decode(adpcm_codec_t *c, uint8_t nibble);

/*
 * 16bit PCMサンプルを4bitコードへエンコードする（録音用）。
 * 内部でadpcm_codec_decode()を呼んで予測状態を更新するため、
 * デコーダ側と状態が同一ロジックで同期する。
 */
uint8_t adpcm_codec_encode(adpcm_codec_t *c, int16_t sample);

#endif /* X68K_ADPCM_CODEC_H */
