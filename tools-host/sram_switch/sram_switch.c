/* tools-host/sram_switch/sram_switch.c
 * sram.dat（オフライン）のメモリスイッチ設定ツール。
 * 実機の SWITCH.X と同じ KEY=VALUE 形式でオフラインの sram.dat を読み書きする。
 * エミュレータ起動前提のオフラインツール（起動中に書き換えても終了時に上書き保存される）。
 *
 * アドレスは『C Compiler PRO-68K ver2.0 プログラマーズマニュアル』(XC20) 付録に基づく。
 *
 * X=（XCHG, $ED0059）/ HD_MAX=（$ED005A）/ ID=（SCSI_ID, $ED0070）/ R=（RS232C, $ED001A）は
 * SWITCH.X Ver2.20実機逆アセンブル（Issue #115、switchx/unbind/SWITCH1.S）に基づく:
 *   - X=: L000ab4。値0-7（cmp.l #$08,d1;bcc err）。ヘルプ文字列L00118d "-Xchg =0 bit0-2"。
 *   - HD_MAX=: L000ae4。値0-16（cmp.l #$11,d1;bcc err）。ヘルプ文字列
 *     "-Hd_max=1 HDのドライブ数(0~16)"。HD<n>起動を機能させるのに必須の設定。
 *   - ID=: L000574-L000610。値0-7（cmp.l #$08,d1;bcc err）。ヘルプ文字列
 *     "-Id=7 SCSIの本体ID(0~7)"。実機はL000684で内蔵SCSI ROM($FC0024="SCSI"+$FC0028="IN")/
 *     外付けボード($EA0024="SCSI"+$EA0028="EX")をハードウェア検出し、ID=O<n>で外付け指定を
 *     許可した上で$ED000Cのベースを$FC0000⇔$EA0020間で切り替える。ix68kは内蔵
 *     SCSIのみ実装（拡張ボードCZ-6BS1は docs/ix68k-internals/design/scsi_design.md で非スコープと
 *     明記）のため、本ツールはID=0-7（内蔵のみ）を受理しO<n>構文は実装しない。マジック
 *     バイト($ED006F=$56)が未設定なら$ED0071(ID別ブート禁止ビットマップ)と併せて初期化する
 *     （L0006d0-L0006f2）。
 *   - R=: 値設定パーサー本体はL000c1e-L000da6。ビットフィールドはbit0-3=ボーレート
 *     (0=75/1=150/2=300/3=600/4=1200/5=2400/6=4800/7=9600/8=19200、L000c40-L000ca6の
 *     4桁数値文字列比較テーブル準拠)、bit7=RS/CS制御、bit9=Xon/Xoff制御(L000d6e-L000d94)、
 *     bit10-11=データビット(0=5/1=6/2=7/3=8、L000cb0-L000cea)、bit12-13=パリティ
 *     (0=None/1=Odd/3=Even、L000cf4-L000d38)、bit14-15=ストップビット(1=1/3=2、
 *     L000d42-L000d64)。実機はL000394で複数RS232Cポート機種向けにドライバ("232C1.11")
 *     問い合わせで書き込み先を$ED001A/$ED005C/$ED005E/$ED0060/$ED0062から動的に選ぶが、
 *     問い合わせ失敗時(実機ドライバなし)は$ED001Aに固定される(L000343初期値$01→L000312)。
 *     本ツールはオフラインでハードウェア問い合わせを再現できないため常に$ED001Aのみを
 *     対象とする。また実機は"9600bps Bits-8 Parity-None Stop-1 Xon"のような順不同・
 *     省略可能なトークン列を許すパーサーだが、本ツールは固定順カンマ区切り
 *     R=<baud>,<bits>,<parity>,<stop>,<flow>（全5フィールド必須）のみ受理する
 *     （実装・パース双方の単純化を優先、SWITCH.X本体との入力構文互換は捨てる）。
 *
 * B=（起動デバイス、$ED0018）は下記一次資料の実測に基づく（Issue #112）:
 *   - IPLROM XVI起動判定: $FF01E6(上位ニブル分岐)/$FF0328(HDスキャン、上位バイト+$0100毎)/
 *     $FF039E(FDスキャン、$9070/$9170/$9270/$9370固定4値)/$FF02D8($ED000Cへ二重間接jsr)/
 *     $FF0310($ED0010へ単純間接jsr)/$FF99FE・$FF9A26(SASI: id=(D1上位バイト&$0F)>>1、
 *     LUN=(D1上位バイト&$0F)&1)
 *   - SWITCH.X Ver2.20 -Boot=パーサー(switchx/unbind/SWITCH1.S):
 *     L0007ca(HD、id<16、$8000+id*$100)/L0007e0(2HD、id<4、$9000+id*$100の下位$70固定)/
 *     L000648(SCSI、id<7、$ED000C=$FC0000+id*4)/L000760(ROM、$ED000C=指定アドレス)/
 *     L0004bc(RAM、$ED0010=指定アドレス)
 *   - 内蔵SCSI BIOS ROM実体(src/peripheral/scsi_rom_data.h、scsi16in/SCSI16IN_code.S):
 *     $FC0000+id*4がID別ブートベクタ
 *   - 参照実装XEiJ(SRAM.java smrParseBootDevice/SPC.java SPC_HANDLE_IN/EX、
 *     HDC.java hdcUnitArray[id<<1|lun])で同じ値・同じHD論理ドライブ番号解釈を確認済み
 *     （XEiJのGUI/CLI起動オプションはSCSI0-7の8個を許容するが、これはXEiJ独自の便利機能で
 *     SWITCH.X本体の制約とは別。本ツールはSWITCH.X本体互換を優先しSCSI0-6の7個とする）
 *
 * ビルド:
 *   make -C tools-host/sram_switch
 *
 * 使い方:
 *   tools-host/sram_switch/sram_switch                          # sram.dat の現在値を表示
 *   tools-host/sram_switch/sram_switch -f other.dat              # ファイル指定して表示
 *   tools-host/sram_switch/sram_switch B=2HD0                    # 起動デバイスをFD0に設定
 *   tools-host/sram_switch/sram_switch B=SCSI0                   # 起動デバイスをSCSI ID0に設定
 *   tools-host/sram_switch/sram_switch -f other.dat S=Program P1=\$F83E
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
/* MSVC / MinGW: <strings.h> が無いので同等の関数へ置き換える。コンソールは UTF-8 出力にする。 */
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define SRAM_BASE 0xED0000u
#define SRAM_SIZE 0x4000u

typedef struct {
    const char *name;
    uint32_t value;
} enum_item_t;

/* switch_def_t.encode_aux()が返す、主フィールド以外への追加書き込み1件分。
 * B=SCSI<n>のように$ED0018(主フィールド)以外に$ED000C等も書く必要があるキー用。 */
typedef struct {
    uint32_t offset;
    int size;
    uint32_t value;
} sram_write_t;

#define SWITCH_MAX_AUX 2 /* B=の$ED000C/$ED0010(1件)、ID=の$ED006F+$ED0071(2件)のいずれか */

typedef struct {
    const char *key;
    uint32_t offset;
    int size; /* 1, 2, 4 バイト */
    int (*encode)(const char *value, uint32_t *out);
    void (*decode)(uint32_t value, char *buf, size_t buflen);
    const char *desc;
    /* 以下2つは複合キー(B=/ID=)専用。NULLなら従来通りoffset/size 1箇所のみの読み書き。 */
    int (*encode_aux)(const uint8_t *sram, const char *value, sram_write_t *aux,
                      int max); /* sram現在値を見て追加書き込み件数を返す */
    void (*decode_ctx)(const uint8_t *sram, char *buf, size_t buflen); /* sram全体を見て表示 */
} switch_def_t;

/* ---- 値の読み書き（ビッグエンディアン、68000ネイティブ） ---- */

static uint32_t read_be(const uint8_t *p, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v = (v << 8) | p[i];
    return v;
}
static void write_be(uint8_t *p, uint32_t v, int size) {
    for (int i = size - 1; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

/* ---- enum テーブル汎用ヘルパ ---- */

static int enum_encode(const enum_item_t *tbl, int n, const char *value, uint32_t *out) {
    for (int i = 0; i < n; i++) {
        if (strcasecmp(tbl[i].name, value) == 0) {
            *out = tbl[i].value;
            return 0;
        }
    }
    fprintf(stderr, "エラー: 値は次のいずれか: ");
    for (int i = 0; i < n; i++)
        fprintf(stderr, "%s%s", i ? "/" : "", tbl[i].name);
    fprintf(stderr, "（指定値: %s）\n", value);
    return -1;
}

static void enum_decode(const enum_item_t *tbl, int n, uint32_t value, char *buf,
                        size_t buflen) {
    for (int i = 0; i < n; i++) {
        if (tbl[i].value == value) {
            snprintf(buf, buflen, "%s", tbl[i].name);
            return;
        }
    }
    snprintf(buf, buflen, "$Unknown(%#x)", value);
}

/* ---- 起動デバイス (B=) ----
 * ファイル冒頭コメントの一次資料に基づく値。$ED0018の上位ニブルで大分類、
 * SCSI/ROM系は$ED000C、RAM系は$ED0010が実際の起動先を決める従属フィールド。
 */

#define SRAM_OFF_BOOT_PTR 0x0Cu /* $ED000C: $Axxx起動時に二重間接jsrされるベクタの在処 */
#define SRAM_OFF_RAM_PTR  0x10u /* $ED0010: $Bxxx起動時のjsr先 */
#define SRAM_OFF_BOOT_DEV 0x18u /* $ED0018: 起動デバイス */

#define SCSI_ROM_INT    0x00FC0000u /* 内蔵SCSI BIOS ROM(scsi_rom_data.h)。ID nのベクタ=+n*4 */
/* SWITCH.X本体はID7をホストIDとして拒否する。XEiJの0-7許容は独自の便利機能。 */
#define SCSI_ID_MAX_ENC 6u
/* ROMのベクタ表は8本あるため、既存値の表示はID7まで解釈する。 */
#define SCSI_ID_MAX_DEC 7u
#define SASI_UNIT_MAX   15u         /* HD<n>のn上限。SWITCH.X L0007caのcmp.l #$10準拠 */
#define TWOHD_UNIT_MAX  3u          /* 2HD<n>のn上限。SWITCH.X L0007e0のcmp.l #4準拠 */
#define ROM_BOOT_DEF    0x00BFFFFCu /* SWITCH.X L000760の引数省略時既定値 */
#define RAM_BOOT_DEF    0x00ED0100u /* SWITCH.X L0004bcの引数省略時(または$0指定)既定値 */
#define RAM_BOOT_ALT    0x00ED0C40u /* SWITCH.X L0004bcの$1指定時 */

typedef struct {
    uint32_t dev; /* $ED0018に書く値 */
    int aux_used;
    uint32_t aux_off; /* SRAM_OFF_BOOT_PTR または SRAM_OFF_RAM_PTR */
    uint32_t aux_val;
    const char *warn; /* 非NULLならencode_boot()側でstderrへ出す */
} boot_spec_t;

static const char HD_WARN_TEXT[] =
    "警告: HD<n>は本体組み込みSASI HDC($E96001)直結の起動経路です。\n"
    "  - SASIはブロック長256・LBA0=IPL本体を前提とするため、ix68kの.HDS\n"
    "    (SCSI-HD/ブロック長512/LBA0=デバイスパラメータ、+$400=第1IPL)からは起動できません。\n"
    "    .HDSから起動したい場合はB=SCSI<id>を使ってください。\n"
    "  - nはSASI IDではなく論理ドライブ番号です(SASI ID=n/2, LUN=n%2、IPLROM $FF9A26実測)。\n";

/* prefix(大小文字不問)に続く残りが10進数字のみ(空文字列も可、その場合idx=0)なら1を返す。
 * SWITCH.X L000e6e(インデックス解析、桁なし=0)準拠。 */
static int match_index(const char *v, const char *prefix, unsigned long *idx) {
    size_t plen = strlen(prefix);
    if (strncasecmp(v, prefix, plen) != 0)
        return 0;
    const char *rest = v + plen;
    if (*rest == '\0') {
        *idx = 0;
        return 1;
    }
    char *end;
    unsigned long n = strtoul(rest, &end, 10);
    if (*end != '\0')
        return 0;
    *idx = n;
    return 1;
}

/* prefixに続く残りが 空 / "$16進" / "10進" なら1を返す。SWITCH.X L000e22準拠。 */
static int match_addr(const char *v, const char *prefix, uint32_t *addr, int *has_arg) {
    size_t plen = strlen(prefix);
    if (strncasecmp(v, prefix, plen) != 0)
        return 0;
    const char *rest = v + plen;
    if (*rest == '\0') {
        *has_arg = 0;
        *addr = 0;
        return 1;
    }
    char *end;
    unsigned long n = (rest[0] == '$') ? strtoul(rest + 1, &end, 16) : strtoul(rest, &end, 10);
    if (*end != '\0' || (rest[0] == '$' && rest[1] == '\0'))
        return 0;
    *has_arg = 1;
    *addr = (uint32_t)n;
    return 1;
}

static int boot_err(const char *msg, const char *value) {
    if (msg)
        fprintf(stderr, "エラー: %s（指定値: %s）\n", msg, value);
    else
        fprintf(stderr,
                "エラー: B= の値は STD / 2HD0〜2HD3 / SCSI0〜SCSI6 / HD0〜HD15 / "
                "ROM[$xxxxxx] / RAM[$xxxxxx] のいずれか（指定値: %s）\n",
                value);
    return -1;
}

static int boot_parse(const char *v, boot_spec_t *sp) {
    unsigned long n;
    uint32_t a;
    int has;
    memset(sp, 0, sizeof(*sp));

    if (strcasecmp(v, "STD") == 0) {
        sp->dev = 0x0000u;
        return 0;
    }
    /* "2HD"は"HD"より先に判定する(先頭文字が違うので実害はないが意図を明示) */
    if (match_index(v, "2HD", &n)) {
        if (n > TWOHD_UNIT_MAX)
            return boot_err("2HDのドライブ番号は0〜3", v);
        sp->dev = 0x9000u + (uint32_t)n * 0x100u + 0x70u;
        return 0;
    }
    if (match_index(v, "HD", &n)) {
        if (n > SASI_UNIT_MAX)
            return boot_err("HDのドライブ番号は0〜15", v);
        sp->dev = 0x8000u + (uint32_t)n * 0x100u;
        sp->warn = HD_WARN_TEXT;
        return 0;
    }
    if (match_index(v, "SCSI", &n)) {
        if (n > SCSI_ID_MAX_ENC)
            return boot_err("SCSIのIDは0〜6(ID7は本体ホストID、実機SWITCH.X本体も7以上を拒否)",
                            v);
        sp->dev = 0xA000u;
        sp->aux_used = 1;
        sp->aux_off = SRAM_OFF_BOOT_PTR;
        sp->aux_val = SCSI_ROM_INT + (uint32_t)n * 4u;
        return 0;
    }
    if (match_addr(v, "ROM", &a, &has)) {
        if (!has || a == 0) {
            a = ROM_BOOT_DEF;
        } else if ((a & 1u) || a < 0x00200000u || a > 0x00FFFFFEu) {
            return boot_err("ROM$は$00200000〜$00FFFFFEの偶数アドレス", v);
        }
        sp->dev = 0xA000u;
        sp->aux_used = 1;
        sp->aux_off = SRAM_OFF_BOOT_PTR;
        sp->aux_val = a;
        return 0;
    }
    if (match_addr(v, "RAM", &a, &has)) {
        if (!has || a == 0) {
            a = RAM_BOOT_DEF;
        } else if (a == 1) {
            a = RAM_BOOT_ALT;
        } else if (a < RAM_BOOT_DEF || a >= 0x00ED4000u) {
            return boot_err("RAM$は$00ED0100〜$00ED3FFF($0=既定/$1=$00ED0C40)", v);
        }
        sp->dev = 0xB000u;
        sp->aux_used = 1;
        sp->aux_off = SRAM_OFF_RAM_PTR;
        sp->aux_val = a;
        return 0;
    }
    return boot_err(NULL, v);
}

static int encode_boot(const char *value, uint32_t *out) {
    boot_spec_t sp;
    if (boot_parse(value, &sp) != 0)
        return -1;
    if (sp.warn)
        fputs(sp.warn, stderr);
    *out = sp.dev;
    return 0;
}

/* B=専用の追加書き込み算出。switch_def_t.encode_auxに登録する。 */
static int encode_boot_aux(const uint8_t *sram, const char *value, sram_write_t *aux, int max) {
    (void)sram;
    boot_spec_t sp;
    if (boot_parse(value, &sp) != 0)
        return -1; /* 通常はencode_bootで先に弾かれる */
    if (!sp.aux_used)
        return 0;
    if (max < 1)
        return -1;
    aux[0].offset = sp.aux_off;
    aux[0].size = 4;
    aux[0].value = sp.aux_val;
    return 1;
}

/* $ED0018単独で決まる分だけを表示する（decode_ctx未使用経路の保険）。 */
static void decode_boot_dev(uint32_t dev, char *buf, size_t buflen) {
    unsigned u = (unsigned)((dev >> 8) & 0x0Fu);
    unsigned lo = (unsigned)(dev & 0xFFu);
    if (dev == 0x0000u) {
        snprintf(buf, buflen, "STD");
        return;
    }
    if ((dev & 0xF000u) == 0x8000u && lo == 0x00u) {
        snprintf(buf, buflen, "HD%u", u);
        return;
    }
    if ((dev & 0xF000u) == 0x9000u && lo == 0x70u && u < 4) {
        snprintf(buf, buflen, "2HD%u", u);
        return;
    }
    snprintf(buf, buflen, "$Unknown($%04X)", (unsigned)dev);
}

/* $ED000C/$ED0010まで見て一意に決める。show()/apply_setting()が使う。 */
static void decode_boot_ctx(const uint8_t *sram, char *buf, size_t buflen) {
    uint32_t dev = read_be(sram + SRAM_OFF_BOOT_DEV, 2);

    if ((dev & 0xF000u) == 0xA000u) {
        uint32_t p = read_be(sram + SRAM_OFF_BOOT_PTR, 4);
        if (p >= SCSI_ROM_INT && p <= SCSI_ROM_INT + SCSI_ID_MAX_DEC * 4u && (p & 3u) == 0u) {
            snprintf(buf, buflen, "SCSI%u", (unsigned)((p - SCSI_ROM_INT) / 4u));
            return;
        }
        if (p == ROM_BOOT_DEF) {
            snprintf(buf, buflen, "ROM");
            return;
        }
        snprintf(buf, buflen, "ROM$%08X", (unsigned)p);
        return;
    }
    if ((dev & 0xF000u) == 0xB000u) {
        uint32_t p = read_be(sram + SRAM_OFF_RAM_PTR, 4);
        if (p == RAM_BOOT_DEF) {
            snprintf(buf, buflen, "RAM");
            return;
        }
        if (p == RAM_BOOT_ALT) {
            snprintf(buf, buflen, "RAM$1");
            return;
        }
        snprintf(buf, buflen, "RAM$%08X", (unsigned)p);
        return;
    }
    decode_boot_dev(dev, buf, buflen);
}

/* ---- On/Off 系 (EJECT) ---- */

static const enum_item_t ONOFF_TBL[] = {{"Off", 0}, {"On", 1}};
static int encode_onoff(const char *value, uint32_t *out) {
    return enum_encode(ONOFF_TBL, 2, value, out);
}
static void decode_onoff(uint32_t value, char *buf, size_t buflen) {
    enum_decode(ONOFF_TBL, 2, value, buf, buflen);
}

/* ---- OPT2KEY (O=) ---- */

static const enum_item_t OPT2KEY_TBL[] = {{"Tvctrl", 0}, {"Normal", 1}};
static int encode_opt2key(const char *value, uint32_t *out) {
    return enum_encode(OPT2KEY_TBL, 2, value, out);
}
static void decode_opt2key(uint32_t value, char *buf, size_t buflen) {
    enum_decode(OPT2KEY_TBL, 2, value, buf, buflen);
}

/* ---- KANA (K=) ---- */

static const enum_item_t KANA_TBL[] = {{"Jis", 0}, {"Aiu", 1}};
static int encode_kana(const char *value, uint32_t *out) {
    return enum_encode(KANA_TBL, 2, value, out);
}
static void decode_kana(uint32_t value, char *buf, size_t buflen) {
    enum_decode(KANA_TBL, 2, value, buf, buflen);
}

/* ---- LCD_MODE (L=) ---- */

static const enum_item_t LCD_TBL[] = {{"Lcd", 0}, {"Normal", 1}};
static int encode_lcd(const char *value, uint32_t *out) {
    return enum_encode(LCD_TBL, 2, value, out);
}
static void decode_lcd(uint32_t value, char *buf, size_t buflen) {
    enum_decode(LCD_TBL, 2, value, buf, buflen);
}

/* ---- SRAM 使用モード (S=) ---- */

static const enum_item_t SRAMMODE_TBL[] = {
    {"No_use", 0}, {"N", 0}, {"Ramdisk", 1}, {"R", 1}, {"Program", 2}, {"P", 2},
};
static int encode_sram_mode(const char *value, uint32_t *out) {
    return enum_encode(SRAMMODE_TBL, 6, value, out);
}
static void decode_sram_mode(uint32_t value, char *buf, size_t buflen) {
    /* 逆引きは重複しない代表値 (No_use/Ramdisk/Program) を使う */
    static const enum_item_t rev[] = {{"No_use", 0}, {"Ramdisk", 1}, {"Program", 2}};
    enum_decode(rev, 3, value, buf, buflen);
}

/* ---- 整数値系 (範囲チェックのみ) ---- */

static int encode_uint_range(const char *value, uint32_t *out, uint32_t lo, uint32_t hi) {
    char *end;
    long v = strtol(value, &end, 0);
    if (*end != '\0' || v < (long)lo || v > (long)hi) {
        fprintf(stderr, "エラー: 値は %u〜%u の範囲（指定値: %s）\n", lo, hi, value);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}
static void decode_uint(uint32_t value, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%u", value);
}

static int encode_contrast(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 15);
}
static int encode_repeat_param(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 15);
}
static int encode_xchg(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 7);
}
static int encode_hd_max(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 16);
}

/* ---- SCSI本体ID (ID=) ----
 * 実機はL000684で内蔵/外付けSCSIボードをハードウェア検出するが、ix68kは内蔵
 * SCSIのみ実装のため、本ツールはID=0-7(内蔵)のみ受理する（$ED0070のbit3=外付けフラグは
 * 常に0固定）。マジックバイト($ED006F=$56)が未設定なら$ED0071(ID別ブート禁止ビットマップ)
 * と併せて初期化する（実機L0006d0-L0006f2準拠）。ファイル冒頭コメント参照。
 */

#define SRAM_OFF_SCSI_MAGIC        0x6Fu /* $ED006F: マジックバイト */
#define SRAM_OFF_SCSI_ID           0x70u /* $ED0070: bit0-2=本体SCSI ID、bit3=外付けフラグ(本ツール非対応) */
#define SRAM_OFF_SCSI_BOOT_DISABLE 0x71u /* $ED0071: ID別ブート禁止ビットマップ */
#define SCSI_ID_MAGIC 0x56u /* 'V' */

static int encode_scsi_id(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 7);
}
static void decode_scsi_id(uint32_t value, char *buf, size_t buflen) {
    /* bit3(外付けフラグ)は本ツールでは常に書かないが、過去データ等で立っていても表示上は
     * 無視する（往復性: 表示値をそのままID=に渡すとbit3はクリアされる＝内蔵へ是正される）。 */
    snprintf(buf, buflen, "%u", value & 0x07u);
}

/* マジックバイト未設定時のみ$ED006F/$ED0071を初期化する。設定済みなら何もしない
 * （既存のブート禁止ビットマップ設定を保持する）。 */
static int encode_scsi_id_aux(const uint8_t *sram, const char *value, sram_write_t *aux, int max) {
    (void)value;
    if (sram[SRAM_OFF_SCSI_MAGIC] == SCSI_ID_MAGIC)
        return 0;
    if (max < 2)
        return -1;
    aux[0].offset = SRAM_OFF_SCSI_MAGIC;
    aux[0].size = 1;
    aux[0].value = SCSI_ID_MAGIC;
    aux[1].offset = SRAM_OFF_SCSI_BOOT_DISABLE;
    aux[1].size = 1;
    aux[1].value = 0;
    return 2;
}

/* ---- メモリサイズ (M=, KB単位で入力/表示、SRAM上は総バイト数) ---- */

static int encode_memory(const char *value, uint32_t *out) {
    char *end;
    long kb = strtol(value, &end, 0);
    if (*end != '\0' || kb <= 0) {
        fprintf(stderr, "エラー: メモリサイズはKB単位の正の整数（指定値: %s）\n", value);
        return -1;
    }
    *out = (uint32_t)kb * 1024u;
    return 0;
}
static void decode_memory(uint32_t value, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%u", value / 1024u);
}

/* ---- 16進ワード値 (パレット、$xxxx) ---- */

static int encode_hex_word(const char *value, uint32_t *out) {
    if (value[0] == '$')
        value++;
    char *end;
    long v = strtol(value, &end, 16);
    if (*end != '\0' || v < 0 || v > 0xFFFF) {
        fprintf(stderr, "エラー: 値は $0000〜$FFFF の16進（指定値: %s）\n", value);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}
static void decode_hex_word(uint32_t value, char *buf, size_t buflen) {
    snprintf(buf, buflen, "$%04X", value);
}

/* ---- 32bit生値 (WAIT_PRN、$xxxxxxxx または10進) ---- */

static int encode_raw32(const char *value, uint32_t *out) {
    char *end;
    long v = strtol(value, &end, 0);
    if (*end != '\0' || v < 0) {
        fprintf(stderr, "エラー: 数値を指定すること（指定値: %s）\n", value);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}
static void decode_raw32_hex(uint32_t value, char *buf, size_t buflen) {
    snprintf(buf, buflen, "$%X", value);
}

/* ---- TV_CTRL ($xx 2桁16進、0-15 に丸めない生値) ---- */

static int encode_tv_ctrl(const char *value, uint32_t *out) {
    return encode_uint_range(value, out, 0, 0x3F);
}
static void decode_tv_ctrl(uint32_t value, char *buf, size_t buflen) {
    snprintf(buf, buflen, "$%02X", value);
}

/* ---- RS232C (R=, $ED001A) ----
 * SWITCH.X Ver2.20実機逆アセンブル(L000c1e-L000da6、Issue #115)に基づくビットフィールド。
 * 実機は"9600bps Bits-8 Parity-None Stop-1 Xon"のような順不同・省略可なトークン列を
 * 許すが、本ツールは固定順カンマ区切り R=<baud>,<bits>,<parity>,<stop>,<flow>（全5
 * フィールド必須）のみ受理する。ファイル冒頭コメント参照。
 */

#define RS232_BAUD_MASK   0x000Fu /* bit0-3: ボーレート */
#define RS232_RS_BIT      0x0080u /* bit7: RS/CS制御 */
#define RS232_XON_BIT     0x0200u /* bit9: Xon/Xoff制御 */
#define RS232_BITS_MASK   0x0C00u /* bit10-11: データビット */
#define RS232_PARITY_MASK 0x3000u /* bit12-13: パリティ */
#define RS232_STOP_MASK   0xC000u /* bit14-15: ストップビット */

static const enum_item_t RS232_BAUD_TBL[] = {
    {"75", 0}, {"150", 1}, {"300", 2}, {"600", 3}, {"1200", 4},
    {"2400", 5}, {"4800", 6}, {"9600", 7}, {"19200", 8},
};
#define RS232_BAUD_N (int)(sizeof(RS232_BAUD_TBL) / sizeof(RS232_BAUD_TBL[0]))

static const enum_item_t RS232_BITS_TBL[] = {
    {"5", 0x0000}, {"6", 0x0400}, {"7", 0x0800}, {"8", 0x0C00},
};
#define RS232_BITS_N (int)(sizeof(RS232_BITS_TBL) / sizeof(RS232_BITS_TBL[0]))

static const enum_item_t RS232_PARITY_TBL[] = {
    {"N", 0x0000}, {"O", 0x1000}, {"E", 0x3000},
};
#define RS232_PARITY_N (int)(sizeof(RS232_PARITY_TBL) / sizeof(RS232_PARITY_TBL[0]))

static const enum_item_t RS232_STOP_TBL[] = {
    {"1", 0x4000}, {"2", 0xC000},
};
#define RS232_STOP_N (int)(sizeof(RS232_STOP_TBL) / sizeof(RS232_STOP_TBL[0]))

static const enum_item_t RS232_FLOW_TBL[] = {
    {"N", 0x0000}, {"X", RS232_XON_BIT}, {"R", RS232_RS_BIT},
};
#define RS232_FLOW_N (int)(sizeof(RS232_FLOW_TBL) / sizeof(RS232_FLOW_TBL[0]))

/* カンマ区切りトークンを1つ取り出しtokへコピー、*pをカンマの次(または終端)へ進める。 */
static int next_csv_token(const char **p, char *tok, size_t toklen) {
    const char *comma = strchr(*p, ',');
    size_t len = comma ? (size_t)(comma - *p) : strlen(*p);
    if (len == 0 || len >= toklen)
        return -1;
    memcpy(tok, *p, len);
    tok[len] = '\0';
    *p = comma ? comma + 1 : *p + len;
    return 0;
}

static void rs232c_format_error(const char *value) {
    fprintf(stderr,
            "エラー: 値は <baud>,<bits>,<parity>,<stop>,<flow> 形式（baud="
            "75/150/300/600/1200/2400/4800/9600/19200、bits=5/6/7/8、parity=N/O/E、"
            "stop=1/2、flow=N/X/R。指定値: %s）\n",
            value);
}

static int encode_rs232c(const char *value, uint32_t *out) {
    char tok[8];
    const char *p = value;
    uint32_t baud, bits, parity, stop, flow;

    if (next_csv_token(&p, tok, sizeof(tok)) != 0) {
        rs232c_format_error(value);
        return -1;
    }
    if (enum_encode(RS232_BAUD_TBL, RS232_BAUD_N, tok, &baud) != 0)
        return -1;
    if (next_csv_token(&p, tok, sizeof(tok)) != 0) {
        rs232c_format_error(value);
        return -1;
    }
    if (enum_encode(RS232_BITS_TBL, RS232_BITS_N, tok, &bits) != 0)
        return -1;
    if (next_csv_token(&p, tok, sizeof(tok)) != 0) {
        rs232c_format_error(value);
        return -1;
    }
    if (enum_encode(RS232_PARITY_TBL, RS232_PARITY_N, tok, &parity) != 0)
        return -1;
    if (next_csv_token(&p, tok, sizeof(tok)) != 0) {
        rs232c_format_error(value);
        return -1;
    }
    if (enum_encode(RS232_STOP_TBL, RS232_STOP_N, tok, &stop) != 0)
        return -1;
    if (next_csv_token(&p, tok, sizeof(tok)) != 0) {
        rs232c_format_error(value);
        return -1;
    }
    if (enum_encode(RS232_FLOW_TBL, RS232_FLOW_N, tok, &flow) != 0)
        return -1;
    if (*p != '\0') { /* 余分なトークンが残っている */
        rs232c_format_error(value);
        return -1;
    }

    *out = baud | bits | parity | stop | flow;
    return 0;
}

static void decode_rs232c(uint32_t value, char *buf, size_t buflen) {
    char baud[24], bits[24], parity[24], stop[24], flow[24];
    enum_decode(RS232_BAUD_TBL, RS232_BAUD_N, value & RS232_BAUD_MASK, baud, sizeof(baud));
    enum_decode(RS232_BITS_TBL, RS232_BITS_N, value & RS232_BITS_MASK, bits, sizeof(bits));
    enum_decode(RS232_PARITY_TBL, RS232_PARITY_N, value & RS232_PARITY_MASK, parity,
                sizeof(parity));
    enum_decode(RS232_STOP_TBL, RS232_STOP_N, value & RS232_STOP_MASK, stop, sizeof(stop));
    enum_decode(RS232_FLOW_TBL, RS232_FLOW_N, value & (RS232_RS_BIT | RS232_XON_BIT), flow,
                sizeof(flow));
    snprintf(buf, buflen, "%s,%s,%s,%s,%s", baud, bits, parity, stop, flow);
}

/* ---- 対応スイッチ定義テーブル ---- */

/* B/BOOTのみencode_aux/decode_ctxを持つ複合キー。定義重複を避けるためマクロ化する。 */
#define BOOT_ROW(k)                                                                  \
    {.key = k,                                                                       \
     .offset = SRAM_OFF_BOOT_DEV,                                                    \
     .size = 2,                                                                      \
     .encode = encode_boot,                                                          \
     .decode = decode_boot_dev,                                                      \
     .desc = "起動デバイス (STD/2HD0-3/SCSI0-6/HD0-15/ROM/RAM。SCSI・ROMは$ED000C、" \
             "RAMは$ED0010も同時に更新)",                                            \
     .encode_aux = encode_boot_aux,                                                  \
     .decode_ctx = decode_boot_ctx}

static const switch_def_t SWITCHES[] = {
    BOOT_ROW("B"),
    BOOT_ROW("BOOT"),
    {.key = "M",
     .offset = 0x08,
     .size = 4,
     .encode = encode_memory,
     .decode = decode_memory,
     .desc = "メインメモリサイズ (KB)"},
    {.key = "MEMORY",
     .offset = 0x08,
     .size = 4,
     .encode = encode_memory,
     .decode = decode_memory,
     .desc = "メインメモリサイズ (KB)"},
    {.key = "E",
     .offset = 0x29,
     .size = 1,
     .encode = encode_onoff,
     .decode = decode_onoff,
     .desc = "電源OFF時の自動イジェクト (On/Off)"},
    {.key = "EJECT",
     .offset = 0x29,
     .size = 1,
     .encode = encode_onoff,
     .decode = decode_onoff,
     .desc = "電源OFF時の自動イジェクト"},
    {.key = "O",
     .offset = 0x27,
     .size = 1,
     .encode = encode_opt2key,
     .decode = decode_opt2key,
     .desc = "OPT.2キーの機能 (Tvctrl/Normal)"},
    {.key = "OPT2KEY",
     .offset = 0x27,
     .size = 1,
     .encode = encode_opt2key,
     .decode = decode_opt2key,
     .desc = "OPT.2キーの機能"},
    {.key = "C",
     .offset = 0x28,
     .size = 1,
     .encode = encode_contrast,
     .decode = decode_uint,
     .desc = "ディスプレイコントラスト (0-15)"},
    {.key = "CONTRAST",
     .offset = 0x28,
     .size = 1,
     .encode = encode_contrast,
     .decode = decode_uint,
     .desc = "ディスプレイコントラスト"},
    {.key = "K",
     .offset = 0x2B,
     .size = 1,
     .encode = encode_kana,
     .decode = decode_kana,
     .desc = "かな配列 (Jis/Aiu)"},
    {.key = "KANA",
     .offset = 0x2B,
     .size = 1,
     .encode = encode_kana,
     .decode = decode_kana,
     .desc = "かな配列"},
    {.key = "L",
     .offset = 0x2C,
     .size = 1,
     .encode = encode_lcd,
     .decode = decode_lcd,
     .desc = "電卓表示モード (Lcd/Normal)"},
    {.key = "LCD_MODE",
     .offset = 0x2C,
     .size = 1,
     .encode = encode_lcd,
     .decode = decode_lcd,
     .desc = "電卓表示モード"},
    {.key = "S",
     .offset = 0x2D,
     .size = 1,
     .encode = encode_sram_mode,
     .decode = decode_sram_mode,
     .desc = "SRAM使用モード (No_use/Ramdisk/Program)"},
    {.key = "SRAM",
     .offset = 0x2D,
     .size = 1,
     .encode = encode_sram_mode,
     .decode = decode_sram_mode,
     .desc = "SRAM使用モード"},
    {.key = "P0",
     .offset = 0x2E,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット0"},
    {.key = "P1",
     .offset = 0x30,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット1"},
    {.key = "P2",
     .offset = 0x32,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット2"},
    {.key = "P3",
     .offset = 0x34,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット3"},
    {.key = "P4",
     .offset = 0x36,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット4-7"},
    {.key = "P8",
     .offset = 0x38,
     .size = 2,
     .encode = encode_hex_word,
     .decode = decode_hex_word,
     .desc = "テキストパレット8-15"},
    {.key = "F",
     .offset = 0x3A,
     .size = 1,
     .encode = encode_repeat_param,
     .decode = decode_uint,
     .desc = "キーオートリピート開始間隔 (0-15)"},
    {.key = "FIRST_KY",
     .offset = 0x3A,
     .size = 1,
     .encode = encode_repeat_param,
     .decode = decode_uint,
     .desc = "キーオートリピート開始間隔"},
    {.key = "N",
     .offset = 0x3B,
     .size = 1,
     .encode = encode_repeat_param,
     .decode = decode_uint,
     .desc = "キーリピート間隔 (0-15)"},
    {.key = "NEXT_KEY",
     .offset = 0x3B,
     .size = 1,
     .encode = encode_repeat_param,
     .decode = decode_uint,
     .desc = "キーリピート間隔"},
    {.key = "W",
     .offset = 0x3C,
     .size = 4,
     .encode = encode_raw32,
     .decode = decode_raw32_hex,
     .desc = "プリンタタイムアウト時間"},
    {.key = "WAIT_PRN",
     .offset = 0x3C,
     .size = 4,
     .encode = encode_raw32,
     .decode = decode_raw32_hex,
     .desc = "プリンタタイムアウト時間"},
    {.key = "TV_CTRL",
     .offset = 0x2A,
     .size = 1,
     .encode = encode_tv_ctrl,
     .decode = decode_tv_ctrl,
     .desc = "電源OFF時のTV制御コード"},
    {.key = "X",
     .offset = 0x59,
     .size = 1,
     .encode = encode_xchg,
     .decode = decode_uint,
     .desc = "XCHG、キー入れ替え設定 (0-7、bit0-2)"},
    {.key = "XCHG",
     .offset = 0x59,
     .size = 1,
     .encode = encode_xchg,
     .decode = decode_uint,
     .desc = "XCHG、キー入れ替え設定"},
    {.key = "HD_MAX",
     .offset = 0x5A,
     .size = 1,
     .encode = encode_hd_max,
     .decode = decode_uint,
     .desc = "内蔵SASI HDCのドライブ数上限 (0-16)"},
    {.key = "ID",
     .offset = SRAM_OFF_SCSI_ID,
     .size = 1,
     .encode = encode_scsi_id,
     .decode = decode_scsi_id,
     .desc = "内蔵SCSI本体ID (0-7、既定7。外付けボードはix68k非対応)",
     .encode_aux = encode_scsi_id_aux},
    {.key = "R",
     .offset = 0x1A,
     .size = 2,
     .encode = encode_rs232c,
     .decode = decode_rs232c,
     .desc = "RS232C設定 (baud,bits,parity,stop,flow。例: 9600,8,N,1,X)"},
};
#define SWITCHES_N (int)(sizeof(SWITCHES) / sizeof(SWITCHES[0]))

/* 表示用の代表キー（B/BOOT のような別名重複を除く） */
static const char *DISPLAY_KEYS[] = {
    "B",  "M",  "E",  "O",  "C",  "K", "L", "S", "P0", "P1", "P2", "P3",
    "P4", "P8", "F",  "N",  "W",  "TV_CTRL", "X", "HD_MAX", "ID", "R",
};
#define DISPLAY_KEYS_N (int)(sizeof(DISPLAY_KEYS) / sizeof(DISPLAY_KEYS[0]))

static const switch_def_t *find_switch(const char *key) {
    for (int i = 0; i < SWITCHES_N; i++) {
        if (strcasecmp(SWITCHES[i].key, key) == 0)
            return &SWITCHES[i];
    }
    return NULL;
}

static void show(const uint8_t *sram) {
    printf("%-9s%-10s%-20s%s\n", "KEY", "ADDR", "現在値", "説明");
    for (int i = 0; i < DISPLAY_KEYS_N; i++) {
        const switch_def_t *sw = find_switch(DISPLAY_KEYS[i]);
        char buf[32];
        if (sw->decode_ctx)
            sw->decode_ctx(sram, buf, sizeof(buf));
        else
            sw->decode(read_be(sram + sw->offset, sw->size), buf, sizeof(buf));
        printf("%-9s$%06X  %-20s%s\n", sw->key, SRAM_BASE + sw->offset, buf, sw->desc);
    }
}

static int apply_setting(uint8_t *sram, const char *token) {
    const char *eq = strchr(token, '=');
    if (!eq) {
        fprintf(stderr, "エラー: KEY=VALUE 形式で指定すること（指定値: %s）\n", token);
        return -1;
    }
    char key[16];
    size_t klen = (size_t)(eq - token);
    if (klen >= sizeof(key))
        klen = sizeof(key) - 1;
    memcpy(key, token, klen);
    key[klen] = '\0';
    const char *value = eq + 1;

    const switch_def_t *sw = find_switch(key);
    if (!sw) {
        fprintf(stderr, "エラー: 未知のキーワード: %s\n", key);
        return -1;
    }

    /* 全件のencode検証を先に済ませてから書き込む（部分適用を作らない）。 */
    sram_write_t w[1 + SWITCH_MAX_AUX];
    int nw = 0;
    if (sw->encode(value, &w[nw].value) != 0)
        return -1;
    w[nw].offset = sw->offset;
    w[nw].size = sw->size;
    nw++;
    if (sw->encode_aux) {
        int k = sw->encode_aux(sram, value, &w[nw], SWITCH_MAX_AUX);
        if (k < 0)
            return -1;
        nw += k;
    }
    for (int i = 0; i < nw; i++)
        write_be(sram + w[i].offset, w[i].value, w[i].size);

    char buf[32];
    if (sw->decode_ctx)
        sw->decode_ctx(sram, buf, sizeof(buf));
    else
        sw->decode(w[0].value, buf, sizeof(buf));
    printf("%s=$%06X <- %s (%s)\n", key, SRAM_BASE + sw->offset, buf, sw->desc);
    for (int i = 1; i < nw; i++)
        printf("%*s $%06X <- $%0*X\n", (int)strlen(key), "", SRAM_BASE + w[i].offset,
               w[i].size * 2, w[i].value);
    return 0;
}

#define SRAM_SWITCH_VERSION "1.0"

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char *path = "sram.dat";
    const char **settings = malloc(sizeof(char *) * (size_t)argc);
    int n_settings = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("sram_switch %s\n", SRAM_SWITCH_VERSION);
            free(settings);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            settings[n_settings++] = argv[i];
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "エラー: %s が見つかりません\n", path);
        free(settings);
        return 1;
    }
    uint8_t sram[SRAM_SIZE];
    size_t n = fread(sram, 1, SRAM_SIZE, f);
    fclose(f);
    if (n != SRAM_SIZE) {
        fprintf(stderr, "警告: %s のサイズが %zu バイト（想定 %u）です\n", path, n, SRAM_SIZE);
    }

    if (n_settings == 0) {
        show(sram);
        free(settings);
        return 0;
    }

    for (int i = 0; i < n_settings; i++) {
        if (apply_setting(sram, settings[i]) != 0) {
            free(settings);
            return 1;
        }
    }
    free(settings);

    char bak_path[1024];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    FILE *bf = fopen(path, "rb");
    FILE *bw = fopen(bak_path, "wb");
    if (bf && bw) {
        uint8_t buf[4096];
        size_t rn;
        while ((rn = fread(buf, 1, sizeof(buf), bf)) > 0)
            fwrite(buf, 1, rn, bw);
    }
    if (bf)
        fclose(bf);
    if (bw)
        fclose(bw);

    FILE *wf = fopen(path, "wb");
    if (!wf) {
        fprintf(stderr, "エラー: %s に書き込めません\n", path);
        return 1;
    }
    fwrite(sram, 1, SRAM_SIZE, wf);
    fclose(wf);
    printf("保存しました: %s（バックアップ: %s）\n", path, bak_path);
    return 0;
}
