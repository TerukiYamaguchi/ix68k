# エージェント作業手順

`AGENTS.md` の詳細版。常駐コンテキストを小さく保つため、該当する作業を行うときだけ
必要な節を読む。

## mc68000 サブモジュールの更新

`mc68000に新しいコミットがあります` と通知された場合、`extern/cmc68000` は main 追従のため、
次の順で更新する。pin が変わった場合だけビルド・テストを行い、成功時に
`chore: mc68000サブモジュールを最新化(<旧hash>→<新hash>)` でコミットする。失敗時は pin を戻して
原因を報告し、無理に修正・push しない。

```sh
git submodule update --remote --init extern/cmc68000
git submodule update --init --recursive
cmake --build build && ctest --test-dir build
git add extern/cmc68000
```

`--remote` に `--recursive` を付けると、mc68000 内の入れ子のサブモジュール
（`berkeley-softfloat-3` 等）まで、mc68000 が記録した pin より先に進んでしまう。
そのため `--remote` は `extern/cmc68000` だけに適用し、入れ子側は 2 行目の
`--init --recursive`（`--remote` なし）で mc68000 の pin に合わせる。

`.claude/settings*.json` は Claude Code 専用であり、Codex 設定として変更しない。

## コマンド・資料の参照

- `/build`・`/rom-grep` 等は `.claude/commands/<名前>.md` を読む。存在しない場合は関連資料を調べ、安全な代替手順を確定できなければ推測で実行しない。
- `/build`: ビルド手順・ROM ファイルパス
- `/arch`: 構成・依存関係・TVRAM/GVRAM
- `/addrmap`: アドレスマップと実機との差異
- `/iocs`: IOCS トラップハンドラ
- `/hw-impl`: FDC / MFP
- `/mcp-debug`: ix68k の不具合調査。推測での修正や一時計装の前に「基本ワークフロー」と該当する症状別レシピを読む。
- `/xeij-trace`: XEiJ との差分デバッグ
- `/article`: 技術記事の規約

ROM は毎回逆アセンブルを生成せず、事前生成済みの `/dis/*.S` を grep する。詳細は `/rom-grep`。

## X68000 ソフトウェアの操作

起動、終了、保存、インストールは `docs/x68k-usage/README.md` の逆引きインデックスから対象を特定し、
記載された MCP アクションまたはインストール手順を使う。記憶で実行ファイル名・パス・手順を推測しない。
複数ソフトの導入は、同文書のバンドル定義と順序に従う。マニュアルがなければ `_TEMPLATE.md` に沿って
一次資料に基づくマニュアルを先に作成する。

## 作業・レビューの規約

- 破壊的操作または実際に設計判断が必要な場合を除き、確認待ちを挟まず進める。
- 1つの論理的なフェーズを完了してから報告する。無関係な変更を混ぜず、コミットも細かく分割しない。
- 実装タスク、バグ、調査項目は GitHub Issue で管理し、本ファイルに TODO を追加しない。
- レビュー記録には実施した AI エージェント名を記載する。機械チェック後にレビューを行い、blocking 指摘が 0 件になるまで再レビューする。
- パスにスペースがある場合はダブルクォートで囲む。

## MCP

MCP は既定で Unix socket `/tmp/ix68k_mcp.sock`、Windows は `127.0.0.1:26800`。詳細は `tools-host/mcp/MCP_TOOLS.md` と `docs/ix68k-internals/mcp_debug_reference.md` を必要な見出しだけ読む。

## コストを増やさないために

- grep / Read だけで足りる調査にはサブエージェントを立てない。
- 大量 OCR はバッチ処理し、生 OCR・長い diff など大きな出力は親コンテキストへ直接載せない。
- OCR 整形を委譲する場合は捏造禁止と判読不能値を `[?]` とするよう明示する。
