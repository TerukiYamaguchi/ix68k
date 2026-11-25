# tools-x68k/joyget_test/Makefile — JOYGET_TEST.X（配布用ジョイパッド確認ツール）のビルド
#
# ソースは tools-x68k/joyget_test/joyget_test.s。IOCS _JOYGET の値を画面に表示するだけの
# 使い切りプログラムのため、標準スタートアップを使わずリンクする（-nostartfiles、
# エントリポイントは_start）。ビルド済みの joyget_test.x をGitで管理し、Release ZIPへは
# それを JOYGET_TEST.X としてコピーする（m68k-xelfの無い環境でもビルドせず同梱できる）。
# joyget_test.s を変更したら再ビルドして joyget_test.x を再コミットする。

CROSS = m68k-xelf-
AS    = $(CROSS)as
CC    = $(CROSS)gcc

ELF2X68K = /opt/homebrew/opt/elf2x68k/bin/elf2x68k.py
SRC      = joyget_test.s

all: joyget_test.x

joyget_test.x: joyget_test.elf
	python3 $(ELF2X68K) -o $@ $<

joyget_test.elf: joyget_test.o
	$(CC) -m68000 -nostdlib -nostartfiles -Wl,-e,_start -o $@ $<

joyget_test.o: $(SRC)
	$(AS) -m68000 -o $@ $<

clean:
	-rm -f joyget_test.o joyget_test.elf joyget_test.x

.PHONY: all clean
