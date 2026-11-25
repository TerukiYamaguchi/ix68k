# aiq-bridge

AIQ のホスト側ブリッジ。シリアル(tty)で受け取った自然言語の指示を LLM で Human68k のコマンド列に変換して返す。
コマンドは実行せず、提案を返すだけ。依存は Python 3 標準ライブラリのみ。

バージョン 0.9（`python3 aiq_bridge.py --version` で表示）。

## 使い方

```
# エミュレータ側: X68K_RS232_PTY=1 で起動し、ログの "RS-232C PTY slave: /dev/ttysNNN" を控える
# Claude API
ANTHROPIC_API_KEY=... python3 aiq_bridge.py --tty /dev/ttysNNN
# Codex API
OPENAI_API_KEY=... python3 aiq_bridge.py --tty /dev/ttysNNN --backend codex
# LM Studio (OpenAI 互換 API, 既定 http://localhost:1234/v1)
python3 aiq_bridge.py --tty /dev/ttysNNN --backend lmstudio --model qwen/qwen3-coder-30b
```

実機接続時は `--tty /dev/tty.usbserial-XXXX` を指定する。

既定モデル: claude は `claude-sonnet-5-5`、codex は `gpt-5.3-codex`、lmstudio は `qwen/qwen3-coder-30b`。

## Claude API

Claude バックエンドは Messages API を `urllib` で直接呼ぶ(依存追加なし)。`effort: low`、`max_tokens: 4096`
(思考トークンも数える)で呼び、`temperature` は送らない(現行モデルは非既定値を 400 で拒否する)。
安全分類器の拒否(`stop_reason: refusal`)は `ERR refused`、`max_tokens` 打ち切りは `ERR truncated` を返す。

## Codex API

Codex バックエンドは Responses API を `urllib` で直接呼ぶ(依存追加なし)。`reasoning.effort: low` と
`max_output_tokens: 4096` を指定し、JSON Schema の strict 出力で `message` と `commands` を受け取る。
`OPENAI_API_KEY` を設定して、次のように起動する。

```
OPENAI_API_KEY=... python3 aiq_bridge.py --tty /dev/ttysNNN --backend codex
```

既定モデルは `gpt-5.3-codex` で、別のモデルを使うときは `--model` を指定する。

```
OPENAI_API_KEY=... python3 aiq_bridge.py --tty /dev/ttysNNN --backend codex --model gpt-5.3-codex
```

Responses API が拒否を返したときは `ERR refused`、`max_output_tokens` で出力が打ち切られたときは
`ERR truncated` を返す。それ以外のHTTPエラーは `ERR api http <code>`、応答形式が不正なときは
`ERR bad api response` になる。

## 動作確認の状況

| バックエンド | 状況 |
|---|---|
| LM Studio | エミュレータ上のゲストとの往復を確認済み(`test_aiq.sh`) |
| Claude API | **多分動くが、動作未確認。** `ANTHROPIC_API_KEY` を設定した実 API 呼び出しは一度も行っていない |
| Codex API | **多分動くが、動作未確認。** `OPENAI_API_KEY` を設定した実 API 呼び出しは一度も行っていない |

Claude API は、リクエストの組み立てと応答の解釈を単体テスト(API をモックにしたもの)で確認しただけである。
`output_config` やモデル名が実 API で受理されるか、システムプロンプトどおりの JSON が返るかは未確認。
最初に使うときは `ERR api http <code>` や `ERR bad llm output` が出る可能性がある。

Codex API も、リクエストの組み立てと応答の解釈を単体テスト(API をモックにしたもの)で確認しただけである。
Responses API のJSON Schema、モデル名、`reasoning.effort` が実 API で受理されるか、システムプロンプトどおりの
JSON が返るかは未確認。最初に使うときは `ERR api http <code>`、`ERR bad api response`、または
`ERR bad llm output` が出る可能性がある。

## LM Studio で使う

1. LM Studio でモデルをダウンロードして読み込む。動作確認済みは `qwen/qwen3-coder-30b`。
   読み込み時のコンテキスト長は **8192 以上**にする(資料付きの質問は 4096 では入らず `ERR context too small` になる。
   `openai/gpt-oss-20b` は 4096 で確認したため、8192 以上に上げて使う)。
2. LM Studio のローカルサーバー(OpenAI 互換 API)を起動する。既定の待ち受けは `http://localhost:1234/v1`。
   別ポートにしたときは `--lmstudio-url` で指定する。
3. `--model` に渡すモデル名を確認する。`curl http://localhost:1234/v1/models` の `id` がそのまま使える。
4. ブリッジを起動する。

   ```
   python3 aiq_bridge.py --tty /dev/ttysNNN --backend lmstudio --model qwen/qwen3-coder-30b
   ```

エミュレータとゲストを含めた通しの確認は、`tools-host/aiq-bridge/test_aiq.sh` が一括で行う(通常のターミナルから実行する)。
AIQ.X のビルド、PTY 有効でのエミュレータ起動、ブリッジ起動までを行い、ゲストが `A>` になったら
HOSTDRV のドライブで `AIQ_ASK_TEST`(固定の ASK)または `AIQ <指示文>` を実行する。

```
tools-host/aiq-bridge/test_aiq.sh                                   # LM Studio、既定モデル
AIQ_MODEL=openai/gpt-oss-20b tools-host/aiq-bridge/test_aiq.sh      # モデルを指定
AIQ_BACKEND=claude ANTHROPIC_API_KEY=... tools-host/aiq-bridge/test_aiq.sh   # Claude API(動作未確認)
AIQ_BACKEND=codex OPENAI_API_KEY=... tools-host/aiq-bridge/test_aiq.sh       # Codex API(動作未確認)
```

`-fd1` で FD を挿すと現行ビルドは IPL エラーで止まるため、`test_aiq.sh` は FD を使わず HOSTDRV で `.X` を渡す。

## プロトコル

1 行 1 メッセージ、区切りは CR / LF、文字コードは Shift_JIS。

| 方向 | 形式 |
|---|---|
| ゲスト→ホスト | `ASK <指示文>` |
| ホスト→ゲスト | `MSG <回答の1行>` と `CMD <コマンド>` を各 0 行以上、最後に `END` |
| エラー | `ERR <理由>`(`END` は付かない) |

`MSG` は質問への回答文で、1 行 76 バイトで折り返し、20 行を超えると「(以下省略)」で打ち切る。
Shift_JIS に無い文字は `?` に置換する(`ERR` にはしない)。`CMD` は、LLM が返した後にホスト側で
制御文字・255 バイト超・cp932 変換不可を検証する。

`ERR` の理由: `bad request` / `bad encoding` / `bad llm output` / `bad command` / `command too long` /
`unencodable` / `refused` / `truncated` / `context too small` / `timeout` / `api unreachable` / `api http <code>` / `bad api response` / `line too long`。

## 操作資料の参照

質問ごとに `docs/x68k-usage`(`guide/`、`official/`、`freeware/`、`development/`)から関連する断片を
検索し、「参考資料」として LLM に渡す。資料の内容は LLM の一般知識より優先され、使った資料は回答の
最後に「参照: ファイル名」で示される。検証状態(📖 マニュアル参照のみ など)も断片の見出しに付き、
未検証の内容には「未検証」と添えさせる。

| オプション | 内容 |
|---|---|
| `--docs DIR` | 資料のディレクトリ(既定: リポジトリ内の `docs/x68k-usage`) |
| `--no-docs` | 資料を参照しない |
| `--docs-budget N` | 1 回の質問に付ける資料の最大文字数(既定 6000) |

資料は起動時に一度だけ読み込むので、資料を直したらブリッジを再起動する。
`ERR context too small` が出るときは、LM Studio のモデルのコンテキスト長が足りない。
コンテキスト長を増やすか(目安 8192 以上)、`--docs-budget` を小さくするか、`--no-docs` を使う。

## 安定動作のための挙動

- **応答の送信間隔**: エミュレータ(ix68k)の SCC ch A 受信リングは 256 バイトで、溢れたバイトは黙って捨てられる。
  応答(資料付きの回答は 1KB を超える)を一度に書くと、ゲストが画面出力している間に溢れ、文字化けや `END` の欠落
  (ゲスト側のタイムアウト)になる。そのためブリッジは応答を 1 行ずつ間隔を空けて書く。
  既定 0.1 秒で、`--line-interval 秒` で変えられる(実機接続で取りこぼすなら大きくする)。
- **LM Studio の暴走**: モデルが同じ文を延々と繰り返して `max_tokens`(1024)で打ち切られることがある。
  `frequency_penalty` で繰り返しを抑え、それでも `finish_reason: length` になったときは `ERR truncated` を返す。
- **XON / XOFF の除去**: ゲストの RS-232C ドライバは受信バッファの残量に応じて XOFF(0x13) / XON(0x11) を
  送ってくる。これが次の ASK の行頭に混ざると `ERR bad request` になるため、受信から取り除く
  (Shift_JIS の1・2バイト目には現れないので、通信文は壊れない)。ブリッジは XOFF を受けても送信を止めない。
- **診断ログ**: 受信した生の 1 行(`RX`)と応答のバイト数(`TX`)を stderr に出す。`bad request` などの切り分けに使う。

## テスト

```
python3 -I -m unittest discover -s tests
```
