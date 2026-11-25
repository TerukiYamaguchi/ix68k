#pragma once
/*
 * disk_window.h — ディスク選択ウィンドウ。main.c のフレームループから毎フレーム
 * update_disk_window() を呼ぶポーリング方式。
 */
#include "bus/x68k_bus.h"
#include "platform/platform.h"

/* 400→464: FDD行にWPトグルボタンを追加するため拡張。
 * ファイル名表示域(32文字)は変更しない。 */
#define DISK_WIN_W         464
/* SCSI ID 0-6（自ID7を除く内蔵ユニット数、peripheral/scsi.hのSCSI_NUM_UNITSと
 * 同値。main.cはbus抽象化のためperipheral/scsi.hを直接includeしない、
 * 固定ハードウェア定数として重複定義する）。 */
#define SCSI_WIN_NUM_UNITS 7

int disk_window_height(int num_drives, int num_scsi_visible);

/*
 * ix68k.cfgで接続宣言済み(present)のSCSI IDだけを表示行として集める。
 * out[]にIDを詰めて件数を返す。out[]はSCSI_WIN_NUM_UNITS件分の領域が必要。
 */
int collect_visible_scsi_ids(x68k_bus_t *bus, int *out);

/*
 * ディスク選択ウィンドウの毎フレーム更新（生成後、winがNULLでなくなって
 * から呼ぶ）。bus層のFDC状態を毎フレーム読み直して描画するポーリング方式。
 * winが閉じられたらdestroyしてNULLを返す（呼び出し側はこの戻り値で
 * 自分のポインタを更新すること）。
 *
 * ファイルダイアログの結果ポーリングはwin==NULLでも行う: ダイアログを
 * 開いたままウィンドウを閉じられた場合でも、非同期コールバック
 * （別スレッドの可能性あり）の結果を取りこぼさないため。
 */
pal_aux_window_t *update_disk_window(x68k_bus_t *bus, pal_aux_window_t *win);
