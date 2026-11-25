/*
 * sjis.c - Shift-JIS ⇔ UTF-8 変換
 *
 * Human68k のファイル名・拡張子・ボリューム名・ディレクトリ名はすべて Shift-JIS。
 * ディスク上の名前は SJIS バイト列として扱い、ホスト（macOS/Linux 端末＝UTF-8）
 * との境界で変換する。経路区切り '/'(0x2F) は SJIS のトレイルバイト範囲
 * (0x40-0x7E,0x80-0xFC) 外なので、SJIS バイト列を '/' で単純分割しても安全。
 */
#include "sjis.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h> /* MultiByteToWideChar/WideCharToMultiByte（Windowsにiconvは無い） */
#else
#include <iconv.h>
#endif

/* SJIS の 2 バイト文字のリードバイトか */
int is_sjis_lead(unsigned char c) {
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

/* 純 ASCII か？（ASCII なら変換不要で iconv/Win32 API 非依存に動ける） */
static int is_ascii(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] & 0x80)
            return 0;
    return 1;
}

#ifdef _WIN32
/* CP932(SJIS)⇔UTF-8 は共にUnicodeの符号化なので、UTF-16を中間経由して変換する
 * （WindowsにiconvはCP932↔UTF-8を直接繋ぐ標準APIが無いため）。
 * ファイル名程度の短い文字列のみ扱う前提でwbufは固定長。 */
static int conv_via_utf16(UINT from_cp, UINT to_cp, const char *in, size_t inlen, char *out,
                          size_t outsz) {
    if (is_ascii(in, inlen)) { /* ASCII は素通し（両符号で同一） */
        if (inlen >= outsz)
            return -1;
        memcpy(out, in, inlen);
        out[inlen] = 0;
        return (int)inlen;
    }
    wchar_t wbuf[1024];
    int wlen = MultiByteToWideChar(from_cp, 0, in, (int)inlen, wbuf,
                                   (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wlen <= 0)
        return -1;
    int blen = WideCharToMultiByte(to_cp, 0, wbuf, wlen, out, (int)(outsz - 1), NULL, NULL);
    if (blen <= 0)
        return -1;
    out[blen] = 0;
    return blen;
}

/* UTF-8（ホスト） → Shift-JIS（ディスク）。CP932 は SJIS の広く使える上位互換。 */
int utf8_to_sjis(const char *in, char *out, size_t outsz) {
    return conv_via_utf16(CP_UTF8, 932, in, strlen(in), out, outsz);
}

/* Shift-JIS（ディスク） → UTF-8（表示用） */
int sjis_to_utf8(const char *in, size_t inlen, char *out, size_t outsz) {
    return conv_via_utf16(932, CP_UTF8, in, inlen, out, outsz);
}
#else
/* iconv ラッパー。成功=出力バイト数, 失敗=-1。out は NUL 終端する。 */
static int conv_enc(const char *tocode, const char *fromcode, const char *in, size_t inlen,
                    char *out, size_t outsz) {
    if (is_ascii(in, inlen)) { /* ASCII は素通し（両符号で同一） */
        if (inlen >= outsz)
            return -1;
        memcpy(out, in, inlen);
        out[inlen] = 0;
        return (int)inlen;
    }
    iconv_t cd = iconv_open(tocode, fromcode);
    if (cd == (iconv_t)-1)
        return -1;
    char *inp = (char *)in;
    size_t inleft = inlen;
    char *outp = out;
    size_t outleft = outsz - 1;
    size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
    iconv_close(cd);
    if (r == (size_t)-1)
        return -1;
    *outp = 0;
    return (int)(outsz - 1 - outleft);
}

/* UTF-8（ホスト） → Shift-JIS（ディスク）。CP932 は SJIS の広く使える上位互換。 */
int utf8_to_sjis(const char *in, char *out, size_t outsz) {
    return conv_enc("CP932", "UTF-8", in, strlen(in), out, outsz);
}

/* Shift-JIS（ディスク） → UTF-8（表示用） */
int sjis_to_utf8(const char *in, size_t inlen, char *out, size_t outsz) {
    return conv_enc("UTF-8", "CP932", in, inlen, out, outsz);
}
#endif
