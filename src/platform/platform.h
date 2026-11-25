#pragma once
#include <stdint.h>

/*
 * PAL — Platform Abstraction Layer
 *
 * x68000 の全プラットフォーム依存処理はこのインターフェース経由で行う。
 * src/peripheral/ や src/main.c から SDL3 等を直接呼ばない。
 *
 * 実装:
 *   platform_sdl3*.c — macOS / Ubuntu / Windows（SDL3。platform_sdl3.c が本体、
 *                      input / audio / aux_window は同名接頭辞のファイルに分割）
 *   platform_null.c  — ヘッドレス（CI / ユニットテスト用）
 */

/* ウィンドウ初期化。失敗時は -1 を返す */
int pal_init(const char *title, int w, int h);
void pal_quit(void);

/* フレームバッファを画面に表示（ARGB8888、w×h ピクセル） */
void pal_present_frame(const uint32_t *argb, int w, int h);

/* イベント処理（ウィンドウメッセージのポーリング） */
void pal_poll_events(void);

/* 終了要求が来ていれば 1 */
int pal_should_quit(void);

/* 単調時刻（マイクロ秒） */
uint64_t pal_get_ticks_us(void);

/*
 * 設定ファイルの探索先（末尾に区切り文字を含む）。pal_init() より前に呼べる。
 * 取得できない・対応しない場合は NULL。
 * pal_get_exe_dir: 実行ファイルのあるディレクトリ。
 * pal_get_user_dir: OSのユーザーデータ領域（macOS: ~/Library/Application Support/ix68k/、
 *   Windows: %APPDATA%\ix68k\、Linux: $XDG_DATA_HOME/ix68k/）。無ければ作成する。
 */
const char *pal_get_exe_dir(void);
const char *pal_get_user_dir(void);
/* OSのユーザー設定領域（末尾に区切り文字を含む）。Linux は $XDG_CONFIG_HOME/ix68k/（既定
 * ~/.config/ix68k/）、それ以外は pal_get_user_dir() と同じ。無ければ作成する。 */
const char *pal_get_user_config_dir(void);

/* ユーザーデータ領域（pal_get_user_dir()）をOSのファイラで開く（Windows・Linuxのメニュー用。
 * macOSは専用実装）。成功 0、失敗・非対応 -1。失敗理由は pal_log() に出し、
 * 開く操作の失敗（xdg-open 無し等）はエラーダイアログでも通知する。 */
int pal_open_user_dir(void);

/* ディレクトリを作る（既にあれば成功）。成功 0、失敗・非対応 -1。 */
int pal_create_dir(const char *path);

/* スリープ（マイクロ秒） */
void pal_sleep_us(uint64_t us);

/* ログ出力（printf 互換） */
void pal_log(const char *fmt, ...);

/*
 * 起動前の致命的エラーを利用者へ知らせる。pal_init() より前に呼べる。
 * stderr へは常に出力する。SDL3版（Windowsを除く）は、stderr が端末でない起動
 * （.app のダブルクリック等）のときエラーダイアログも出す。
 * 実行ファイルのダブルクリック（Terminal/コンソール内で起動され stderr が端末になる）
 * は呼び出し側（main.c）がキー入力待ちで画面を残す。
 * Windows版とヘッドレス版は stderr のみ（Windowsのダイアログは未対応）。
 */
void pal_show_error(const char *title, const char *msg);

/*
 * テクスチャを native_w×native_h に、ウィンドウを display_w×display_h にリサイズする。
 * SDL は RenderCopy 時にテクスチャをウィンドウサイズにストレッチするため、
 * ピクセルアスペクト比補正はこの 2 値の差として表現する。
 */
void pal_resize(int native_w, int native_h, int display_w, int display_h);

/* CGROM ANK（半角8x16）フォントをプラットフォーム層へ伝える。Linux(SDL)版の
 * File/Machine/View/Toolsメニュー（ネイティブメニューAPIが無いため自前
 * 描画する、platform_linux_aux.c参照）が文字描画に使う。macOS/Windows
 * （ネイティブメニュー）・ヘッドレス版では無視される。ROM/CGROMロード後に
 * 一度呼べばよい（ポインタはアプリ生存中変化しない）。 */
void pal_set_menu_cgrom_ank(const uint8_t *cgrom_ank);

/* ASCII 文字 ch を MFP USART RX FIFO へ注入する（テスト・自動化用、MCP paste_text）。
 * 大文字・一部記号は SHIFT 押下→キー押下→キー離上→SHIFT 離上の4バイトシーケンスに
 * 展開する。'\n' は Enter として扱う。マッピングのない文字は無視する。 */
void pal_key_inject_ascii(uint8_t ch);

/* X68k 物理キーコードを押下→離上の2バイトシーケンスとして注入する（MCP press_key）。
 * val = (物理キーコード << 8) | ascii_or_0（下位 ascii は未使用）。 */
void pal_key_inject_raw(uint32_t val);

/* X68k 物理キーコードの押下・離上を個別に注入する（MCP key_down/key_up）。
 * val = (物理キーコード << 8) | ascii_or_0（下位 ascii は未使用、pal_key_inject_raw
 * と同形式）。press_key(=pal_key_inject_raw)は押下と離上を間を置かず送るため、実際の指の
 * 押下時間（数十〜数百ms）を模擬したいテストには使えない。呼び出し側が
 * pal_key_inject_down → 任意の待機 → pal_key_inject_up の順で呼ぶことで
 * 実キー入力に近いタイミングを再現できる。 */
void pal_key_inject_down(uint32_t val);
void pal_key_inject_up(uint32_t val);

/* MFP USART RX FIFO を空にする（PASTE 前の残留文字フラッシュ用）。 */
void pal_key_clear_queue(void);

/* ホストキーボード配列（keyboard.cfg layout キー由来）。
 * US: ';'キー(SHIFT+;='：')をJIS配列の「：＊」専用キー(0x28)へ橋渡しする。
 * JIS: 橋渡しせず、SHIFT+；は物理キーそのまま(JIS「；＋」のSHIFT面='+')。
 * デフォルトは JIS（従来動作を変えない）。 */
void pal_key_set_us_layout(int is_us);

/* ホスト側予約キーの設定。キー名は f1〜f24、修飾子は shift/ctrl/alt/gui、
 * "none" は無効。F13〜F17によるXF1〜XF5の直接入力はこの設定に関係なく有効。 */
typedef struct {
    const char *disk_window;
    const char *software_keyboard;
    const char *xf[5];
    const char *kigou;
    const char *touroku;
    const char *break_key;
} pal_hotkey_config_t;
void pal_key_set_hotkey_config(const pal_hotkey_config_t *config);

/* NMI 要求フラグ。1 を返すと同時にフラグをリセットする。
 * SDL3: PAUSE キーで立てる。platform_null: 常に 0。 */
int pal_nmi_requested(void);

/* 前面電源スイッチOFF要求フラグ（Machine > Power Switch (OFF)）。1 を返すと同時に
 * フラグをリセットする。platform_null: 常に 0。 */
int pal_power_switch_off_requested(void);

/*
 * SDL_KEYDOWN/UP・pal_key_inject_* を MFP USART RX FIFO（実プロトコルバイト経路）へ
 * 供給するためのコールバック登録。
 * byte = (物理キーコード & 0x7F) | (離上なら 0x80)。
 * platform 層は bus 型を知らないため、呼び出し側（main.c）がラッパー経由で渡す。
 * platform_null: 登録しても呼ばれない。
 */
typedef void (*pal_key_push_fn)(void *ctx, uint8_t byte);
typedef void (*pal_key_clear_fn)(void *ctx);
void pal_key_set_callbacks(pal_key_push_fn push_fn, pal_key_clear_fn clear_fn, void *ctx);

/*
 * SDL_MOUSEMOTION/BUTTONDOWN/UP を SCC ch B(マウス)へ供給するためのコールバック
 * 登録。platform 層は bus 型を知らないため、呼び出し側（main.c）が
 * ラッパー経由で渡す。x/yはホストの現在位置（ネイティブ解像度換算後の絶対座標）、
 * left/right は押下状態(0/1)。SCC側はRTS立ち上がり時にこの位置とIOCS側の現在
 * カーソル座標との差分をその場で計算する（基準点をホスト側で推定・保持しない
 * ことで、ウィンドウ境界の出入り等による累積ズレを防ぐ）。
 * platform_null: 登録しても呼ばれない。
 */
typedef void (*pal_mouse_position_fn)(void *ctx, int x, int y);
typedef void (*pal_mouse_button_fn)(void *ctx, int left, int right);
void pal_mouse_set_callbacks(pal_mouse_position_fn position_fn, pal_mouse_button_fn button_fn,
                             void *ctx);

/* MCP mouse_move/mouse_click から登録済みコールバック経由でマウスイベントを注入する。
 * dx/dyは相対移動量（MCPツールの外部インターフェースは変えない）。内部で仮想
 * ホスト座標へ加算してから位置コールバックを呼ぶ。 */
void pal_mouse_inject_motion(int dx, int dy);
void pal_mouse_inject_button(int left, int right);

/*
 * ジョイスティック#0/#1の現在状態をX68k PPIポートA/B形式の1バイトで返す
 * アクティブロー: 未接続/未押下=1、方向ON/トリガON=0。
 * bit0=上 bit1=下 bit2=左 bit3=右 bit5=トリガA bit6=トリガB
 * （『Inside X68000』 p.378 図1ブロック図の信号順 F/B/L/R/TRG-A/TRG-B
 *   をPA0-PA3/PA5/PA6に対応させたもの。bit4/bit7は常に1）。
 * 未接続時・platform_null では常に 0xFF。main.c が毎フレーム呼び、
 * x68k_bus_set_joystick() へ結果を渡す（マウスと異なりイベントでなく
 * レベルなのでコールバックでなくポーリング型）。
 */
uint8_t pal_joystick_read(int index);

/* 同時に開くホストのゲームパッド数の上限（列挙順に 1..JOYSTICK_PAD_MAX と番号を付ける）。 */
#define JOYSTICK_PAD_MAX 8

/*
 * X68000 のジョイスティック port(0=#1, 1=#2) に使うゲームパッドを、列挙番号
 * pad_no（1始まり。起動ログの「Gamepad#N connected」の N）で指定する。
 * 0=自動（port+1 番目のゲームパッドを使う）。列挙順と X68000 のポート番号が
 * 合わない環境で、使いたいパッドを #1 に割り当てるために使う。
 */
void pal_joystick_set_pad(int port, int pad_no);

/* ゲームパッドのSELECT/STARTを上下/左右の同時押しとして出すか（既定1）。
 * 方向レバー操作だけでSELECT/STARTを誤検出する不良パッド向けに無効化できる。 */
void pal_joystick_set_select_start(int enabled);

/* MCP からの仮想ジョイスティック入力。inject 中は実コントローラより優先して
 * この値を返す。release()で実コントローラ読み取り（未接続なら0xFF）に戻す。
 * valueはpal_joystick_read()と同じPPIポートA/B形式（アクティブロー）。 */
void pal_joystick_inject(int index, uint8_t value);
void pal_joystick_release(int index);

/*
 * 音声出力初期化。sample_rate_hz/channels（interleaved
 * int16、例: 44100/2）で固定フォーマットの出力デバイスを開く。失敗時
 * （SDL_INIT_AUDIO失敗等）や platform_null（ヘッドレス）では 0 を返し、
 * 以降 pal_audio_available() も 0 になる。
 */
int pal_audio_init(int sample_rate_hz, int channels);

/* 音声出力が利用可能か（pal_audio_init 成功時のみ 1） */
int pal_audio_available(void);

/*
 * 出力キューに残る未再生PCMをフレーム数で返す（1フレーム = channels 分の
 * サンプル）。デバイス未初期化時または残量を取得できないときは負値。
 * SDL のbyte単位の表現をアプリ層へ漏らさないための問い合わせであり、
 * この値を根拠にゲスト時刻を実時間より先行させてはならない。
 */
int pal_audio_queued_frames(void);

/* 出力キューの回復／破棄に関する実行中診断カウンタ。 */
typedef struct {
    uint64_t reset_count;       /* pal_audio_reset() を行った総回数 */
    uint64_t underflow_count;   /* 再生中のキュー枯渇を検出した回数 */
    uint64_t queue_limit_count; /* 滞留上限超過でキューを破棄した回数 */
} pal_audio_stats_t;

/* 診断カウンタを取得する。platform_nullでは全フィールド0を返す。 */
void pal_audio_get_stats(pal_audio_stats_t *out);

/*
 * PCMサンプル(int16, interleaved)をキューへ積む。frame_count はフレーム数
 * （1フレーム = channels 分のサンプル）。pal_audio_available()==0 のときは
 * 何もしない（呼び出し側でのガードは不要）。
 */
void pal_audio_queue(const int16_t *samples, int frame_count);

/*
 * 出力キューに積んである未再生分を破棄する。リセットのように生成側の音が
 * その場で途切れる場面で呼ぶ（破棄しないと、リセット前の音が滞留分だけ
 * 鳴り続ける）。破棄後は pal_audio_queue() 側の事前バッファリングが
 * やり直され、溜まってから再生が再開する。
 */
void pal_audio_reset(void);

/* 音声出力デバイスを閉じる。pal_quit() から呼ばれる。 */
void pal_audio_quit(void);

/*
 * 音声入力（マイク）初期化（_ADPCMINP 等の録音経路用）。
 * sample_rate_hz/channels（例: 44100/1）でキャプチャデバイスを開く。失敗時
 * （macOS TCC 権限拒否・デバイス無し・platform_null 等）は 0 を返し、以降
 * pal_audio_capture_available() も 0 になる。呼び出し側（adpcm.c 経由）は
 * この場合入力を無音として扱う（追加のフォールバック処理は不要）。
 */
int pal_audio_capture_init(int sample_rate_hz, int channels);

/* 音声入力が利用可能か（pal_audio_capture_init 成功時のみ 1） */
int pal_audio_capture_available(void);

/*
 * キャプチャ済みPCMサンプル(int16)を取り出す（ノンブロッキング）。
 * out へ最大 max_samples 個書き込み、実際に取り出せた個数を返す
 * （キューが空なら 0）。pal_audio_capture_available()==0 のときは常に 0。
 */
int pal_audio_capture_dequeue(int16_t *out, int max_samples);

/* 音声入力デバイスを閉じる。pal_quit() から呼ばれる。 */
void pal_audio_capture_quit(void);

/*
 * 音声入力デバイスの実際の録音動作を開始/一時停止する。pal_audio_capture_init()
 * 成功直後はpaused状態（デバイスは開いているがOSのマイクインジケータは点灯
 * しない）。ゲスト側が実際に録音中（x68k_bus_adpcm_is_recording()==1）の
 * 間だけ active=1 で呼び、不要な間はOSのマイク使用インジケータを点灯させ
 * ないようにする。pal_audio_capture_available()==0 なら no-op。
 */
void pal_audio_capture_set_active(int active);

/*
 * RS-232C(SCC ch A)のホスト側データ経路。環境変数
 * X68K_RS232_DEV=<path> 指定時はそのパスを直接openする（実機の物理
 * シリアルポート等への差し替え用）。未指定時は X68K_RS232_PTY=1 のとき
 * のみ posix_openpt でPTYマスタを開き、スレーブパスをpal_log()で出力
 * する。どちらも未指定/失敗/platform_null（ヘッドレス）/Windowsでは
 * 0を返し、以降pal_serial_available()も0になる。
 */
int pal_serial_init(void);

/* RS-232Cホスト経路が利用可能か（pal_serial_init成功時のみ1） */
int pal_serial_available(void);

/*
 * 受信済みバイトを取り出す（ノンブロッキング）。out へ最大 max バイト
 * 書き込み、実際に取り出せた個数を返す（データが無ければ0）。
 * pal_serial_available()==0 のときは常に0。
 */
int pal_serial_read(uint8_t *out, int max);

/*
 * バイト列をホスト側へノンブロッキングで書き込む。実際に書き込めた
 * バイト数を返す（送信不可時0）。pal_serial_available()==0 のときは
 * 常に0。
 */
int pal_serial_write(const uint8_t *buf, int n);

/* RS-232Cホスト経路を閉じる。pal_quit() から呼ばれる。 */
void pal_serial_quit(void);

/*
 * ホスト側補助ウィンドウ（ソフトウェアキーボード・
 * ディスク選択ウィンドウの共通基盤）。
 *
 * メインウィンドウとは別のSDLウィンドウ+レンダラーを持つ。イベントは
 * pal_poll_events()が内部で（開いている全補助ウィンドウ分もまとめて）
 * 処理するため、呼び出し側は毎フレームpal_poll_events()を呼ぶだけでよい
 * （専用のポール関数は不要）。クリック・クローズ要求は
 * pal_aux_window_take_click()/take_close_request()で「読んだら消費」
 * 方式により取得する。
 *
 * bus状態（CGROM等）はplatform層が知らないため、描画時に呼び出し側
 * （main.c等）から都度渡す設計にしてある。
 */
typedef struct pal_aux_window pal_aux_window_t;

/* 補助ウィンドウを生成する。失敗時はNULL。macOSではフルスクリーン中の
 * Space切り替え問題への対処（NSWindowCollectionBehaviorFullScreenAuxiliary）
 * も内部で行う（呼び出し側は意識不要）。 */
pal_aux_window_t *pal_aux_window_create(const char *title, int w, int h);
void pal_aux_window_destroy(pal_aux_window_t *win);

/* ユーザーがOSのクローズボタン（赤丸等）を押したら1（消費後0に戻る）。
 * 呼び出し側はこれを見てpal_aux_window_destroy()すること。 */
int pal_aux_window_take_close_request(pal_aux_window_t *win);

/* このフレーム中に左クリックが完了（押下→離上）していれば1を返し、離上時の
 * ウィンドウローカル座標をout_x/out_yへ書く（消費後0に戻る）。ヒットテスト
 * 自体は呼び出し側の責務（pal_rect_contains参照）。ボタンの「押している間だけ
 * ハイライト」表示にはpal_aux_window_is_pressed()を併用する。 */
int pal_aux_window_take_click(pal_aux_window_t *win, int *out_x, int *out_y);

/* 現在左ボタンが押し下げられている（まだ離されていない）なら1を返し、
 * 現在のウィンドウローカル座標をout_x/out_yへ書く（消費しない、毎フレーム
 * 読める状態値）。ボタン押下中のハイライト表示専用。実際の操作確定は
 * pal_aux_window_take_click()（離上時）で行うこと。 */
int pal_aux_window_is_pressed(pal_aux_window_t *win, int *out_x, int *out_y);

/* 単純な矩形（ボタン当たり判定用）。GUIツールキット不要のため素の構造体。 */
typedef struct {
    int x, y, w, h;
} pal_rect_t;
int pal_rect_contains(const pal_rect_t *r, int px, int py);

/* このウィンドウがフォーカスされている間、物理キーボード入力をメイン
 * ウィンドウと同じsdl_to_x68k_keycode変換でMFP USART RXへ転送するか設定する
 * （ソフトウェアキーボードウィンドウ用）。デフォルトは無効（ディスク選択
 * ウィンドウ等では物理キー入力をゲストへ送らない）。無効化すると
 * pal_aux_window_is_kcode_pressed()の押下状態も即座にクリアされる。 */
void pal_aux_window_set_key_forward(pal_aux_window_t *win, int enable);

/* kcodeに対応する物理キーが現在押し下げられているか（ハイライト表示用、
 * 消費しない状態値）。pal_aux_window_set_key_forward()を有効にしたウィンドウ
 * でのみ追跡される（無効時は常に0）。 */
int pal_aux_window_is_kcode_pressed(pal_aux_window_t *win, uint8_t kcode);

/* 矩形塗りつぶし（ボタン等）。argbは0xAARRGGBB。 */
void pal_aux_window_fill_rect(pal_aux_window_t *win, const pal_rect_t *r, uint32_t argb);

/*
 * CGROM ANK（半角）フォントで文字/文字列を描画する（8x16。idx=(ch+0x80)&0xFFが
 * 実ROMの_FNTADR計算値と一致することを確認済み）。cgrom_ankは
 * bus->iocs_rom + 0x3A000（呼び出し側が用意）。漢字（2バイトコード）は
 * 描画できない。argbは0xAARRGGBB。
 */
void pal_aux_window_draw_char(pal_aux_window_t *win, int x, int y, char ch,
                              const uint8_t *cgrom_ank, uint32_t argb);
void pal_aux_window_draw_text(pal_aux_window_t *win, int x, int y, const char *s,
                              const uint8_t *cgrom_ank, uint32_t argb);

/* このフレームの描画を確定する（フリップ）。 */
void pal_aux_window_present(pal_aux_window_t *win);

/*
 * ホスト予約キー（F11フルスクリーントグルと同じ流儀）またはメニュー操作での
 * ウィンドウ起動要求。既定ではF12=ディスク選択ウィンドウ、Shift+F12=
 * ソフトウェアキーボード（pal_key_set_hotkey_config()で変更可）。
 * RESET以降はメニュー（macOS: Cocoa、Windows: Win32、Linux: SDL自前描画）専用で、
 * SDLキーイベントではなくメニュー選択から立つ（macOSのRESETはCmd+Rの
 * keyEquivalent経由、platform_macos_aux.m参照。SCSIをディスク選択ウィンドウから
 * 挿入した後にHuman68kへ再検出させるための実用上の手段）。
 * 読んだら消費される（pal_nmi_requested()と同型）。
 */
typedef enum {
    PAL_AUX_HOTKEY_NONE = 0,
    PAL_AUX_HOTKEY_DISK_WINDOW = 1,     /* 既定F12 */
    PAL_AUX_HOTKEY_KEYBOARD_WINDOW = 2, /* 既定Shift+F12 */
    PAL_AUX_HOTKEY_RESET = 3,           /* メニュー「Machine > Reset」(macOSはCmd+R) */
    /* 4〜8は旧メニュー「CPU Clock」(廃止)の欠番。 */
    /* 実機の「OPT.1を押しながら電源投入/リセット」の再現（強制的にFD0から
     * 起動する）。 */
    PAL_AUX_HOTKEY_RESET_FD0 = 9,
    /* メニュー「View」。ネイティブ解像度が小さいモード
     * （例: 256x256の正方形ドットモード）だとウィンドウが小さくなりすぎる
     * 問題への対処として、表示倍率をユーザーが選べるようにする。
     * ZOOM_1〜ZOOM_2の連番はmain.cが添字計算に使うため、間に割り込ませると
     * 壊れる。 */
    PAL_AUX_HOTKEY_ZOOM_1 = 10,   /* 等倍 */
    PAL_AUX_HOTKEY_ZOOM_1_5 = 11, /* x1.5 */
    PAL_AUX_HOTKEY_ZOOM_2 = 12,   /* x2 */
    /* メニュー「View > Fullscreen」。F11ホットキーと
     * 同じフルスクリーントグルをメニューからも起動できるようにする。 */
    PAL_AUX_HOTKEY_FULLSCREEN_TOGGLE = 13
} pal_aux_hotkey_t;
pal_aux_hotkey_t pal_aux_window_take_hotkey(void);

/*
 * 現在のウィンドウ表示倍率(x10、10=等倍/15=x1.5/20=x2)をplatform層へ通知
 * する。メニュー「View」が選択中の項目にチェックマークを
 * 表示するための参照値。main()が起動時と倍率変更時
 * （PAL_AUX_HOTKEY_ZOOM_*処理後）に呼ぶ。
 */
void pal_aux_set_current_window_zoom_x10(int zoom_x10);

/*
 * F11キーと同じフルスクリーントグルをメニュー「View > Fullscreen」
 * (PAL_AUX_HOTKEY_FULLSCREEN_TOGGLE処理)から呼び出すための関数。
 * ヘッドレス(platform_null.c)では無視される。
 */
void pal_toggle_fullscreen(void);

/*
 * 現在フルスクリーン中かどうかを返す。メニュー「View > Fullscreen」
 * のチェックマーク表示に使う。ヘッドレス(platform_null.c)では常に0。
 */
int pal_is_fullscreen(void);

/*
 * ネイティブファイルダイアログを開く（ディスクイメージ選択）。
 * SDL3の SDL_ShowOpenFileDialog は非ブロッキングで、結果は別スレッドから
 * コールバックされる場合があるため、スレッド安全なメールボックス経由で
 * 受け渡す。呼び出し側は毎フレーム
 * pal_aux_window_take_file_dialog_result() をポーリングすること。
 *
 * win: モーダル対象の補助ウィンドウ（NULL可）。
 * filter_pattern: セミコロン区切りの拡張子（例 "xdf;hdm;2hd"）。NULLなら
 *                 フィルタなし（全ファイル）。
 */
void pal_aux_window_open_file_dialog(pal_aux_window_t *win, const char *filter_pattern);

/*
 * ファイルダイアログの結果を取得する（読んだら消費）。
 * 戻り値: 0=まだ結果なし、1=選択された（out_pathへパスを書き込み）、
 *         2=キャンセルされた/エラー。
 * out_path_cap は out_path のバッファサイズ。
 */
int pal_aux_window_take_file_dialog_result(char *out_path, int out_path_cap);
