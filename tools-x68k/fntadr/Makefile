# tools-x68k/fntadr/Makefile — FNTADR報告ツール（Issue #157）のビルド
#
# _FNTADR($16)の実アドレスを_B_PRINT($21)経由でエミュレータへ報告するだけの
# 常駐しない使い切りプログラムのため、HOSTDRV.SYSと同じく標準スタートアップを
# 使わずリンクする（-nostartfiles、エントリポイントは_start）。

CROSS = m68k-xelf-
AS    = $(CROSS)as
CC    = $(CROSS)gcc

ELF2X68K = /opt/homebrew/opt/elf2x68k/bin/elf2x68k.py

all: FNTADR.X

FNTADR.X: fntadr.elf
	python3 $(ELF2X68K) -o $@ $<

fntadr.elf: fntadr.o
	$(CC) -m68000 -nostdlib -nostartfiles -Wl,-e,_start -o $@ $<

fntadr.o: fntadr.s
	$(AS) -m68000 -o $@ $<

clean:
	-rm -f fntadr.o fntadr.elf FNTADR.X

.PHONY: all clean
