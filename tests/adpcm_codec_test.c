/*
 * ADPCM(MSM6258)コーデック単体テスト。
 *
 * disk/otoko100_sound 配下の *.PCM（実機作成の生ADPCMデータ、ヘッダなし・
 * 1バイト=2サンプル・下位nibbleが時間的に先）を全19本デコードし、
 * WAV化してX68K_TEST_OUTPUT_DIRへ出力する（聴感確認用、自動判定はしない）。
 * 併せてencode(decode(x))のラウンドトリップ誤差を確認する（ADPCMは4bit/
 * サンプルの非可逆圧縮なのでビット完全一致はしない。振幅に対して誤差が
 * 発散していないことを粗く検査する）。
 *
 * 実機/MAME `okim6258_device`との数値一致はここでは検証できない
 * （音声出力を伴わないため）。SDL2音声出力の実装以降で改めて裏取りする。
 */
#include "peripheral/adpcm_codec.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* mkdir() 相当（clang-clは暗黙の関数宣言を拒否する） */
#endif

#ifndef X68K_REPO_ROOT
#error "X68K_REPO_ROOT must be defined by CMake"
#endif
#ifndef X68K_TEST_OUTPUT_DIR
#error "X68K_TEST_OUTPUT_DIR must be defined by CMake"
#endif

/* disk/はgitignore対象（実機吸い出しのゲーム資産のため配布不可）。CIランナー等
 * 資産が存在しない環境ではCTestのSKIP扱い（終了コード77、CMakeLists.txtの
 * SKIP_RETURN_CODEプロパティと対）にして失敗させない。 */
#define X68K_TEST_SKIP_CODE 77

/* IOCS _ADPCMOUTの最高サンプリング周波数(d1 bit10-8=4)。実際の収録レートは
 * ファイルにヘッダがなく不明なため、WAV再生用の仮値として使う。 */
#define ADPCM_TEST_WAV_RATE 15600

static const char *const k_sample_files[] = {
    "BEAM.PCM",    "BOM2.PCM",     "DOOR1.PCM",   "DOWI_01.PCM", "DSEG003.PCM",
    "DSEG004.PCM", "DSEQ004.PCM",  "DSEX001.PCM", "GUWI_01.PCM", "KW_SE21.PCM",
    "KW_SE7.PCM",  "LOCK.PCM",     "MSE023.PCM",  "MSE049.PCM",  "MSE059.PCM",
    "SHOT2.PCM",   "SMALLGUN.PCM", "VIDEO_K.PCM", "ZAKOBOM.PCM",
};
#define NUM_SAMPLE_FILES (sizeof(k_sample_files) / sizeof(k_sample_files[0]))

static uint8_t *read_file(const char *path, long *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open failed: %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "read failed: %s\n", path);
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    *out_size = size;
    return buf;
}

/* 1バイト=2サンプル、下位nibbleが時間的に先（$E92003仕様） */
static void decode_stream(const uint8_t *data, long byte_count, int16_t *out) {
    adpcm_codec_t c;
    adpcm_codec_reset(&c);
    for (long i = 0; i < byte_count; i++) {
        out[i * 2 + 0] = adpcm_codec_decode(&c, (uint8_t)(data[i] & 0x0F));
        out[i * 2 + 1] = adpcm_codec_decode(&c, (uint8_t)((data[i] >> 4) & 0x0F));
    }
}

static void encode_stream(const int16_t *pcm, long sample_count, uint8_t *out_nibbles) {
    adpcm_codec_t c;
    adpcm_codec_reset(&c);
    for (long i = 0; i < sample_count; i++) {
        out_nibbles[i] = adpcm_codec_encode(&c, pcm[i]);
    }
}

static void write_wav(const char *path, const int16_t *pcm, long sample_count, int rate) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "wav open failed: %s\n", path);
        return;
    }

    uint16_t channels = 1, bits = 16, fmt_tag = 1;
    uint32_t data_bytes = (uint32_t)(sample_count * 2);
    uint32_t riff_size = 36 + data_bytes;
    uint32_t rate_u32 = (uint32_t)rate;
    uint32_t byte_rate = rate_u32 * channels * bits / 8;
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    uint32_t fmt_size = 16;

    fwrite("RIFF", 1, 4, fp);
    fwrite(&riff_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&fmt_tag, 2, 1, fp);
    fwrite(&channels, 2, 1, fp);
    fwrite(&rate_u32, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, 4, 1, fp);
    fwrite(pcm, 2, (size_t)sample_count, fp);
    fclose(fp);
}

int main(void) {
    mkdir(X68K_TEST_OUTPUT_DIR, 0755); /* 既存ディレクトリの場合のEEXISTは無視してよい */

    char first_path[1024];
    snprintf(first_path, sizeof(first_path), "%s/disk/otoko100_sound/%s", X68K_REPO_ROOT,
             k_sample_files[0]);
    struct stat st;
    if (stat(first_path, &st) != 0) {
        printf("adpcm_codec_test: SKIP (disk/otoko100_sound アセット未配置: %s)\n", first_path);
        return X68K_TEST_SKIP_CODE;
    }

    for (size_t i = 0; i < NUM_SAMPLE_FILES; i++) {
        char in_path[1024];
        snprintf(in_path, sizeof(in_path), "%s/disk/otoko100_sound/%s", X68K_REPO_ROOT,
                 k_sample_files[i]);

        long byte_count = 0;
        uint8_t *raw = read_file(in_path, &byte_count);
        assert(raw != NULL);
        assert(byte_count > 0);

        long sample_count = byte_count * 2;
        int16_t *pcm_a = (int16_t *)malloc(sizeof(int16_t) * (size_t)sample_count);
        decode_stream(raw, byte_count, pcm_a);

        /* ラウンドトリップ誤差確認: ADPCMは4bit/サンプルの非可逆圧縮なので
         * encode(A)をdecodeし直したBはAとビット完全一致しない（それが正常）。
         * 誤差が波形振幅に対して十分小さいかを確認する。 */
        uint8_t *nibbles = (uint8_t *)malloc((size_t)sample_count);
        encode_stream(pcm_a, sample_count, nibbles);

        adpcm_codec_t c;
        adpcm_codec_reset(&c);
        int16_t *pcm_b = (int16_t *)malloc(sizeof(int16_t) * (size_t)sample_count);
        for (long s = 0; s < sample_count; s++) {
            pcm_b[s] = adpcm_codec_decode(&c, nibbles[s]);
        }

        int16_t peak = 0;
        int32_t max_abs_err = 0;
        int64_t sum_abs_err = 0;
        for (long s = 0; s < sample_count; s++) {
            int16_t v = pcm_a[s] < 0 ? (int16_t)(-pcm_a[s]) : pcm_a[s];
            if (v > peak)
                peak = v;

            int32_t err = (int32_t)pcm_a[s] - (int32_t)pcm_b[s];
            if (err < 0)
                err = -err;
            if (err > max_abs_err)
                max_abs_err = err;
            sum_abs_err += err;
        }
        int32_t mean_abs_err = (int32_t)(sum_abs_err / sample_count);

        /* 波形の非自明性チェック（全ゼロ＝デコード完全破綻の検出用） */
        assert(peak > 0);
        /* 誤差が振幅に対して発散していないか（コーデック破綻の粗い検出用） */
        assert(mean_abs_err <= peak);

        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s.wav", X68K_TEST_OUTPUT_DIR,
                 k_sample_files[i]);
        write_wav(out_path, pcm_a, sample_count, ADPCM_TEST_WAV_RATE);

        printf(
            "%-14s %6ldB -> %7ld samples, peak=%5d, roundtrip mean_err=%d max_err=%d -> %s\n",
            k_sample_files[i], byte_count, sample_count, peak, mean_abs_err, max_abs_err,
            out_path);

        free(raw);
        free(pcm_a);
        free(pcm_b);
        free(nibbles);
    }

    printf("adpcm_codec_test: OK (%zu files)\n", NUM_SAMPLE_FILES);
    return 0;
}
