#!/bin/bash
# tools-host/aiq-bridge/test_aiq.sh -- AIQ ブリッジとゲストの往復テストを一括で起動する。
#
# 通常のターミナルから実行すること(ix68k は GUI アプリのため)。
#
# 手順:
#   1. AIQ_ASK_TEST.X(固定の ASK を送る試験用)と AIQ.X(本体)をビルドし、
#      HOSTDRV 用ディレクトリに置く
#      (-fd1 でFDを挿すと現行ビルドは IPL エラーで起動しないため、FD は使わない)
#   2. PTY 有効で ix68k を起動し、スレーブパスを取得する
#   3. aiq-bridge をそのパスで起動する
#   4. ゲストで HOSTDRV のドライブ(DIR で確認)の AIQ_ASK_TEST、または
#      AIQ <指示文> を実行して確認する
#   5. Enter を押すとブリッジとエミュレータを停止する
#
# 環境変数:
#   AIQ_BACKEND  lmstudio(既定) / claude(要 ANTHROPIC_API_KEY) / codex(要 OPENAI_API_KEY)
#   AIQ_MODEL    モデル名(省略時はバックエンドごとの既定値)

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR/../.."

BACKEND="${AIQ_BACKEND:-lmstudio}"
PTY_WAIT_SEC=180

WORKDIR=$(mktemp -d)
XDF="$WORKDIR/aiq.XDF"
HOSTDRV_DIR="$WORKDIR/hostdrv"
TEST_PROGRAM=AIQ_ASK_TEST.X
EMU_LOG="$WORKDIR/ix68k.log"
BRIDGE_LOG="$WORKDIR/bridge.log"
EMU_PID=""
BRIDGE_PID=""

cleanup() {
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null || true
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

cp disk/BLANK.XDF "$XDF"
tools-x68k/asmtest/build.sh tools-x68k/asmtest/aiq_ask_test.s "$XDF"
mkdir -p "$HOSTDRV_DIR"
tools-host/hu68disk/hu68disk extract "$XDF" "/$TEST_PROGRAM" "$HOSTDRV_DIR/$TEST_PROGRAM"
make -C tools-x68k/aiq
cp tools-x68k/aiq/AIQ.X "$HOSTDRV_DIR/AIQ.X"

X68K_RS232_PTY=1 tools-host/dev/run_x68000.sh -hostdrv "$HOSTDRV_DIR" > "$EMU_LOG" 2>&1 &
EMU_PID=$!

PTY_PATH=""
for _ in $(seq "$PTY_WAIT_SEC"); do
    PTY_PATH=$(sed -n 's/^RS-232C PTY slave: //p' "$EMU_LOG" | head -n 1)
    [ -n "$PTY_PATH" ] && break
    kill -0 "$EMU_PID" 2>/dev/null || { echo "ix68k が終了しました: $EMU_LOG" >&2; exit 1; }
    sleep 1
done
[ -n "$PTY_PATH" ] || { echo "PTY パスが取得できません: $EMU_LOG" >&2; exit 1; }
echo "PTY: $PTY_PATH"

BRIDGE_ARGS=(--tty "$PTY_PATH" --backend "$BACKEND")
[ -n "${AIQ_MODEL:-}" ] && BRIDGE_ARGS+=(--model "$AIQ_MODEL")
python3 tools-host/aiq-bridge/aiq_bridge.py "${BRIDGE_ARGS[@]}" > "$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!
sleep 1
kill -0 "$BRIDGE_PID" 2>/dev/null || { cat "$BRIDGE_LOG" >&2 && exit 1; }

echo "ブリッジ起動 (backend=$BACKEND)。ログ: $BRIDGE_LOG"
echo "ゲストが A> プロンプトになったら、HOSTDRV のドライブで次のどちらかを実行する。"
echo "  AIQ_ASK_TEST        固定の ASK を送り、CMD ... / END が出る(ESC で終了)"
echo "  AIQ <指示文>        例: AIQ show files on drive A"
read -r -p "確認が終わったら Enter で停止します: " _
