#!/bin/sh
# ix68k-debugger を、tkinter が使える Python で起動する。
# ix68k の File > Open Debugger... とターミナルの両方から使う。追加引数は
# ix68k_debugger.py へそのまま渡す（--socket PATH / --port PORT）。
#
# 使う Python: 環境変数 IX68K_PYTHON があればそれ。無ければ下の候補を順に試し、
# `import tkinter` が通った最初のものを使う（python3 が tkinter なしの版を指す
# 環境があるため、バージョン付きの名前も探す）。
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
debugger="$script_dir/ix68k_debugger.py"

has_tkinter() {
    "$1" -c 'import tkinter' >/dev/null 2>&1
}

python="${IX68K_PYTHON:-}"
if [ -z "$python" ]; then
    for candidate in python3 python3.15 python3.14 python3.13 python3.12 python3.11 python3.10 /usr/bin/python3
    do
        if command -v "$candidate" >/dev/null 2>&1 && has_tkinter "$candidate"
        then
            python="$candidate"
            break
        fi
    done
fi

if [ -z "$python" ]
then
    echo "ix68k-debugger: tkinter が使える Python が見つかりません。" >&2
    echo "  Homebrew: brew install python-tk@<バージョン>" >&2
    echo "  使う Python を指定する場合: IX68K_PYTHON=/path/to/python $0" >&2
    exit 1
fi

exec "$python" "$debugger" "$@"
