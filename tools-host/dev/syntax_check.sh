#!/usr/bin/env bash
# 単体ファイルの構文チェックをcompile_commands.jsonの実ビルドコマンドで行う。
# 手書きの-Iフラグ（-Isrc -Iincludeのみ等）はextern/cmc68000/includeの指定漏れによる
# 偽陽性の「include path解決の問題」を繰り返し発生させるため、実ビルドと同一のコマンドを
# 引いて使うことでその余地自体をなくす。
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <source-file>" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CDB="$REPO_ROOT/compile_commands.json"

if [ ! -f "$CDB" ]; then
    echo "error: $CDB not found (or broken symlink). run cmake -S . -B build first." >&2
    exit 1
fi

TARGET_FILE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

python3 - "$CDB" "$TARGET_FILE" <<'EOF'
import json
import shlex
import subprocess
import sys

cdb_path, target = sys.argv[1], sys.argv[2]
with open(cdb_path) as f:
    entries = json.load(f)

matches = [e for e in entries if e["file"] == target]
if not matches:
    print(f"error: {target} not found in compile_commands.json "
          "(target not built, or build dir stale — rerun cmake)", file=sys.stderr)
    sys.exit(1)

# 複数ターゲットでビルドされるファイルは先頭エントリで代表させる
# （-Iフラグはどのターゲットでも一致することをCMakeLists.txt側で保証）。
entry = matches[0]
args = shlex.split(entry["command"])

# -c ... -o ... を -fsyntax-only に置き換えてオブジェクト生成をスキップする。
out = []
skip_next = False
for a in args:
    if skip_next:
        skip_next = False
        continue
    if a == "-c":
        continue
    if a == "-o":
        skip_next = True
        continue
    out.append(a)
out.append("-fsyntax-only")

print("+ " + " ".join(shlex.quote(a) for a in out), file=sys.stderr)
sys.exit(subprocess.run(out, cwd=entry.get("directory", ".")).returncode)
EOF
