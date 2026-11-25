#!/bin/bash
# HUMAN302.XDF の純正 FLOAT4.X (v1.02) を使う FPCP 回帰ディスクを作る。
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
src="$root/disk/HUMAN302.XDF"
dst="$root/disk/FLOAT4.XDF"
hu68disk="$root/tools-host/hu68disk/hu68disk"
reference="$root/../dis/disasm/float_drivers/FLOAT4.X"
asm_build="$root/tools-x68k/asmtest/build.sh"
asm_source="$root/tools-x68k/asmtest/float4test.s"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -f "$src" ] || { echo "error: $src がありません" >&2; exit 1; }
[ -x "$hu68disk" ] || { echo "error: $hu68disk がありません" >&2; exit 1; }
[ -f "$reference" ] || { echo "error: 比較元 $reference がありません" >&2; exit 1; }
[ -x "$asm_build" ] || { echo "error: $asm_build がありません" >&2; exit 1; }

cp "$src" "$dst"
"$hu68disk" extract "$dst" /SYS/FLOAT4.X "$work/FLOAT4.X" > /dev/null
cmp -s "$work/FLOAT4.X" "$reference" || {
    echo "error: HUMAN302.XDF の SYS/FLOAT4.X が比較元と一致しません" >&2
    exit 1
}

"$hu68disk" extract "$dst" /CONFIG.SYS "$work/CONFIG.SYS" > /dev/null
python3 - "$work/CONFIG.SYS" <<'PY'
import sys
path = sys.argv[1]
data = open(path, "rb").read()
if data.count(b"FLOAT2.X") != 1 or data.count(b"FLOAT3.X") != 0 or data.count(b"FLOAT4.X") != 0:
    sys.exit("error: CONFIG.SYS の FLOAT ドライバ構成が想定外です")
open(path, "wb").write(data.replace(b"FLOAT2.X", b"FLOAT4.X"))
PY
"$hu68disk" del "$dst" /CONFIG.SYS > /dev/null
"$hu68disk" copy "$dst" "$work/CONFIG.SYS" / > /dev/null

"$hu68disk" extract "$dst" /AUTOEXEC.BAT "$work/AUTOEXEC.BAT" > /dev/null
python3 - "$work/AUTOEXEC.BAT" <<'PY'
import sys
path = sys.argv[1]
data = open(path, "rb").read()
if data.count(b"FLOAT4TEST.X") != 0 or data.count(b"\x1a") != 1 or not data.endswith(b"\x1a"):
    sys.exit("error: AUTOEXEC.BAT の EOF または FLOAT4TEST.X が想定外です")
open(path, "wb").write(data[:-1] + b"FLOAT4TEST.X\r\n\x1a")
PY
"$hu68disk" del "$dst" /AUTOEXEC.BAT > /dev/null
"$hu68disk" copy "$dst" "$work/AUTOEXEC.BAT" / > /dev/null

ASMTEST_CPU=68030 "$asm_build" "$asm_source" "$dst" > /dev/null
"$hu68disk" list "$dst" | grep -Eq '^[[:space:]]*FLOAT4TEST\.X[[:space:]]' || {
    echo "error: FLOAT4TEST.X を XDF 内に確認できません" >&2
    exit 1
}
"$hu68disk" fsck "$dst" > /dev/null
echo "created: $dst"
