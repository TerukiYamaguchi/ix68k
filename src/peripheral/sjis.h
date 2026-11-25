/*
 * sjis.h - Shift-JIS ⇔ UTF-8 変換（tools-host/hu68disk と共有（hu68disk の Makefile
 * がこのファイルを直接ビルドする））
 */
#ifndef SJIS_H
#define SJIS_H

#include <stddef.h>

int is_sjis_lead(unsigned char c);
int utf8_to_sjis(const char *in, char *out, size_t outsz);
int sjis_to_utf8(const char *in, size_t inlen, char *out, size_t outsz);

#endif
