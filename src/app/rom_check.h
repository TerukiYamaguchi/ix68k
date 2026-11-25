#pragma once

#include "app/config.h"
#include "mc68000/cpu_types.h"

#include <stddef.h>

/* IPLROM の起動前検証結果。 */
typedef enum {
    X68K_ROM_CHECK_OK,
    X68K_ROM_CHECK_UNREADABLE,      /* 開けない・読めない・ディレクトリ */
    X68K_ROM_CHECK_BAD_SIZE,        /* 128KB でも 1MB でもない */
    X68K_ROM_CHECK_BAD_VECTOR,      /* リセットベクタが ROM 領域を指さない */
    X68K_ROM_CHECK_CPU_MISMATCH,    /* 030 系 ROM に 68000 / 68010 の構成 */
    X68K_ROM_CHECK_SERIES_MISMATCH, /* ファイル名の機種系統と series が不一致 */
} x68k_rom_check_result_t;

/*
 * IPLROM を読み込む前に、サイズ・リセットベクタ・CPU・series との整合を調べる。
 * profile は x68k_config_resolve_timing 後の値（accelerator は X68030 系として扱う）。
 * X68K_TIMING_PROFILE_NONE（bus_timing = off）のときは series との照合を行わない。
 * size_out は BAD_SIZE の報告用（取得できなければ 0）。NULL 可。
 */
x68k_rom_check_result_t x68k_rom_check(const char *path, x68k_timing_profile_t profile,
                                       mc68k_model_t cpu_model, long *size_out);

/* 検証結果をダイアログ用の日本語メッセージにする。OK のときは空文字列。 */
void x68k_rom_check_format(x68k_rom_check_result_t result, const char *path, long size,
                           char *buf, size_t buf_size);

/* 128KB の IPLROM は文字フォントを含まないため CGROM が必要。1MB フルイメージは含む。 */
int x68k_cgrom_required(long rom_size);

/* CGROM の指定ミスのダイアログ用メッセージ。path が NULL
 * か空なら「未指定」、それ以外は「読み込み失敗」。 */
void x68k_cgrom_error_format(const char *path, char *buf, size_t buf_size);
