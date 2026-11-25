"""ix68k MCP の長さプレフィックス wire client。

FastMCP サーバ、HOSTFILE CLI、将来のデバッガ GUI が同じ接続・HELLO・tag
契約を使うための標準ライブラリだけの共有層である。接続は呼び出しごとに閉じ、
pipeline() だけが一接続へ複数の読み取り要求を送る。
"""

from __future__ import annotations

from dataclasses import dataclass
import os
import socket
import struct
import sys
from typing import Callable, Iterable

PROTO_EXPECTED = 5
DEFAULT_PORT = 26800
PORT_MIN = 1024
PORT_MAX = 65535


class WireError(RuntimeError):
    """MCP wire 層の基底例外。"""


class WireConnectionError(WireError):
    """connect() が失敗した。"""


class WireEOFError(WireError):
    """長さまたは本文の途中で peer が切断した。"""


class WireTimeoutError(WireError, TimeoutError):
    """ソケット timeout。呼び出し側は busy として再試行できる。"""


class WireProtocolError(WireError):
    """tag または response header が wire 契約に反する。"""


class WireProtocolVersionError(WireProtocolError):
    """HELLO の PROTO がこの client の期待値と異なる。"""


class WirePipelineRequestError(WireError):
    """pipeline に待機または状態変更コマンドが渡された。"""


def parse_port(value: str | None, default: int = DEFAULT_PORT) -> int:
    """C 側 mcp_port_resolve() と同じ ASCII decimal / 範囲の規約。"""
    if value is None or value == "":
        return default
    if not (1 <= len(value) <= 5) or not all("0" <= c <= "9" for c in value):
        raise ValueError(
            f"IX68K_MCP_PORT の値が不正です: {value!r}（{PORT_MIN}..{PORT_MAX} の10進数字のみ）"
        )
    port = int(value)
    if not PORT_MIN <= port <= PORT_MAX:
        raise ValueError(f"IX68K_MCP_PORT の値が範囲外です: {value!r}（{PORT_MIN}..{PORT_MAX}）")
    return port


def endpoint_from_environment(
    *, platform: str | None = None, environ: dict[str, str] | None = None
) -> tuple[str | tuple[str, int], bool]:
    """C 側と同じ既定 endpoint と transport を返す。"""
    platform = sys.platform if platform is None else platform
    environ = os.environ if environ is None else environ
    if platform == "win32":
        return ("127.0.0.1", parse_port(environ.get("IX68K_MCP_PORT"))), True
    return environ.get("IX68K_MCP_SOCK") or "/tmp/ix68k_mcp.sock", False


@dataclass(frozen=True)
class Request:
    command: str
    body: bytes = b""


@dataclass(frozen=True)
class Response:
    tag: str
    status: str
    body: bytes
    # pipeline の応答は順不同で到着する。GUI 等の呼び出し側が tag を再実装して
    # 応答を要求へ結び直さずに済むよう、wire 層で対応する要求を保持する。
    request: Request | None = None


SocketFactory = Callable[[int, int], socket.socket]


class WireClient:
    """PROTO 検証キャッシュを endpoint ごとに持つ短命 MCP client。"""

    def __init__(
        self,
        endpoint: str | tuple[str, int],
        *,
        expected_proto: int = PROTO_EXPECTED,
        socket_factory: SocketFactory = socket.socket,
    ) -> None:
        self.endpoint = endpoint
        self.expected_proto = expected_proto
        self.socket_factory = socket_factory
        self._proto_checked = False
        self._tag_seq = 0

    def invalidate_protocol(self) -> None:
        """次の接続で HELLO を必ず再送する。"""
        self._proto_checked = False

    def _next_tag(self) -> str:
        self._tag_seq += 1
        # tagは共有wire層だけが払い出す。空白なしASCII・31バイト以下のT<decimal>
        # という契約を、呼び出し側が変更できないようここで固定する。
        return f"T{self._tag_seq}"

    def _connect(self, timeout: float) -> socket.socket:
        family = socket.AF_INET if isinstance(self.endpoint, tuple) else socket.AF_UNIX
        sock = self.socket_factory(family, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(self.endpoint)
        except socket.timeout as exc:
            sock.close()
            raise WireTimeoutError("MCP接続がtimeoutしました") from exc
        except OSError as exc:
            sock.close()
            raise WireConnectionError(f"エミュレータに接続できません ({self.endpoint}): {exc}") from exc
        return sock

    @staticmethod
    def _recv_exact(sock: socket.socket, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            chunk = sock.recv(size - len(chunks))
            if not chunk:
                raise WireEOFError("MCP応答が途中で切断されました")
            chunks.extend(chunk)
        return bytes(chunks)

    def _exchange_on_socket(self, sock: socket.socket, requests: Iterable[Request]) -> list[Response]:
        tagged = [(self._next_tag(), request) for request in requests]
        if not tagged:
            return []
        for tag, request in tagged:
            payload = f"{tag} {request.command}\n".encode("ascii") + request.body
            sock.sendall(struct.pack(">I", len(payload)) + payload)

        pending = {tag for tag, _ in tagged}
        requests_by_tag = dict(tagged)
        responses: list[Response] = []
        while pending:
            (length,) = struct.unpack(">I", self._recv_exact(sock, 4))
            raw = self._recv_exact(sock, length)
            if b"\n" not in raw:
                raise WireProtocolError("MCP応答headerに改行がありません")
            raw_header, body = raw.split(b"\n", 1)
            try:
                header = raw_header.decode("ascii")
            except UnicodeDecodeError as exc:
                raise WireProtocolError("MCP応答headerがASCIIではありません") from exc
            if " " not in header:
                raise WireProtocolError("MCP応答headerにtagがありません")
            tag, status = header.split(" ", 1)
            if not tag or tag not in pending:
                raise WireProtocolError(f"MCP応答tagが不正です: {header!r}")
            pending.remove(tag)
            responses.append(Response(tag, status, body, requests_by_tag[tag]))
        return responses

    def _exchange(self, requests: Iterable[Request], timeout: float) -> list[Response]:
        sock = self._connect(timeout)
        try:
            return self._exchange_on_socket(sock, requests)
        except socket.timeout as exc:
            raise WireTimeoutError("MCP応答がtimeoutしました") from exc
        except OSError as exc:
            raise WireConnectionError(f"MCP通信に失敗しました ({self.endpoint}): {exc}") from exc
        finally:
            sock.close()

    def _raw_send(self, request: Request, timeout: float) -> Response:
        return self._exchange([request], timeout)[0]

    def _verify_hello(self, response: Response) -> None:
        if not response.status.startswith("OK "):
            raise WireProtocolVersionError(
                f"HELLO に応答しません（{response.status}）。エミュレータが古い可能性があります。"
            )
        fields = dict(token.split("=", 1) for token in response.status[3:].split() if "=" in token)
        try:
            proto = int(fields.get("PROTO", "-1"))
        except ValueError:
            proto = -1
        if proto != self.expected_proto:
            raise WireProtocolVersionError(
                "MCPワイヤプロトコルの版が違います"
                f"（エミュレータ={proto} / このクライアント={self.expected_proto}、"
                f"エミュレータのビルド={fields.get('BUILD', '不明')}）。"
            )

    def ensure_protocol(self, timeout: float = 10.0) -> None:
        if self._proto_checked:
            return
        try:
            self._verify_hello(self._raw_send(Request("HELLO"), timeout))
        except socket.timeout as exc:
            self.invalidate_protocol()
            raise WireTimeoutError("MCP応答がtimeoutしました") from exc
        except WireError:
            self.invalidate_protocol()
            raise
        except OSError as exc:
            self.invalidate_protocol()
            raise WireConnectionError(f"MCP通信に失敗しました ({self.endpoint}): {exc}") from exc
        self._proto_checked = True

    def send(self, command: str, timeout: float = 10.0, body: bytes = b"") -> Response:
        try:
            self.ensure_protocol(timeout)
            return self._raw_send(Request(command, body), timeout)
        except WireError:
            self.invalidate_protocol()
            raise
        except OSError as exc:
            self.invalidate_protocol()
            raise WireConnectionError(f"MCP通信に失敗しました ({self.endpoint}): {exc}") from exc

    @staticmethod
    def _is_pipeline_read_request(request: Request) -> bool:
        """GUI更新で安全にパイプライン化できる、本文なしの照会だけを許可する。"""
        if request.body or "\n" in request.command or "\r" in request.command:
            return False
        parts = request.command.split()
        if request.command in {"DBG STATUS", "REGS", "MMU GET", "BP LIST", "WP LIST", "EXC GET"}:
            return True
        if len(parts) == 3 and parts[0] == "DISASM":
            return True
        # MEM READ は I/O read-clear 等の副作用を持つ。標準更新で安全に
        # pipeline 化できるメモリアクセスは、4トークンの MEM PEEK だけである。
        if len(parts) == 4 and parts[:2] == ["MEM", "PEEK"]:
            return True
        return len(parts) in (2, 3) and parts[0] == "RING" and parts[1] in {"PCRING", "REGRING", "CALLRING"}

    def pipeline(self, requests: Iterable[Request], timeout: float = 10.0) -> dict[str, Response]:
        """一接続でHELLO確認後、同期・読み取り専用要求だけをtag照合して送る。"""
        queued = tuple(requests)
        if not all(self._is_pipeline_read_request(request) for request in queued):
            raise WirePipelineRequestError("pipelineには同期・読み取り専用の照会だけを指定できます")
        if not queued:
            return {}
        try:
            sock = self._connect(timeout)
            try:
                if not self._proto_checked:
                    self._verify_hello(self._exchange_on_socket(sock, [Request("HELLO")])[0])
                    self._proto_checked = True
                responses = self._exchange_on_socket(sock, queued)
            finally:
                sock.close()
        except socket.timeout as exc:
            self.invalidate_protocol()
            raise WireTimeoutError("MCP応答がtimeoutしました") from exc
        except WireError:
            self.invalidate_protocol()
            raise
        except OSError as exc:
            self.invalidate_protocol()
            raise WireConnectionError(f"MCP通信に失敗しました ({self.endpoint}): {exc}") from exc
        return {response.tag: response for response in responses}
