#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mc68000/bus_hooks.h"
#include "mc68000/cpu_types.h"

typedef struct x68k_bus x68k_bus_t;

typedef struct {
    uint64_t fdc;
    uint64_t scsi;
    bool fdc_overflow;
    bool scsi_overflow;
} x68k_media_generation_t;

/* CRTCのラッチ済みでない現在レジスタ値から求める1フレームの有理数秒。
 * numerator / denominator 秒。valid=0は未初期化・不正設定・未対応インターレース等で
 * 呼び出し側が60Hzへフォールバックすべきことを表す。 */
typedef struct {
    uint64_t numerator;
    uint64_t denominator;
    int valid;
} x68k_crtc_frame_timing_t;

/* ===== バスの生成・CPU接続・実行時保護 ===== */

x68k_bus_t *x68k_bus_create(void);
void x68k_bus_destroy(x68k_bus_t *bus);
x68k_media_generation_t x68k_bus_media_generation(const x68k_bus_t *bus);
bool x68k_media_generation_equal(x68k_media_generation_t a, x68k_media_generation_t b);
bool x68k_bus_set_local_memory(x68k_bus_t *bus, unsigned mb);
mc68k_bus_t x68k_bus_get_hooks(x68k_bus_t *bus);

/*
 * mc68k_create() が返す CPU ハンドルを配線する。read8/write8 の未マップ
 * アドレスフォールバックから mc68k_signal_bus_error() を呼ぶために必要
 * （x68k_bus_get_hooks() の直後、mc68k_create() の直後に呼ぶこと）。
 */
void x68k_bus_set_cpu(x68k_bus_t *bus, mc68k_state_t *cpu);

typedef enum {
    X68K_TIMING_PROFILE_X68000,
    X68K_TIMING_PROFILE_XVI,
    X68K_TIMING_PROFILE_X68030,
    X68K_TIMING_PROFILE_060TURBO,
    X68K_TIMING_PROFILE_040TURBO,
    X68K_TIMING_PROFILE_NONE,
    X68K_TIMING_PROFILE_COUNT,
} x68k_timing_profile_t;

/* バスの timing profile を設定する。x68k_bus_create() 直後は X68000 で、CPU モデルとの
 * 整合は検査しない（起動時に x68k_config_resolve_timing() が検証済みであること）。
 * 範囲外の値は拒否し、直前の profile を保つ。 */
bool x68k_bus_set_timing_profile(x68k_bus_t *bus, x68k_timing_profile_t profile);

/*
 * pull型IRQ受付コールバック本体。x68k_bus_get_hooks()の
 * 直後、mc68k_create()の直後に
 *   mc68k_set_irq_level_hook(cpu, x68k_bus_irq_level, bus);
 * で登録する。mc68k_irq_level_fn_t（int (*)(void *ctx)）と同じシグネチャ。
 */
int x68k_bus_irq_level(void *ctx);

/*
 * ユーザ/スーパーバイザ メモリ保護（『Inside X68000』 5-5節）の有効/無効を
 * 切り替える。有効時、ユーザモードから低位メモリ保護領域・GVRAM以降の領域へアクセスすると
 * バスエラーになる（実機のファンクションコード保護相当）。既定OFF（無効）。
 * X68K_SUPER_PROTECT 環境変数で main() が有効化する。
 */
void x68k_bus_set_super_protect(x68k_bus_t *bus, int enable);

/*
 * 指定範囲へのRAM書き込みをpal_log()へ記録するデバッグウォッチ。
 * len=0 で無効化（既定OFF）。MCP WATCH_ON/WATCH_OFF から切り替えられる。
 */
void x68k_bus_set_ram_watch(x68k_bus_t *bus, uint32_t addr, uint32_t len);

/*
 * CPUクロック(Hz)を変更する。既定10,000,000(10MHz)。MFP(4MHz固定)・OPM(4MHz固定)・
 * ADPCM DMAの実時間ペーシングをこの値に合わせて再計算する（起動オプション
 * -cpu-clock またはメニュー選択からmain.cが呼ぶ）。
 */
void x68k_bus_set_cpu_hz(x68k_bus_t *bus, uint32_t hz);

/*
 * 縦方向のラスタスクロール(R11/R13/R15/R17/R19)をラッチせず常にライブ参照させる
 * （逃げ道オプション）。
 * -raster-y live 起動オプションから呼ぶ。既定は0（ラッチ、実機準拠）。
 */
void x68k_bus_set_raster_y_live(x68k_bus_t *bus, int live);

/* パレットをV-DISP開始時の値へラッチせず、各ラスタでライブ参照する互換モード。
 * -raster-pal live から呼ぶ。既定は0（latched）。 */
void x68k_bus_set_raster_pal_live(x68k_bus_t *bus, int live);

/*
 * MFP VRのSビット（ソフトウェアEOI）を有効化するかどうか。0（既定）なら
 * Sビット書き込みを無視して常に自動EOIへ倒す（XEiJ互換）。
 * -mfp-soft-eoi on 起動オプションから呼ぶ。
 */
void x68k_bus_set_mfp_soft_eoi(x68k_bus_t *bus, int enabled);

/*
 * GPIP bit7(H-SYNC)/bit4(V-DISP)をCRTCラスターモデル由来で合成するかどうか。
 * 1（既定）ならラスター由来（raster.valid=0時はフォールバック位相へ自動退避）。
 * 0なら常にフォールバック位相のみ（逃げ道）。-mfp-gpip-raster off 起動オプションから呼ぶ。
 */
void x68k_bus_set_mfp_gpip_raster(x68k_bus_t *bus, int enabled);

/*
 * FPU-CIR（CZ-6BP1/CZ-6BP1A, MC68881）ボードの有効化。既定は無効で、
 * $E9E000-$E9FFFF 全域が従来どおり CPU バスエラーになる。有効化すると mc68k_fpu_create()
 * でモデルを一度だけ生成し、選んだ32-byte window だけを XB1 gap から外す
 * （primary=$E9E000-$E9E01F, secondary=$E9E080-$E9E09F）。window 内の CIR は
 * fpu_cir.c が mc68000 の FPU モデルへ中継する（未実装/拒否された read は全1）。
 * モデル生成に失敗したら false を返し、無効のまま。
 * x68k_bus_create() と CPU 配線の間に一度だけ呼ぶ（実行中の切替は不可）。
 * enable=0 は no-op（有効化済みの無効化は destroy のみ）。
 */
typedef enum {
    X68K_FPU_CIR_PRIMARY,
    X68K_FPU_CIR_SECONDARY,
} x68k_fpu_cir_address_t;
bool x68k_bus_set_fpu_cir(x68k_bus_t *bus, int enable, x68k_fpu_cir_address_t address);

/* ===== 映像 (VRAM / CRTC / パレット / ラスター) ===== */

/* グラフィック VRAM の先頭ポインタ（0xC00000 相当、2MB）をレンダラーに渡す用 */
const uint8_t *x68k_bus_get_gvram(const x68k_bus_t *bus);

/* テキスト VRAM の先頭ポインタ（0xE00000 相当、512KB）をレンダラーに渡す用 */
const uint8_t *x68k_bus_get_tvram(const x68k_bus_t *bus);

/* スプライト/BG VRAM の先頭ポインタ（0xEB0000 相当、64KB）をレンダラーに渡す用 */
const uint8_t *x68k_bus_get_sp_vram(const x68k_bus_t *bus);

/* R00/R04、R20 HF/HD、システムポート#4 HRLからCRTCのフレーム周期を導出する。 */
void x68k_bus_crtc_get_frame_timing(const x68k_bus_t *bus, x68k_crtc_frame_timing_t *out);

/* CRTC R11 (0xE80016) の 10bit テキストスクロール Y 値（0-1023 scan line）を返す。
 * ラスターが有効なら垂直映像期間開始でラッチした値、無効なら従来どおり
 * ライブ値を返す。 */
int x68k_bus_get_tvram_scroll_y(const x68k_bus_t *bus);

/* CRTC R10 (0xE80014) の 10bit テキストスクロール X 値（0-1023 dot）を返す。
 * 横方向は常にライブ参照（ラッチしない）。 */
int x68k_bus_get_tvram_scroll_x(const x68k_bus_t *bus);

/*
 * グラフィック画面の表示ページ記述（GVRAM 合成用）。
 *   base_off : GVRAM 先頭（0xC00000）からのバイトオフセット（1024ドットモードでは
 *              未使用、page_sel で動的に決まる）
 *   sx, sy   : そのページのスクロール量（CRTC R12-R19、10bit 0-1023）
 *   mask     : パレットインデックスマスク（0x0F=16色 / 0xFF=256色 / 0=65536色=直値）
 *   visible  : ビデオコントローラ R2（$E82600）GS4-0 によるページ単位の表示ON/OFF
 *              （『Inside X68000』 図31、3-5-2節）
 *   page_sel : 1024ドットモードのみ使用。ビデオコントローラ R1（$E82500）下位バイトの
 *              生値（GP0/GP1/GP2/GP3、各2bit）。画面を4分割した象限（左上/右上/
 *              左下/右下512×512）ごとに、GVRAM のどのバンク（0-3）を参照するかを
 *              動的に選ぶ。1024ドット時の GVRAM は 1ドット1ワードのリニア2MB面で、
 *              バンク0-3はその左上/右上/左下/右下の512×512領域にあたる
 *              （『Inside X68000』3-2-2節・3-2-6節）
 */
typedef struct {
    int base_off;
    int sx;
    int sy;
    int mask;
    int visible;
    int page_sel;
} gvram_page_t;

/*
 * スプライト/BG画面の合成に必要なモード派生値（$EB0810/$EB0808 から導出。CRTCの
 * レジスタは参照しない。CRTC直叩きの正方形ドットモード等ではCRTC R20とスプライト
 * コントローラ自身の設定が食い違うため）。
 *   pat_size     : BGパタンサイズ 8 or 16（スプライトは常に16固定）
 *   real_wh      : BG実画面 512 or 1024（=表示画面の2倍）
 *   two_bg_pages : 1=BG1利用可（256表示モード）、0=BG0のみ（512表示モード）
 *   disp_on      : DISP/CPU(bit9)。0ならスプライト/BGレイヤ全体を非表示
 *   bg0/bg1_on   : 各BG画面のON/OFF（bit0/bit3）
 *   bg0/bg1_sx,sy: 各BGのスクロール位置（$EB0800-0806）
 *   bg0/bg1_base_off: 使用するBGデータエリアのsp_ram内オフセット（SP_BGDATA0/1_OFF）
 */
typedef struct {
    int pat_size;
    int real_wh;
    int two_bg_pages;
    int disp_on;
    int bg0_on, bg0_sx, bg0_sy, bg0_base_off;
    int bg1_on, bg1_sx, bg1_sy, bg1_base_off;
} sprite_scene_t;

/* $EB0810/$EB0808 からシーン記述子を導出する（CRTCは参照しない） */
int x68k_bus_get_sprite_scene(const x68k_bus_t *bus, sprite_scene_t *sc);

/*
 * 現在の色数モード・1024dot フラグに応じて表示すべきページを優先度順
 * （out[0] が最優先＝最前面）に埋め、ページ数（1/2/4）を返す。
 * IOCS _HOME/_SCROLL ($B3/$1D) が書く CRTC R12-R19 をページごとに反映する
 * （256色は page1 が R16/R17 である点に注意。『Inside X68000』 3-3-1節）。
 * ビデオコントローラ R1（$E82500）下位8bitの GP0-3（ページ間優先順位）を解釈して
 * 並べ替える。
 */
int x68k_bus_get_gvram_pages(const x68k_bus_t *bus, gvram_page_t out[4]);

/*
 * ビデオコントローラ R2（$E82600）の TON（bit5）。0ならテキスト画面（TVRAM）
 * レイヤー全体を非表示にする（『Inside X68000』 図31）。
 */
int x68k_bus_get_text_visible(const x68k_bus_t *bus);

/*
 * ビデオコントローラ R2（$E82600）の SON（bit6）。0ならスプライト+BG画面
 * レイヤー全体を非表示にする（『Inside X68000』 図31）。
 */
int x68k_bus_get_sprite_visible(const x68k_bus_t *bus);

/*
 * ビデオコントローラ R2（$E82600）の特殊表示モードを、描画側が使う派生状態へ
 * デコードしたもの。一次資料はB/P=0を未定義とするが、参照実装XEiJは「パレット
 * 出力のLSB(輝度ビットI)で領域を指定するモード」として実装しており、mint.xの
 * 疑似半輝度がこれに依存する（
 * docs/ix68k-internals/design/video_controller_design.md §5）。
 */
typedef enum {
    X68K_R2_EFFECT_NORMAL,
    X68K_R2_EFFECT_HALF_PALETTE0,
    X68K_R2_EFFECT_HALF,
    X68K_R2_EFFECT_SPECIAL_PRIORITY,
} x68k_r2_effect_t;

/* R2 bit10 B/P: 半透明・特殊プライオリティの領域指定方式。
 * 一次資料はB/P=0を未定義とするが、XEiJは「パレット出力のLSB(Iビット)で
 * 領域を指定する」モードとして実装しており、mint.xの疑似半輝度がこれに依存する。 */
typedef enum {
    X68K_R2_LSB_PAGE,  /* B/P=1: ベースページの生コード bit0 */
    X68K_R2_LSB_COLOR, /* B/P=0: パレット出力の bit0 */
} x68k_r2_lsb_source_t;

typedef struct {
    x68k_r2_effect_t effect;
    x68k_r2_lsb_source_t lsb_source;
    int blend_second; /* R2 bit9 G/G */
    int blend_text;   /* R2 bit8 G/T */
} x68k_r2_state_t;

/* R2の現在値を上記の派生状態へデコードする。compose.cがラスタごとに呼ぶ。
 * R2のビット解釈のみを行い、表示モード（解像度・色数）は一切参照しない
 * （1024ドット時のB/P=0無効化はcompose.c側のゲートで行う。§10.8(d)）。 */
void x68k_bus_get_r2_state(const x68k_bus_t *bus, x68k_r2_state_t *out);

/*
 * CRTC動作ポート ($E80481) bit1 のグラフィック画面高速クリアを実行する。
 * 選択プレーン（R21 bits3-0）の「表示高さ分の行」だけを、ページごとのYスクロール
 * (R13/R15/R17/R19) を加味した位置で0クリアする。1024ドットモードはプレーン選択を
 * 無視しGVRAM全域を1024ドット幅の行フォーマットでクリアする。
 * 呼び出し側（バス層）は完了印として動作ポートの bit1 を0へ戻すこと。
 */
void x68k_bus_crtc_rapid_clear(x68k_bus_t *bus);

/*
 * CRTC動作ポート ($E80481) bit3 のテキストラスタコピーを1回実行する。
 * R22高バイトのラスタブロックをR22低バイトへ、R21 bits3-0で選択したプレーンだけ
 * コピーする。書き込み時の即時実行と、bit3が1の間の毎水平フロントポーチ実行
 * （crtc.c crtc_raster_emit_line()）の両方から呼ばれる。
 */
void x68k_bus_crtc_raster_copy(x68k_bus_t *bus);

/*
 * ビデオコントローラ R1（$E82500）上位8bitのTX（bit11-10）/GR（bit9-8）優先順位を
 * 比較し、グラフィック画面がテキスト画面より前面に表示されるべきなら非0を返す。
 * 優先順位値は00が最高、01、10が最低（11は禁止）で、値が小さいほど
 * 優先度が高い（『Inside X68000』 図17）。GR/TXの2値判定のみでテスト互換のため
 * 残置。SPを含む3レイヤの合成順序には x68k_bus_get_screen_order() を使う。
 */
int x68k_bus_gvram_priority_over_text(const x68k_bus_t *bus);

/*
 * R1上位バイトのGR/TX/SP優先順位（各2bit）から、GR・TX・SPの3レイヤの
 * 背面→前面描画順を order[0..2] に返す（値: 0=GR, 1=TX, 2=SP）。
 */
void x68k_bus_get_screen_order(const x68k_bus_t *bus, int order[3]);

/*
 * CGROM をファイルからロードする（768KB、0xF00000–0xFBFFFF）。
 * BIOS が TVRAM へのフォントコピーに使用する。
 */
int x68k_bus_load_cgrom(x68k_bus_t *bus, const char *path);

/*
 * IOCS ROM をファイルからロードする。
 * 成功時は 0、失敗時は -1 を返す。
 * ロード後、ROM 先頭 8 バイト（SSP/PC リセットベクタ）を RAM[0..7] にコピーする
 * （実機のブートベクタオーバーレイ相当）。
 */
int x68k_bus_load_iocs_rom(x68k_bus_t *bus, const char *path);

/*
 * CGROM ANK（半角8x16、256エントリ×16バイト）フォントテーブルの先頭を返す
 * （ホスト側補助ウィンドウのラベル描画用）。実ROMの_FNTADR
 * 計算値（$F3A800）と一致することを逆アセンブルで確認済み
 * （iocs_rom + 0x3A000、インデックスは(ch+0x80)&0xFF）。
 */
const uint8_t *x68k_bus_get_cgrom_ank(const x68k_bus_t *bus);

/*
 * 現在の表示解像度を返す。
 *   幅: R02/R03（水平映像期間の実カラム数、(R03-R02)*8）から算出。
 *   高さ: CRTC R20 (0xE80028) bits 3-2 (VD): 00=256px 01=512px 10=1024px(interlace)。
 * R20 が未設定 (0) の場合は起動直後とみなし 512×512 をデフォルトとする。
 */
void x68k_bus_get_display_size(const x68k_bus_t *bus, int *w, int *h);

/*
 * 表示1ドットが占める横幅を、768x512モード（_CRTMOD 16）のドットを 2 とする
 * 整数単位で返す。ホストのウィンドウ幅は「表示ドット数 × この値 / 2」で求める。
 * 標準 CRTMOD では 512ドット→3・256ドット→6・768/1024ドット→2 となり、
 * 1024ドット系（mode 17/18）以外はすべて幅 768 に揃う。CRTC を直接叩いて
 * 256x256 の表示範囲のまま正方形ドットを作る非標準手法（俗称"384x256モード"、
 * otoko.c 等が使用）では 4 を返し、幅は 768 の 2/3 = 512 になる。
 * 値は R20 bit4（highReso）・bit1-0（hReso）とビデオコントローラ 0xE8E007
 * bit1（HRL）から、XEiJ の実測に基づくオシレータ分周比テーブル準拠で求める。
 */
int x68k_bus_get_dot_width_units(const x68k_bus_t *bus);

/*
 * システムポート#1 ($E8E001) の下位4bit = 画面コントラスト (0=最暗/消灯,
 * 15=最明)。『Inside X68000』システムポート 1-1 節。Human68kの常用値は
 * SRAM $ED0028 由来の $0E。
 */
unsigned x68k_bus_get_contrast(const x68k_bus_t *bus);

/*
 * 描画に使う現在のコントラストを1/3段階単位（0=最暗、45=最明）で返す。
 * $E8E001 の目標値とは別に、フレーム先頭ごとに目標へ1ずつ追従する。
 */
unsigned x68k_bus_get_contrast_scaled(const x68k_bus_t *bus);

/*
 * コントラスト適用用の256エントリLUT（8bit成分→8bit成分、線形スケール
 * out=round(in*scaled/45)）へのポインタを返す。scaled==45のとき恒等変換。
 * 現在値の変化時のみ再構築してcrtc_t側にキャッシュするため、呼び出しコストは
 * 通常ポインタ返却のみ（compose.cが行ごとに呼ぶ想定）。
 */
const uint8_t *x68k_bus_get_contrast_lut(x68k_bus_t *bus);

/*
 * CRTC ラスターモデル。走査線位置をCPUサイクル基準でドリフト無く追跡する。
 * main.c のフレームループからフレーム先頭で x68k_bus_crtc_raster_frame_start()、
 * CPUチャンク実行後に x68k_bus_crtc_raster_advance()、チャンクループ終了後に
 * x68k_bus_crtc_raster_flush() を呼ぶ。ライン単位の描画（ラスタースクロール・
 * スプライトダブラー対応）と、MFPのラスタ一致(ch14)/H-SYNC(ch15)割り込み判定の
 * 両方が、ここで追跡する走査線位置を参照する。
 */

/* フレーム先頭で呼ぶ。コントラストを目標へ1/45段階進め、CRTC R04-R07・表示高さから
 * ラスターパラメータを再導出する。
 * cycles_per_frame: このフレームのCPUサイクル数（cpu_hz/60）。
 * display_h: このフレームの表示高さ（x68k_bus_get_display_size()のh）。 */
void x68k_bus_crtc_raster_frame_start(x68k_bus_t *bus, int cycles_per_frame, int display_h);

/* ラスターを cpu_cycles 分進める。新しく到達した絶対ラスタ番号を昇順で out_lines[] へ
 * 最大 max_lines 個返し、その個数を戻り値とする（既定 x68k_raster_max_lines=64で足りる）。 */
int x68k_bus_crtc_raster_advance(x68k_bus_t *bus, int cpu_cycles, int *out_lines,
                                 int max_lines);

/* フレーム末尾で呼ぶ。残りのラスタを一気に消化して out_lines[] へ返す
 * （CPU が STOP/HALT で止まってもフレームの全行が必ず1回ずつ通知される）。 */
int x68k_bus_crtc_raster_flush(x68k_bus_t *bus, int *out_lines, int max_lines);

/* 絶対ラスタ番号 → 表示行（0..display_h-1）。映像期間外、またはラスタ2度読みの
 * 中間ラスタなら -1 を返す。 */
int x68k_bus_crtc_raster_display_row(const x68k_bus_t *bus, int line);

/* ラスタ同期描画に使えるフレームか（0ならR04-R07が不正、呼び出し側は従来の
 * 一括合成へフォールバックする）。 */
int x68k_bus_crtc_raster_valid(const x68k_bus_t *bus);

/*
 * GVRAM カラーモード。一次情報はビデオコントローラ R0（$E82400）の bit2-0（GrMM）で、
 * CRTC R20 bits 10-8（crtMemoryModePort、3bit）と同じ値をROMが組で書く。値:
 *   0 = 512dot 16色    1 = 512dot 256色    3 = 512dot 65536色
 *   4 = 1024dot 16色   5 = 1024dot 256色   7 = 1024dot 65536色指定
 * （実機は1024dotで65536色を表示できない。7はレジスタ値の列挙のみ）
 * 色数（下位2bit）と 512/1024dot（bit2）は独立した意味を持つため、
 * このヘッダでは色数のみを返す（1024dot 判定は x68k_bus_is_gvram_1024dot）。
 *   00 = 16色  (4bit パレットインデックス)
 *   01 = 256色 (8bit パレットインデックス)
 *   11 = 65536色 (直接 GRB555)
 */
typedef enum {
    GVRAM_COLOR_16 = 0,
    GVRAM_COLOR_256 = 1,
    GVRAM_COLOR_65536 = 3
} gvram_color_mode_t;

/* ビデオコントローラ R0（$E82400）bit1-0（GrMM下位2bit）から現在の GVRAM カラーモードを返す */
gvram_color_mode_t x68k_bus_get_gvram_color_mode(const x68k_bus_t *bus);

/* ビデオコントローラ R0（$E82400）bit2（GrMM bit2）: GVRAM が 1024dot 幅レイアウトかどうか。
 * 0 なら 512dot 幅（例: CRTMOD 0-15）、1 なら 1024dot 幅（例: CRTMOD 16-27）。 */
int x68k_bus_is_gvram_1024dot(const x68k_bus_t *bus);

/* 選択中（通常はV-DISP開始時にラッチ、live/fallback時はlive）の生パレットへの
 * ポインタを返す。寿命はbusの寿命中。次のsnapshotで内容は更新され得る。 */
const uint16_t *x68k_bus_get_gfx_palette_raw(const x68k_bus_t *bus);
const uint16_t *x68k_bus_get_text_palette_raw(const x68k_bus_t *bus);

/* このフレームのV-DISP中パレットbyte write数を取得して0へ戻す。 */
void x68k_bus_take_raster_palette_writes(x68k_bus_t *bus, unsigned *gfx, unsigned *text);

/*
 * グラフィックパレット (0xE82000: 256 エントリ × 2 バイト、CRTC バッファ内) を
 * ホストバイトオーダーの GRB555 配列として返す。16色モードでは先頭 16 エントリのみ使用。
 */
void x68k_bus_get_gfx_palette(const x68k_bus_t *bus, uint16_t pal[256]);

/*
 * テキストパレット (0xE82200–0xE8221F、CRTC レジスタ内の 16 エントリ × 2 バイト) を
 * ホストバイトオーダーの GRB555 配列として返す。
 * 透過判定はエントリ番号ではなくパレット生値が 0x0000 かどうかで行う（レンダラ側）。
 * pal[] は呼び出し元が uint16_t[16] を確保して渡す。
 */
void x68k_bus_get_text_palette(const x68k_bus_t *bus, uint16_t pal[16]);

/* テキスト/スプライト共用パレット全256色のGRB555生値を返す。 */
void x68k_bus_get_text_palette_all(const x68k_bus_t *bus, uint16_t pal[256]);

/*
 * パレットのARGB8888変換済みキャッシュを返す。CRTC書き込み時にeager更新されるため、
 * 呼び出しコストはポインタ返しのみ（O(1)）。ライン単位レンダラから毎行呼んでよい。
 */
const uint32_t *x68k_bus_get_gfx_palette_argb(const x68k_bus_t *bus);
const uint32_t *x68k_bus_get_text_palette_argb(const x68k_bus_t *bus);

/* ===== ホスト連携ストレージ (HOSTDRV / HOSTFILE) ===== */

/*
 * HOSTDRV: macOS 側ディレクトリを Human68k リモートデバイスとして公開する
 * MMIO 裏口（0xEA0200-0xEA020F）を有効化する。host_dir が NULL/空なら未設定のままにする。
 */
void x68k_bus_hostdrv_configure(x68k_bus_t *bus, const char *host_dir);

/*
 * HOSTFILE: 稼働中のエミュレータと macOS 間でファイルを1本単位で
 * やり取りする MMIO 裏口（0xEA0100-0xEA0117）のホスト側API。HOSTDRV と異なり
 * ファイルシステムを合成しない。100MB超のファイルでもホスト側メモリを圧迫しない
 * よう、ファイル全体を RAM に載せずソースファイルの直接ストリームおよび
 * 一時ファイルへのスプールで仲介する（詳細は bus_internal.h 参照）。
 * 68k 側は HOSTFILE.X が _dos_create/_dos_write 等の通常の DOS コールで実ドライブに
 * 書くため、Human68k 自身のディレクトリ/FATキャッシュと矛盾しない。
 */

/* macOS の path を開き、68k 側の HOSTFILE GET が読み出せる状態にする
 * （macOS側から見て「PUT」= 自分から送り出す操作。68k側はこれを `HOSTFILE GET`
 * で受け取る。GET/PUT は send/scp 等と同じく「呼び出す側から見た自分の動作」を
 * 表す — 絶対方向ラベルではない）。ファイルはオープンしたまま保持し、68k の
 * READ 要求ごとにオンデマンドで読む（ファイルサイズによらずホスト側メモリは
 * O(1)）。前回の未完了/未回収の転送があれば破棄してから開始する。
 * 成功時はファイルサイズ（0以上）を返す。失敗時は -1（ファイルを開けない等）。 */
long x68k_bus_hostfile_put(x68k_bus_t *bus, const char *path);

/* 68k 側が HOSTFILE PUT で一時ファイルへ書き込み済みのデータを macOS の path へ
 * 移す（macOS側から見て「GET」= 自分が受け取る操作）。一時ファイルは rename(2)
 * で目的地へ移動するだけで済ませる（同一ファイルシステムならデータコピー不要、
 * O(1)。クロスデバイス時のみ手動コピーへフォールバック）。
 * 成功時は書き出したバイト数（0以上）を返す。
 * 失敗時は負値: -1 = 68k側が未PUT（hf_state != PUT待ち）またはファイル移動/書き込み失敗 */
long x68k_bus_hostfile_get(x68k_bus_t *bus, const char *path);

/* 転送状態を取得する（デバッグ・待ち合わせ用）。
 * *state: 0=空 / 1=GET待ち(macOSがPUT済み、68kのHOSTFILE GET待ち)
 *              / 2=PUT待ち(68kがHOSTFILE PUT済み、macOS側のGET回収待ち) */
void x68k_bus_hostfile_status(const x68k_bus_t *bus, int *state, uint32_t *len);

/* ===== ディスク装置 (FDC / SCSI) ===== */

/*
 * FDC (µPD72065B) フロッピーディスクイメージをロードする。
 * drive: 0〜3。path: XDF 形式 (77cyl×2head×8sec×1024bytes = 1,261,568 bytes)。
 * 成功 0, 失敗 -1。
 */
int x68k_bus_load_fdc(x68k_bus_t *bus, int drive, const char *path);

/*
 * 実行中のディスク差し替え用（MCP INSERT_DISK 経由）。x68k_bus_load_fdc() と
 * 異なり、挿入後に IOI FDD 抜き挿し割り込みを発生させ、Human68k に変化を
 * 通知する。起動時ロード（main() 初期化）では使わないこと（BIOS のドライブ
 * ポーリングを混乱させるため）。
 * 実機同様、対象ドライブに既にディスクが入っている場合は先に
 * x68k_bus_eject_fdc() で排出しないと挿入できない（-2 を返す）。
 * 成功 0, 失敗 -1, 既にディスクが入っている -2。
 */
int x68k_bus_insert_fdc_runtime(x68k_bus_t *bus, int drive, const char *path);

/*
 * drive (0〜3) のディスクを排出する。実機の $E94005 イジェクト書き込み
 * （bit5=EJECT ON/OFF, bit3-0=ドライブの1→0エッジ）と同じ効果を持ち、
 * IOI FDD 抜き挿し割り込みを発生させる。
 */
void x68k_bus_eject_fdc(x68k_bus_t *bus, int drive);

/*
 * drive (0〜3) が前回この関数を呼んで以降にイジェクトされていたら 1 を返し、
 * ラッチをクリアする（読み出しでクリア）。IOCS _B_DRVCHK($4E) mode=8
 * 「前回検査後に排出したか」の実装に使う。物理($E94005)/MCP eject_disk
 * どちらの経路でも `fdc_eject_drive()` 経由でこのラッチが立つ。
 */
int x68k_bus_fdc_take_ejected_flag(x68k_bus_t *bus, int drive);

/* FDC 完了割り込みが pending なら 1 を返す */
int x68k_bus_fdc_pending_irq(const x68k_bus_t *bus);

/* IOI (I/O Interrupt Controller) に pending な割り込み要求があれば 1 を返す */
int x68k_bus_ioi_pending_irq(const x68k_bus_t *bus);

/* DMAC ch2 (IOCS _DMAMOVE系) の完了割り込みが pending なら 1 を返す */
int x68k_bus_dmac2_pending_irq(const x68k_bus_t *bus);

/* DMAC ch3 (ADPCM, IOCS _ADPCMOUT等) の完了割り込みが pending なら 1 を返す */
int x68k_bus_dmac3_pending_irq(const x68k_bus_t *bus);

/*
 * DMAC ch3(ADPCM)の実時間ペースド転送をcpu_cycles分だけ進める。MSM6258の
 * サンプリング周波数から「1バイト(=2サンプル)あたりのCPUサイクル数」を求め、
 * たまった分だけ転送し、完了時にch3_irq_pendingを立てる。
 * x68k_bus_mfp_tick()/x68k_bus_opm_tick()と同じチャンク粒度で呼ぶこと。
 * フレーム単位では音楽ドライバが行う「CCR SABで
 * 一時停止→約4456サイクル後にCCR STARTで再武装」の窓がフレーム境界を
 * 跨いだときに短い停止を正しく表現できないためである。
 */
void x68k_bus_adpcm_tick(x68k_bus_t *bus, int cpu_cycles);

/*
 * DMAC ch2(汎用メモリ間転送)の転送時間を cpu_cycles 分進める。完了するとCSR.ACTを
 * 落としてCOCと割り込み要求を立てる。x68k_bus_adpcm_tick()と同じチャンク粒度で呼ぶこと。
 */
void x68k_bus_dmac_tick(x68k_bus_t *bus, int cpu_cycles);

/*
 * ch2の転送完了までのCPUサイクル数（>=1）。動作中でなければ INT_MAX。main.cが
 * 実行チャンクを切り詰め、完了直後の命令境界でCOC・割り込みが見えるようにする
 * （x68k_bus_mfp_cycles_to_next_event()と同じ流儀）。
 */
int x68k_bus_dmac_cycles_to_next_event(const x68k_bus_t *bus);

/*
 * ch2のチャンク内同期（x68k_bus_mfp_set_cycle_source()と同じ流儀）。fnは現在のCPU
 * アクセス直前までのチャンク内経過サイクルを*outへ返しtrueを返す。同期できない
 * 経路（68060以外・callback外・debug_run）ではfalseを返す。登録中は、CPUのバス
 * アクセス直前にx68k_bus_dmac_sync_to_now()がその時刻までch2を進める。
 */
void x68k_bus_dmac_set_cycle_source(x68k_bus_t *bus, bool (*fn)(void *ctx, uint64_t *out),
                                    void *ctx);

/* 実行チャンクの開始。これ以降のsync_to_now()だけが同期を行う。 */
void x68k_bus_dmac_chunk_begin(x68k_bus_t *bus);

/* チャンク終了。同期済みの分を除いた残り(consumed - 同期済み)だけch2を進めて閉じる。 */
void x68k_bus_dmac_chunk_tick(x68k_bus_t *bus, int consumed);
/* CPU Hz変更時にch2のcreditを新しいHzへ換算する（x68k_bus_set_cpu_hz()が呼ぶ）。 */
void x68k_bus_dmac_cpu_hz_changed(x68k_bus_t *bus, uint32_t old_hz, uint32_t new_hz);

/* CPUのバスアクセス直前に呼ぶ。DMA自身のアクセス中・チャンク外・同期不能時は何もしない。 */
void x68k_bus_dmac_sync_to_now(x68k_bus_t *bus);

/*
 * 1フレーム分のADPCM再生出力サンプルを取り出す。
 * x68k_bus_adpcm_tick()をそのフレーム分だけ進めた後に毎フレーム呼ぶこと。モノラルint16サンプルを
 * outへ最大max_count個書き込み、実際の個数を返す。native_rate_hz/left_on/
 * right_onには現在の実効サンプリング周波数・パンポット状態を書き戻す
 * （audio_mix_resample_pan()へそのまま渡せる）。
 */
int x68k_bus_adpcm_take_samples(x68k_bus_t *bus, int16_t *out, int max_count,
                                uint32_t *native_rate_hz, int *left_on, int *right_on);

/*
 * 現在のADPCM実効サンプリング周波数(Hz)を返す。
 * マイクcaptureサンプルをadpcm_get_sample_rate_hz()相当へダウンサンプルする
 * 際にaudio_resample_mono()へ渡す出力レートとして使う。
 */
uint32_t x68k_bus_adpcm_sample_rate_hz(const x68k_bus_t *bus);

/*
 * ダウンサンプル済みマイクサンプルを録音入力リングへpushする。main.cが毎フレーム呼ぶ。
 */
void x68k_bus_adpcm_push_rec_samples(x68k_bus_t *bus, const int16_t *in, int count);

/*
 * ゲストが録音を開始している（$E92001へ「REC ST」を書き込み済み）かどうかを
 * 返す。ホスト側マイクデバイスの起動/停止判定（不要な時はOSのマイク
 * インジケータを点灯させない）に使う。
 */
int x68k_bus_adpcm_is_recording(const x68k_bus_t *bus);

/* FDC 割り込みをクリアする（BIOS ハンドラが処理後に呼ぶ） */
void x68k_bus_fdc_ack_irq(x68k_bus_t *bus);

/* drive (0〜3) にディスクイメージが挿入済みなら 1 を返す */
int x68k_bus_fdc_disk_loaded(const x68k_bus_t *bus, int drive);

/* 搭載ドライブ数（2 または 4）を設定・取得する */
void x68k_bus_fdc_set_num_drives(x68k_bus_t *bus, int n);
int x68k_bus_fdc_num_drives(const x68k_bus_t *bus);

/*
 * drive (0〜3) に挿入中のディスクイメージのファイルパスを返す
 * （ディスク選択ウィンドウの表示用）。未挿入ならNULL。
 * 呼び出し側はコピーせず読むだけにすること（eject/insertで無効化される）。
 */
const char *x68k_bus_fdc_disk_path(const x68k_bus_t *bus, int drive);

/*
 * アクセスランプ用: $E94007で最後に選択されたドライブ番号
 * (0〜3)と、そのドライブのMOTOR ON(bit7)状態を返す。同時に「モータON」
 * にできるドライブは実機同様1台のみ（同レジスタの単一書き込みで両方が
 * 決まるため）。全ドライブぶんの赤/緑判定は、呼び出し側が
 * `x68k_bus_fdc_selected_drive(bus) == drive && x68k_bus_fdc_motor_on(bus)`
 * で行う（trueなら赤、falseなら緑）。
 */
int x68k_bus_fdc_selected_drive(const x68k_bus_t *bus);
int x68k_bus_fdc_motor_on(const x68k_bus_t *bus);

/*
 * x68k_bus_load_fdc()/x68k_bus_insert_fdc_runtime()の戻り値。
 * 挿入失敗理由をGUIへ返せるよう名前付きエラーコードへ拡張してある。
 * 文字列組み立てはホストUI側の責務（bus層はUI文言に関知しない）。
 */
typedef enum {
    X68K_FDC_OK = 0,
    X68K_FDC_ERR_INVALID_DRIVE = -1,    /* drive が範囲外(0〜3以外) */
    X68K_FDC_ERR_ALREADY_INSERTED = -2, /* 既にディスクが入っている(要eject) */
    X68K_FDC_ERR_BAD_EXTENSION = -3,    /* 未対応の拡張子 */
    X68K_FDC_ERR_IO = -4,               /* ファイルを開けない/読み込み失敗 */
    X68K_FDC_ERR_ALLOC = -5,            /* メモリ確保失敗 */
    X68K_FDC_ERR_NO_DISK = -6,          /* ディスク未挿入(WPトグル用) */
    X68K_FDC_ERR_BUSY = -7,             /* FDC実行中(WPトグル用) */
    X68K_FDC_ERR_SIZE_MISMATCH = -8, /* ファイルサイズが拡張子の媒体と一致しない */
} x68k_fdc_err_t;

/*
 * drive (0〜3) に挿入中のディスクがライトプロテクトされていれば 1 を返す。
 * 挿入時（x68k_bus_load_fdc）にホストのファイル書き込み権限から判定した
 * キャッシュ値を返すだけで、host FS へは問い合わせない。
 */
int x68k_bus_fdc_write_protected(const x68k_bus_t *bus, int drive);

/*
 * drive のライトプロテクトを設定/解除する（実機のプロテクトノッチ操作に相当）。
 * ホストのイメージファイルのパーミッション（owner writeビットのみ）を実際に
 * 変更し、変更後に再判定した結果を内部フラグへ反映する。同一ファイルを
 * 複数ドライブへ挿入している場合に備え、挿入中の全ドライブを再判定する
 * 成功 0、失敗は x68k_fdc_err_t の負値。
 */
int x68k_bus_fdc_set_write_protect(x68k_bus_t *bus, int drive, int protect);

/* ===== 周辺機器のフレーム駆動・入出力 ===== */

/*
 * MFP (MC68901) の USART RX 昇格待ちと Timer B/C/D を cpu_cycles 分進める。
 * main.c の実行チャンクごとに呼ぶこと（チャンク内同期時は MFP レジスタアクセス直前にも
 * 内部から呼ばれる）。
 */
void x68k_bus_mfp_tick(x68k_bus_t *bus, int cpu_cycles);

/*
 * MFPタイマ（B/C/D）のいずれかが次に満了するまでのCPUサイクル数（>=1）を返す。
 * 動作中のタイマが無ければ INT_MAX。main.cが実行チャンクを min(MFP_TICK_CYCLES, この値)
 * に切り詰め、満了直後の命令境界でIRQ線が見えるようにする。
 */
int x68k_bus_mfp_cycles_to_next_event(const x68k_bus_t *bus);

/*
 * チャンク内同期。fnは「現在のCPU実行呼出しで消費済みのサイクル数」を
 * 返す。登録中はMFPレジスタのread/write直前に、消費済み分までMFPをtickする
 * （書込み前のサイクルを新設定へ遡及させない）。fn==NULLで解除。
 */
void x68k_bus_mfp_set_cycle_source(x68k_bus_t *bus, int (*fn)(void *ctx), void *ctx);

/* チャンク開始時に呼ぶ。tick済みサイクルを0に戻す。 */
void x68k_bus_mfp_chunk_begin(x68k_bus_t *bus);

/* チャンク末尾で呼ぶ。consumed のうちレジスタアクセス時に同期済みの分を除いて
 * x68k_bus_mfp_tick() し、同期済みサイクルを0に戻す。 */
void x68k_bus_mfp_chunk_tick(x68k_bus_t *bus, int consumed);

/*
 * OPM(YM2151)内蔵タイマA/Bを cpu_cycles 分進める。
 * x68k_bus_mfp_tick()と同じチャンク粒度で呼ぶこと。音声生成(ym2151_generate)とは無関係。
 */
void x68k_bus_opm_tick(x68k_bus_t *bus, int cpu_cycles);

/*
 * OPM(YM2151)のステレオint32サンプルをframes分生成する(62500Hz固定、中間クリップ無し)。
 * main.cがフレーム単位(60Hz)で呼ぶ。
 */
void x68k_bus_opm_generate(x68k_bus_t *bus, int32_t *buf, int frames);

/* 内蔵SCSI(SPC)関連 API */

/*
 * x68k_bus_scsi_set_write_protect()の戻り値。
 * 文字列組み立てはホストUI側の責務（bus層はUI文言に関知しない、x68k_fdc_err_tと同じ方針）。
 */
typedef enum {
    X68K_SCSI_OK = 0,
    X68K_SCSI_ERR_INVALID_ID = -1,    /* id が範囲外(0〜6以外) */
    X68K_SCSI_ERR_NOT_SUPPORTED = -2, /* 非removable(HDD)またはCD-ROM: WP切替非対応 */
    X68K_SCSI_ERR_NO_MEDIA = -3,      /* メディア未挿入 */
    X68K_SCSI_ERR_BUSY = -4,          /* このIDが現在SCSIバスの選択中ターゲット */
    X68K_SCSI_ERR_IO = -5,            /* stat/chmod失敗、または再判定が要求値と食い違う */
} x68k_scsi_err_t;

/* SPC割り込み要求を発火する（IOI経由、レベル1・ベクタ$6C固定、常時許可）。
 * scsi.c の spc_set_interrupt() から呼ばれる。 */
void x68k_bus_scsi_irq_raise(x68k_bus_t *bus);

/* SCSI HDD イメージを id (0-6) に接続する。X68SCSI1デバイスパラメータを検証する。
 * 戻り値: 0=成功, -1=失敗 */
int x68k_bus_scsi_insert(x68k_bus_t *bus, int id, const char *path);
int x68k_bus_scsi_enable_overlay(x68k_bus_t *bus, int id, const char *path);

/*
 * SCSI ID (0-6) のユニット状態を参照する（MCPのDISK_STATUS用）。
 * 戻り値: 1=このIDにドライブが存在する, 0=存在しない（範囲外も0）。
 * *out_path にはメディアのイメージパスを書く（メディア無しはNULL）。
 * リムーバブルはドライブが存在してもメディア無しになり得るため、両者を区別する。
 */
int x68k_bus_scsi_unit_status(const x68k_bus_t *bus, int id, const char **out_path);
void x68k_bus_scsi_eject(x68k_bus_t *bus, int id);

/*
 * メディア未挿入の空リムーバブルドライブ（MO等）をid(0-6)へ接続する
 * （ix68k.cfgで`scsi_diskN=MO`と指定した場合用）。
 * 戻り値: 0=成功, -1=失敗（idが範囲外）。
 */
int x68k_bus_scsi_insert_empty_removable(x68k_bus_t *bus, int id);

/*
 * メディア未挿入の空CD-ROMドライブをid(0-6)へ接続する
 * （ix68k.cfgで`scsi_diskN=CD`と指定した場合用）。
 * 戻り値: 0=成功, -1=失敗（idが範囲外）。
 */
int x68k_bus_scsi_insert_empty_cdrom(x68k_bus_t *bus, int id);

/* id (0-6) にSCSIユニットが接続済みかどうかを返す（IOCS _B_DSKINI等のブート判定用）。 */
int x68k_bus_scsi_unit_attached(const x68k_bus_t *bus, int id);

/*
 * id (0-6) にドライブ自体が存在するか（メディア有無を問わない）を返す
 * （ディスク選択ウィンドウで「空のリムーバブルドライブ」と
 * 「そもそも未接続」を区別表示するために使う）。
 */
int x68k_bus_scsi_unit_present(const x68k_bus_t *bus, int id);

/*
 * id (0-6) に接続中のイメージファイルパスを返す（ディスク選択
 * ウィンドウの表示用）。未接続ならNULL。
 */
const char *x68k_bus_scsi_disk_path(const x68k_bus_t *bus, int id);

/*
 * id (0-6) のユニットがリムーバブルメディア(MO等)かどうかを返す
 * （ディスク選択ウィンドウでイジェクトボタンの表示要否を判定するため。
 * デバイスパラメータ$0F(EJ)由来のscsi_unit_t.removableをそのまま返す。
 * 1=リムーバブル(イジェクト可)、0=非リムーバブル(HDD相当、イジェクト不可)）。
 */
int x68k_bus_scsi_unit_removable(const x68k_bus_t *bus, int id);

/*
 * id (0-6) のユニットがCD-ROM(CloneCD)かどうかを返す
 * （ディスク選択ウィンドウでの表示・ファイルダイアログの拡張子フィルタ選択用）。
 * 1=CD-ROM、0=HDD/MO等それ以外。
 */
int x68k_bus_scsi_unit_is_cdrom(const x68k_bus_t *bus, int id);

/*
 * id (0-6) に挿入中のメディアがライトプロテクトされていれば1を返す。
 * 挿入時（x68k_bus_scsi_insert）にホストのファイル書き込み権限から判定した
 * キャッシュ値を返すだけで、host FSへは問い合わせない。
 */
int x68k_bus_scsi_write_protected(const x68k_bus_t *bus, int id);

/*
 * id のライトプロテクトを設定/解除する（実機のプロテクトノッチ操作に相当）。
 * 対象はremovableかつSCSI_UNIT_DISK(MO)のユニットのみ。ホストのイメージ
 * ファイルのパーミッション（owner writeビットのみ）を実際に変更し、変更後に
 * 再判定した結果を内部フラグへ反映する。同一ファイルを複数SCSI IDへ
 * 挿入している場合に備え、接続中の全SCSIユニットを再判定する。
 * 成功0、失敗は x68k_scsi_err_t の負値。
 */
int x68k_bus_scsi_set_write_protect(x68k_bus_t *bus, int id, int protect);

/*
 * 現在バス上で選択中のSCSIターゲットID(0-6)を返す。busフリー(未選択)なら-1
 * （アクセスランプ用。FDDの$E94007 fdc_drive_selに相当）。
 */
int x68k_bus_scsi_target_id(const x68k_bus_t *bus);

/*
 * SCSIバスがデータ転送中(Data-In/Data-Out、DMAドレイン中)なら1を返す
 * （アクセスランプ用。FDDのmotor_onに相当。ただし単一の共有
 * バスのため、点灯対象ドライブはx68k_bus_scsi_target_id()と組み合わせて
 * 判定する: target_id==id かつ この関数が1のときだけ赤にする）。
 */
int x68k_bus_scsi_transferring(const x68k_bus_t *bus);

/* MFP IRQ6 が保留中なら 1 を返す */
int x68k_bus_mfp_pending_irq(const x68k_bus_t *bus);

/* TCDCR レジスタに val を書いて Timer C を起動/停止する（IOCS fn=0x0C から呼ぶ） */
void x68k_bus_mfp_set_tcdcr(x68k_bus_t *bus, uint8_t val);

/* Timer A(ch13, IOCS 0x6C _VDISPST)のV-DISPイベントカウントを1フレーム分進める。
 * ラスターが有効なフレームでは垂直映像期間終了のラスタ到達時に crtc.c から呼ばれ、
 * ラスター無効時のみ main.c のフレーム末尾から呼ばれる（いずれも1フレーム1回）。 */
void x68k_bus_mfp_vdisp_tick(x68k_bus_t *bus);

/*
 * RTC(RP5C15, $E8A001-$E8A01F)の時刻源。iocs.c の _DATESET/_TIMESET($51/$53)・
 * _DATEGET/_TIMEGET($54/$56)がMMIOレジスタ読み出しと同じ時計を共有するために使う。
 *x68k_bus_rtc_now(): guest_unix+オフセット秒。
 * x68k_bus_rtc_set(): オフセット秒 = target_unix - guest_unix。
 */
int64_t x68k_bus_rtc_now(const x68k_bus_t *bus);
uint64_t x68k_bus_guest_mono_us(const x68k_bus_t *bus);
void x68k_bus_guest_clock_set_paused(x68k_bus_t *bus, int paused);
void x68k_bus_rtc_set(x68k_bus_t *bus, int64_t target_unix);

/*
 * キーボードの押下/離上バイト（下位7bit=キーコード、bit7=1:離上）を
 * MFP USART RX FIFO へ積む。armed 状態なら割り込み pending への昇格も試みる。
 * FIFO 満杯時は黙って破棄する（XEiJ MC68901.mfpKeyboardInput 準拠）。
 * platform 層 (SDL_KEYDOWN/UP) および MCP press_key/paste_text から呼ぶ。
 */
void x68k_bus_mfp_usart_rx_push(x68k_bus_t *bus, uint8_t byte);

/* USART RX のUDRまたはFIFOに、ROMがまだ消費していないキーボードバイトが
 * あれば1を返す。IOCS B_KEYINP入口の待機判定など、バス内部を見せずに
 * 入力到着済みか調べる用途に使う。 */
int x68k_bus_mfp_usart_rx_pending(const x68k_bus_t *bus);

/* FIFO 満杯で破棄したキーボードバイトの累計数。 */
uint32_t x68k_bus_mfp_usart_rx_dropped(const x68k_bus_t *bus);

/* MFP USART RX FIFO を空にする（MCP paste_text 前のフラッシュ用）。 */
void x68k_bus_mfp_usart_rx_clear(x68k_bus_t *bus);

/* SCC ch B(マウス)関連 API。
 * x/y はホストの現在位置（ネイティブ解像度換算後の絶対座標。MCP mouse_move の
 * 相対移動量は platform 層が仮想座標へ積算して渡す）、left/right は現在のボタン押下状態(0/1)。
 * platform 層 (SDL_MOUSEMOTION/BUTTONDOWN/UP) および MCP mouse_move/mouse_click
 * から呼ぶ。実際の3バイト送出は WR5(RTS) 0→1 立ち上がり時（ROM側が駆動）。 */
void x68k_bus_scc_mouse_position(x68k_bus_t *bus, int x, int y);
void x68k_bus_scc_mouse_button(x68k_bus_t *bus, int left, int right);

/* SCC ch A(RS-232C)/ch B(マウス) IRQ5 が保留中なら 1 を返す（main.c の IPL 選択用） */
int x68k_bus_scc_pending_irq(const x68k_bus_t *bus);

/* SCC ch A(RS-232C)実データ経路。main.cが毎フレーム
 * ホストRS-232C経路(PTY等)との間でバイト列を橋渡しする。 */
void x68k_bus_scc_recv_a(x68k_bus_t *bus, const uint8_t *buf, int n);
int x68k_bus_scc_send_a_dequeue(x68k_bus_t *bus, uint8_t *out, int max);
void x68k_bus_scc_serial_set_carrier(x68k_bus_t *bus, int connected);

/* PPIポートA/B(ジョイスティック#1/#2、$E9A001/$E9A003)関連API。
 * index: 0=#1(ポートA) 1=#2(ポートB)。val はX68kポート形式の
 * 1バイト（アクティブロー、未接続/未押下=1）をそのまま渡す。
 * main.cが毎フレーム pal_joystick_read() の結果をここへ流し込む。 */
void x68k_bus_set_joystick(x68k_bus_t *bus, int index, uint8_t val);

/* ===== CPU / デバッガのバスアクセス =====
 * CPUフックは x68k_bus_get_hooks() から取得する。以下の read/write は
 * MCP・IOCS向けの直接アクセスで、CPU保護やバスエラー通知を伴わない。 */

/* RAM からバイトを読み出す（IOCS _B_PRINT ハンドラ用） */
uint8_t x68k_bus_read_ram_byte(const x68k_bus_t *bus, uint32_t addr);

/* 任意アドレスの読み書き（IOCS _B_PEEK/_POKE 等で使用） */
uint8_t x68k_bus_read8(x68k_bus_t *bus, uint32_t addr);
uint16_t x68k_bus_read16(x68k_bus_t *bus, uint32_t addr);
uint32_t x68k_bus_read32(x68k_bus_t *bus, uint32_t addr);
void x68k_bus_write8(x68k_bus_t *bus, uint32_t addr, uint8_t val);
void x68k_bus_write16(x68k_bus_t *bus, uint32_t addr, uint16_t val);
void x68k_bus_write32(x68k_bus_t *bus, uint32_t addr, uint32_t val);

/* 非侵襲peek（mc68k_set_peek_hooks経由でmc68000コアのデバッガから使う）。
 * I/Oレジスタのread-clear等の副作用を一切起こさない（x68k_bus.c peek8()参照）。 */
uint8_t x68k_bus_peek8(const x68k_bus_t *bus, uint32_t addr);
uint16_t x68k_bus_peek16(const x68k_bus_t *bus, uint32_t addr);
uint32_t x68k_bus_peek32(const x68k_bus_t *bus, uint32_t addr);

/* 任意アドレスに 1 バイトを書き込む（デバッグ用） */
void x68k_bus_write_byte(x68k_bus_t *bus, uint32_t addr, uint8_t val);
/* ローカルSIMMを含む32bitアドレスの裏口アクセス（HOSTDRV・MCP用。DMACは使わない） */
uint8_t x68k_bus_read8_full(x68k_bus_t *bus, uint32_t addr);
void x68k_bus_write8_full(x68k_bus_t *bus, uint32_t addr, uint8_t val);

/* ===== 永続状態・リセット・起動監視 ===== */

int x68k_bus_load_sram(x68k_bus_t *bus, const char *path);
int x68k_bus_save_sram(const x68k_bus_t *bus, const char *path);
void x68k_bus_init_sram_defaults(x68k_bus_t *bus);

/*
 * MCP/ソフトリセット用: 電源投入時ROMリセットベクタを RAM $0-$7 へ復元する。
 * mc68k_reset() の直前に呼ぶこと（呼ばないと Human68k が上書きしたゴミを
 * SSP/PC として読み暴走する）。
 */
void x68k_bus_reset_vectors(x68k_bus_t *bus);

/*
 * ベクタ24(スプリアス割り込み)を IPLROM 内の単独 rte 命令
 * ($0000FE006C)へ恒久的に固定する。x68k_cycle_catchup() から毎tick冪等に
 * 呼ぶこと（IPLROM起動時のベクタ一括フィルに上書きされるため）。
 */
void x68k_bus_pin_spurious_vector(x68k_bus_t *bus);

/* MCP/ソフトリセット用: IOI/内蔵SCSI等の周辺を電源投入相当へ戻す。 */
void x68k_bus_reset_peripheral(x68k_bus_t *bus);

/*
 * 「Machine > Reset (Force FD0 Boot)」(Cmd+Option+R)向け。$ED0018
 * (SRAM起動デバイス設定)をROMが実際に読んだ瞬間を検出するための監視を
 * 有効/無効にする。enable=1で監視開始（read_seenフラグもクリアされる）、
 * enable=0で監視終了。armed中のみread8()内でチェックが走るため、通常実行
 * への性能影響はない。
 */
void x68k_bus_set_boot_dev_watch(x68k_bus_t *bus, int enable);
int x68k_bus_boot_dev_watch_armed(const x68k_bus_t *bus);

/*
 * 監視中に$ED0018が読まれていたら1を返し、フラグを消費する
 * （pal_nmi_requested()と同型）。監視自体はx68k_bus_set_boot_dev_watch()で
 * 別途止めること（このAPIはフラグの消費のみ）。
 */
int x68k_bus_take_boot_dev_read(x68k_bus_t *bus);

/*
 * NMIボタン相当の要求を立てる（sticky・edge-consumed）。
 * pull型IRQ受付（mc68k_set_irq_level_hook()で登録するbus_irq_level()）が
 * 最優先でこれを読み、実際にCPUが受け付けた時点でクリアする。
 * x68k_cycle_catchup()がホストのNMIボタンをここへ橋渡しし、MCPの
 * デバッグ用NMI注入もこれを使う（pull型ではCPUがpending_irqを見ないため、
 * mc68k_set_irq()を直接呼ぶ経路は使えない）。
 */
void x68k_bus_set_nmi_pending(x68k_bus_t *bus);

/*
 * システムポート#6 ($E8E00F) の電源断シーケンス（前面スイッチOFF中に
 * $00,$0F,$0F）が完了していれば 1 を返す。ラッチで、読んでも
 * リセットしても解除されない。main ループが終了判定に使う。
 */
int x68k_bus_power_off_requested(const x68k_bus_t *bus);

/*
 * TVRAM plane 0 のビットマップを CGROM ANK と照合してテキスト文字列に変換する。
 * cols: 読み取り列数（例: 96）, rows: 行数（例: 32）
 * out: 出力バッファ（最低 cols*rows + rows + 1 バイト）
 * 戻り値: 書き込んだバイト数（NULL 終端を除く）。
 */
int x68k_bus_tvram_to_text(const x68k_bus_t *bus, int cols, int rows, uint8_t *out,
                           int out_cap);

/*
 * IOCS _B_PUTMES($2F)呼び出しの引数（表示文字列そのもの）をキャプチャ
 * リングに記録する。iocs.c が TRAP #15 フックで呼ぶ。TVRAM ピクセル解析では
 * 反転表示（白帯等）を読み取れないため、x68k_bus_tvram_to_text() は
 * キャプチャがあるセルではピクセル解析より先にこの内容を採用する。
 * row: 0-63, col: 0-127, len: text の有効長(0 なら何もしない)。
 * len は内部で col+len が 128 を超えないようクランプされる。
 */
void x68k_bus_record_putmes(x68k_bus_t *bus, int row, int col, int len, const char *text);

/*
 * 記録済みで未スナップショットの _B_PUTMES キャプチャについて、描画後の
 * TVRAM内容（セルごとに4プレーン×16スキャンライン）を取り込む。
 * main.c のフレームループから1フレームに1回呼ぶ。
 * 記録は _B_PUTMES の実行「前」フック（iocs.c）で行われるため描画後の内容を
 * その場では取れず、また記録フレームの残りサイクルだけでは ROM ハンドラの
 * 完了を保証できないので、記録フレームの「次の」フレームが完全に終わった
 * 境界まで待ってからスナップショットする。以後 x68k_bus_tvram_to_text() は
 * この内容と現在のTVRAM内容が一致するキャプチャだけを採用する。
 * 呼び出し側は「そのフレームのCPU実行を完走した」場合にのみ呼ぶこと
 * （デバッガ停止で早期に抜けたフレームで呼ぶと描画途中を取り込みうる）。
 */
void x68k_bus_putmes_snapshot_tick(x68k_bus_t *bus);

/*
 * _B_PUTMES キャプチャリングを空にする。リセット時に呼び、リセット前の
 * 白帯・ダイアログ文言が read_bitmap_text に残らないようにする。
 */
void x68k_bus_putmes_clear(x68k_bus_t *bus);

/*
 * ANKフォント差し替え報告を受け取る。addr は実際に描画に
 * 使われているANKフォントパターンの先頭アドレス（16バイト/文字、
 * インデックス=文字コード）。iocs.c の _B_PRINT($21) ハンドラが
 * "FNTADR=XXXXXXXX;" マーカーを検出すると呼ぶ。以後 x68k_bus_tvram_to_text()
 * はROM固定オフセットの代わりにこのアドレスを参照する。
 * ゲスト側で報告するプログラムは tools-x68k/fntadr/fntadr.s。
 */
void x68k_bus_set_ank_font_override(x68k_bus_t *bus, uint32_t addr);

/*
 * ANKフォント差し替え報告をクリアし、ROM固定オフセット参照に戻す。
 * リセット時に呼ぶ（常駐ドライバの状態も失われるため）。
 */
void x68k_bus_clear_ank_font_override(x68k_bus_t *bus);

/*
 * buf[0..len) の中から "FNTADR=XXXXXXXX;" マーカーを探し、見つかれば
 * x68k_bus_set_ank_font_override() を呼ぶ。iocs.c の _B_PRINT($21) ハンドラが
 * 出力全文を渡す。戻り値はマーカーを検出・適用したら1、しなければ0。
 */
int x68k_bus_scan_fntadr_report(x68k_bus_t *bus, const uint8_t *buf, int len);
