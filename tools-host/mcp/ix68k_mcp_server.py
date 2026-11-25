#!/usr/bin/env python3
"""X68000 エミュレータ MCP サーバー

代表的なツールの例（全ツールの一覧・説明は各 @mcp.tool() 関数の docstring 参照）:
  paste_text(text)    — UTF-8→Shift-JIS 変換してエミュレータにテキストを送る
  read_stdout_text() — B_PUTC/B_PRINT で出力されたテキストを取得する
  screenshot()        — 現在の画面を PNG で保存してパスを返す
"""

import asyncio
import bisect
import codecs
import collections
import hashlib
import io
import os
import re
import struct
import sys
import tempfile
import time
import tomllib
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP, Image
from ix68k_wire import Request, WireClient, WireConnectionError, WireTimeoutError, parse_port

# トランスポートは「エミュレータ側が何をlistenしているか」で決まる。Windowsビルドの
# ix68k は AF_INET(127.0.0.1) だけを listen する（Issue #185: python.org配布の
# Windows版Pythonは socket.AF_UNIX を持たず、定数を補っても connect が bad family に
# なる）。したがって判定は hasattr(socket, "AF_UNIX") ではなく sys.platform で行う
# ——Pythonの能力ではなくサーバの実装が決めることなので。
_USE_TCP = sys.platform == "win32"

if _USE_TCP:
    # src/mcp/port.h の MCP_DEFAULT_PORT と一致必須（変えるときは同時に直す）。
    _DEFAULT_PORT = 26800
    try:
        PORT = parse_port(os.environ.get("IX68K_MCP_PORT"), _DEFAULT_PORT)
    except ValueError as e:
        # import時に握り潰さない。既定ポートへ落ちて別インスタンスへ気付かず
        # 繋がるより、起動時に例外で落ちる方がよい（D14と同じ理屈）。
        raise SystemExit(str(e))
    # "localhost" は環境によって ::1 を先に返し AF_INET のサーバへ繋がらないので使わない。
    HOST = "127.0.0.1"
    ENDPOINT = f"{HOST}:{PORT}"
else:
    # Windowsに/tmpは無いため%TEMP%を使う（C側のMCPソケットと同じ場所に
    # 合流させる、Issue #108）。
    _DEFAULT_SOCK = "/tmp/ix68k_mcp.sock"
    # ソケットパス。IX68K_MCP_SOCK があればそれを使う（Issue #144: 複数インスタンスを
    # 同時に動かすとき、エミュレータ(C)とこのサーバは別プロセスなので、両方に同じ値を
    # 環境変数で配る以外に待ち合わせる手段が無い）。
    SOCK_PATH = os.environ.get("IX68K_MCP_SOCK") or _DEFAULT_SOCK
    ENDPOINT = SOCK_PATH

if _USE_TCP:
    if os.environ.get("IX68K_MCP_PORT"):
        # 複数インスタンス運用時、既定のスクリーンショット保存先を2台で奪い合わない
        # よう、ポート番号から導出する（Issue #144 D9のWindows版、Issue #185 D12）。
        _DEFAULT_SCREENSHOT_PATH = str(
            Path(tempfile.gettempdir()) / f"x68k_screenshot_{PORT}.png"
        )
    else:
        _DEFAULT_SCREENSHOT_PATH = str(Path(tempfile.gettempdir()) / "x68k_screenshot.png")
elif os.environ.get("IX68K_MCP_SOCK"):
    # 複数インスタンス運用時、既定のスクリーンショット保存先を2台で奪い合わない
    # よう、ソケットパスの語幹から導出する（Issue #144）。
    _sock_stem = Path(SOCK_PATH)
    _DEFAULT_SCREENSHOT_PATH = str(_sock_stem.with_name(_sock_stem.stem + "_screenshot.png"))
else:
    _DEFAULT_SCREENSHOT_PATH = "/tmp/x68k_screenshot.png"

mcp = FastMCP("x68k-emulator")


def _raw_send(cmd: str, timeout: float, body: bytes = b"") -> tuple[str, bytes]:
    """接続してコマンドを1本送り、(ステータス文字列, 本体バイト列) を受け取って
    ソケットを閉じる（1コマンド1接続の運用）。プロトコル版チェックはしない
    （_check_protocol からも使うため、ここで呼ぶと無限再帰になる）。"""
    response = _wire._raw_send(Request(cmd, body), timeout)
    return response.status, response.body


# ワイヤプロトコル版。エミュレータ側の MCP_PROTO_VERSION（src/mcp/commands.c）と
# 一致していなければならない。ずれたまま使うと全コマンドが ERR UNKNOWN になり、
# 「エミュレータのビルドが古い」ことに気付けないので接続時に必ず確認する。
_PROTO_EXPECTED = 5
_wire = WireClient((HOST, PORT) if _USE_TCP else SOCK_PATH)


def _check_protocol() -> None:
    _wire.ensure_protocol()


def _send_command(cmd: str, timeout: float = 10.0, body: bytes = b"") -> tuple[str, bytes]:
    """エミュレータへコマンドを送り、(ステータス文字列, 本体バイト列) を返す。
    本体バイト列は旧base64応答（MEM READ/DISASM/RING/DISK STATUS/SCREENSHOT等）の
    置き換えで、生バイト列がそのまま入る（base64廃止、Issue #103 Stage 2）。
    `DBG WAIT`/`KEY WAIT` のようにサーバ側が待つコマンドでは、その待ち時間より
    長い timeout を渡すこと。"""
    response = _wire.send(cmd, timeout, body)
    return response.status, response.body


def _parse_regs(line: str) -> dict:
    """`REGS` / `DBG STATUS` レスポンス行をパースしてdict を返す。
    16進として解釈できないトークン（`DBG STATUS`等の "STATE=paused" "STOP=BREAKPOINT"
    のような非レジスタ値）は無視する（mcp_design.md §3、新コマンドの
    stopinfoと同じパーサを将来共用できるようにする堅牢化）。"""
    result = {}
    for token in line.split():
        if "=" in token:
            k, v = token.split("=", 1)
            try:
                result[k] = int(v, 16)
            except ValueError:
                continue
    return result


def _format_regs(regs: dict) -> str:
    dregs = "  ".join(f"D{i}={regs.get(f'D{i}', 0):08X}" for i in range(8))
    aregs = "  ".join(f"A{i}={regs.get(f'A{i}', 0):08X}" for i in range(8))
    misc  = f"PC={regs.get('PC', 0):08X}  SR={regs.get('SR', 0):04X}  USP={regs.get('USP', 0):08X}  SSP={regs.get('SSP', 0):08X}"
    return f"{dregs}\n{aregs}\n{misc}"


@mcp.tool()
def read_stdout_text() -> str:
    """エミュレータから B_PUTC/B_PRINT で出力されたテキストを読み取る
    （プログラムの標準出力に相当）。前回の呼び出し以降に出力された分が返り、
    バッファはクリアされる。
    画面に見えている文字そのものが欲しい場合は read_bitmap_text() を使う
    （こちらはTVRAMのピクセルをCGROMと照合する別経路）。"""
    status, body = _send_command("TEXT STDOUT")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    if not body:
        return "（出力なし）"
    return body.decode("shift_jis", errors="replace")


_TEXT_EVENT_BUFFER_LIMIT = 1024 * 1024
# 1文字1イベントのログでdictが際限なく増えないよう、エミュレータ側リング(16384件)の2倍で打ち切る。
_TEXT_EVENT_BUFFER_MAX_EVENTS = 32768
_TEXT_EVENT_FIELDS = frozenset({
    "seq", "epoch", "pc", "x", "y", "attr", "flags", "source", "probe", "encoding",
    "raw", "raw_hex", "text",
})
_text_event_buffer: collections.deque[dict[str, Any]] = collections.deque()
_text_event_buffer_size = 0
_text_event_epoch: int | None = None
_text_event_contiguous_from: int | None = None
# バッファを破棄(エミュレータの再起動・リセット検出)するたびに増やす。
_text_event_generation = 0


def _decode_text_event(raw: bytes, encoding: str) -> str:
    return raw.decode("shift_jis" if encoding == "shift-jis" else "utf-8", errors="replace")


def _append_text_events(events: list[dict[str, Any]], epoch: int) -> None:
    """ドレイン済みイベントを1 MiBのペイロード上限で保持する。

    epoch が変わるか seq が巻き戻ったら、エミュレータが再起動・リセットされたとみなして
    旧バッファを捨てる。取りこぼしや seq の飛びがあれば、連続して読める範囲の先頭を
    新しいイベントの先頭へ進める。
    """
    global _text_event_buffer_size, _text_event_epoch, _text_event_contiguous_from
    global _text_event_generation
    last_seq = _text_event_buffer[-1]["seq"] if _text_event_buffer else None
    restarted = _text_event_epoch is not None and (
        epoch != _text_event_epoch
        or (events and last_seq is not None and events[0]["seq"] <= last_seq))
    if restarted:
        _text_event_buffer.clear()
        _text_event_buffer_size = 0
        _text_event_contiguous_from = None
        _text_event_generation += 1
        last_seq = None
    _text_event_epoch = epoch
    if events and (last_seq is None or events[0]["seq"] != last_seq + 1):
        _text_event_contiguous_from = events[0]["seq"]
    _text_event_buffer.extend(events)
    _text_event_buffer_size += sum(len(event["raw_bytes"]) for event in events)
    while _text_event_buffer and (_text_event_buffer_size > _TEXT_EVENT_BUFFER_LIMIT
                                  or len(_text_event_buffer) > _TEXT_EVENT_BUFFER_MAX_EVENTS):
        removed = _text_event_buffer.popleft()
        _text_event_buffer_size -= len(removed["raw_bytes"])


def _drain_text_events(timeout: float = 10.0) -> int:
    """TEXT EVENTS を一度だけ消費して検索用バッファへ追加し、今回の取りこぼし件数を返す。"""
    status, body = _send_command("TEXT EVENTS", timeout=timeout)
    if status != "OK" or len(body) < 20 or body[:4] != b"TEV1":
        raise RuntimeError(f"エラー: {status}")
    count = int.from_bytes(body[4:8], "big")
    dropped = int.from_bytes(body[8:12], "big")
    epoch = int.from_bytes(body[12:20], "big")
    pos, events = 20, []
    for _ in range(count):
        if pos + 118 > len(body):
            raise RuntimeError("TEXT EVENTS response truncated")
        seq = int.from_bytes(body[pos:pos+8], "big"); event_epoch = int.from_bytes(body[pos+8:pos+16], "big")
        pc = int.from_bytes(body[pos+16:pos+20], "big")
        x = int.from_bytes(body[pos+20:pos+24], "big", signed=True); y = int.from_bytes(body[pos+24:pos+28], "big", signed=True)
        attr = int.from_bytes(body[pos+28:pos+32], "big", signed=True); flags = int.from_bytes(body[pos+32:pos+34], "big")
        source = body[pos+34:pos+66].split(b"\0", 1)[0].decode("ascii", "replace")
        probe = body[pos+66:pos+98].split(b"\0", 1)[0].decode("ascii", "replace")
        encoding = body[pos+98:pos+114].split(b"\0", 1)[0].decode("ascii", "replace")
        size = int.from_bytes(body[pos+114:pos+118], "big"); pos += 118
        if pos + size > len(body): raise RuntimeError("TEXT EVENTS payload truncated")
        raw = body[pos:pos+size]; pos += size
        events.append({"seq": seq, "epoch": event_epoch, "pc": pc, "x": x, "y": y, "attr": attr,
                       "flags": flags, "source": source, "probe": probe, "encoding": encoding,
                       "raw_bytes": raw})
    if pos != len(body):
        raise RuntimeError("TEXT EVENTS response has trailing bytes")
    _append_text_events(events, epoch)
    return dropped


def _event_detail(event: dict[str, Any], fields: list[str]) -> dict[str, Any]:
    result = {}
    for field in fields:
        if field == "raw":
            result[field] = event["raw_bytes"].decode("ascii", "backslashreplace")
        elif field == "raw_hex":
            result[field] = event["raw_bytes"].hex()
        elif field == "text":
            result[field] = _decode_text_event(event["raw_bytes"], event["encoding"])
        else:
            result[field] = event[field]
    return result


def _event_streams(events: list[dict[str, Any]]) -> list[dict[str, str]]:
    """同一probeのrawバイトを連結してからデコードする。

    Shift-JISの2バイト文字がイベント境界で分断されても、ここで復元される。
    """
    streams: dict[str, list[tuple[str, bytearray]]] = {}
    for event in events:
        chunks = streams.setdefault(event["probe"], [])
        if not chunks or chunks[-1][0] != event["encoding"]:
            chunks.append((event["encoding"], bytearray()))
        chunks[-1][1].extend(event["raw_bytes"])
    return [{"probe": probe, "text": "".join(_decode_text_event(bytes(raw), encoding)
             for encoding, raw in chunks)} for probe, chunks in streams.items()]


def _event_chunks(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """連続する同じ出所・エンコーディングのイベントを1つにまとめる。"""
    chunks: list[dict[str, Any]] = []
    for event in events:
        if (chunks and chunks[-1]["probe"] == event["probe"]
                and chunks[-1]["source"] == event["source"]
                and chunks[-1]["encoding"] == event["encoding"]
                and chunks[-1]["last_seq"] + 1 == event["seq"]):
            chunks[-1]["raw_bytes"].extend(event["raw_bytes"])
            chunks[-1]["last_seq"] = event["seq"]
            continue
        chunks.append({"first_seq": event["seq"], "last_seq": event["seq"],
                       "probe": event["probe"], "source": event["source"],
                       "encoding": event["encoding"], "raw_bytes": bytearray(event["raw_bytes"])})
    return [{"first_seq": chunk["first_seq"], "last_seq": chunk["last_seq"],
             "probe": chunk["probe"], "source": chunk["source"], "encoding": chunk["encoding"],
             "text": _decode_text_event(bytes(chunk["raw_bytes"]), chunk["encoding"])}
            for chunk in chunks]


def _filter_text(text: str, after: str | None, until: str | None,
                 contains: str | None, regex: bool, include_match: bool) -> str | None:
    finder = re.compile if regex else None
    def search(pattern: str, value: str) -> re.Match[str] | None:
        return finder(pattern).search(value) if finder else re.search(re.escape(pattern), value)

    if after is not None:
        match = search(after, text)
        if match is None:
            return None
        text = text[match.start() if include_match else match.end():]
    if until is not None:
        match = search(until, text)
        if match is None:
            return None
        text = text[:match.end() if include_match else match.start()]
    if contains is not None and search(contains, text) is None:
        return None
    return text


@mcp.tool()
def read_text_events(probe: str | None = None, source: str | None = None,
                     since_seq: int | None = None, max_events: int | None = None,
                     after: str | None = None, until: str | None = None,
                     contains: str | None = None, regex: bool = False,
                     include_match: bool = True, detail: str = "text",
                     fields: list[str] | None = None) -> dict[str, Any]:
    """TEXT EVENTSを圧縮テキストとして読む。

    既定のdetail="text"はprobeごとの連結済み文字列を返す。since_seqには前回の
    next_seqを渡す。after、until、containsは連結後の各probeテキストに順に適用する。
    detail="chunks"は連続イベントの要約、detail="events"はfieldsで選んだ生イベントを返す。
    サーバは直近1 MiBのrawペイロードを保持するため、ドレイン後にも検索できる。"""
    if since_seq is not None and since_seq < 0:
        raise ValueError("since_seq は0以上で指定してください")
    if max_events is not None and max_events <= 0:
        raise ValueError("max_events は1以上で指定してください")
    if detail not in {"text", "chunks", "events"}:
        raise ValueError('detail は "text", "chunks", "events" のいずれかです')
    if fields is not None and detail != "events":
        raise ValueError("fields は detail=\"events\" のときだけ指定できます")
    if fields is not None:
        unknown = set(fields) - _TEXT_EVENT_FIELDS
        if unknown:
            raise ValueError(f"未知のfields: {sorted(unknown)}")

    dropped = _drain_text_events()
    selected = [event for event in _text_event_buffer
                if (probe is None or event["probe"] == probe)
                and (source is None or event["source"] == source)
                and (since_seq is None or event["seq"] >= since_seq)]
    truncated = max_events is not None and len(selected) > max_events
    if truncated:
        selected = selected[:max_events]

    available_from_seq = (max(_text_event_buffer[0]["seq"], _text_event_contiguous_from or 0)
                          if _text_event_buffer else None)
    high_water = _text_event_buffer[-1]["seq"] + 1 if _text_event_buffer else 0
    result: dict[str, Any] = {
        "next_seq": selected[-1]["seq"] + 1 if truncated else high_water,
        "available_from_seq": available_from_seq,
    }
    if dropped:
        result["dropped"] = dropped

    if detail == "events":
        result["events"] = [_event_detail(event, fields or ["seq", "probe", "source", "text"])
                            for event in selected]
    elif detail == "chunks":
        result["chunks"] = _event_chunks(selected)
    else:
        streams = _event_streams(selected)
        result["streams"] = [
            {"probe": stream["probe"], "text": text}
            for stream in streams
            if (text := _filter_text(stream["text"], after, until, contains, regex, include_match)) is not None
        ]
    return result


_WAIT_FOR_MAX_MS = 25000
_WAIT_FOR_POLL_S = 0.05
_WAIT_FOR_POLL_TIMEOUT_S = 2.0
_WAIT_FOR_LOCAL_LIMIT_CHARS = 4 * 1024 * 1024
_WAIT_FOR_TAIL_CHARS = 200

# wait_for() が既定で探し始める位置: (seq, そのイベント内のバイト位置, 世代)。
# 更新するのは「入力系ツールの送信直前」「wait_for のマッチ」「バッファ破棄」の3契機だけ。
_wait_cursor: tuple[int, int, int] | None = None


def _text_event_high_water() -> int:
    return _text_event_buffer[-1]["seq"] + 1 if _text_event_buffer else 0


def _mark_wait_cursor() -> None:
    """入力系ツールが送信する直前に、wait_for の既定の起点を現在の出力末尾へ進める。

    drain に失敗しても入力そのものは止めず、カーソルを無効にするだけにする
    （無効なら次の wait_for は呼び出し時点の末尾から探す）。"""
    global _wait_cursor
    try:
        _drain_text_events()
    except (RuntimeError, OSError):
        _wait_cursor = None
        return
    _wait_cursor = (_text_event_high_water(), 0, _text_event_generation)


class _ProbeScan:
    """1つのprobeのrawバイトを増分デコードして追記し、パターンを増分検索する。

    文字位置は破棄済みの先頭を含む絶対位置で持つ。マッチ末尾の文字位置から、
    それが属するイベントとイベント内のバイト位置を引ける。"""

    def __init__(self, probe: str):
        self.probe = probe
        self.base = 0
        self.text = ""
        self.scanned = 0
        self.truncated = False
        self.last_seq = -1
        self._encoding: str | None = None
        self._decoder: codecs.IncrementalDecoder | None = None
        self._mark_ends: list[int] = []
        self._marks: list[tuple[int, int, int, str]] = []

    @property
    def end(self) -> int:
        return self.base + len(self.text)

    def _flush_decoder(self) -> None:
        if self._decoder is not None:
            self.text += self._decoder.decode(b"", final=True)
            self._decoder = None

    def feed(self, event: dict[str, Any], byte_offset: int) -> None:
        if event["encoding"] != self._encoding:
            self._flush_decoder()
            self._encoding = event["encoding"]
            self._decoder = codecs.getincrementaldecoder(
                "shift_jis" if self._encoding == "shift-jis" else "utf-8")(errors="replace")
        chunk = self._decoder.decode(event["raw_bytes"][byte_offset:])
        self.text += chunk
        self.last_seq = event["seq"]
        self._mark_ends.append(self.end)
        self._marks.append((event["seq"], byte_offset, len(chunk), self._encoding))
        excess = len(self.text) - _WAIT_FOR_LOCAL_LIMIT_CHARS
        if excess > 0:
            self.text = self.text[excess:]
            self.base += excess
            self.truncated = True
            keep = bisect.bisect_left(self._mark_ends, self.base)
            del self._mark_ends[:keep], self._marks[:keep]

    def search(self, literal: str | None, pattern: re.Pattern[str] | None) -> tuple[int, int] | None:
        """未検索の追記分を含めて検索し、(開始, 終了) の絶対位置を返す。"""
        if literal is not None:
            index = self.text.find(literal, max(0, self.scanned - self.base - len(literal) + 1))
            self.scanned = self.end
            return None if index < 0 else (self.base + index, self.base + index + len(literal))
        self.scanned = self.end
        match = pattern.search(self.text)
        return None if match is None else (self.base + match.start(), self.base + match.end())

    def cursor_after(self, position: int) -> tuple[int, int]:
        """絶対位置の直後を指す (seq, イベント内バイト位置) を返す。"""
        i = bisect.bisect_left(self._mark_ends, position)
        if i >= len(self._marks):
            i = len(self._marks) - 1
        seq, byte_offset, chunk_len, encoding = self._marks[i]
        end = self._mark_ends[i]
        if position >= end:
            return seq + 1, 0
        prefix = self.text[end - chunk_len - self.base:position - self.base]
        codec = "shift_jis" if encoding == "shift-jis" else "utf-8"
        return seq, byte_offset + len(prefix.encode(codec, errors="replace"))


def _wait_start_position(since_seq: int | None) -> tuple[int, int, bool]:
    """(開始seq, イベント内バイト位置, 世代変更で先頭に戻したか) を決める。"""
    if since_seq is not None:
        return since_seq, 0, False
    if _wait_cursor is None:
        return _text_event_high_water(), 0, False
    seq, byte_offset, generation = _wait_cursor
    if generation != _text_event_generation:
        return 0, 0, True
    return seq, byte_offset, False


@mcp.tool()
def wait_for(pattern: str, timeout_ms: int = 10000, regex: bool = False,
             since_seq: int | None = None, probe: str | None = None,
             source: str | None = None, include_match: bool = True,
             max_chars: int = 4000) -> dict[str, Any]:
    """パターンがテキスト出力に現れるまで待つ（Issue #242）。

    約50ms間隔でTEXT EVENTSを読み、probeごとの連結テキストを検索する。
    1回の呼び出しで「login:まで待つ」が完結する。
    since_seq省略時は、直前の入力（paste_text/key/run_command）の送信直前、または
    直前のwait_forのマッチ末尾の直後から探す。タイムアウトしてもその位置は動かないので、
    引数なしで呼び直せる。サーバ起動直後はこの位置が未設定で、呼び出し時点の末尾から探す。
    返すtextはその位置からマッチ末尾まで（include_match=Falseならマッチ直前まで）。
    max_charsを超える場合は末尾側を残す（マッチ文字列は残す）。
    タイムアウトは例外にせずmatched=falseで返す（key_waitとは異なる）。その場合
    textは直近のprobeの末尾、next_seqは起点のままである。
    timeout_msの上限は25000。待機中は他のツール呼び出しが直列化され、キャンセル通知も
    待機の終了まで処理されない。timeout_ms<=0は1回だけ読んで照合する。
    regex=Trueなら位置からの連結テキストに対してre.searchする（'^'は起点にだけ一致する。
    行頭にはinline flagの(?m)を使う）。ポーリング時点までのデータで判定するため、
    'login:\\s*'のような貪欲なパターンは途中で確定する。
    複数probeが同時に一致したら、マッチ末尾のseqが最小のものを1件返す。同じテキストを
    二重に出すprobeがあるときはsourceで絞る。
    next_seqはread_text_eventsと同じくイベント単位（マッチ末尾を含むイベントの次）。
    同じイベント内の続きは、既定の位置から探すかぎり失われない。"""
    global _wait_cursor
    if since_seq is not None and since_seq < 0:
        raise ValueError("since_seq は0以上で指定してください")
    if max_chars <= 0:
        raise ValueError("max_chars は1以上で指定してください")
    if not pattern:
        raise ValueError("pattern は空にできません")
    literal, compiled = pattern, None
    if regex:
        try:
            literal, compiled = None, re.compile(pattern)
        except re.error as exc:
            raise ValueError(f"pattern が正規表現として不正です: {exc}") from exc

    started = time.monotonic()
    deadline = started + min(max(timeout_ms, 0), _WAIT_FOR_MAX_MS) / 1000.0
    dropped_total, warnings = 0, []
    connection_errors = 0
    restarted = False

    def poll() -> bool:
        nonlocal dropped_total, connection_errors
        remaining = max(deadline - time.monotonic(), 0.0)
        _auto_text_probe_tick(_AUTO_WAIT_TICK_INTERVAL_S,
                              min(remaining + 1.0, _WAIT_FOR_POLL_TIMEOUT_S))
        try:
            dropped_total += _drain_text_events(min(remaining + 1.0, _WAIT_FOR_POLL_TIMEOUT_S))
        except (WireConnectionError, WireTimeoutError):
            # エミュレータの再起動中などは、期限まで再試行する。
            connection_errors += 1
            return False
        return True

    poll()
    start_seq, start_offset, generation_reset = _wait_start_position(since_seq)
    restarted = generation_reset
    generation = _text_event_generation
    since_seq_used = start_seq
    if since_seq is None:
        # 位置が未設定・無効だった場合も確定させ、タイムアウト後の呼び直しが同じ起点から続くようにする。
        _wait_cursor = (start_seq, start_offset, generation)
    available_from = (max(_text_event_buffer[0]["seq"], _text_event_contiguous_from or 0)
                      if _text_event_buffer else None)
    # 起点0は「最初から」の意味(バッファが空のときのカーソルを含む)なので、欠落とはみなさない。
    if (available_from is not None and 0 < start_seq < available_from
            and not generation_reset):
        warnings.append(f"起点seq {start_seq} が保持範囲の先頭 {available_from} より前です。"
                        "その間の出力は失われています")

    scans: dict[str, _ProbeScan] = {}
    last_seq = start_seq - 1
    while True:
        if _text_event_generation != generation:
            generation = _text_event_generation
            scans.clear()
            start_seq, start_offset, last_seq = 0, 0, -1
            restarted = True
        fresh = []
        for event in reversed(_text_event_buffer):
            if event["seq"] <= last_seq:
                break
            fresh.append(event)
        fresh.reverse()
        if fresh and last_seq >= start_seq and fresh[0]["seq"] != last_seq + 1:
            warnings.append(f"seq {last_seq + 1}..{fresh[0]['seq'] - 1} の出力が欠落しています")
        touched = []
        for event in fresh:
            last_seq = event["seq"]
            if event["seq"] < start_seq:
                continue
            if (probe is not None and event["probe"] != probe) or (
                    source is not None and event["source"] != source):
                continue
            scan = scans.setdefault(event["probe"], _ProbeScan(event["probe"]))
            scan.feed(event, start_offset if event["seq"] == start_seq else 0)
            if scan not in touched:
                touched.append(scan)

        hits = []
        for scan in touched:
            found = scan.search(literal, compiled)
            if found is not None:
                hits.append((scan.cursor_after(found[1]), scan, found))
        if hits:
            (cursor_seq, cursor_offset), scan, (match_start, match_end) = min(
                hits, key=lambda hit: (hit[0][0], hit[0][1]))
            cut = match_end if include_match else match_start
            text = scan.text[:cut - scan.base]
            truncated = scan.truncated
            if len(text) > max_chars:
                text = text[-max(max_chars, match_end - match_start if include_match else 0):]
                truncated = True
            _wait_cursor = (cursor_seq, cursor_offset, _text_event_generation)
            result: dict[str, Any] = {
                "matched": True, "probe": scan.probe, "text": text,
                "next_seq": cursor_seq if cursor_offset == 0 else cursor_seq + 1,
            }
            if truncated:
                result["truncated"] = True
            break
        if time.monotonic() >= deadline:
            latest = max(scans.values(), key=lambda scan: scan.last_seq, default=None)
            result = {"matched": False, "probe": latest.probe if latest else None,
                      "text": latest.text[-min(max_chars, _WAIT_FOR_TAIL_CHARS):] if latest else "",
                      "next_seq": start_seq}
            break
        time.sleep(min(_WAIT_FOR_POLL_S, max(deadline - time.monotonic(), 0.0)))
        poll()

    if connection_errors:
        warnings.append(f"エミュレータとの通信に{connection_errors}回失敗しました（再試行）")
    if dropped_total:
        result["dropped"] = dropped_total
        warnings.append(f"エミュレータ側リングで{dropped_total}件取りこぼしました。"
                        "マッチを見逃した可能性があります")
    result["available_from_seq"] = (
        max(_text_event_buffer[0]["seq"], _text_event_contiguous_from or 0)
        if _text_event_buffer else None)
    result["since_seq_used"] = since_seq_used
    result["elapsed_ms"] = int((time.monotonic() - started) * 1000)
    if restarted:
        result["restarted"] = True
    if warnings:
        result["warning"] = " / ".join(dict.fromkeys(warnings))
    return result


_TEXT_PROFILE_DIR = Path(__file__).with_name("text_profiles")
_PROFILE_NAME_RE = re.compile(r"[a-z0-9][a-z0-9_-]*")


def _read_x68k_symbols(binary_path: str) -> tuple[int, dict[str, list[tuple[int, int]]]]:
    """Human68k X形式末尾のシンボル表を (load_base, name -> [(type, value)]) として読む。"""
    data = Path(binary_path).read_bytes()
    if len(data) < 64 or data[:2] != b"HU":
        raise ValueError("symbol を使う profile の binary_path は Human68k X形式である必要があります")
    base, _entry, text_size, data_size, _bss_size, reloc_size, symbol_size = struct.unpack(
        ">7I", data[4:32]
    )
    if symbol_size == 0:
        raise ValueError("対象Xファイルにシンボル表がありません")
    start = 64 + text_size + data_size + reloc_size
    end = start + symbol_size
    if end > len(data):
        raise ValueError("対象Xファイルのシンボル表がファイル範囲外です")
    symbols: dict[str, list[tuple[int, int]]] = {}
    pos = start
    while pos < end:
        if pos + 6 > end:
            raise ValueError("対象Xファイルのシンボル表が途中で終わっています")
        symbol_type, value = struct.unpack(">HI", data[pos:pos + 6])
        name_start = pos + 6
        nul = data.find(b"\0", name_start, end)
        if nul < 0:
            raise ValueError("対象Xファイルのシンボル名が終端されていません")
        name_bytes = data[name_start:nul]
        try:
            name = name_bytes.decode("cp932")
        except UnicodeDecodeError as exc:
            raise ValueError("対象Xファイルのシンボル名がCP932として不正です") from exc
        pos = nul + (1 if len(name_bytes) & 1 else 2)
        if pos > end:
            raise ValueError("対象Xファイルのシンボル表のパディングが不正です")
        symbols.setdefault(name, []).append((symbol_type, value))
    if pos != end:
        raise ValueError("対象Xファイルのシンボル表サイズが不正です")
    return base, symbols


def _text_probe_command(command: str, timeout: float = 10.0) -> str:
    status, body = _send_command(f"TEXT PROBE {command}", timeout=timeout)
    if status != "OK" and not status.startswith("OK "):
        raise RuntimeError(f"TEXT PROBE {command}: {status}")
    # STATUS は値（COUNT=.. ENABLED=..）を `OK <値>` のヘッダ行に載せ、本文は空
    value = status[len("OK "):] if status.startswith("OK ") else ""
    return (value + body.decode("ascii", "replace")).strip()


def _load_text_profile(name: str, binary_path: str | None) -> tuple[dict, list[dict]]:
    if not _PROFILE_NAME_RE.fullmatch(name):
        raise ValueError("profile は英小文字・数字・_・-だけで指定してください")
    profile_path = _TEXT_PROFILE_DIR / f"{name}.toml"
    try:
        profile = tomllib.loads(profile_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"text profile がありません: {name}") from exc
    target = profile.get("target")
    probes = profile.get("probe")
    if not isinstance(target, dict) or not isinstance(probes, list) or not probes:
        raise ValueError(f"text profile の形式が不正です: {name}")
    expected_hash = target.get("sha256")
    if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
        raise ValueError(f"text profile の SHA-256 が不正です: {name}")
    if not binary_path:
        raise ValueError("profile を有効化するには検証対象の binary_path が必要です")
    with open(binary_path, "rb") as binary:
        actual_hash = hashlib.file_digest(binary, "sha256").hexdigest()
    if actual_hash != expected_hash:
        raise ValueError(f"対象バイナリの SHA-256 が一致しません: {name}")
    for probe in probes:
        required = ("action", "value", "size", "source", "name", "encoding", "signature")
        if not isinstance(probe, dict) or any(key not in probe for key in required):
            raise ValueError(f"text profile の probe 定義が不正です: {name}")
        if probe.get("space", "current") not in _TEXT_PROBE_SPACES:
            raise ValueError(f"text profile の probe space が不正です: {name}")
        location_keys = [key for key in ("pc", "file_offset", "symbol") if key in probe]
        if (len(location_keys) != 1 or
                ("pc" in probe and (not isinstance(probe["pc"], int) or probe["pc"] < 0)) or
                ("file_offset" in probe and
                 (not isinstance(probe["file_offset"], int) or probe["file_offset"] < 0x40)) or
                ("symbol" in probe and
                 (not isinstance(probe["symbol"], str) or not probe["symbol"])) or
                not isinstance(probe["signature"], str) or
                not re.fullmatch(r"(?:[0-9a-fA-F]{2}){8,}", probe["signature"])):
            raise ValueError(f"text profile のPC/ファイルオフセットまたは入口署名が不正です: {name}")
    if any("symbol" in probe for probe in probes):
        xbase, symbols = _read_x68k_symbols(binary_path)
        for probe in probes:
            if "symbol" not in probe:
                continue
            matches = symbols.get(probe["symbol"], [])
            if len(matches) != 1:
                raise ValueError(
                    f"対象Xファイルのシンボル {probe['symbol']!r} を一意に解決できません: "
                    f"{len(matches)} candidates"
                )
            symbol_type, value = matches[0]
            if symbol_type != 0x0201:
                raise ValueError(f"シンボル {probe['symbol']!r} はTEXTシンボルではありません")
            if xbase:
                probe["pc"] = value
            else:
                probe["file_offset"] = value + 0x40
    return target, probes


def _resolve_text_probe_offsets(target: dict, probes: list[dict]) -> None:
    """file_offset形式のプローブを、実行中メモリの一意な入口署名から解決する。"""
    unresolved = [probe for probe in probes if "pc" not in probe]
    if not unresolved:
        return
    start, end = target.get("scan_start"), target.get("scan_end")
    if (not isinstance(start, int) or not isinstance(end, int) or
            start < 0 or end > 0x1000000 or end <= start or end - start > 0x200000):
        raise ValueError("file_offset profile には2 MiB以下の scan_start/scan_end が必要です")
    anchor = unresolved[0]
    signature = bytes.fromhex(anchor["signature"])
    matches: list[int] = []
    overlap = b""
    for address in range(start, end, 256):
        length = min(256, end - address)
        status, data = _send_command(f"MEM PEEK {address:X} {length:X}")
        if status != "OK" or len(data) != length:
            raise RuntimeError(f"入口探索でメモリを読めません: PC={address:06X}")
        window = overlap + data
        pos = 0
        while True:
            pos = window.find(signature, pos)
            if pos < 0:
                break
            matches.append(address - len(overlap) + pos)
            pos += 1
        overlap = window[-(len(signature) - 1):]
    if len(matches) != 1:
        raise RuntimeError(f"入口署名が一意に解決できません: {len(matches)} candidates")
    load_base = matches[0] - (anchor["file_offset"] - 0x40)
    for probe in unresolved:
        probe["pc"] = load_base + (probe["file_offset"] - 0x40)


# probe の入口を読むアドレス空間。supervisor はゲストがユーザーモード中でもカーネルテキストを読む（MEM PEEK ... SUPER）。
_TEXT_PROBE_SPACES = ("current", "supervisor")


def _verify_text_probe_signature(probe: dict) -> None:
    signature = bytes.fromhex(probe["signature"])
    suffix = " SUPER" if probe.get("space") == "supervisor" else ""
    status, actual = _send_command(f"MEM PEEK {probe['pc']:X} {len(signature):X}{suffix}")
    if status != "OK" or actual != signature:
        raise RuntimeError(f"入口署名が一致しません: PC={probe['pc']:06X}")


def _format_text_probe_add(probe: dict) -> str:
    if not isinstance(probe["action"], str) or not isinstance(probe["value"], str):
        raise ValueError("text profile の probe action/value が不正です")
    action = probe["action"].upper()
    value = probe["value"].upper()
    size = probe["size"]
    labels = (probe["source"], probe["name"], probe["encoding"])
    if not isinstance(size, int):
        raise ValueError("text profile の probe size が不正です")
    valid_size = ((action == "STRING" and 1 <= size <= 4096) or
                  (action == "CHAR" and size in (1, 2, 4)) or
                  (action == "CURSOR" and size == 0))
    if (action not in ("STRING", "CHAR", "CURSOR") or
            not re.fullmatch(r"(?:D|A)[0-7]|S[+-](?:0X)?[0-9A-F]+", value) or
            not valid_size or
            any(not isinstance(label, str) or not re.fullmatch(r"[A-Za-z0-9._-]+", label)
                for label in labels)):
        raise ValueError("text profile の probe 引数が不正です")
    suffix = " SUPER" if probe.get("space") == "supervisor" else ""
    return (f"ADD 0x{probe['pc']:X} {action} {value} {size} {probe['source']} "
            f"{probe['name']} {probe['encoding']}{suffix}")


# --- 出力採取点の自動有効化 ---------------------------------------------------
# HIOCS.X / IOCS.X を組み込むと、DOS 出力は TRAP #15 を通らず、各ドライバの _B_PUTC 本体へ
# 直接つながる。DOS コール表の _PUTCHAR（$1808）が各ドライバのフックを指すことを手がかりに、
# 本体の入口を実行時のメモリ上の署名2か所（フック入口と本体入口）で特定し、TEXT PROBE を
# 自動で有効にする。profile の SHA-256 照合は使わない（binary_path が要らない）。
# 有効かどうかはエミュレータの設定 text_auto_probe（.cfg / -text-auto-probe、既定 true）。
_AUTO_DOS_PUTCHAR_ENTRY = 0x1808
_AUTO_SIGNATURE_LEN = 8
_AUTO_BRA_W_PREFIX = b"\x60\x00"  # CONDRV 併用時に HIOCS 本体の先頭 4 バイトへ入る BRA.W
_AUTO_TEXT_DRIVERS = (
    {
        "source": "hiocs",
        "hook_signature": bytes.fromhex("207a24e40c100001"),
        "hook_to_body": 0x30AC,  # $1808 の指す先 − 本体入口
        "body_heads": (bytes.fromhex("10380990"),),
        "body_tail": bytes.fromhex("66260cb8"),
        "body_may_be_bra": True,
    },
    {
        "source": "iocsx",
        "hook_signature": bytes.fromhex("207a05280c100001"),
        "hook_to_body": 0x2A9E,
        "body_heads": (bytes.fromhex("41ed0990"),),
        "body_tail": bytes.fromhex("1010662e"),
        "body_may_be_bra": False,
    },
)
_AUTO_PROBE_NAME = "B_PUTC_body"
_AUTO_WAIT_TICK_INTERVAL_S = 1.0


def _auto_text_state_initial() -> dict[str, Any]:
    return {"owned": False, "suppressed": False, "pc": None, "source": None, "note": ""}


_auto_text = _auto_text_state_initial()
_auto_text_last_tick = 0.0


def _auto_text_reset_state() -> None:
    """エミュレータのリセットなど、プローブが失われる契機で自動有効化の状態を初期化する。"""
    _auto_text.update(_auto_text_state_initial())


def _auto_text_option_enabled(timeout: float) -> bool:
    status, _ = _send_command("CONFIG GET TEXT_AUTO_PROBE", timeout=timeout)
    # 古いエミュレータは CONFIG を知らず ERR を返す。その場合は機能なしとして扱う。
    return status == "OK TEXT_AUTO_PROBE=1"


def _auto_text_body_matches(driver: dict[str, Any], data: bytes) -> bool:
    head, tail = data[:4], data[4:]
    if tail != driver["body_tail"]:
        return False
    return head in driver["body_heads"] or (
        driver["body_may_be_bra"] and head[:2] == _AUTO_BRA_W_PREFIX)


def _auto_text_identify(timeout: float) -> tuple[dict[str, Any], int] | None:
    """$1808 の指す先から、常駐中のドライバと _B_PUTC 本体の入口アドレスを求める。"""
    status, vector = _send_command(f"MEM PEEK {_AUTO_DOS_PUTCHAR_ENTRY:X} 4", timeout=timeout)
    if status != "OK" or len(vector) != 4:
        return None
    hook = int.from_bytes(vector, "big")
    if hook & 1:
        return None
    status, head = _send_command(f"MEM PEEK {hook:X} {_AUTO_SIGNATURE_LEN:X} SUPER",
                                 timeout=timeout)
    if status != "OK":
        return None
    for driver in _AUTO_TEXT_DRIVERS:
        if head != driver["hook_signature"]:
            continue
        body_pc = hook - driver["hook_to_body"]
        if body_pc < 0 or body_pc & 1:
            return None
        status, body = _send_command(f"MEM PEEK {body_pc:X} {_AUTO_SIGNATURE_LEN:X} SUPER",
                                     timeout=timeout)
        if status == "OK" and _auto_text_body_matches(driver, body):
            return driver, body_pc
        return None
    return None


def _auto_text_arm(driver: dict[str, Any], body_pc: int, timeout: float) -> None:
    probe = {"pc": body_pc, "action": "char", "value": "d1", "size": 1,
             "source": driver["source"], "name": _AUTO_PROBE_NAME,
             "encoding": "shift-jis", "space": "supervisor"}
    add_command = _format_text_probe_add(probe)
    _text_probe_command("CLEAR", timeout)
    try:
        _text_probe_command(add_command, timeout)
        _text_probe_command("ENABLE", timeout)
    except Exception:
        _text_probe_command("CLEAR", timeout)
        raise
    _auto_text.update(owned=True, pc=body_pc, source=driver["source"], note="")


def _auto_text_probe_tick(min_interval_s: float = 0.0, timeout: float = 10.0) -> None:
    """DOS コール表を見て、採取点の有効化・張り替え・無効化を行う。失敗してもツール本来の
    動作は妨げず、理由を `_auto_text["note"]`（text_probe の status に出る）へ残す。
    timeout は各コマンドの待ち時間（wait_for の待機中は残り時間に合わせて短くする）。"""
    global _auto_text_last_tick
    now = time.monotonic()
    if min_interval_s > 0 and now - _auto_text_last_tick < min_interval_s:
        return
    _auto_text_last_tick = now
    state = _auto_text
    try:
        if not _auto_text_option_enabled(timeout):
            return
        enabled = "ENABLED=1" in _text_probe_command("STATUS", timeout)
        if state["owned"] and not enabled:
            _auto_text_reset_state()  # リセットなどでプローブが失われた
        if state["suppressed"] or (enabled and not state["owned"]):
            return  # 利用者が clear した、または別の（手動の）プローブが有効
        found = _auto_text_identify(timeout)
        if found is None:
            if state["owned"]:
                _text_probe_command("CLEAR", timeout)  # ドライバが解除された。旧 PC のプローブを外す
                _auto_text_reset_state()
            return
        driver, body_pc = found
        if state["owned"] and state["pc"] == body_pc:
            return
        _auto_text_arm(driver, body_pc, timeout)
    except Exception as exc:  # 自動採取の失敗でツール本来の動作を妨げない。理由は status で見える
        state["note"] = f"{type(exc).__name__}: {exc}"


def _auto_text_status_suffix() -> str:
    state = _auto_text
    if state["owned"]:
        return f" AUTO={state['source']}@{state['pc']:X}"
    if state["suppressed"]:
        return " AUTO=SUPPRESSED"
    if state["note"]:
        return f" AUTO=ERROR({state['note']})"
    return " AUTO=IDLE"


@mcp.tool()
def text_probe(action: str = "status", profile: str | None = None,
               binary_path: str | None = None) -> str:
    """TEXT PROBE を管理する。status/clear はそのまま実行し、enable は TOML
    profile の SHA-256 を binary_path と照合してから定義を投入・有効化する。"""
    if action == "status":
        if profile or binary_path:
            raise ValueError("status に profile/binary_path は指定できません")
        return _text_probe_command("STATUS") + _auto_text_status_suffix()
    if action == "clear":
        if profile or binary_path:
            raise ValueError("clear に profile/binary_path は指定できません")
        result = _text_probe_command("CLEAR")
        # 利用者が明示的に外した。performance_counter などのため、次のリセットまで自動有効化しない。
        _auto_text.update(owned=False, suppressed=True, pc=None, source=None)
        return result
    if action != "enable" or not profile:
        raise ValueError("action は status、clear、または profile を伴う enable です")
    target, probes = _load_text_profile(profile, binary_path)
    _resolve_text_probe_offsets(target, probes)
    for probe in probes:
        _verify_text_probe_signature(probe)
    add_commands = [_format_text_probe_add(probe) for probe in probes]
    _text_probe_command("CLEAR")
    _auto_text.update(owned=False, pc=None, source=None)  # 手動の有効化。自動有効化は触らない
    try:
        for command in add_commands:
            _text_probe_command(command)
        return _text_probe_command("ENABLE")
    except Exception:
        _text_probe_command("CLEAR")
        raise


# X68000 キー値マップ（KeyWitch.X より。値 = (X68kキーコード << 8) | 文字コード）
# 文字コードが 0x00 の場合は特殊キー（プログラムはキーコードで判別する）
# カーソルキー・RET/ESC/BS 等は char code あり（Human68k コマンドラインで使用）
_SCAN_CODES: dict[str, int] = {
    # ファンクションキー（char = 0）
    "F1": 0x6300, "F2": 0x6400, "F3": 0x6500, "F4": 0x6600, "F5": 0x6700,
    "F6": 0x6800, "F7": 0x6900, "F8": 0x6A00, "F9": 0x6B00, "F10": 0x6C00,
    # XF キー（下段, char = 0）
    "XF1": 0x5500, "XF2": 0x5600, "XF3": 0x5700, "XF4": 0x5800, "XF5": 0x5900,
    # ナビゲーション（char = 0）
    "HOME": 0x3600, "INSERT": 0x5E00, "INS": 0x5E00,
    "DELETE": 0x3700, "DEL": 0x3700,
    "ROLLUP": 0x3800, "PAGEUP": 0x3800,
    "ROLLDOWN": 0x3900, "PAGEDOWN": 0x3900,
    "UNDO": 0x3A00,
    "HELP": 0x5400,
    "CLR": 0x3F00,
    "BREAK": 0x6100, "COPY": 0x6200,
    # カーソルキー（char = 0x00: COMMAND.X は kcode=0x3B-0x3E で判別する）
    "LEFT":  0x3B00,
    "RIGHT": 0x3D00,
    "UP":    0x3C00,
    "DOWN":  0x3E00,
    # 基本キー（char あり）
    "RETURN": 0x1D0D, "ENTER": 0x4E0D,  # メインとテンキー
    "ESCAPE": 0x011B, "ESC": 0x011B,
    "BACKSPACE": 0x0F08, "BS": 0x0F08,
    "TAB": 0x1009,
    "SPACE": 0x3520,
    # 英字キー（CTRLとの組み合わせ入力にも使う）
    "A": 0x1E61, "B": 0x2E62, "C": 0x2C63, "D": 0x2064,
    "E": 0x1365, "F": 0x2166, "G": 0x2267, "H": 0x2368,
    "I": 0x1869, "J": 0x246A, "K": 0x256B, "L": 0x266C,
    "M": 0x306D, "N": 0x2F6E, "O": 0x196F, "P": 0x1A70,
    "Q": 0x1171, "R": 0x1472, "S": 0x1F73, "T": 0x1574,
    "U": 0x1775, "V": 0x2D76, "W": 0x1277, "X": 0x2B78,
    "Y": 0x1679, "Z": 0x2A7A,
    # 修飾キー（char = 0）
    "SHIFT": 0x7000, "CTRL": 0x7100, "OPT1": 0x7200, "OPT2": 0x7300,
    # 日本語キー（char = 0）
    "KANA": 0x5F00, "ZENEI": 0x5A00, "HIRAGANA": 0x5300, "CAPS": 0x5D00,
    "CODE": 0x5C00, "KIGOU": 0x5200,
    # テンキー（ホスト側で拾えないためkey_downで代替、char = 0）
    "KP_MINUS": 0x4200,
}


def _resolve_scan_code(key: str) -> int:
    name = key.upper().strip()
    sc = _SCAN_CODES.get(name)
    if sc is None:
        known = ", ".join(sorted(_SCAN_CODES))
        raise ValueError(f"不明なキー名: {key!r}\n対応キー: {known}")
    return sc


def _key_down_impl(key: str) -> str:
    sc = _resolve_scan_code(key)
    status, _ = _send_command(f"KEY DOWN {sc:04X}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    kcode = (sc >> 8) & 0xFF
    return f"{key} (kcode=0x{kcode:02X}) を押下しました"


def _key_up_impl(key: str) -> str:
    sc = _resolve_scan_code(key)
    status, _ = _send_command(f"KEY UP {sc:04X}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    kcode = (sc >> 8) & 0xFF
    return f"{key} (kcode=0x{kcode:02X}) を離上しました"


@mcp.tool()
def key(action: str, key: str, hold_ms: float = 0) -> str:
    """X68000 の特殊キー入力を送信する。
    キー名は大文字・小文字不問。
    対応キー: F1-F10, XF1-XF5, HELP, HOME, INSERT, DELETE, ROLLUP, ROLLDOWN,
    UNDO, BREAK, COPY, UP, DOWN, LEFT, RIGHT, RETURN, ENTER, ESCAPE,
    BACKSPACE, TAB, SPACE, A-Z, SHIFT, CTRL, OPT1, OPT2, KANA, ZENEI 等。
    action:
      "press" — 1回押す。hold_ms(ミリ秒)が既定の0なら押下→離上の2バイトを
                間を置かず送る(メニュー操作等の単純タップ用)。0より大きい
                値を指定すると"down"→待機→"up"の3手順に切り替わり、
                実際の指の押下時間を模擬できる(例: mint.xのようにキー
                入力を1バイト変数でポーリングするソフトでは、押下と同時に
                離上してしまうと入力が欠落することがある)。
      "down"  — 押下する(離上は送らない、"up"で別途送ること)。呼び出し側が
                必ず"up"を呼ぶ責任を持つ(呼び忘れるとキーが押しっぱなしの
                まま残り、以後のテストが壊れる。例外が起きうる処理を
                挟む場合は try/finally で"up"を保証すること。通常は
                hold_ms引数付きの"press"の方が安全)。修飾キー(SHIFT等)を
                押しっぱなしにして別キーと組み合わせたい場合に使う。
      "up"    — "down"で押下したキーを離す。
    hold_ms: action="press"でのみ使う(down/upでは無視)。
    """
    action = action.lower()
    if action in ("press", "down"):
        _mark_wait_cursor()

    if action == "press":
        if hold_ms > 0:
            _key_down_impl(key)
            try:
                time.sleep(hold_ms / 1000.0)
            finally:
                _key_up_impl(key)
            sc = _resolve_scan_code(key)
            kcode = (sc >> 8) & 0xFF
            char  = sc & 0xFF
            return f"{key} (kcode=0x{kcode:02X}, char=0x{char:02X}, hold_ms={hold_ms}) を送信しました"

        sc = _resolve_scan_code(key)
        status, _ = _send_command(f"KEY SCAN {sc:04X}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        kcode = (sc >> 8) & 0xFF
        char  = sc & 0xFF
        return f"{key} (kcode=0x{kcode:02X}, char=0x{char:02X}) を送信しました"

    if action == "down":
        return _key_down_impl(key)

    if action == "up":
        return _key_up_impl(key)

    raise ValueError('action は "press"/"down"/"up" のいずれか')


_PASTE_LINE_STABLE_POLLS = 6     # KEY CHECKがこの回数連続でtrueなら1行分の消化完了とみなす
_PASTE_LINE_POLL_INTERVAL_S = 0.02  # C側のPASTE送信間隔と同じ20ms
_PASTE_LINE_TIMEOUT_S = 8.0


def _wait_paste_line_stable(timeout_s: float = _PASTE_LINE_TIMEOUT_S) -> bool:
    """KEY CHECKが安定してtrue（=真の入力待ちに戻った）になるまでポーリングする。
    PASTEキューは呼び出す度に前回の未消化分を破棄する実装（commands.c）なので、
    複数行をまとめて1回のPASTEで送ると、MFP USART RX FIFO（32バイト、src/bus/bus_internal.h）
    がCPU側の処理速度に追いつかず溢れて文字が黙って破棄されることがある。
    1行ずつ送ってここで完了を確認してから次の行を送ることでそれを避ける。"""
    deadline = time.time() + timeout_s
    consecutive = 0
    while time.time() < deadline:
        status, _ = _send_command("KEY CHECK")
        if status == "OK 1":
            consecutive += 1
            if consecutive >= _PASTE_LINE_STABLE_POLLS:
                return True
        else:
            consecutive = 0
        time.sleep(_PASTE_LINE_POLL_INTERVAL_S)
    return False


_PASTE_QUEUE_MAX_BYTES = 512     # C側 mcp_context_t.paste_queue の容量。超過分は黙って切り捨てられる
_PASTE_DRAIN_BASE_TIMEOUT_S = 1.0
_PASTE_DRAIN_PER_BYTE_TIMEOUT_S = 0.1  # 実効約33ms/文字（フレーム単位消化）に余裕を持たせる
_PASTE_WAIT_IOCS = "iocs"
_PASTE_WAIT_NONE = "none"


def _paste_status() -> tuple[int, int, int]:
    """`KEY PASTE` を発行し、(PASTEキュー残りバイト数, MFP USART RX未読の有無, FIFO破棄累計) を返す。"""
    status, _ = _send_command("KEY PASTE")
    parts = status.split()
    if len(parts) != 4 or parts[0] != "OK":
        raise RuntimeError(f"エラー: {status}")
    return int(parts[1]), int(parts[2]), int(parts[3])


def _wait_paste_drained(byte_count: int) -> bool:
    """PASTEキューが空になり、かつゲストがMFP USART RXを読み切るまでポーリングする。
    IOCSを使わないゲスト（NetBSD等）向けで、ゲストの種類に依存しない。"""
    deadline = time.time() + _PASTE_DRAIN_BASE_TIMEOUT_S + _PASTE_DRAIN_PER_BYTE_TIMEOUT_S * byte_count
    while time.time() < deadline:
        queued, rx_pending, _ = _paste_status()
        if queued == 0 and rx_pending == 0:
            return True
        time.sleep(_PASTE_LINE_POLL_INTERVAL_S)
    return False


@mcp.tool()
def paste_text(text: str, wait: str = _PASTE_WAIT_IOCS) -> str:
    """テキストをエミュレータにペーストする（UTF-8→Shift-JIS 変換）。
    改行は \\n で指定（リテラル \\n も実際の改行も両方受け付ける）。
    Shift-JIS で表現できない文字は ? に置換される。
    複数行の場合は1行ずつ送信する（BASIC.X/ED.X/COMMAND.X 等、行入力形式のアプリで
    長い/複数行テキストが送信中に欠落・連結するのを防ぐため）。1行は改行込みで
    512バイトまで（超えるとエラー）。
    wait: 各行を送った後の待ち方。
      "iocs"（既定）— IOCSのキー入力待ちに安定して戻るまで待つ。Human68k向け。
                     IOCSを使わないゲスト（NetBSD等）ではタイムアウトする。
      "none"        — IOCSの入力待ちは確認せず、PASTEキューの送出完了と、ゲストが
                     MFP USART RXを読み切ったことだけを確認する。NetBSD等向け。
                     保証するのは「ゲストに届いた」ことまでで、前のコマンドの完了や、
                     入力を捨てるプログラム（パスワード入力等）が受け取ったことは
                     保証しない。MFP USART RX FIFOの溢れで破棄されたバイトがあれば
                     戻り値に件数を書く。"""
    if wait not in (_PASTE_WAIT_IOCS, _PASTE_WAIT_NONE):
        raise ValueError(f'wait は "{_PASTE_WAIT_IOCS}"/"{_PASTE_WAIT_NONE}" のいずれか')
    text = text.replace("\\r\\n", "\n").replace("\\r", "\n").replace("\\n", "\n")
    lines = text.split("\n")
    chunks = []
    for i, line in enumerate(lines):
        raw = line.encode("shift_jis", errors="replace")
        if i != len(lines) - 1:
            raw += b"\r"
        if len(raw) > _PASTE_QUEUE_MAX_BYTES:
            raise ValueError(
                f"{i + 1}行目が{len(raw)}バイトで、1行の上限{_PASTE_QUEUE_MAX_BYTES}バイトを超えています"
            )
        if raw:
            chunks.append((i + 1, line, raw))
    _auto_text_probe_tick()
    _mark_wait_cursor()
    _send_command("KEY CLEAR")
    dropped_base = _paste_status()[2] if wait == _PASTE_WAIT_NONE else 0
    total_bytes = 0
    for line_no, line, raw in chunks:
        status, _ = _send_command(f"PASTE {raw.hex().upper()}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        total_bytes += len(raw)
        if wait == _PASTE_WAIT_IOCS:
            if not _wait_paste_line_stable():
                raise RuntimeError(
                    f"タイムアウト: {line_no}行目 {line!r} の入力確認ができませんでした"
                    f"（{total_bytes}バイト送信済み。以降の行は送っていません）。"
                    f'IOCSを使わないゲスト（NetBSD等）では確認できないため wait="none" を使ってください'
                )
        elif not _wait_paste_drained(len(raw)):
            raise RuntimeError(
                f"タイムアウト: {line_no}行目 {line!r} の送出完了を確認できませんでした"
                f"（{total_bytes}バイト送信済み。以降の行は送っていません）"
            )
    result = f"{total_bytes} バイトを送信しました"
    if wait == _PASTE_WAIT_NONE:
        dropped = _paste_status()[2] - dropped_base
        if dropped > 0:
            result += f"（警告: MFP USART RX FIFO溢れで{dropped}バイトが破棄されました）"
    return result


@mcp.tool()
def mouse_move(dx: int, dy: int) -> str:
    """マウスを相対移動させる（SCC ch B経由）。
    ROMが約60Hzでポーリングするタイミングで消費されるため、即時反映ではない。
    dx: X方向の相対移動量（右が正）。dy: Y方向の相対移動量（下が正）。"""
    status, _ = _send_command(f"MOUSE MOVE {dx} {dy}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"マウスを ({dx}, {dy}) 移動しました"


@mcp.tool()
def mouse_click(button: str = "left") -> str:
    """マウスボタンをクリックする（押下→50ms待機→離上）。
    ROMは約60Hzでボタン状態をポーリングするため、間に待機を挟んで
    確実に押下状態を1回はサンプリングさせる（キーボードPASTEペーシングと同じ理由）。
    button: "left" または "right"。"""
    name = button.lower().strip()
    if name not in ("left", "right"):
        raise ValueError(f"不明なボタン名: {button!r}（left/rightのいずれか）")
    left, right = (1, 0) if name == "left" else (0, 1)
    status, _ = _send_command(f"MOUSE BUTTON {left} {right}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    time.sleep(0.05)
    status, _ = _send_command("MOUSE BUTTON 0 0")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"{button} クリックしました"


@mcp.tool()
def joystick(
    action: str,
    index: int,
    up: bool = False,
    down: bool = False,
    left: bool = False,
    right: bool = False,
    trigger_a: bool = False,
    trigger_b: bool = False,
) -> str:
    """ジョイスティックの状態をMCPから注入・解除する。
    action:
      "set"     — 状態を注入する（実コントローラより優先）。次に
                  action="release"を呼ぶまで押しっぱなし状態が維持される
                  （レベル入力のため、mouse_clickのような自動離上はしない）。
                  up/down/left/right/trigger_a/trigger_bを全てFalseにしても
                  MCP注入自体は解除されない（実コントローラへは戻らない）。
                  解除は必ずaction="release"を使うこと。
      "release" — MCP注入を解除し、実コントローラの読み取りへ戻す
                  （up以下の引数は無視）。
    index: 0=ジョイスティック#1、1=ジョイスティック#2。
    up/down/left/right: 方向。trigger_a/trigger_b: トリガボタンA/B
    （action="set"のみ使用）。"""
    action = action.lower()
    if index not in (0, 1):
        raise ValueError(f"不明なindex: {index}（0または1）")

    if action == "set":
        value = 0xFF
        if up:
            value &= ~0x01
        if down:
            value &= ~0x02
        if left:
            value &= ~0x04
        if right:
            value &= ~0x08
        if trigger_a:
            value &= ~0x20
        if trigger_b:
            value &= ~0x40
        value &= 0xFF
        status, _ = _send_command(f"JOY SET {index} {value:02X}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"ジョイスティック#{index + 1}: value=0x{value:02X}"

    if action == "release":
        status, _ = _send_command(f"JOY RELEASE {index}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"ジョイスティック#{index + 1}のMCP注入を解除しました"

    raise ValueError('action は "set"/"release" のいずれか')


# SCREENSHOT <mask> のビット値。src/render/renderer.h の X68K_LAYER_* と要同期。
_SCREENSHOT_LAYER_BITS: dict[str, int] = {"gvram": 1, "tvram": 2, "sprite": 4}


# 戻り値注釈を付けないのは意図的。inlineで戻り値の型が str と Image に分かれるが、
# `-> str | Image` と書くとFastMCPが出力スキーマを作れずサーバ全体が起動不能になる
# （pydanticがImageのスキーマを生成できない。`-> Image` 単独なら通る）。
@mcp.tool()
def screenshot(
    layers: str = "display",
    output_path: str = _DEFAULT_SCREENSHOT_PATH,
    inline: bool = False,
):
    """現在の画面をスクリーンショットとして保存する。
    layers: "display"（既定）は実際の表示状態をそのまま返す。"tvram", "gvram",
            "sprite" またはそれらを "+" で結合した文字列（例: "tvram+gvram"）を
            指定すると、指定レイヤーだけに絞り込んで合成し直す。ただしTON/SON/
            DISP/GS4-0等の各レイヤー自身のON/OFF状態はそのまま反映するため、
            非表示中のレイヤーを指定すると黒が返る（display指定時に見えている
            範囲でのレイヤー切り分け用途）。表示状態と無関係にレイヤーの生データを
            見たい場合はCPUを一時停止して read_memory で直接読むこと。
            注意: displayはメインループが毎フレーム更新するキャッシュ済み
            バッファをそのまま返すのに対し、非displayの各レイヤーはコマンドを
            受けた瞬間に非同期で再合成する。可視性ビットがフレーム内の一瞬しか
            立たない実装のゲームでは、displayに写っていてもタイミングを外して
            黒が返ることがある（実例: 男弾のスプライトはDISP/CPUビットが短時間
            しか1にならず、多くの試行で黒だった）。確実に捉えたいときは対象
            レジスタに watchpoint を張って値変化を検出してから撮ること。
    output_path: 保存先パス。Pillow があれば PNG、なければ PPM で保存する。
    inline: True にすると画像そのものを返す（ファイルを開き直さずにそのまま
            見られる。Pillow必須）。既定の False は従来どおり保存先パスを
            示す文字列を返す。"""
    if layers == "display":
        cmd = "SCREENSHOT"
    else:
        requested = {l.strip() for l in layers.split("+")}
        unknown = requested - _SCREENSHOT_LAYER_BITS.keys()
        if unknown:
            raise ValueError(
                f"未知のレイヤー: {', '.join(sorted(unknown))}"
                f"（有効値: display, {', '.join(_SCREENSHOT_LAYER_BITS)}）"
            )
        mask = 0
        for name in requested:
            mask |= _SCREENSHOT_LAYER_BITS[name]
        cmd = f"SCREENSHOT {mask}"

    status, ppm_data = _send_command(cmd, timeout=30.0)
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")

    try:
        from PIL import Image as PILImage
        buf = io.BytesIO()
        PILImage.open(io.BytesIO(ppm_data)).save(buf, "PNG")
        png_data = buf.getvalue()
    except ImportError:
        png_data = None

    if inline:
        if png_data is None:
            raise RuntimeError(
                "inline=True には Pillow が必要です（PPMのままでは画像として返せません）。"
                "inline=False にすると PPM ファイルとして保存します。"
            )
        return Image(data=png_data, format="png")

    if png_data is not None:
        Path(output_path).write_bytes(png_data)
        out = output_path
    else:
        out = output_path.replace(".png", ".ppm")
        Path(out).write_bytes(ppm_data)

    return f"保存: {out}"


@mcp.tool()
def get_registers() -> str:
    """CPU レジスタ (D0-D7, A0-A7, PC, SR, USP, SSP) を取得する。"""
    status, _ = _send_command("REGS")
    if not status.startswith("OK "):
        raise RuntimeError(f"エラー: {status}")
    return _format_regs(_parse_regs(status[3:]))


@mcp.tool()
def get_mmu_regs() -> str:
    """PMMU / access-control レジスタ (TC/CRP/SRP/TT0/TT1/MMUSR) を取得する。
    68030 のみ全フィールドを返す。68EC030 では TT0/TT1/MMUSR が AC0/AC1/ACUSR
    という名前で返り、常に0の契約の TC/CRP/SRP は出ない。68000/68010/68020
    は PMMU 非搭載のため MMU=UNSUPPORTED を返す（0 を返すと有効な値と区別が
    つかなくなるため）。"""
    status, _ = _send_command("MMU GET")
    if not status.startswith("OK "):
        raise RuntimeError(f"エラー: {status}")
    return status[3:].strip()


@mcp.tool()
def kbd_state() -> str:
    """MFP USART TX（UDR書き込み）でキーボードへ送られたコマンドの保持状態を返す。
    IOCS _LEDCTRL/_KEYDLY/_KEYREP/_KEY_INIT 等の送信が実際に届いたかの確認用。
    戻り値: "LED=<hex> BRIGHT=<hex> DELAY=<hex> RATE=<hex> TX=<送信バイト累計>"
    LEDはbit=1で点灯（bit0かな 1ローマ字 2コード入力 3CAPS 4INS 5ひらがな 6全角）。
    DELAY/RATEはコマンド下位4bitの生値（ms換算はしない）。"""
    status, _ = _send_command("KEY KBDSTATE")
    if not status.startswith("OK "):
        raise RuntimeError(f"エラー: {status}")
    return status[3:].strip()


@mcp.tool()
def snapshot(action: str) -> str:
    """停止中の仮想マシン状態を、同一プロセス内の単一スロットへ保存・復元する。

    action: "save" | "load" | "status"。読み取り専用媒体だけで使い、保存後に
    ディスク媒体を操作するとスロットは無効になる。入力・シリアル通信を止めてから
    実行すること。SRAMは終了時にload後の内容で書き出される。
    """
    wire = {
        "save": "SNAPSHOT SAVE",
        "load": "SNAPSHOT LOAD",
        "status": "SNAPSHOT STATUS",
    }
    if action not in wire:
        raise ValueError('action は "save"、"load"、"status" のいずれか')
    status, _ = _send_command(wire[action])
    if not status.startswith("OK "):
        raise RuntimeError(f"エラー: {status}")
    return status[3:].strip()


@mcp.tool()
def read_memory(address: int, length: int = 16, side_effects: bool = False) -> str:
    """指定アドレスからメモリを読み出す（最大 256 バイト）。
    address: 読み出し開始アドレス（整数）
    length: 読み出しバイト数（デフォルト 16、最大 256）
    side_effects: True にするとI/Oレジスタのread-clear等の副作用を許容する実読み出し
        （`MEM READ`）を使う。既定 False は非侵襲peek（`MEM PEEK`）を使い、探索的な読み出しで
        FDC結果FIFO等の内部状態を壊さない。
    戻り値: アドレスとバイト値のダンプ文字列"""
    if length < 1:
        length = 1
    if length > 256:
        length = 256
    cmd = "MEM READ" if side_effects else "MEM PEEK"
    status, raw = _send_command(f"{cmd} {address:X} {length:X}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    lines = []
    for i in range(0, len(raw), 16):
        chunk = raw[i:i+16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        asc_part = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append(f"{address+i:08X}  {hex_part:<47}  {asc_part}")
    return "\n".join(lines)


@mcp.tool()
def disassemble(address: int, count: int = 1) -> str:
    """指定アドレスから命令を非侵襲に逆アセンブルする（最大 64 命令）。
    address: 開始アドレス（整数）
    count: 逆アセンブルする命令数（デフォルト 1、最大 64）
    戻り値: "アドレス  生バイト(16bitワード単位)  ニーモニック" 形式の複数行文字列"""
    if count < 1:
        count = 1
    if count > 64:
        count = 64
    status, body = _send_command(f"DISASM {address:X} {count}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return body.decode("ascii", errors="replace").rstrip("\n")


@mcp.tool()
def write_memory(address: int, data: list[int]) -> str:
    """指定アドレスにバイト列を書き込む（最大 256 バイト）。
    address: 書き込み開始アドレス（整数）
    data: 書き込むバイト値のリスト（0–255）
    戻り値: 書き込んだバイト数の確認文字列"""
    if not data:
        raise ValueError("data が空です")
    if len(data) > 256:
        raise ValueError("最大 256 バイトまで")
    raw = bytes(b & 0xFF for b in data)
    status, _ = _send_command(f"MEM WRITE {address:X}", body=raw)
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"{len(data)} バイトを 0x{address:08X} に書き込みました"


@mcp.tool()
def reset(force_fd0: bool = False) -> str:
    """エミュレータをソフトリセットする（SRAM は保持）。

    force_fd0=True で実機の「OPT.1キーを押しながら電源投入/リセット」相当
    （強制的にFD0から起動する）を再現する。
    """
    status, _ = _send_command("RESET FD0" if force_fd0 else "RESET")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    _auto_text_reset_state()
    return "OPT.1相当でリセットしました（FD0強制起動）" if force_fd0 else "リセットしました"


@mcp.tool()
def send_interrupt() -> str:
    """NMI（レベル7割り込み）を発生させ、実行中のプログラムを中断する。
    Human68k では BREAK キー相当の動作になる。"""
    status, _ = _send_command("INTERRUPT")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return "NMI を送信しました"


@mcp.tool()
def power_switch(on: bool = False) -> str:
    """前面電源スイッチを操作する（既定はOFF）。OFFにするとMFP GPIP2割り込みが入り、
    ゲスト（IPLROM）の電源断処理がシステムポート#6 ($E8E00F) へ $00,$0F,$0F を書くと
    ix68k が終了する。スイッチOFFから終了までエミュレーション時間で数秒かかり、
    DOS 実行中は後回しになる（COMMAND.X のプロンプトで実施すること）。"""
    status, _ = _send_command("POWER ON" if on else "POWER OFF")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return "前面電源スイッチをONにしました" if on else "前面電源スイッチをOFFにしました"


@mcp.tool()
def whiteband(action: str) -> str:
    """白帯（Human68k エラー）検出による CPU 自動停止の管理。
    白帯（_ABORTRST $FD）検出時に CPU を停止する機能は既定オフ。有効化
    しないと "continue" は使えない（停止していない CPU を再開する対象が
    無いため）。
    停止はデバッガの停止として扱われるので、debug_control(action="status")は
    STATE=paused STOP=WHITEBAND を返し、read_memory/get_registers/disassemble
    で状態を調べたり step で先へ進めたりできる。再開はこの "continue" でも
    debug_control(action="continue") でも同じ。

    有効/無効はエミュレータのプロセス状態で、再起動すると "off" に戻る。現在の
    armed 状態は debug_control(action="status") の WB=ON/OFF で確認できる
    （Issue #156）。
    停止を待つときは run_command(...) か debug_control(action="continue",
    wait=True) を使う。paste_text は 20ms/文字でキューに積むだけなので、直後の
    debug_control(action="status") は必ず STATE=running を返す（白帯が出ないの
    ではなく、まだ実行されていない）。
    action:
      "on"       — 白帯検出で CPU を停止する機能を有効化する
      "off"      — 上記を無効化する
      "continue" — "on" 中の白帯検出で停止した CPU の実行を再開する。
                   停止時は直前の IOCS 呼び出し履歴がエミュレータのログに
                   ダンプされる。状態（registers/memory）を調べ終えたら
                   呼ぶこと。
    """
    action = action.lower()
    cmd_map = {"on": "WHITEBAND ON", "off": "WHITEBAND OFF", "continue": "WHITEBAND CONT"}
    if action not in cmd_map:
        raise ValueError('action は "on"/"off"/"continue" のいずれか')
    status, _ = _send_command(cmd_map[action])
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    if action == "continue":
        return "CPU 実行を再開しました"
    return f"白帯検出 HALT を{'有効化' if action == 'on' else '無効化'}しました"


@mcp.tool()
def disk(action: str, media: str = "fd", index: int = 0, path: str = "") -> str:
    """FD/SCSIディスクイメージの挿入・排出・ライトプロテクトを管理する。
    action:
      "insert"    — mediaで指定したユニットにpathのイメージを挿入する（path必須）
      "eject"     — mediaで指定したユニットからイメージを排出する（pathは指定しない）
      "status"    — 現在装着中のイメージ一覧を返す（media/index/pathは無視）。
                    セッションを跨いで「今どのドライブに何が入っているか」を
                    確認するのに使う
      "protect"   — mediaで指定したユニットのディスクをライトプロテクトする。
                    ホストのイメージファイルのパーミッションを実際に変更する
                    （実機のプロテクトノッチ操作に相当）。fd: 全ドライブ対応
                    （Issue #167）。scsi: リムーバブルのMO（デバイスパラメータ
                    $0F(EJ)!=0のHDD/MOイメージ、CD-ROM除く）のみ対応。固定HDD・
                    CD-ROMは物理的なライトプロテクト機構が無い/常に保護済みの
                    ため`ERR BADARG`になる（Issue #168）
      "unprotect" — 同ユニットのライトプロテクトを解除する
    media: "fd"（フロッピー、index 0–3）または "scsi"（SASI/SCSI、index 0–6）
    index: ユニット番号（既定0）
    path: action="insert"時のイメージファイル絶対パス

    FD: 実機同様、対象ドライブに既にディスクが入っている場合は先に
    action="eject"で排出しないと失敗する（排出なしで差し替えるとHuman68kが
    メディア交換を検知できずFAT/ディレクトリキャッシュが古いままになる
    ため）。eject は実機の $E94005 イジェクト相当で、Human68k には抜去の
    抜き挿し割り込みが通知される。

    SCSI: デバイスパラメータ$0F(EJ)!=0（hu68disk mkfsの--removable）で
    作成したリムーバブルメディア(MO等)イメージは、既に起動済みのドライブに
    対してもreset無しでメディア交換/切断として即座に認識される（Unit
    Attention対応）。EJ=0の固定ディスク相当イメージは実機同様reset無しでは
    反映されない。

    SCSI・CD-ROM: pathに拡張子.ccd（大小無視）のCloneCDイメージ（同ディレクトリに
    対の.imgが必要）を指定するとCD-ROMとして接続される（形式は拡張子/ファイル内容
    から自動判別、mediaやpathの書式に専用の指定は不要）。CD-ROMは常にリムーバブル
    扱いのため、他のMO等と同じくreset無しでメディア交換が反映される。書き込み系
    コマンドはDATA PROTECTで拒否される。CONFIG.SYSに`DEVICE = \\SYS\\SUSIE.X
    -ID<n> *:`を登録済みのドライブへ接続すると、SUSIE.XのDriver欄がIS9・
    Volume欄がISO-9660のボリュームラベルになる。
    """
    action = action.lower()

    if action == "status":
        status, body = _send_command("DISK STATUS")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        entries = [e for e in body.decode("utf-8", errors="replace").splitlines() if "=" in e]
        # "FD<n>.WP=1" 行はユニット行(FD<n>=...)とは別に集約する（Issue #167 D10）。
        # ".WP" キーはユニット行として表示しない -- 単純に全行をentry.split("=",1)
        # するだけだと偽のユニット行として表示されてしまうため必ず先に分離する。
        wp_units = {
            unit[: -len(".WP")]
            for unit, value in (e.split("=", 1) for e in entries)
            if unit.endswith(".WP") and value == "1"
        }
        lines = []
        for entry in entries:
            unit, path = entry.split("=", 1)
            if unit.endswith(".WP"):
                continue
            wp_suffix = "（ライトプロテクト）" if unit in wp_units else ""
            if path:
                lines.append(f"{unit}: {path}{wp_suffix}")
            elif unit.startswith("FD"):
                lines.append(f"{unit}: （空）")
            else:
                lines.append(f"{unit}: ドライブ有り・メディア無し")
        if not any(l.startswith("SCSI") for l in lines):
            lines.append("SCSI: 接続されているドライブなし")
        return "\n".join(lines)

    media = media.lower()
    if media not in ("fd", "scsi"):
        raise ValueError('media は "fd"/"scsi" のいずれか')
    max_index = 3 if media == "fd" else 6
    if not (0 <= index <= max_index):
        raise ValueError(f"{media} の index は 0-{max_index} の範囲: {index}")

    if action in ("protect", "unprotect"):
        onoff = "ON" if action == "protect" else "OFF"
        cmd = f"DISK WP {media.upper()} {index} {onoff}"
        status, _ = _send_command(cmd)
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        verb = "設定" if action == "protect" else "解除"
        return f"{media.upper()}{index} のライトプロテクトを{verb}しました"

    if action == "insert":
        if not path:
            raise ValueError('action="insert" には path が必須です')
        cmd = f"DISK INSERT {media.upper()} {index} {path}"
        status, _ = _send_command(cmd)
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        if media == "fd":
            return f"FD{index} に {path} を挿入しました"
        return f"SCSI ID{index} に {path} を接続しました（リムーバブルメディアはresetなしで認識されます）"

    if action == "eject":
        if path:
            raise ValueError('action="eject" では path を指定しません')
        cmd = f"DISK EJECT {media.upper()} {index}"
        status, _ = _send_command(cmd)
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        if media == "fd":
            return f"FD{index} を排出しました"
        return f"SCSI ID{index} を切り離しました（リムーバブルメディアは即座に反映されます）"

    raise ValueError('action は "insert"/"eject"/"status"/"protect"/"unprotect" のいずれか')


_HOSTFILE_STATE_LABELS = {
    0: "空",
    1: "GET待ち（macOS側PUT済み、Human68k側の `HOSTFILE GET` 待ち）",
    2: "PUT待ち（Human68k側PUT完了、macOS側の回収(action=\"get\")待ち）",
}


@mcp.tool()
def hostfile(action: str, macos_path: str = "") -> str:
    """macOSとHuman68k間でHOSTFILE MMIO裏口経由のファイル転送を行う。
    GET/PUTはsend/scp等と同じく「自分から見た自分の動作」を表す
    （put=自分から送り出す、get=自分が受け取る）。
    action:
      "put"    — macos_pathのファイルをHuman68k側へ送る（macos_path必須）。
                 この後Human68k側で `HOSTFILE GET <出力ファイル名>` を実行
                 すると実ドライブにファイルが作られる（エミュレータ
                 再起動不要）。
      "get"    — 68k側が `HOSTFILE PUT <入力ファイル名>` で送ったファイルを
                 macos_pathへ受け取る（macos_path必須）。先にHuman68k側で
                 `HOSTFILE PUT` を実行しておくこと。
      "status" — 転送のハンドシェイク状態を取得する（macos_pathは無視）。
                 put/getが相手側の操作待ちで進まないときの確認用。
    macos_path: action="put"/"get"で使うmacOS側ファイルの絶対パス
    """
    action = action.lower()

    if action == "put":
        if not macos_path:
            raise ValueError('action="put" には macos_path が必須です')
        status, _ = _send_command(f"HOSTFILE PUT {macos_path}")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        size = status[3:]
        return f"{size} バイトの送信準備ができました（Human68k側で `HOSTFILE GET <出力ファイル名>` を実行してください）"

    if action == "get":
        if not macos_path:
            raise ValueError('action="get" には macos_path が必須です')
        status, _ = _send_command(f"HOSTFILE GET {macos_path}")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}（先に Human68k 側で `HOSTFILE PUT` を実行してください）")
        size = status[3:]
        return f"{size} バイトを {macos_path} に書き出しました"

    if action == "status":
        status, _ = _send_command("HOSTFILE STATUS")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        parts = status[3:].split()
        state = int(parts[0])
        length = int(parts[1]) if len(parts) > 1 else 0
        label = _HOSTFILE_STATE_LABELS.get(state, f"不明({state})")
        return f"state={state} ({label}), len={length}"

    raise ValueError('action は "put"/"get"/"status" のいずれか')


@mcp.tool()
def trace_iocs(enable: bool = True) -> str:
    """IOCS/TRAP 呼び出しの全トレースをエミュレータの stderr ログへ出力する。
    enable=True で TRACE_ON、False で TRACE_OFF。デバッグ調査用。"""
    status, _ = _send_command("DIAG IOCS ON" if enable else "DIAG IOCS OFF")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"IOCS トレースを {'有効化' if enable else '無効化'} しました"


@mcp.tool()
def ram_watch(address: int = 0, length: int = 1, enable: bool = True) -> str:
    """指定範囲へのRAM書き込みをエミュレータのstderrログへ記録する
    （書き込みアドレス・旧値・新値・PCを [RAMWATCH] 行で出力）。デバッグ調査用、
    ビルドし直さずON/OFF切り替え可能。同時に監視できるのは1範囲のみ
    （再度 enable=True で呼ぶと範囲を上書き）。
    address: 監視開始アドレス（例: 0x000807）
    length: 監視バイト長（デフォルト1）
    enable=False で WATCH_OFF（address/lengthは無視）"""
    if not enable:
        status, _ = _send_command("WATCH OFF")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return "RAM書き込みウォッチを無効化しました"
    status, _ = _send_command(f"WATCH ON {address:06X} {length:X}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"RAM書き込みウォッチを有効化しました: {address:06X}-{address + length - 1:06X}"


_PERF_PC_MAX = 0xFFFFFF


def _perf_pc(name: str, value: int) -> int:
    if not 0 <= value <= _PERF_PC_MAX or value & 1:
        raise ValueError(f"{name} は 0x000000〜0xFFFFFE の偶数アドレス")
    return value


@mcp.tool()
def performance_counter(action: str = "status", start_pc: int = 0, end_pc: int = 0) -> str:
    """性能チューニング用の仮想CPUサイクル計測（Issue #231）。ホスト壁時計に
    依存せず、同一バイナリ・同一入力なら同じ値を返す。開始・終了PCに到達した
    時点で64-bitのサイクルカウンタをラッチする（ブレークポイント・ウォッチ
    ポイント・debug_runは使わず、通常実行のまま測る）。
    action:
      "arm"    — start_pc/end_pc（偶数・相異なる）を登録し、前回の結果を破棄する
      "status" — 設定・ラッチ・差分を返す
      "clear"  — 登録を解除してIDLEに戻す（累積カウンタは保持）
    戻り値: "SEQ=<世代> STATE=<IDLE|WAIT_START|RUNNING|COMPLETE|INVALID>
    START_PC=... END_PC=... CPU_START/END/DELTA=... ELAPSED_START/END/DELTA=...
    CPU_TOTAL=... ELAPSED_TOTAL=..." 形式の文字列（サイクル値は10進数）。
    - CPU_*: 命令実行＋モデル化済みバスウェイトのサイクル数。最適化の主指標。
    - ELAPSED_*: 周辺へ進めた仮想経過サイクル。STOP/HALT中の待ちを含む。
    - 値は各PCの命令の直前で採取する。差分は開始命令を含み終了命令を含まない
      ので、測定用プログラムは開始・終了位置に専用のnopを置く。
    - DELTAは STATE=COMPLETE のときだけ有効（それ以外は NA）。
    - TEXT PROBE有効中、またはデバッグ実行経路（debug_control の mode="ON"・
      ブレークポイント・ウォッチポイント・例外ブレーク）では arm できない。
      測定中にこれらを有効化すると STATE=INVALID になる。
    - 実行リセットで IDLE に戻る。同時に測れる区間は1つだけ。"""
    action = action.lower()
    if action == "arm":
        command = f"PERF ARM {_perf_pc('start_pc', start_pc):06X} {_perf_pc('end_pc', end_pc):06X}"
    elif action == "status":
        command = "PERF STATUS"
    elif action == "clear":
        command = "PERF CLEAR"
    else:
        raise ValueError('action は "arm"/"status"/"clear" のいずれか')
    status, _ = _send_command(command)
    if action == "clear":
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return "性能計測を解除しました"
    if not status.startswith("OK "):
        raise RuntimeError(f"エラー: {status}")
    return status[3:]


@mcp.tool()
def watchpoint(action: str, address: int = 0, size: int = 1, id: int = -1, enable: bool = True) -> str:
    """値変化検出型ウォッチポイントの管理（Issue #88 Phase 5、
    mcp_design.md §3）。書き込みアクセスそのものではなく「値が変化
    したこと」を検出するエッジトリガ方式で、実際にCPUを停止させる
    （mc68k_debug_run()経由の実行でのみ機能する）。書き込み元PCを知りたい・
    同じ値の書き戻しも検出したい場合は代わりに ram_watch() を使う（併存、
    どちらも非推奨化はしない。前者は「値が変わった瞬間に止めたい」、後者は
    「誰が書いたか知りたい」用途で使い分ける）。
    action:
      "add"    — addressにsize(1/2/4)バイトのウォッチポイントを追加する
      "remove" — idのウォッチポイントを削除する
      "enable" — idのウォッチポイントの有効/無効をenableで切り替える
      "clear"  — 登録済み全ウォッチポイントを削除する
      "list"   — 登録中のウォッチポイント一覧を返す
    address: action="add"で監視するアドレス（整数）
    size: action="add"の監視サイズ（1/2/4バイト、デフォルト1）
    id: action="remove"/"enable"で対象とするウォッチポイントID
    enable: action="enable"で有効化するか（Falseで無効化）
    戻り値: action="add"は "ID=<id>"、"list"は "id=0,addr=...,size=...,en=...,
        hits=...,last=...;..." 形式の一覧文字列、それ以外は簡潔な確認メッセージ"""
    action = action.lower()

    if action == "add":
        status, _ = _send_command(f"WP ADD {address:X} {size}")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return f"ID={status[3:].strip()}"

    if action == "remove":
        status, _ = _send_command(f"WP DEL {id}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"WP {id} を削除しました"

    if action == "enable":
        status, _ = _send_command(f"WP ENABLE {id} {1 if enable else 0}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"WP {id} を{'有効化' if enable else '無効化'}しました"

    if action == "clear":
        status, _ = _send_command("WP CLEAR")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return "全ウォッチポイントを削除しました"

    if action == "list":
        status, _ = _send_command("WP LIST")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return status[3:]

    raise ValueError('action は "add"/"remove"/"enable"/"clear"/"list" のいずれか')


@mcp.tool()
def exception_break(vectors: list[int] | None = None, action: str = "set") -> str:
    """例外ブレークマスクの設定・取得（Issue #88 Phase 5、
    mcp_design.md §3）。mc68k_debug_run()経由の実行中、指定した
    例外ベクタが発生した時点でCPUを停止する。Line-F(11)やTRAP(32-47)は
    X68000のOS/API呼び出し機構として頻繁に使われるため、安易に含めると
    通常のOS動作中に止まり続ける点に注意（既定は全ビット0=無効。暴走の兆候に
    絞るなら例えばバスエラー(2)・アドレスエラー(3)・ILLEGAL(4)・特権違反(8)）。
    action:
      "set" — vectorsで指定したベクタ番号(0-47)の集合をマスクとして設定する
              （vectors=[]またはNoneで全解除）
      "get" — 現在設定されているベクタ番号のリストを返す（vectorsは無視）
    vectors: action="set"で有効化する例外ベクタ番号のリスト（例: [2,3,4,8]）
    戻り値: 現在有効なベクタ番号のリストを表す文字列"""
    action = action.lower()
    if action == "get":
        status, _ = _send_command("EXC GET")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        mask = int(status[3:].strip(), 16)
    elif action == "set":
        mask = 0
        for v in vectors or []:
            if not (0 <= v <= 47):
                raise ValueError(f"ベクタ番号は0-47の範囲: {v}")
            mask |= 1 << v
        status, _ = _send_command(f"EXC SET {mask:016X}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
    else:
        raise ValueError('action は "set"/"get" のいずれか')
    active = [v for v in range(48) if mask & (1 << v)]
    return f"有効な例外ベクタ: {active}"


@mcp.tool()
def dump_ring(kind: str, count: int = 32) -> str:
    """PC/レジスタ/コール/フォールトの履歴リングバッファをダンプする
    （Issue #88 Phase 6、mcp_design.md §3。fault は Issue #206、mc68000 #50）。
    暴走直前の実行履歴解析、MMU デマンドページングのフォールト解析用。
    kind: "pcring"（PC履歴。記録自体は既定オフのため、事前に
          diag_log(kind="pcring") で有効化が必要）/ "regring"（レジスタ履歴。記録自体は
          既定オフのため、事前に diag_log(kind="regring") で有効化が必要）/
          "callring"（BSR/JSR/RTSの呼び出し・復帰履歴、常時記録）/
          "faultring"（group0例外＝バスエラー/アドレスエラーの履歴。PC/フォールトした
          論理アドレス/SSW/PMMU変換情報を記録。記録自体は既定オフのため、事前に
          diag_log(kind="faultring") で有効化が必要）
    count: ダンプする件数（0で全件、既定32）
    戻り値: ダンプテキスト（古い順）"""
    cmd_map = {"pcring": "RING PCRING", "regring": "RING REGRING", "callring": "RING CALLRING", "faultring": "RING FAULTRING"}
    kind = kind.lower()
    if kind not in cmd_map:
        raise ValueError('kind は "pcring"/"regring"/"callring"/"faultring" のいずれか')
    cmd = cmd_map[kind]
    status, body = _send_command(f"{cmd} {count}" if count else cmd)
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return body.decode("ascii", errors="replace").rstrip("\n")


@mcp.tool()
def diag_log(kind: str, enable: bool = True) -> str:
    """診断ログ・トレース記録のON/OFF（Issue #88 Phase 6、mcp_design.md §3）。
    kind:
      "badjump"     — 上位8bitが$00/$FF以外へのジャンプを検出時にstderrへ出力
      "badmovea"    — 上位8bitが$04-$FEの不正ポインタ値をAnに積んだ操作を検出時に
                      stderrへ出力
      "badaddr"     — word/longアクセスの上位8bitが$00/$FF以外ならstderrへ出力する
                      X68000固有の24-bitアドレス診断。アクセスごとに大量出力されうる
      "badori"      — opcode 0x0000（ORI.B #0,D0、ゼロ埋め領域への暴走で頻出）の
                      実行を検出時にstderrへ出力。実行結果は変えない（挙動を
                      変えたい場合はエミュレータ起動時の -ori-illegal true で
                      同opcodeをILLEGAL例外化する。本kindとは独立の別機能）
      "regring"     — レジスタ履歴リングバッファへの記録を有効化する
                      （dump_ring(kind="regring")で中身を取得する前提。記録の
                      ON/OFFとダンプ取得は別操作）
      "pcring"      — PC履歴リングバッファへの記録を有効化する
                      （dump_ring(kind="pcring")で中身を取得する前提。記録の
                      ON/OFFとダンプ取得は別操作）
      "faultring"   — フォールト（group0例外）履歴リングバッファへの記録を
                      有効化する（dump_ring(kind="faultring")で中身を取得する前提。
                      記録のON/OFFとダンプ取得は別操作。Issue #206、mc68000 #50）
      "pctrace1024" — 1024命令ごとのPCをstderrへ周期出力する（実行傾向の把握用）
    いずれも既定オフ（正常起動でも大量に出るタイトルがありノイズになるため）。
    enable: True=有効化, False=無効化"""
    kinds = (
        "badjump",
        "badmovea",
        "badaddr",
        "badori",
        "regring",
        "pcring",
        "faultring",
        "pctrace1024",
    )
    kind = kind.lower()
    if kind not in kinds:
        raise ValueError(f"kind は {'/'.join(repr(k) for k in kinds)} のいずれか")
    status, _ = _send_command(f"DIAG {kind.upper()} {'ON' if enable else 'OFF'}")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    return f"{kind} 診断ログを{'有効化' if enable else '無効化'}しました"


def _compose_bp_condition(condition: str, pass_count: int) -> str:
    cond = condition.strip()
    if pass_count > 0:
        cond = f"{cond} && #{pass_count}" if cond else f"#{pass_count}"
    return cond


_IOCS_VECTOR_TABLE_BASE = 0x400  # IOCSコールベクタ表の先頭（$400 + 4*fn）
_IOCS_VECTOR_SIZE = 4
_IOCS_FN_MAX = 0xFF


def _resolve_iocs_vector(fn: int) -> int:
    """IOCSコール番号 fn の現在の飛び先を、生きているベクタ表から読む。

    Human68k は常駐ドライバ（"@IOCS" 等）で `$400+4*fn` のIOCSベクタ表と
    `$BC`(TRAP#15) をRAMへ差し替えるため、ROM逆アセンブルのアドレスに
    ブレークポイントを張っても実行されず命中しない（Issue #156）。
    """
    if not 0 <= fn <= _IOCS_FN_MAX:
        raise ValueError(f"iocs は 0-{_IOCS_FN_MAX} の IOCS コール番号")
    addr = _IOCS_VECTOR_TABLE_BASE + _IOCS_VECTOR_SIZE * fn
    status, raw = _send_command(f"MEM PEEK {addr:X} {_IOCS_VECTOR_SIZE:X}")
    if status != "OK":
        raise RuntimeError(f"エラー: IOCS ${fn:02X} のベクタを読めない: {status}")
    if len(raw) < _IOCS_VECTOR_SIZE:
        raise RuntimeError(f"エラー: IOCS ${fn:02X} のベクタが {len(raw)} バイトしか返らない")
    vector = int.from_bytes(raw[:_IOCS_VECTOR_SIZE], "big")
    if vector == 0:
        raise RuntimeError(f"エラー: IOCS ${fn:02X} のベクタが $00000000（未初期化）")
    return vector


@mcp.tool()
def breakpoint(
    action: str,
    address: int = 0,
    condition: str = "",
    pass_count: int = 0,
    id: int = -1,
    enable: bool = True,
    timeout_ms: int = 5000,
    iocs: int = -1,
) -> str:
    """アドレス比較型ブレークポイントの管理（Issue #88 Phase 4、
    mcp_design.md §3）。実際にCPUを止め、条件式・パスカウント・
    複数同時登録に対応する（通過するだけのアドレスも取りこぼさない）。

    **IOCSルーチンにBPを張るときはROM逆アセンブルのアドレスをそのまま使わない**
    （Issue #156）。Human68k の常駐ドライバが `$400+4*fn` のIOCSベクタ表と
    `$BC`(TRAP#15) をRAMへ差し替えるため、ROM側のエントリ（例: `_B_PUTMES`＝
    `$00FF76DC`）は実行されず、BPは `hits=0` のまま命中しない。`iocs=<fn>` を
    使ってその場のベクタから実体アドレスを解決するか、`read_memory(0x400+4*fn, 4)`
    で実体アドレスを確認してから `address` に渡すこと。
    action:
      "add"    — addressにブレークポイントを追加する。conditionで条件式
                 （例: "D0==12345678"、"[000807].b!=00"、"&&"でAND結合のみ）、
                 pass_countでパスカウント（最初のn回は無視）を指定できる
                 （併用可、"&& #n"として合成される）。ヒットしてもCPUは
                 paused状態のまま残る。状態確認・再開は debug_control() を
                 使う（複数箇所を仕込んで様子見したい場合に向く）。
      "wait"   — addressに一時ブレークポイントを追加して即座に実行を再開し、
                 到達（またはtimeout_ms経過）までブロックする。condition/
                 pass_countも"add"と同様に指定できる。ヒットしてもCPUは
                 paused状態のまま残る（内部的には`BP ADD`→`DBG CONT`→`DBG WAIT`を
                 合成しており、一時ブレークポイントはヒット後・タイムアウト後
                 いずれも自動的に削除される）。単発で「ここまで進めて止めたい」
                 用途に向く。再開はaddと同じくdebug_control()を使う。
      "remove" — idのブレークポイントを削除する
      "enable" — idのブレークポイントの有効/無効をenableで切り替える
      "clear"  — 登録済み全ブレークポイントを削除する
      "list"   — 登録中のブレークポイント一覧を返す
    address: action="add"/"wait"で登録するアドレス（整数）
    condition: action="add"/"wait"の条件式（省略時は無条件）
    pass_count: action="add"/"wait"のパスカウント（省略時0=無視しない）
    id: action="remove"/"enable"で対象とするブレークポイントID
    enable: action="enable"で有効化するか（Falseで無効化）
    timeout_ms: action="wait"のタイムアウト(ミリ秒, 最大25000)
    iocs: action="add"/"wait"でIOCSコール番号（0-255）を指定すると、その場の
        IOCSベクタ表 `$400+4*fn` を読んで実体アドレスを解決し、addressの代わりに
        使う（例: iocs=0x2F で _B_PUTMES の現在の飛び先）。省略時(-1)はaddressを
        そのまま使う
    戻り値: action="add"は "ID=<id>"（iocs指定時は解決したアドレスを併記）、
        "wait"は debug_control(action="continue") と同じ
        "STATE=paused STOP=... MODE=... CAP=... EXEC=... WB=... PC=... D0=...D7=... A0=...A7=... SR=...
        USP=... SSP=..." 形式、"list"は "id=0,addr=...,en=...,hits=...,
        cond=...;..." 形式の一覧文字列、それ以外は簡潔な確認メッセージ"""
    action = action.lower()

    resolved_from_iocs = ""
    if action in ("add", "wait") and iocs >= 0:
        address = _resolve_iocs_vector(iocs)
        resolved_from_iocs = f" (IOCS ${iocs:02X} → ${address:06X})"

    if action == "add":
        cond = _compose_bp_condition(condition, pass_count)
        cmd = f"BP ADD {address:X}" + (f" {cond}" if cond else "")
        status, _ = _send_command(cmd)
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return f"ID={status[3:].strip()}{resolved_from_iocs}"

    if action == "wait":
        cond = _compose_bp_condition(condition, pass_count)
        cmd = f"BP ADD {address:X}" + (f" {cond}" if cond else "")
        status, _ = _send_command(cmd)
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        bp_id = status[3:].strip()
        try:
            status, _ = _send_command("DBG CONT")
            if status != "OK":
                raise RuntimeError(f"エラー: {status}")
            capped_timeout_ms = min(timeout_ms, 25000)
            status, _ = _send_command(
                f"DBG WAIT {capped_timeout_ms}",
                timeout=capped_timeout_ms / 1000.0 + 5.0,
            )
            if not status.startswith("OK "):
                raise RuntimeError(f"ブレークポイント未到達: {status}")
            return status[3:]
        finally:
            _send_command(f"BP DEL {bp_id}")

    if action == "remove":
        status, _ = _send_command(f"BP DEL {id}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"BP {id} を削除しました"

    if action == "enable":
        status, _ = _send_command(f"BP ENABLE {id} {1 if enable else 0}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"BP {id} を{'有効化' if enable else '無効化'}しました"

    if action == "clear":
        status, _ = _send_command("BP CLEAR")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return "全ブレークポイントを削除しました"

    if action == "list":
        status, _ = _send_command("BP LIST")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return status[3:]

    raise ValueError('action は "add"/"wait"/"remove"/"enable"/"clear"/"list" のいずれか')


@mcp.tool()
def key_wait(action: str = "wait", timeout_ms: int = 5000) -> str:
    """CPU が IOCS B_KEYINP(fn=0) でキー入力待ち(キューが空)かを調べる。
    COMMAND.X 等がプロンプトで次の入力を待っている瞬間を検出でき、固定の
    time.sleep より速く確実に paste_text/key(action="press") を送れる
    （早すぎる送信による文字欠落や、待ちすぎによる時間浪費を防ぐ）。
    action:
      "wait"  — キー入力待ちになるまでブロックする（既定）。既にキー入力待ち
                なら即座に返る。timeout_msを過ぎたら例外を送出する。
      "check" — 今その状態かどうかを即座に返す（ノンブロッキング、timeout_msは無視）。
    timeout_ms: action="wait"の最大待機ミリ秒（デフォルト5000, 最大25000）
    戻り値: "WAITING=1"（キー入力待ち）または "WAITING=0"（そうでない。
        action="check"のみ返りうる）。
    B_KEYINP/B_KEYSNSのIOCSベクタ先で動くROM待機ループも即時に検出する。
    """
    action = action.lower()

    if action == "check":
        status, _ = _send_command("KEY CHECK")
        return "WAITING=1" if status.strip() == "OK 1" else "WAITING=0"

    if action == "wait":
        timeout_ms = min(timeout_ms, 25000)
        status, _ = _send_command(
            f"KEY WAIT {timeout_ms}", timeout=timeout_ms / 1000.0 + 5.0
        )
        if not status.startswith("OK"):
            raise RuntimeError(
                f"タイムアウト: キー入力待ちを検出できませんでした ({status})"
            )
        return "WAITING=1"

    raise ValueError('action は "wait"/"check" のいずれか')


@mcp.tool()
def debug_control(
    action: str, count: int = 1, wait: bool = True, timeout_ms: int = 5000, mode: str = "AUTO"
) -> str:
    """デバッガの実行制御（Issue #88 Phase 3、mcp_design.md §3。
    ツール数抑制のためaction引数でグルーピングする）。
    action:
      "status"   — 現在の状態を返す
      "continue" — 停止を解除し実行を再開する。wait=Trueなら次の停止まで
                   ブロックしてその結果を返す（wait=Falseなら即座に返る）
      "step"     — count命令だけ同期的に実行する（実行後は自動的に停止状態になる）
      "break"    — 次のチャンク境界での停止を要求する（応答は要求時点の状態を
                   即座に返す。実際に停止したかは action="status"/"continue"で確認する）
      "mode"     — modeで指定した実行経路に切り替える
    count: action="step"で実行する命令数（デフォルト1）
    wait: action="continue"で次の停止まで待つか（デフォルトTrue）
    timeout_ms: wait=True時の最大待機ミリ秒（デフォルト5000、最大25000）
    mode: action="mode"で指定する実行経路。"AUTO"(既定。ブレークポイント/
        ウォッチポイント登録時のみ低速なデバッグ実行経路を使う) / "ON"(常に
        デバッグ実行経路を使う) / "OFF"(常に通常実行を強制する)
    戻り値: "STATE=running MODE=... CAP=... EXEC=... WB=..." または
    "STATE=paused STOP=... MODE=... CAP=... EXEC=... WB=... PC=... D0=...D7=... A0=...A7=... SR=... USP=... SSP=..."
    形式の文字列（action="mode"のみ "DBG_MODE=<mode>" を返す）。
    STOPの値: USER（action="break"）/ BREAKPOINT / WATCHPOINT / TRACE /
    EXCEPTION / HALTED / STEP / WHITEBAND（白帯検出。whiteband(action="on")中）。
    WBの値: 白帯検出HALTのarmed状態 ON/OFF（whiteband(action="on"/"off")、
    Issue #156。エミュレータ再起動でOFFに戻る）。"""
    action = action.lower()
    if action == "status":
        status, _ = _send_command("DBG STATUS")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return status[3:]

    if action == "continue":
        status, _ = _send_command("DBG CONT")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        if not wait:
            return "再開しました"
        timeout_ms = min(timeout_ms, 25000)
        status, _ = _send_command(
            f"DBG WAIT {timeout_ms}", timeout=timeout_ms / 1000.0 + 5.0
        )
        if not status.startswith("OK "):
            raise RuntimeError(f"タイムアウト: デバッガが停止しませんでした ({status})")
        return status[3:]

    if action == "step":
        status, _ = _send_command(f"DBG STEP {count}")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return status[3:]

    if action == "break":
        status, _ = _send_command("DBG BREAK")
        if not status.startswith("OK "):
            raise RuntimeError(f"エラー: {status}")
        return status[3:]

    if action == "mode":
        m = mode.upper()
        if m not in ("AUTO", "ON", "OFF"):
            raise ValueError("mode は AUTO/ON/OFF のいずれか")
        status, _ = _send_command(f"DBG MODE {m}")
        if status != "OK":
            raise RuntimeError(f"エラー: {status}")
        return f"DBG_MODE={m}"

    raise ValueError('action は "status"/"continue"/"step"/"break"/"mode" のいずれか')


@mcp.tool()
def read_bitmap_text() -> str:
    """現在の TVRAM 画面テキストをそのまま文字列として返す（CGROM ANK 照合）。
    trailing space は各行から除去される。"""
    status, body = _send_command("TEXT BITMAP")
    if status != "OK":
        raise RuntimeError(f"エラー: {status}")
    if not body:
        return ""
    return body.decode("shift_jis", errors="replace")


def _read_bitmap_lines() -> list[str]:
    """READ_BITMAP_TEXT を呼び出し、空行を除いた行リストを返す。"""
    status, body = _send_command("TEXT BITMAP")
    if status != "OK":
        return []
    text = body.decode("shift_jis", errors="replace")
    return [ln for ln in text.splitlines() if ln.strip()]


# 最終プロンプト行の判定（"A>" / "A:\\SUB>" 等。空白を含まず ">" で終わる短い行）。
_PROMPT_RE = re.compile(r"[^\s>]{0,32}>")


@mcp.tool()
def run_command(command: str, timeout: float = 10.0) -> str:
    """Human68k コマンドを実行し、プロンプトが戻った後の画面テキスト全体を返す。
    command: 実行するコマンド文字列 (改行不要)
    timeout: 最大待機秒数
    注: 画面の最終状態を返す。スクロールで消えた出力は含まれない。"""
    cmd = command.rstrip("\r\n")

    _auto_text_probe_tick()
    _mark_wait_cursor()
    _send_command("KEY CLEAR")

    # 送信前: B_KEYINP でキー入力待ちになるまで待つ（早すぎる送信による文字欠落を防ぐ。
    # 既に入力待ちなら即座に返る）。timeout をそのまま使う: 起動/リセット直後は
    # ヒストリドライバ初期化等で入力待ちになるまで約10秒かかることがあり、
    # ここを短い固定値に制限すると早すぎる PASTE で文字が欠落する。
    presend_timeout_ms = int(timeout * 1000)
    status, _ = _send_command(
        f"KEY WAIT {presend_timeout_ms}", timeout=presend_timeout_ms / 1000.0 + 5.0
    )
    if not status.startswith("OK"):
        # ここで送ると早すぎて文字が欠落する。黙って進めると「コマンドが実行
        # されない・出力が空」という分かりにくい失敗になるので明示的に失敗させる。
        raise RuntimeError(
            f"キー入力待ちにならないため {command!r} を送信できませんでした（{status}）。"
            "エミュレータが起動中か、B_KEYINPを呼ばないプログラムが動作中の可能性があります。"
        )

    raw = (cmd + "\n").encode("shift_jis", errors="replace")
    _send_command(f"PASTE {raw.hex().upper()}")

    # コマンド実行完了(次のキー入力待ちに戻る)まで待つ。タイムアウトしても
    # 画面の最終状態は返す（ハング時のフォールバック）。
    timeout_ms = int(timeout * 1000)
    _send_command(f"KEY WAIT {timeout_ms}", timeout=timeout + 5.0)

    final_lines = _read_bitmap_lines()

    # コマンドエコー行（"A>cmd" 形式）を末尾から探し、その後ろの出力だけを返す
    # エコー行は「プロンプト + コマンド」で終わる行。split(">")で前後に分けると
    # `TYPE FOO > BAR` のようにコマンド自身が ">" を含むときに一致しなくなるため、
    # 行末がコマンドと一致し、かつその手前にプロンプトの ">" があることで判定する。
    cmd_echo_idx = -1
    for i, line in enumerate(final_lines):
        if cmd and line.endswith(cmd) and ">" in line[: len(line) - len(cmd)]:
            cmd_echo_idx = i

    if cmd_echo_idx >= 0:
        output_lines = final_lines[cmd_echo_idx + 1:]
        # 最終プロンプト行（"A>" や "A:\\SUB>" 等、空白を含まず ">" で終わる行）を除去
        if output_lines and _PROMPT_RE.fullmatch(output_lines[-1].strip()):
            output_lines = output_lines[:-1]
        return "\n".join(output_lines)

    # フォールバック: 画面全体を返す
    return "\n".join(final_lines)


if __name__ == "__main__":
    mcp.run()
