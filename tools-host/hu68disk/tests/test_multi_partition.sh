#!/bin/sh
set -eu

bin=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hu68disk-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$bin" mkfs "$tmp/multi.hds" --type scsi --size 100 \
  --volume 40:SYSTEM --volume 60:DATA --force
"$bin" partitions "$tmp/multi.hds" | grep -q '1     boot.*SYSTEM'
"$bin" partitions "$tmp/multi.hds" | grep -q '2     usable.*DATA'
"$bin" fsck "$tmp/multi.hds" --part 1 | grep -q '0 problem(s) found'
"$bin" fsck "$tmp/multi.hds" --part 2 | grep -q '0 problem(s) found'
part1_before=$(dd if="$tmp/multi.hds" bs=1024 count=40992 2>/dev/null | cksum)
printf 'second partition\n' > "$tmp/input.x"
"$bin" copy "$tmp/multi.hds" --part 2 "$tmp/input.x" /
"$bin" extract "$tmp/multi.hds" --part 2 /input.x "$tmp/output.x"
cmp "$tmp/input.x" "$tmp/output.x"
part1_after=$(dd if="$tmp/multi.hds" bs=1024 count=40992 2>/dev/null | cksum)
test "$part1_before" = "$part1_after"
"$bin" list "$tmp/multi.hds" --part 1 / | grep -q '0 file(s)'
"$bin" copyall "$tmp/multi.hds" / "$tmp/multi.hds" /BACKUP --src-part 2 --dst-part 1
"$bin" list "$tmp/multi.hds" --part 1 /BACKUP | grep -q 'input.x'
"$bin" mkdir "$tmp/multi.hds" --part 2 /EMPTY
"$bin" rmdir "$tmp/multi.hds" --part 2 /EMPTY
"$bin" del "$tmp/multi.hds" --part 2 /input.x
"$bin" list "$tmp/multi.hds" --part 2 / | grep -q '0 file(s)'

if "$bin" list "$tmp/multi.hds" --part nope >/dev/null 2>&1; then exit 1; else
  test "$?" -eq 2
fi
if "$bin" copyall "$tmp/multi.hds" / "$tmp/multi.hds" / --src-part 1 --dst-part 1 >/dev/null 2>&1; then exit 1; fi
if "$bin" mkfs "$tmp/bad.hds" --type scsi --size 11 --volume 5:A --volume 5:B --force >/dev/null 2>&1; then exit 1; fi

# 上限の15区画を生成・選択できること。POSIX sh で --volume を組み立てる。
set -- "$bin" mkfs "$tmp/fifteen.hds" --type scsi --size 75
i=1
while test "$i" -le 15; do
  set -- "$@" --volume "5:P$i"
  i=$((i + 1))
done
"$@" --force
test "$("$bin" partitions "$tmp/fifteen.hds" | grep -c 'Human68k')" -eq 15
"$bin" fsck "$tmp/fifteen.hds" --part 1 | grep -q '0 problem(s) found'
"$bin" fsck "$tmp/fifteen.hds" --part 15 | grep -q '0 problem(s) found'
set -- "$bin" mkfs "$tmp/sixteen.hds" --type scsi --size 80
i=1
while test "$i" -le 16; do
  set -- "$@" --volume "5:P$i"
  i=$((i + 1))
done
if "$@" --force >/dev/null 2>&1; then exit 1; fi

# 既定の単一区画HDSおよびFDイメージは、区画指定なしで従来どおり使えること。
"$bin" mkfs "$tmp/single.hds" --type scsi --force
"$bin" partitions "$tmp/single.hds" | grep -q '1     boot.*Human68k'
"$bin" fsck "$tmp/single.hds" | grep -q '0 problem(s) found'
"$bin" mkfs "$tmp/single.xdf" --force
"$bin" fsck "$tmp/single.xdf" | grep -q '0 problem(s) found'
if "$bin" list "$tmp/single.xdf" --part 1 >/dev/null 2>&1; then exit 1; fi

# 領域を確保しない空HDS（XEiJ「領域確保」OFF相当）。ヘッダ32KBだけを書き、
# 区画エントリと区画領域は0のまま。
"$bin" mkfs "$tmp/empty.hds" --type scsi --size 20 --no-partition --force
test "$(wc -c < "$tmp/empty.hds")" -eq 21004288
# $0804 = 確保済みの最後の区画の末尾 = 区画領域の開始位置32（＝1区画も確保していない）。
test "$(dd if="$tmp/empty.hds" bs=1 skip=2052 count=4 2>/dev/null |
        od -An -tx1 | tr -d ' \n')" = "00000020"
# 第1区画エントリ（$0810〜）は空。
test "$(dd if="$tmp/empty.hds" bs=1 skip=2064 count=16 2>/dev/null |
        od -An -tx1 | tr -d ' \n')" = "00000000000000000000000000000000"
# SCSIディスクIDとDisk IPLは領域未確保でも書く（ドライブとして認識させるため）。
test "$(dd if="$tmp/empty.hds" bs=1 count=8 2>/dev/null)" = "X68SCSI1"
test "$(dd if="$tmp/empty.hds" bs=1 skip=1024 count=4 2>/dev/null |
        od -An -tx1 | tr -d ' \n')" = "600000ca"
if "$bin" partitions "$tmp/empty.hds" --all | grep -q 'Human68k'; then exit 1; fi
if "$bin" list "$tmp/empty.hds" >/dev/null 2>&1; then exit 1; fi
if "$bin" fsck "$tmp/empty.hds" >/dev/null 2>&1; then exit 1; fi
if "$bin" mkfs "$tmp/bad4.hds" --type scsi --no-partition --label X --force \
     >/dev/null 2>&1; then exit 1; fi
if "$bin" mkfs "$tmp/bad5.hds" --type scsi --no-partition --volume 5:A --force \
     >/dev/null 2>&1; then exit 1; fi
if "$bin" mkfs "$tmp/bad6.xdf" --no-partition --force >/dev/null 2>&1; then exit 1; fi

# fsck --fix のバックアップはメモリ上のバッファではなくファイルを直接複製する
# （選択区画しかマップしないため）。修復前のイメージと1バイトも違わないこと。
"$bin" mkfs "$tmp/fix.hds" --type scsi --size 100 --volume 40:SYSTEM --volume 60:DATA --force
printf 'fsck backup\n' > "$tmp/fix.x"
"$bin" copy "$tmp/fix.hds" --part 2 "$tmp/fix.x" /
# 第2区画（開始40992セクタ）の FAT を壊し、未参照の割り当て済みクラスタを作る。
printf '\377\377\377\377\377\377\377\377' |
  dd of="$tmp/fix.hds" bs=1 seek=$((40992 * 1024 + 1024 + 8)) conv=notrunc 2>/dev/null
cp "$tmp/fix.hds" "$tmp/fix.before"
p1_before=$(dd if="$tmp/fix.hds" bs=1024 skip=32 count=40960 2>/dev/null | cksum)
"$bin" fsck "$tmp/fix.hds" --part 2 --fix -y | grep -q 'fixed'
cmp "$tmp/fix.before" "$tmp/fix.hds.bak"
"$bin" fsck "$tmp/fix.hds" --part 2 | grep -q '0 problem(s) found'
# 書き戻しは選択区画の外へ及ばない（第1区画は不変）。
p1_after=$(dd if="$tmp/fix.hds" bs=1024 skip=32 count=40960 2>/dev/null | cksum)
test "$p1_before" = "$p1_after"
