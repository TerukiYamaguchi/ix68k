#!/bin/sh
# 空ファイルは開始クラスタ 0 ではなく 1 クラスタを持つこと（Human68k 互換）、
# 既存の開始クラスタ 0 のエントリを fsck が警告し --fix で直すこと。
# 開始クラスタ 0 の空ファイルへのゼロ長 _WRITE は Human68k 3.02 で白帯になる。
set -eu

bin=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hu68disk-empty.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# ディレクトリエントリ(32 バイト)の先頭オフセットを返す。開始クラスタは +26 の LE16。
entry_offset() {
  LC_ALL=C grep -abo 'EMPTY   TXT' "$1" | head -n 1 | cut -d: -f1
}
entry_cluster() {
  off=$(entry_offset "$1")
  od -An -tu2 -j $((off + 26)) -N2 "$1" | tr -d ' '
}

"$bin" mkfs "$tmp/disk.hds" --type scsi --force
: > "$tmp/EMPTY.TXT"
"$bin" copy "$tmp/disk.hds" "$tmp/EMPTY.TXT" /
"$bin" list "$tmp/disk.hds" / | grep -q 'EMPTY.TXT'
test "$(entry_cluster "$tmp/disk.hds")" -ge 2
"$bin" fsck "$tmp/disk.hds" | grep -q '0 problem(s) found'

# 開始クラスタ 0 のエントリを作る（FAT 標準の空ファイル表現。Human68k では白帯の原因）。
off=$(entry_offset "$tmp/disk.hds")
printf '\000\000' | dd of="$tmp/disk.hds" bs=1 seek=$((off + 26)) conv=notrunc 2>/dev/null
test "$(entry_cluster "$tmp/disk.hds")" -eq 0
"$bin" fsck "$tmp/disk.hds" | grep -q '開始クラスタ 0 のファイル'
if "$bin" fsck "$tmp/disk.hds" | grep -q '0 problem(s) found'; then exit 1; fi

"$bin" fsck "$tmp/disk.hds" --fix --yes --no-backup >/dev/null
test "$(entry_cluster "$tmp/disk.hds")" -ge 2
"$bin" fsck "$tmp/disk.hds" | grep -q '0 problem(s) found'
