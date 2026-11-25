#!/bin/sh
# X68000エミュレータ（自作、製品名ix68k）起動スクリプト。
# local/profiles/ix68k.cfg があればそれを読み込む。
# 追加オプション（-hostdrv <dir> 等）はそのまま渡せる。
#
# 使い方:
#   tools-host/dev/run_x68000.sh                          # 通常起動
#   tools-host/dev/run_x68000.sh -hostdrv /path/to/shared   # HOSTDRV有効化
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir/../.."

# build/ is usually PLATFORM_NULL=ON for tests; use a separate SDL3/Cocoa build.
gui_build_dir=/tmp/x68000-gui
cmake -S . -B "$gui_build_dir" -DPLATFORM_NULL=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build "$gui_build_dir" --target ix68k -j4

if pgrep -f "$gui_build_dir/ix68k" > /dev/null 2>&1; then
    echo "警告: ix68k は既に起動中です（多重起動時は新プロセスのMCPサーバが起動しません）" >&2
fi

config_path=local/profiles/ix68k.cfg
if [ -f "$config_path" ]; then
    exec "$gui_build_dir/ix68k" -config "$config_path" "$@"
fi
exec "$gui_build_dir/ix68k" "$@"
