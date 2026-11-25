#!/bin/bash
# tools-x68k/asmtest/build.sh — GNU m68k asm -> Human68k .X -> XDF フロッピーへ書き込む
#
# 用途: 「実CPU上で狙った命令を1本だけ実行させて、例外/バスエラー等の実機挙動を
#        確認したい」ときの使い捨てテストプログラムを素早く作る。
#        NMIベクタ乗っ取り等のハックではなく、本物のHuman68k .Xとして実行できる。
#
# 使い方:
#   tools-x68k/asmtest/build.sh <foo.s> [XDFパス(既定 disk/WORK.XDF)]
#   ASMTEST_CPU=68030 ...  # 68020/68030 専用命令を使うテストだけ明示指定
#
# 前提 (brew install yunkya2/tap/elf2x68k で導入):
#   m68k-xelf-as / m68k-xelf-gcc  (/opt/homebrew/bin/)
#   elf2x68k.py                   (/opt/homebrew/opt/elf2x68k/bin/)
#   tools-host/hu68disk/hu68disk       (本プロジェクト自作、要ビルド済み)
#
# mtools(mcopy)は使わないこと: VFAT長名(LFN)エントリを生成しHuman68kが
# 「無効なメディアを使用しました」と誤認する既知の問題がある（2026-07-12発見）。
# ファイル名は Human68k ネイティブ長名の上限（拡張子込み最大21バイト）に
# 収まるよう <foo.s> のベース名を決めること（例: super_protect_test.s は
# ちょうど18バイトでOK、super_protect_ok_test.s は21バイト超で失敗する）。
#
# 書き込み後は必ずエミュレータを完全再起動すること（MCPのdisk(action="eject"/
# "insert", media="fd")による
# その場リロードではHuman68k側のディレクトリキャッシュが更新されず、
# 新しいファイルがDIRに出てこないことを確認済み）。

set -euo pipefail

SRC="${1:?usage: build.sh <foo.s> [xdf_path]}"
XDF="${2:-disk/WORK.XDF}"
CPU="${ASMTEST_CPU:-68000}"

case "$CPU" in
    68000|68010|68020|68030) ;;
    *)
        echo "error: ASMTEST_CPU must be 68000, 68010, 68020, or 68030" >&2
        exit 2
        ;;
esac

XELF_BIN=/opt/homebrew/bin
ELF2X68K=/opt/homebrew/opt/elf2x68k/bin/elf2x68k.py
HU68DISK="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../tools-host/hu68disk" && pwd)/hu68disk"

NAME=$(basename "$SRC" .s)
UPPER=$(echo "$NAME" | tr '[:lower:]' '[:upper:]')
WORKDIR=$(mktemp -d)

"$XELF_BIN/m68k-xelf-as" -m"$CPU" -o "$WORKDIR/$NAME.o" "$SRC"
"$XELF_BIN/m68k-xelf-gcc" -m"$CPU" -nostdlib -nostartfiles -Wl,-e,_start \
    -o "$WORKDIR/$NAME.elf" "$WORKDIR/$NAME.o"
"$ELF2X68K" -o "$WORKDIR/$UPPER.X" "$WORKDIR/$NAME.elf"
"$HU68DISK" del "$XDF" "/$UPPER.X" 2>/dev/null || true
"$HU68DISK" copy "$XDF" "$WORKDIR/$UPPER.X" /

echo "OK: $UPPER.X -> $XDF"
echo "NOTE: エミュレータが起動中なら完全に再起動してからでないと B:$UPPER が見えない"
