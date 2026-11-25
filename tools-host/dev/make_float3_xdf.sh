#!/bin/bash
# disk/HUMAN302.XDF から、CONFIG.SYS で FLOAT3.X（MC68881 用ドライバ）を常駐させる起動ディスク
# disk/FLOAT3.XDF を作る。FLOAT3.X は HUMAN302.XDF の \SYS\FLOAT3.X をそのまま使う。
# disk/ は git 管理外のため、必要になったらこのスクリプトで再生成する。
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
src="$root/disk/HUMAN302.XDF"
dst="$root/disk/FLOAT3.XDF"
hu68disk="$root/tools-host/hu68disk/hu68disk"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -f "$src" ] || { echo "error: $src がありません" >&2; exit 1; }
[ -x "$hu68disk" ] || { echo "error: $hu68disk がありません（make -C tools-host/hu68disk）" >&2; exit 1; }

cp "$src" "$dst"
"$hu68disk" extract "$dst" /CONFIG.SYS "$work/CONFIG.SYS" > /dev/null
# DEVICE 行の FLOAT2.X（ソフトウェア浮動小数点）を FLOAT3.X（MC68881）へ置換する。同じ長さなので
# 改行・Shift_JIS のバイト列はそのまま保たれる
python3 - "$work/CONFIG.SYS" <<'PY'
import sys
path = sys.argv[1]
data = open(path, "rb").read()
old, new = b"FLOAT2.X", b"FLOAT3.X"
if data.count(old) != 1:
    sys.exit("error: CONFIG.SYS に FLOAT2.X がちょうど1つ必要です")
open(path, "wb").write(data.replace(old, new))
PY
"$hu68disk" del "$dst" /CONFIG.SYS > /dev/null
"$hu68disk" copy "$dst" "$work/CONFIG.SYS" / > /dev/null
"$hu68disk" fsck "$dst" > /dev/null
echo "created: $dst"
