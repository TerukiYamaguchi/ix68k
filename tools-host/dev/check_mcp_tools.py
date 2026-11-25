#!/usr/bin/env python3
"""MCPツール定義と、それを参照する設定・ドキュメントの突き合わせ（Issue #103 C-2）。

`tools-host/mcp/ix68k_mcp_server.py` の `@mcp.tool()` が正本。ツールを追加・改名した
ときの追随漏れは「動かなくなる」のではなく **毎回パーミッション確認が出る**、
**ドキュメントに載っていないので使われない** という形で静かに劣化するため、
ビルドでもテストでも検出できない。ここで機械的に突き合わせる。

チェック対象:
  - `.claude/settings.json`      … 双方向（不足も、存在しないツール名の残存も失敗）
  - `tools-host/mcp/MCP_TOOLS.md`     … 記載漏れのみ（旧ワイヤコマンド名の言及は正当なので
                                    逆方向は見ない）
  - `.claude/settings.local.json`… gitignore済みのマシンローカル設定。存在するときだけ、
                                    存在しないツール名の残存を失敗として報告する
                                    （不足は各自の許可方針なので警告に留める）

`docs/x68k-usage/mcp/mcp_llm_setup.md` は「代表的なツール」＋正本へのポインタという
意図的なサブセット文書なので対象外（厳格に突き合わせると恒常的に偽陽性になる）。

使い方: python3 tools-host/dev/check_mcp_tools.py
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SERVER_PY = ROOT / "tools-host/mcp/ix68k_mcp_server.py"
SETTINGS = ROOT / ".claude/settings.json"
SETTINGS_LOCAL = ROOT / ".claude/settings.local.json"
TOOLS_MD = ROOT / "tools-host/mcp/MCP_TOOLS.md"

PERMISSION_PREFIX = "mcp__x68k-emulator__"

# 意図的に許可リストへ載せないツールがあればここに書く（理由もコメントで残すこと）。
INTENTIONALLY_UNLISTED: set[str] = set()


def defined_tools() -> set[str]:
    """@mcp.tool() が付いた関数名を正本から抽出する。"""
    src = SERVER_PY.read_text(encoding="utf-8")
    return set(re.findall(r"@mcp\.tool\(\)\s*\ndef (\w+)", src))


def allowed_tools(path: Path) -> set[str]:
    """settings*.json の permissions.allow から MCP ツール名を取り出す。"""
    data = json.loads(path.read_text(encoding="utf-8"))
    allow = data.get("permissions", {}).get("allow", [])
    return {e[len(PERMISSION_PREFIX):] for e in allow if e.startswith(PERMISSION_PREFIX)}


def main() -> int:
    tools = defined_tools()
    if not tools:
        print(f"NG: {SERVER_PY.relative_to(ROOT)} からツールを抽出できませんでした")
        return 1

    errors: list[str] = []
    warnings: list[str] = []

    allowed = allowed_tools(SETTINGS)
    missing = sorted(tools - allowed - INTENTIONALLY_UNLISTED)
    stale = sorted(allowed - tools)
    if missing:
        errors.append(f"{SETTINGS.relative_to(ROOT)}: 未許可のツール {missing}")
    if stale:
        errors.append(f"{SETTINGS.relative_to(ROOT)}: 存在しないツール名の残存 {stale}")

    md = TOOLS_MD.read_text(encoding="utf-8")
    undocumented = sorted(t for t in tools if not re.search(rf"\b{re.escape(t)}\b", md))
    if undocumented:
        errors.append(f"{TOOLS_MD.relative_to(ROOT)}: 未記載のツール {undocumented}")

    if SETTINGS_LOCAL.exists():
        local = allowed_tools(SETTINGS_LOCAL)
        local_stale = sorted(local - tools)
        local_missing = sorted(tools - local - INTENTIONALLY_UNLISTED)
        if local_stale:
            errors.append(
                f"{SETTINGS_LOCAL.relative_to(ROOT)}: 存在しないツール名の残存 {local_stale}"
            )
        if local_missing:
            warnings.append(
                f"{SETTINGS_LOCAL.relative_to(ROOT)}: 未許可のツール {local_missing}"
                "（毎回パーミッション確認が出ます）"
            )

    for w in warnings:
        print(f"警告: {w}")
    if errors:
        for e in errors:
            print(f"NG: {e}")
        print(f"\n正本は {SERVER_PY.relative_to(ROOT)} の @mcp.tool()（{len(tools)}個）です。")
        return 1

    print(f"OK: MCPツール {len(tools)}個 が設定・ドキュメントと一致しています")
    return 0


if __name__ == "__main__":
    sys.exit(main())
