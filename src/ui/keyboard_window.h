#pragma once
/*
 * keyboard_window.h — ソフトウェアキーボードウィンドウ。main.c のフレームループから毎フレーム
 * update_keyboard_window() を呼ぶポーリング方式。
 */
#include "bus/x68k_bus.h"
#include "platform/platform.h"

/* フル配列(XEiJ同等)と、ホストキーボードにないキーだけの小型パレットを切り替える。
 * ウィンドウ右上のボタンで
 * トグルする（切り替え時はウィンドウをdestroy→サイズ違いで再createする、
 * 補助ウィンドウにリサイズAPIが無いため）。 */
typedef enum { KB_MODE_FULL = 0, KB_MODE_PALETTE = 1 } kb_win_mode_t;

/* mode に応じたウィンドウ生成サイズを返す（kb_window_create()内部、および
 * モード切り替えボタンのレイアウト計算用）。 */
void kb_win_dims(kb_win_mode_t mode, int *out_w, int *out_h);

/* ソフトウェアキーボードウィンドウを現在のモードのサイズで生成する。
 * pal_aux_window_create()に加えてpal_aux_window_set_key_forward()を有効化
 * する（このウィンドウがフォーカスされている間、物理キーボード入力を
 * ゲストへ転送するため）。初回生成(main.c)・モード切り替え
 * 時の再生成(update_keyboard_window()内部)の両方から呼ぶ。失敗時はNULL。 */
pal_aux_window_t *kb_window_create(void);

/* 現在の表示モード（update_keyboard_window() 内部のモード切り替えボタンで
 * トグルされる）を返す。 */
kb_win_mode_t kb_window_current_mode(void);

/* ディスク選択ウィンドウと同じポーリング方式（毎フレーム呼ぶ、winがNULLなら何もしない）。
 * クリックされたキーは即座に押下→離上をMFP USART受信キューへ送る
 * （ホストのASCII貼り付け機能 mfp_push_key_sequence と同じ発想、キー押しっぱなし
 * のリピート入力はソフトウェアキーボードの用途上不要と判断）。トグル対象キーの
 * ON状態はkcodeをインデックスに使ったstatic配列で保持する（0x00-0x7Fの範囲、
 * フル/パレット両モードで共有するのでモード切り替えをまたいでも維持される）。 */
pal_aux_window_t *update_keyboard_window(x68k_bus_t *bus, pal_aux_window_t *win);
