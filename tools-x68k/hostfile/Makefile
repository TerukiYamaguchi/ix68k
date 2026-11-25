# tools-x68k/hostfile/Makefile — HOSTFILE.X（Issue #39）のビルド
#
# HOSTDRV.SYS（-nostartfiles、tools-x68k/hostdrv/Makefile）とは別系統: HOSTFILE.X は
# argv と stdio(printf) が必要なため標準スタートアップをそのままリンクする
# （elf2x68k の x68k.specs が x68kcrt0.o + -lx68k -lx68kdos -lx68kiocs を自動リンク）。
# 動作検証済み（Issue #39 実装前チェックコメント参照）。

CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -Wall -Wextra

ELF2X68K = /opt/homebrew/opt/elf2x68k/bin/elf2x68k.py

all: HOSTFILE.X

HOSTFILE.X: hostfile.elf
	python3 $(ELF2X68K) -o $@ $<

hostfile.elf: hostfile.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	-rm -f hostfile.elf HOSTFILE.X

.PHONY: all clean
