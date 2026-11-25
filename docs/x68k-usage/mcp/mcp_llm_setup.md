# LLM × MCP でエミュレータを操作する

AIエージェントからMCP経由でX68000エミュレータ（ix68k）を操作するための設定手順。
用途に応じて2パターンから選びます。

| パターン | 想定ツール | ネット接続 | コスト | tool use精度 |
|---|---|---|---|---|
| クラウドLLM | Claude Code / Codex CLI | 必要（API呼び出し） | API利用料 | 高い |
| ローカルLLM | LM Studio | 不要（オフライン） | 無料（電力のみ） | モデル依存（7B等は不安定なことも） |

MCPサーバ本体（`ix68k_mcp_server.py`）はどちらも共通。クライアントを切り替えるだけで
同じツールを使えます。

---

## 共通の前提

| 項目 | 確認方法 |
|------|---------|
| エミュレータを用意済み（ソースからビルド、またはリリースZIPを導入） | ビルド: macOS/Linux `ls build/ix68k` ／ Windows `dir build_win\ix68k.exe`。リリースZIP: 展開先の `ix68k`・`ix68k.exe`・`ix68k.app` |
| `uv` がインストール済み | macOS/Linux: `which uv` ／ Windows: `where uv` |

`uv`が無ければ導入する:

```bash
# macOS/Linux
brew install uv
# または: curl -LsSf https://astral.sh/uv/install.sh | sh
```

```powershell
# Windows
winget install astral-sh.uv
```

---

## 手順 1: エミュレータを起動する

```bash
cd /path/to/x68000
./build/ix68k              # Windowsは build_win\ix68k.exe
```

リリースZIPを導入した場合は、展開先で `./ix68k`（Windowsは `ix68k.exe`）を実行するか、
macOSでは `ix68k.app` を起動します。ZIPには `tools-host/mcp/` と `.mcp.json` も入っているため、
展開先で `claude` を起動すれば、リポジトリのcloneなしでMCPサーバを登録できます。

起動すると既定でソケットが作成される（環境変数 `IX68K_MCP_SOCK` で変更可、複数インスタンスを
同時に動かす場合は `tools-host/mcp/MCP_TOOLS.md` の「複数インスタンスの起動」節を参照）。

| OS | 既定ソケットパス | 存在確認 |
|---|---|---|
| macOS / Linux | `/tmp/ix68k_mcp.sock` | `ls /tmp/ix68k_mcp.sock` |
| Windows | TCP `127.0.0.1:26800`（下記） | `netstat -ano \| findstr 26800` |

> **Windows**: AF_UNIXではなくTCPループバック `127.0.0.1:26800` で待ち受ける
> （python.org配布のWindows版Pythonが`socket.AF_UNIX`を持たないため）。
> ポート番号は環境変数 `IX68K_MCP_PORT` で変更可。起動確認は `ls` の代わりに
> `netstat -ano | findstr 26800` を使う（`127.0.0.1:26800` がLISTENINGなら OK）。

---

## 手順 2: MCP サーバの動作確認

クライアント（Claude Code / Codex CLI / LM Studio）に登録する前に、MCP サーバ単体で
動くことを確認しておきます。

```bash
cd /path/to/x68000
uv run --project tools-host/mcp tools-host/mcp/ix68k_mcp_server.py
# → 何も表示されずに待機状態になれば OK（stdio モードなので無音が正常）
# → Ctrl+C で終了
```

---

## 手順 3: MCP クライアントに登録する

### クラウドLLM: Claude Code の場合

このリポジトリには`.mcp.json`（プロジェクトルート）が既に含まれており、`uv`とリポジトリ
相対パスでMCPサーバを起動します。リポジトリのルートで`claude`を起動する限り、通常は編集不要です。
`uv`がPATHに無い場合や、別ディレクトリから使う場合は、次の例のように絶対パスへ書き換えます。

```json
{
  "mcpServers": {
    "x68k-emulator": {
      "command": "/opt/homebrew/bin/uv",
      "args": [
        "run",
        "--project", "/path/to/x68000/tools-host/mcp",
        "/path/to/x68000/tools-host/mcp/ix68k_mcp_server.py"
      ]
    }
  }
}
```

- `command`は`which uv`（Windowsは`where uv`）の結果に置き換える
- `args`のパスは自分のリポジトリの絶対パスに置き換える（Windowsは`C:\path\to\x68000\...`の
  ようにバックスラッシュでもスラッシュでもよい）
- 編集後、プロジェクトディレクトリで`claude`を起動すればMCPサーバが自動接続される
  （`/mcp`コマンドで接続状態を確認できる）
- 複数インスタンスに繋ぐ場合は`env`に`{"IX68K_MCP_SOCK": "/tmp/ix68k_mcp_b.sock"}`を追加する
  （詳細は`tools-host/mcp/MCP_TOOLS.md`の「複数インスタンスの起動」節）

### クラウドLLM: Codex CLI の場合

`~/.codex/config.toml`に`[mcp_servers.x68k-emulator]`テーブルを追加する（`codex mcp add`
のような対話コマンドで登録できる場合もあるが、サブコマンド名はバージョンにより変わり
うるため`codex mcp --help`で確認します。ここでは設定ファイルを直接編集する手順を示す）。

```toml
[mcp_servers.x68k-emulator]
command = "/opt/homebrew/bin/uv"
args = [
  "run",
  "--project", "/path/to/x68000/tools-host/mcp",
  "/path/to/x68000/tools-host/mcp/ix68k_mcp_server.py",
]
```

- パスの置き換え方はClaude Codeの場合と同じ
- 複数インスタンスに繋ぐ場合は同テーブルに`env`を追加する:
  ```toml
  [mcp_servers.x68k-emulator.env]
  IX68K_MCP_SOCK = "/tmp/ix68k_mcp_b.sock"
  ```

### ローカルLLM: LM Studio の場合

LM Studioの右サイドバーで「Program」タブを開き、`Install > Edit mcp.json`から
`mcp.json`を編集します。利用可能なサーバでは「Add to LM Studio」リンクを使える場合もあるが、
このローカルstdioサーバは次の設定を手動で追加します。画面構成が変わった場合は
[LM Studio公式MCP手順](https://lmstudio.ai/docs/app/mcp)を参照します。

```json
{
  "mcpServers": {
    "x68k-emulator": {
      "command": "/opt/homebrew/bin/uv",
      "args": [
        "run",
        "--project",
        "/path/to/x68000/tools-host/mcp",
        "/path/to/x68000/tools-host/mcp/ix68k_mcp_server.py"
      ]
    },
    "filesystem": {
      "command": "/opt/homebrew/bin/npx",
      "args": [
        "-y",
        "@modelcontextprotocol/server-filesystem",
        "/path/to/x68000/docs/x68k-usage"
      ]
    }
  }
}
```

- `filesystem` サーバは `docs/x68k-usage/` 以下にのみアクセスできるよう制限している（ソースコード等は読めない）。
  `npx` はNode.jsに含まれるため、無ければ `brew install node` で導入する（Windowsは `winget install OpenJS.NodeJS.LTS`）。`command` は `which npx`（Windowsは `where npx`）の結果に置き換える
- パスはすべて絶対パスで記載すること
- 編集後は LM Studio を再起動する
- 複数インスタンスに繋ぐ場合は `x68k-emulator` の設定に `"env": {"IX68K_MCP_SOCK": "/tmp/ix68k_mcp_b.sock"}` を追加し、エミュレータ側も同じ環境変数で起動する（詳細は `tools-host/mcp/MCP_TOOLS.md` の「複数インスタンスの起動」節）
- **Windows**: 上記の `IX68K_MCP_SOCK` の代わりに `"env": {"IX68K_MCP_PORT": "26801"}` を使う（`tools-host/mcp/MCP_TOOLS.md` の「Windows版」節を参照）

---

## 手順 4: tool use 対応モデルを選ぶ（ローカルLLMのみ）

Claude Code / Codex CLI はモデル側がtool useに対応済みのためこの手順は不要（手順5へ進む）。

LM Studioの場合、MCP ツールを呼び出せるのは **function calling（tool use）に対応した
モデル** のみ。Model Hub でフィルタ "Tools" を使うと対応モデルだけ絞り込めます。
モデルごとにツール選択の精度と必要メモリが異なるため、まず小さな`get_registers`や
`screenshot`呼び出しで確認します。

---

## 手順 5: 動かしてみる

- **Claude Code / Codex CLI**: プロジェクトディレクトリでそのまま会話すればよい
  （`.mcp.json`/`config.toml`登録済みなら追加操作は不要）
- **LM Studio**: Chat 画面を開き、MCP サーバが有効になっていることを確認して会話する

### プロンプト例

```
エミュレータの現在の画面を見せてください。
```

```
Human68k のコマンドプロンプトで DIR コマンドを実行して、結果を教えてください。
```

```
A ドライブのファイル一覧を取得して、拡張子が .X のファイルだけ列挙してください。
```

LM Studioで `filesystem` サーバを登録している場合は、資料を読ませてから操作させられます。
`list_directory` ツールは、登録したディレクトリ（`docs/x68k-usage/`）の中を一覧します。

```
list_directory ツールで /path/to/x68000/docs/x68k-usage/official の内容を確認して、ED.X の起動方法を調べてください。
その後、エミュレータ上で ED.X を起動してください。
```

```
list_directory ツールで /path/to/x68000/docs/x68k-usage の内容を確認して、README.md の逆引きインデックスから
SWITCH.X のマニュアルを探して、現在の起動順を確認する手順を教えてください。
```

---

## 利用可能なツール一覧

MCP サーバが公開している代表的なツール（クライアントが自動的に認識する）:

| ツール | 説明 |
|--------|------|
| `screenshot` | 画面キャプチャ（PNG）を取得 |
| `read_bitmap_text` | TVRAM のテキスト内容を取得 |
| `read_stdout_text` | IOCS `_B_PUTC`/`_B_PRINT` の出力文字列をキャプチャして取得 |
| `read_memory` | 任意アドレスから最大 256 バイトを hex ダンプで読み出し |
| `write_memory` | 任意アドレスにバイト列を書き込む（最大 256 バイト） |
| `key` | キー入力を送信（`action="press"/"down"/"up"`） |
| `paste_text` | 文字列をキー入力として流し込む |
| `run_command` | Human68k コマンドを実行して出力を返す |
| `disk` | FD/SCSIディスクイメージの挿入・排出（`action="insert"/"eject"`、`media="fd"/"scsi"`） |
| `get_registers` | CPU レジスタを取得 |
| `send_interrupt` | NMI（レベル7割り込み）を発生させる |
| `reset` | エミュレータをリセット |

ほかにマウス、ジョイスティック、HOSTFILE、待機判定（`key_wait`）、診断トレース
（`trace_iocs`/`diag_log`）、RAM書き込みウォッチ（`ram_watch`）、白帯検出HALTの
制御（`whiteband`）、およびCPUを実際に一時停止させて調べるデバッガツール群
（`disassemble`/`breakpoint`/`watchpoint`/`debug_control`/`exception_break`/
`dump_ring`）もあります。全ツールと引数の正本は`tools-host/mcp/MCP_TOOLS.md`を参照。

---

## トラブルシューティング

### "エミュレータに接続できません" エラー

```
RuntimeError: エミュレータに接続できません (/tmp/ix68k_mcp.sock)
```

エミュレータが起動していないか、ソケットが生成されていません。
macOS/Linuxでは`ls /tmp/ix68k_mcp.sock`を確認し、エミュレータを起動し直します。
エラーメッセージのパスが既定と違う場合は、`IX68K_MCP_SOCK`が設定されている
（クライアント側とエミュレータ側で値が食い違っていないか確認する）。

Windowsではエラーメッセージが `127.0.0.1:26800` のようなTCPエンドポイントになる
（AF_UNIXではなくTCPループバック）。`netstat -ano | findstr 26800`で
LISTENINGを確認し、無ければエミュレータを起動し直します。ポートが既定と違う場合は
`IX68K_MCP_PORT`が設定されている（クライアント側とエミュレータ側で値が食い違って
いないか確認する）。「別のix68kが使っている」と断定できないbind失敗ログが出た
場合は、他プログラムとのポート衝突の可能性もある（`IX68K_MCP_PORT`を別の値に
変更する）。

### ツールがクライアントに表示されない

- MCP サーバの起動に失敗している可能性がある → クライアントのMCPログを確認する
  （Claude Codeは`/mcp`、Codex CLIは起動時ログ、LM Studioはアプリ内MCPログ）
- `uv` のパスが通っていない → `which uv`（Windowsは`where uv`）で確認し、絶対パスを使う
- （LM Studioのみ）モデルが tool use 非対応の可能性がある → tool use 対応モデルに切り替える

### `uv` が見つからない

```bash
# Homebrew でインストール
brew install uv

# または公式インストーラ
curl -LsSf https://astral.sh/uv/install.sh | sh
```

### MCP サーバが起動するが応答しない

`tools-host/mcp` ディレクトリで依存関係を手動インストールして確認:

```bash
cd /path/to/x68000/tools-host/mcp
uv sync
uv run ix68k_mcp_server.py
```

---

## セキュリティに関する注意（Windows）

WindowsのみTCPループバック（`127.0.0.1:26800`）で待ち受ける都合上、macOS/Linuxの
AF_UNIXと比べてアクセス制御が後退しています。MCPコマンドには認証機構が無いため、
以下を理解した上で利用すること（いずれも個人開発機での利用を前提に受容している）。

- ループバック限定bindによりネットワーク越しの露出は防げるが、**同一マシン上の
  任意のローカルユーザー・任意のプロセスが接続できる**。`MEM WRITE`（メモリ書き込み）や
  `HOSTFILE GET/PUT`（ホスト側ファイルの読み書き）も実行できてしまいます。
- AF_UNIX版はソケットファイルが`%TEMP%`配下のユーザー別ディレクトリに置かれるため、
  ACLによるアクセス制限が**期待できた**（OS・ファイルシステム依存であり保証ではない）。
- **ローカル先取り（port squatting）**: ix68k本体が起動するより先に他のローカル
  プロセスが`127.0.0.1:26800`を掴んでいた場合、Pythonクライアントはそれを正規サーバと
  区別できません。ix68k側から見ればbind失敗として明瞭にログへ出るが、Pythonクライアント
  だけを単独で起動した場合はこの限りではありません。

---

## 応用: AIエージェントにHDDインストールを任せる

ホストのシェルコマンドを実行できるAIエージェント（Claude Code / Codex CLI等）であれば、
自然文で指示するだけで起動可能なSCSI HDDイメージを作成させることができます。固定手順
（質問の順序・文言・`hu68disk`/`sram_switch`の呼び方）は `docs/x68k-usage/mcp/provisioning/hdd_install.md` 参照。
