#include "ui/disk_window.h"
#include <stdio.h>
#include <string.h>

#define DISK_WIN_HEADER_H 8 /* ヘッダーテキストなし、上端マージンのみ */
#define DISK_WIN_ROW_H    32

int disk_window_height(int num_drives, int num_scsi_visible) {
    return DISK_WIN_HEADER_H + (num_drives + num_scsi_visible) * DISK_WIN_ROW_H + 8;
}

int collect_visible_scsi_ids(x68k_bus_t *bus, int *out) {
    int n = 0;
    for (int id = 0; id < SCSI_WIN_NUM_UNITS; id++) {
        if (x68k_bus_scsi_unit_present(bus, id))
            out[n++] = id;
    }
    return n;
}

/* パスからファイル名部分だけを取り出す（表示用、パス自体は変更しない）。 */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ボタン矩形ヘルパ。クリック判定側と描画側が別々にリテラルを
 * 持っていた重複を解消する前準備。WPボタンはFDD行とSCSI MO行に存在する
 * （SCSI側は後から追加）。 */
static pal_rect_t disk_row_wp_rect(int y0) {
    /* "WP"/"RW" 2文字分(8px等幅×2=16px)+左右4pxパディング。SELECT/EJECTボタン
     * (x=400)側へ寄せてある。 */
    pal_rect_t r = {368, y0 + 4, 24, 20};
    return r;
}
static pal_rect_t disk_row_action_rect(int y0) {
    pal_rect_t r = {400, y0 + 4, 56, 20};
    return r;
}

pal_aux_window_t *update_disk_window(x68k_bus_t *bus, pal_aux_window_t *win) {
    static int pending_select_drive = -1;   /* FDD、-1=なし */
    static int pending_select_scsi_id = -1; /* SCSI、-1=なし */

    char path[1024];
    int dr = pal_aux_window_take_file_dialog_result(path, (int)sizeof(path));
    if (dr == 1 && pending_select_drive >= 0) {
        /* SELECT 時に別のディスクが入っていれば、先にイジェクトする。ビジー中/EJECT MASK中
         * はx68k_bus_eject_fdc()側のガードで黙って無視され、続く挿入は
         * 従来通りX68K_FDC_ERR_ALREADY_INSERTEDになる（安全側の挙動）。 */
        if (x68k_bus_fdc_disk_loaded(bus, pending_select_drive))
            x68k_bus_eject_fdc(bus, pending_select_drive);
        int err = x68k_bus_insert_fdc_runtime(bus, pending_select_drive, path);
        if (err == X68K_FDC_OK)
            pal_log("disk window: FD%d へ %s を挿入しました\n", pending_select_drive, path);
        else if (err == X68K_FDC_ERR_ALREADY_INSERTED)
            pal_log("disk window: FD%d は現在イジェクトできない状態のため挿入できません"
                    "（ビジー中またはイジェクト無効化中）\n",
                    pending_select_drive);
        else if (err == X68K_FDC_ERR_BAD_EXTENSION)
            pal_log("disk window: 未対応の拡張子です: %s\n", path);
        else if (err == X68K_FDC_ERR_SIZE_MISMATCH)
            pal_log("disk window: ファイルサイズが拡張子の媒体と一致しません: %s\n", path);
        else
            pal_log("disk window: FD%d への挿入に失敗しました(err=%d): %s\n",
                    pending_select_drive, err, path);
        pending_select_drive = -1;
    } else if (dr == 1 && pending_select_scsi_id >= 0) {
        /* SCSI removable もFDDと同様eject-first。ボタンが表示された時点でmedia空のはず
         * だが、ファイルダイアログの非同期完了までの間に外部（MCP等）から挿入され得るため
         * 念のため確認する。 */
        if (x68k_bus_scsi_disk_path(bus, pending_select_scsi_id))
            x68k_bus_scsi_eject(bus, pending_select_scsi_id);
        int err = x68k_bus_scsi_insert(bus, pending_select_scsi_id, path);
        if (err == 0)
            pal_log("disk window: SCSI%d へ %s を挿入しました\n", pending_select_scsi_id, path);
        else
            pal_log("disk window: SCSI%d への挿入に失敗しました: %s\n", pending_select_scsi_id,
                    path);
        pending_select_scsi_id = -1;
    } else if (dr == 2) {
        pending_select_drive = -1;
        pending_select_scsi_id = -1;
    }

    if (!win)
        return NULL;
    if (pal_aux_window_take_close_request(win)) {
        pal_aux_window_destroy(win);
        return NULL;
    }

    int num_drives = x68k_bus_fdc_num_drives(bus);
    const uint8_t *font = x68k_bus_get_cgrom_ank(bus);

    /* ix68k.cfg で接続宣言済みの SCSI ID だけを表示する。 */
    int scsi_visible_ids[SCSI_WIN_NUM_UNITS];
    int num_scsi_visible = collect_visible_scsi_ids(bus, scsi_visible_ids);

    int cx, cy;
    if (pal_aux_window_take_click(win, &cx, &cy)) {
        int row = (cy - DISK_WIN_HEADER_H) / DISK_WIN_ROW_H;
        int y0 = DISK_WIN_HEADER_H + row * DISK_WIN_ROW_H;
        /* SELECT/EJECTは状態依存の1ボタンに統合済み。
         * 空→SELECT（クリックでファイルダイアログ）、挿入中→EJECT（クリックで排出のみ、
         * 差し替えは必ずEJECT→SELECTの2クリックにする）。 */
        pal_rect_t btn = disk_row_action_rect(y0);
        if (row >= 0 && row < num_drives) {
            /* WPトグル: 未挿入行には表示しない（D9、WPはメディアの属性のため）。
             * ホストのファイルパーミッションを実際に変更する。 */
            pal_rect_t wp_btn = disk_row_wp_rect(y0);
            if (x68k_bus_fdc_disk_loaded(bus, row) && pal_rect_contains(&wp_btn, cx, cy)) {
                int cur = x68k_bus_fdc_write_protected(bus, row);
                int err = x68k_bus_fdc_set_write_protect(bus, row, !cur);
                if (err == X68K_FDC_ERR_BUSY)
                    pal_log("disk window: FD%d はアクセス中のためライトプロテクトを"
                            "変更できません\n",
                            row);
                else if (err != 0)
                    pal_log("disk window: FD%d のライトプロテクトを変更できませんでした"
                            "（ファイルの権限を確認してください）: err=%d\n",
                            row, err);
            } else if (pal_rect_contains(&btn, cx, cy)) {
                if (x68k_bus_fdc_disk_loaded(bus, row)) {
                    x68k_bus_eject_fdc(bus, row);
                } else {
                    pending_select_drive = row;
                    pal_aux_window_open_file_dialog(
                        win, "xdf;hdm;2hd;2hs;2hde;2ht;2hc;2dd9;2dd8;2hq;2hdez");
                }
            }
        } else if (row >= num_drives && row < num_drives + num_scsi_visible) {
            int scsi_id = scsi_visible_ids[row - num_drives];
            /* WPトグル: MO(removableかつCD-ROMでない)のメディア挿入中の行にのみ
             * 表示する（HDD行にはそもそもボタンが無く、CD-ROMは
             * write_protected=1固定でトグル不能のため出さない）。ホストの
             * ファイルパーミッションを実際に変更する。 */
            pal_rect_t wp_btn = disk_row_wp_rect(y0);
            int scsi_wp_eligible = x68k_bus_scsi_unit_removable(bus, scsi_id) &&
                                   !x68k_bus_scsi_unit_is_cdrom(bus, scsi_id) &&
                                   x68k_bus_scsi_disk_path(bus, scsi_id) != NULL;
            if (scsi_wp_eligible && pal_rect_contains(&wp_btn, cx, cy)) {
                int cur = x68k_bus_scsi_write_protected(bus, scsi_id);
                int err = x68k_bus_scsi_set_write_protect(bus, scsi_id, !cur);
                if (err == X68K_SCSI_ERR_BUSY)
                    pal_log("disk window: SCSI%d はアクセス中のためライトプロテクトを"
                            "変更できません\n",
                            scsi_id);
                else if (err != 0)
                    pal_log("disk window: SCSI%d のライトプロテクトを変更できませんでした"
                            "（ファイルの権限を確認してください）: err=%d\n",
                            scsi_id, err);
            } else if (x68k_bus_scsi_unit_removable(bus, scsi_id) &&
                       pal_rect_contains(&btn, cx, cy)) {
                /* 非リムーバブル(HDD相当)はボタン自体を描画しない（下の描画ループ参照）
                 * ので、クリック判定側でも同じ条件でガードする。 */
                if (x68k_bus_scsi_disk_path(bus, scsi_id)) {
                    x68k_bus_scsi_eject(bus, scsi_id);
                } else {
                    pending_select_scsi_id = scsi_id;
                    /* ユニットの現在の種別で拡張子を絞る（CD-ROMドライブにHDS、
                     * MOドライブにCCDを誤って挿さないようにする）。 */
                    const char *filter =
                        x68k_bus_scsi_unit_is_cdrom(bus, scsi_id) ? "ccd" : "hds";
                    pal_aux_window_open_file_dialog(win, filter);
                }
            }
        }
    }

    /* ボタンは押し下げ中だけハイライトする。 */
    int press_x = -1, press_y = -1;
    int is_pressed = pal_aux_window_is_pressed(win, &press_x, &press_y);

    pal_rect_t bg = {0, 0, DISK_WIN_W, disk_window_height(num_drives, num_scsi_visible)};
    pal_aux_window_fill_rect(win, &bg, 0xFF202030u);

    int selected_drive = x68k_bus_fdc_selected_drive(bus);
    int motor_on = x68k_bus_fdc_motor_on(bus);

    for (int i = 0; i < num_drives; i++) {
        int y0 = DISK_WIN_HEADER_H + i * DISK_WIN_ROW_H;
        char label[8];
        snprintf(label, sizeof(label), "FD%d:", i);
        pal_aux_window_draw_text(win, 8, y0 + 8, label, font, 0xFFFFFFFFu);

        /* アクセスランプ: $E94007直結。1=モータON→赤、
         * 0=モータOFF→緑（『Inside X68000』 図6）。 */
        pal_rect_t lamp = {56, y0 + 10, 12, 12};
        uint32_t lamp_color = (selected_drive == i && motor_on) ? 0xFFFF3030u : 0xFF30A030u;
        pal_aux_window_fill_rect(win, &lamp, lamp_color);

        const char *loaded_path = x68k_bus_fdc_disk_path(bus, i);
        pal_aux_window_draw_text(win, 76, y0 + 8,
                                 loaded_path ? basename_of(loaded_path) : "(empty)", font,
                                 0xFFCCCCCCu);

        /* SELECT/EJECT統合ボタン: 挿入中はEJECT（見た目は常に有効のまま変えない、
         * 決定事項6。ビジー中・EJECT MASK中のクリックはx68k_bus_eject_fdc()側の
         * ガードで黙って無視される）、空はSELECT。 */
        pal_rect_t btn = disk_row_action_rect(y0);
        int btn_pressed = is_pressed && pal_rect_contains(&btn, press_x, press_y);
        int loaded = loaded_path != NULL;
        uint32_t btn_color = loaded ? (btn_pressed ? 0xFFF08080u : 0xFFA04040u)
                                    : (btn_pressed ? 0xFF80A0F0u : 0xFF4060A0u);
        pal_aux_window_fill_rect(win, &btn, btn_color);
        pal_aux_window_draw_text(win, 404, y0 + 6, loaded ? "EJECT" : "SELECT", font,
                                 0xFFFFFFFFu);

        /* WPトグルボタン: 未挿入行には表示しない。 */
        if (loaded) {
            pal_rect_t wp_btn = disk_row_wp_rect(y0);
            int wp_pressed = is_pressed && pal_rect_contains(&wp_btn, press_x, press_y);
            int wp = x68k_bus_fdc_write_protected(bus, i);
            uint32_t wp_color = wp ? (wp_pressed ? 0xFFFFD060u : 0xFFC08020u)
                                   : (wp_pressed ? 0xFFA0A0A0u : 0xFF606060u);
            pal_aux_window_fill_rect(win, &wp_btn, wp_color);
            pal_aux_window_draw_text(win, 372, y0 + 6, wp ? "WP" : "RW", font, 0xFFFFFFFFu);
        }
    }

    /* SCSI HDD/MO 行を FDD 行の下に続けて表示する。 */
    int scsi_target = x68k_bus_scsi_target_id(bus);
    int scsi_busy = x68k_bus_scsi_transferring(bus);

    for (int i = 0; i < num_scsi_visible; i++) {
        int scsi_id = scsi_visible_ids[i];
        int row = num_drives + i;
        int y0 = DISK_WIN_HEADER_H + row * DISK_WIN_ROW_H;
        char label[8];
        snprintf(label, sizeof(label), "SC%d:", scsi_id);
        pal_aux_window_draw_text(win, 8, y0 + 8, label, font, 0xFFFFFFFFu);

        /* アクセスランプ: 現在選択中のSCSIターゲットIDかつ転送中なら赤
         * （FDDのモータON相当）。SCSIバスは単一共有のため、
         * 同時に赤くなり得るのは高々1台。 */
        pal_rect_t lamp = {56, y0 + 10, 12, 12};
        uint32_t lamp_color = (scsi_target == scsi_id && scsi_busy) ? 0xFFFF3030u : 0xFF30A030u;
        pal_aux_window_fill_rect(win, &lamp, lamp_color);

        const char *loaded_path = x68k_bus_scsi_disk_path(bus, scsi_id);
        /* この行は必ずpresent==1（collect_visible_scsi_idsでフィルタ済み）。
         * メディア未挿入であり得るのはリムーバブル(MO/CD-ROM等)のみ（非リムーバブル
         * =HDD相当はファイル接続必須のためpath常にnon-NULL）。種別によって
         * 空表示の文言を出し分ける（CD-ROMドライブに"(MO, no media)"は誤り）。 */
        const char *empty_label =
            x68k_bus_scsi_unit_is_cdrom(bus, scsi_id) ? "(CD, no media)" : "(MO, no media)";
        pal_aux_window_draw_text(win, 76, y0 + 8,
                                 loaded_path ? basename_of(loaded_path) : empty_label, font,
                                 0xFFCCCCCCu);

        /* 非リムーバブル(HDD相当)にはSELECT/EJECT統合ボタン自体を表示しない
         * （実機のHDDはイジェクト機構を持たず、GUIからの差し替え対象外）。 */
        if (x68k_bus_scsi_unit_removable(bus, scsi_id)) {
            pal_rect_t btn = disk_row_action_rect(y0);
            int btn_pressed = is_pressed && pal_rect_contains(&btn, press_x, press_y);
            int loaded = loaded_path != NULL;
            uint32_t btn_color = loaded ? (btn_pressed ? 0xFFF08080u : 0xFFA04040u)
                                        : (btn_pressed ? 0xFF80A0F0u : 0xFF4060A0u);
            pal_aux_window_fill_rect(win, &btn, btn_color);
            pal_aux_window_draw_text(win, 404, y0 + 6, loaded ? "EJECT" : "SELECT", font,
                                     0xFFFFFFFFu);

            /* WPトグルボタン: MO(CD-ROMでないremovable)かつメディア挿入中のみ
             * 表示する。CD-ROMはwrite_protected=1固定で
             * トグル不能のため出さない。 */
            if (loaded && !x68k_bus_scsi_unit_is_cdrom(bus, scsi_id)) {
                pal_rect_t wp_btn = disk_row_wp_rect(y0);
                int wp_pressed = is_pressed && pal_rect_contains(&wp_btn, press_x, press_y);
                int wp = x68k_bus_scsi_write_protected(bus, scsi_id);
                uint32_t wp_color = wp ? (wp_pressed ? 0xFFFFD060u : 0xFFC08020u)
                                       : (wp_pressed ? 0xFFA0A0A0u : 0xFF606060u);
                pal_aux_window_fill_rect(win, &wp_btn, wp_color);
                pal_aux_window_draw_text(win, 372, y0 + 6, wp ? "WP" : "RW", font, 0xFFFFFFFFu);
            }
        }
    }

    pal_aux_window_present(win);
    return win;
}
