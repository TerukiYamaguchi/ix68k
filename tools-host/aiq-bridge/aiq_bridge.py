#!/usr/bin/env python3
"""AIQ ホスト側ブリッジ。

tty(エミュレータの PTY スレーブ、または実シリアルデバイス)から自然言語の指示を受け取り、
LLM に Human68k コマンド列へ変換させて返す。コマンドは実行せず、提案を返すだけ。

プロトコル(1 行 1 メッセージ、区切りは CR / LF、文字コードは Shift_JIS):
  ゲスト→ホスト: ASK <指示文>
  ホスト→ゲスト: MSG <回答の1行> を 0 行以上、CMD <コマンド> を 0 行以上、最後に END
  エラー時     : ERR <理由>(END は送らない)
"""

import argparse
import json
import os
import re
import select
import sys
import time
import tty
import urllib.error
import urllib.request
from pathlib import Path
from typing import Callable

import docs_search

GUEST_ENCODING = "cp932"
LINE_TERMINATOR = b"\r\n"
MAX_LINE_BYTES = 255
MSG_WRAP_BYTES = 76
MAX_MESSAGE_LINES = 20
MESSAGE_TRUNCATED_LINE = "(以下省略)"
READ_CHUNK_BYTES = 256
SELECT_TIMEOUT_SEC = 1.0
VERSION = "0.9"
ANTHROPIC_VERSION = "2023-06-01"
ANTHROPIC_URL = "https://api.anthropic.com/v1/messages"
OPENAI_RESPONSES_URL = "https://api.openai.com/v1/responses"
DEFAULT_CLAUDE_MODEL = "claude-sonnet-5-5"
DEFAULT_CODEX_MODEL = "gpt-5.3-codex"
DEFAULT_LMSTUDIO_MODEL = "qwen/qwen3-coder-30b"
DEFAULT_LMSTUDIO_URL = "http://localhost:1234/v1"
DEFAULT_TIMEOUT_SEC = 60.0
MAX_OUTPUT_TOKENS = 1024
# Claude は思考トークンも max_tokens に数えるため、LM Studio より大きく取る
CLAUDE_MAX_OUTPUT_TOKENS = 4096
CODEX_MAX_OUTPUT_TOKENS = 4096
CLAUDE_EFFORT = "low"
CODEX_EFFORT = "low"
STOP_REASON_REFUSAL = "refusal"
STOP_REASON_MAX_TOKENS = "max_tokens"
FINISH_REASON_LENGTH = "length"
# 同じ文を延々と繰り返すモデルがあるため、繰り返しにペナルティを掛ける
LMSTUDIO_FREQUENCY_PENALTY = 0.3
# ゲストの SCC 受信リングは 256 バイトで、溢れると黙って捨てる。
# 応答は 1 行ずつ間隔を空けて書き、ゲストが画面出力している間に溢れないようにする
DEFAULT_LINE_INTERVAL_SEC = 0.1
# ゲストの RS-232C ドライバは受信バッファの残量に応じて XOFF / XON を送ってくる。
# 通信文には現れない(Shift_JIS の1・2バイト目にも使われない)ので、受信から取り除く
FLOW_CONTROL_BYTES = b"\x11\x13"
RESPONSE_STATUS_INCOMPLETE = "incomplete"
RESPONSE_INCOMPLETE_MAX_OUTPUT_TOKENS = "max_output_tokens"
RESPONSE_CONTENT_REFUSAL = "refusal"
RESPONSE_CONTENT_OUTPUT_TEXT = "output_text"
AIQ_REPLY_SCHEMA_NAME = "aiq_reply"
AIQ_REPLY_SCHEMA = {
    "type": "object",
    "properties": {
        "message": {"type": "string"},
        "commands": {"type": "array", "items": {"type": "string"}},
    },
    "required": ["message", "commands"],
    "additionalProperties": False,
}
SYSTEM_PROMPT_PATH = Path(__file__).parent / "prompts" / "system.md"
DEFAULT_DOCS_DIR = Path(__file__).resolve().parents[2] / "docs" / "x68k-usage"
DEFAULT_DOCS_BUDGET_CHARS = 6000
REFERENCE_HEADING = "## 参考資料"
HTTP_BAD_REQUEST = 400
CONTEXT_OVERFLOW_HINT = "context length"
QUESTION_HEADING = "## 質問"

THINK_PATTERN = re.compile(r"<think>.*?</think>", re.DOTALL)
CONTROL_PATTERN = re.compile(r"[\x00-\x1f\x7f]")

Llm = Callable[[str], str]


class BridgeError(Exception):
    """ゲストへ ERR として返す失敗。メッセージは短い英数字の理由。"""


def parse_request(raw_line: bytes) -> str:
    """受信した 1 行から指示文を取り出す。"""
    try:
        line = raw_line.decode(GUEST_ENCODING)
    except UnicodeDecodeError as exc:
        raise BridgeError("bad encoding") from exc
    command, _, argument = line.partition(" ")
    if command != "ASK" or not argument.strip():
        raise BridgeError("bad request")
    return argument.strip()


def parse_commands(commands: object) -> list[str]:
    """LLM が返した commands を検証済みのコマンド列にする。"""
    if not isinstance(commands, list):
        raise BridgeError("bad llm output")
    validated = []
    for command in commands:
        if not isinstance(command, str) or CONTROL_PATTERN.search(command):
            raise BridgeError("bad command")
        command = command.strip()
        if not command:
            continue
        try:
            encoded = command.encode(GUEST_ENCODING)
        except UnicodeEncodeError as exc:
            raise BridgeError("unencodable") from exc
        if len(encoded) > MAX_LINE_BYTES:
            raise BridgeError("command too long")
        validated.append(command)
    return validated


def wrap_message(message: object) -> list[str]:
    """回答文を画面幅で折り返した行にする。Shift_JIS に無い文字は ? にする。"""
    if not isinstance(message, str):
        raise BridgeError("bad llm output")
    lines: list[str] = []
    for paragraph in message.replace("\t", " ").splitlines():
        paragraph = CONTROL_PATTERN.sub("", paragraph).rstrip()
        current, current_bytes = "", 0
        for char in paragraph:
            char_bytes = len(char.encode(GUEST_ENCODING, "replace"))
            if current_bytes + char_bytes > MSG_WRAP_BYTES:
                lines.append(current)
                current, current_bytes = "", 0
            current += char
            current_bytes += char_bytes
        if current:
            lines.append(current)
    while lines and not lines[-1].strip():
        lines.pop()
    if len(lines) > MAX_MESSAGE_LINES:
        lines = lines[:MAX_MESSAGE_LINES] + [MESSAGE_TRUNCATED_LINE]
    return lines


def parse_llm_output(llm_text: str) -> tuple[list[str], list[str]]:
    """LLM 出力から (回答の行, 検証済みコマンド列) を取り出す。"""
    text = THINK_PATTERN.sub("", llm_text)
    start = text.find("{")
    if start < 0:
        raise BridgeError("bad llm output")
    try:
        # 先頭の JSON 1個だけを読む(モデルが後ろに重複や残骸を付けることがある)
        parsed, _ = json.JSONDecoder().raw_decode(text, start)
        if not isinstance(parsed, dict) or not ({"message", "commands"} & parsed.keys()):
            raise BridgeError("bad llm output")
    except json.JSONDecodeError as exc:
        raise BridgeError("bad llm output") from exc
    message_lines = wrap_message(parsed["message"]) if "message" in parsed else []
    commands = parse_commands(parsed.get("commands", []))
    return message_lines, commands


def format_reply(message_lines: list[str], commands: list[str]) -> bytes:
    lines = [f"MSG {line}" for line in message_lines]
    lines += [f"CMD {command}" for command in commands] + ["END"]
    return b"".join(line.encode(GUEST_ENCODING, "replace") + LINE_TERMINATOR for line in lines)


def format_error(reason: str) -> bytes:
    return f"ERR {reason}".encode(GUEST_ENCODING, "replace") + LINE_TERMINATOR


def handle_line(raw_line: bytes, llm: Llm) -> bytes:
    """1 行の受信データに対する応答バイト列を返す。例外は ERR 応答に変換する。"""
    try:
        instruction = parse_request(raw_line)
        return format_reply(*parse_llm_output(llm(instruction)))
    except BridgeError as exc:
        return format_error(str(exc))


def post_json(url: str, headers: dict, payload: dict, timeout: float) -> dict:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", **headers},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as exc:
        response_body = exc.read().decode("utf-8", "replace")
        exc.close()
        if exc.code == HTTP_BAD_REQUEST and CONTEXT_OVERFLOW_HINT in response_body:
            raise BridgeError("context too small") from exc
        raise BridgeError(f"api http {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        if isinstance(exc, TimeoutError) or isinstance(getattr(exc, "reason", None), TimeoutError):
            raise BridgeError("timeout") from exc
        raise BridgeError("api unreachable") from exc
    except json.JSONDecodeError as exc:
        raise BridgeError("bad api response") from exc


def extract_claude_text(reply: dict) -> str:
    """Messages API の応答から回答テキストを取り出す。拒否・打ち切りは BridgeError にする。"""
    try:
        stop_reason = reply.get("stop_reason")
        if stop_reason == STOP_REASON_REFUSAL:
            raise BridgeError("refused")
        if stop_reason == STOP_REASON_MAX_TOKENS:
            raise BridgeError("truncated")
        return "".join(b["text"] for b in reply["content"] if b.get("type") == "text")
    except (KeyError, TypeError, AttributeError) as exc:
        raise BridgeError("bad api response") from exc


def make_claude_llm(model: str, system_prompt: str, timeout: float) -> Llm:
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        raise SystemExit("ANTHROPIC_API_KEY が未設定です")

    def ask(instruction: str) -> str:
        reply = post_json(
            ANTHROPIC_URL,
            {"x-api-key": api_key, "anthropic-version": ANTHROPIC_VERSION},
            {
                "model": model,
                "max_tokens": CLAUDE_MAX_OUTPUT_TOKENS,
                "output_config": {"effort": CLAUDE_EFFORT},
                "system": system_prompt,
                "messages": [{"role": "user", "content": instruction}],
            },
            timeout,
        )
        return extract_claude_text(reply)

    return ask


def extract_codex_text(reply: dict) -> str:
    """Responses API の応答から出力テキストを取り出す。拒否・打ち切りは BridgeError にする。"""
    try:
        if reply.get("status") == RESPONSE_STATUS_INCOMPLETE:
            details = reply.get("incomplete_details") or {}
            if details.get("reason") == RESPONSE_INCOMPLETE_MAX_OUTPUT_TOKENS:
                raise BridgeError("truncated")
        text_blocks = []
        for item in reply["output"]:
            for content in item["content"]:
                if content.get("type") == RESPONSE_CONTENT_REFUSAL:
                    raise BridgeError("refused")
                if content.get("type") == RESPONSE_CONTENT_OUTPUT_TEXT:
                    text_blocks.append(content["text"])
        return "".join(text_blocks)
    except (KeyError, TypeError, AttributeError) as exc:
        raise BridgeError("bad api response") from exc


def make_codex_llm(model: str, system_prompt: str, timeout: float) -> Llm:
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise SystemExit("OPENAI_API_KEY が未設定です")

    def ask(instruction: str) -> str:
        reply = post_json(
            OPENAI_RESPONSES_URL,
            {"Authorization": f"Bearer {api_key}"},
            {
                "model": model,
                "instructions": system_prompt,
                "input": instruction,
                "max_output_tokens": CODEX_MAX_OUTPUT_TOKENS,
                "reasoning": {"effort": CODEX_EFFORT},
                "text": {
                    "format": {
                        "type": "json_schema",
                        "name": AIQ_REPLY_SCHEMA_NAME,
                        "schema": AIQ_REPLY_SCHEMA,
                        "strict": True,
                    }
                },
            },
            timeout,
        )
        return extract_codex_text(reply)

    return ask


def extract_lmstudio_text(reply: dict) -> str:
    """OpenAI 互換の応答から回答テキストを取り出す。打ち切りは BridgeError にする。"""
    try:
        choice = reply["choices"][0]
        if choice.get("finish_reason") == FINISH_REASON_LENGTH:
            raise BridgeError("truncated")
        return choice["message"]["content"] or ""
    except (KeyError, IndexError, TypeError, AttributeError) as exc:
        raise BridgeError("bad api response") from exc


def make_lmstudio_llm(model: str, base_url: str, system_prompt: str, timeout: float) -> Llm:
    def ask(instruction: str) -> str:
        reply = post_json(
            f"{base_url.rstrip('/')}/chat/completions",
            {},
            {
                "model": model,
                "max_tokens": MAX_OUTPUT_TOKENS,
                "temperature": 0,
                "frequency_penalty": LMSTUDIO_FREQUENCY_PENALTY,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": instruction},
                ],
            },
            timeout,
        )
        return extract_lmstudio_text(reply)

    return ask


def open_tty(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    return fd


def write_paced(fd: int, reply: bytes, line_interval: float) -> None:
    """応答を 1 行ずつ、間隔を空けて書く。"""
    lines = reply.split(LINE_TERMINATOR)[:-1]
    for index, line in enumerate(lines):
        if index:
            time.sleep(line_interval)
        os.write(fd, line + LINE_TERMINATOR)


def serve(fd: int, llm: Llm, line_interval: float = DEFAULT_LINE_INTERVAL_SEC) -> None:
    """tty を読み続け、1 行ごとに応答する。EOF(PTY の相手切断)で戻る。"""
    buffer = b""
    while True:
        readable, _, _ = select.select([fd], [], [], SELECT_TIMEOUT_SEC)
        if not readable:
            continue
        try:
            chunk = os.read(fd, READ_CHUNK_BYTES)
        except OSError:
            return
        if not chunk:
            return
        buffer += chunk.translate(None, FLOW_CONTROL_BYTES)
        # CR / LF のどちらでも区切る。CRLF は空行が挟まるだけなので無視する
        *lines, buffer = re.split(rb"[\r\n]", buffer)
        if len(buffer) > MAX_LINE_BYTES * 4:
            buffer = b""
            os.write(fd, format_error("line too long"))
        for line in lines:
            if line:
                reply = handle_line(line, llm)
                # 切り分け用: 受信した生の 1 行と応答のバイト数を残す
                print(f"aiq-bridge: RX {line!r}", file=sys.stderr, flush=True)
                print(f"aiq-bridge: TX {len(reply)} bytes {reply[:60]!r}", file=sys.stderr, flush=True)
                write_paced(fd, reply, line_interval)


def with_reference_docs(llm: Llm, index: docs_search.DocsIndex, budget_chars: int) -> Llm:
    """質問に関連する資料の断片を「参考資料」として付けて LLM に渡す。"""

    def ask(instruction: str) -> str:
        chunks = index.search(instruction, budget_chars)
        if not chunks:
            return llm(instruction)
        reference = docs_search.format_reference(chunks)
        return llm(f"{REFERENCE_HEADING}\n\n{reference}\n\n{QUESTION_HEADING}\n\n{instruction}")

    return ask


def build_llm(args: argparse.Namespace) -> Llm:
    system_prompt = SYSTEM_PROMPT_PATH.read_text(encoding="utf-8")
    if args.backend == "claude":
        llm = make_claude_llm(args.model or DEFAULT_CLAUDE_MODEL, system_prompt, args.timeout)
    elif args.backend == "codex":
        llm = make_codex_llm(args.model or DEFAULT_CODEX_MODEL, system_prompt, args.timeout)
    else:
        llm = make_lmstudio_llm(
            args.model or DEFAULT_LMSTUDIO_MODEL, args.lmstudio_url, system_prompt, args.timeout
        )
    if args.no_docs:
        return llm
    docs_dir = Path(args.docs)
    if not docs_dir.is_dir():
        print(f"aiq-bridge: 資料ディレクトリがありません。資料なしで動作します: {docs_dir}", file=sys.stderr)
        return llm
    index = docs_search.DocsIndex(docs_dir)
    print(f"aiq-bridge: 資料 {len(index.chunks)} 断片を読み込みました: {docs_dir}", file=sys.stderr)
    return with_reference_docs(llm, index, args.docs_budget)


def main() -> int:
    parser = argparse.ArgumentParser(description="AIQ ホスト側ブリッジ")
    parser.add_argument("--version", action="version", version=f"aiq-bridge {VERSION}")
    parser.add_argument("--tty", required=True, help="PTY スレーブまたはシリアルデバイスのパス")
    parser.add_argument("--backend", choices=["claude", "codex", "lmstudio"], default="claude")
    parser.add_argument("--model", help="モデル名(省略時はバックエンドごとの既定値)")
    parser.add_argument("--lmstudio-url", default=DEFAULT_LMSTUDIO_URL)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SEC)
    parser.add_argument("--line-interval", type=float, default=DEFAULT_LINE_INTERVAL_SEC,
                        help="応答の 1 行ごとの送信間隔(秒)。ゲストの受信バッファ溢れを防ぐ")
    parser.add_argument("--docs", default=str(DEFAULT_DOCS_DIR), help="操作資料のディレクトリ")
    parser.add_argument("--no-docs", action="store_true", help="操作資料を参照しない")
    parser.add_argument("--docs-budget", type=int, default=DEFAULT_DOCS_BUDGET_CHARS,
                        help="1 回の質問に付ける資料の最大文字数")
    args = parser.parse_args()

    llm = build_llm(args)
    fd = open_tty(args.tty)
    print(f"aiq-bridge: {args.tty} backend={args.backend}", file=sys.stderr)
    serve(fd, llm, args.line_interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
