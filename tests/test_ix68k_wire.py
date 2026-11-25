#!/usr/bin/env python3
"""tools-host/mcp/ix68k_wire.py の wire 契約テスト。"""

import socket
import struct
import sys
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "mcp"))
from ix68k_wire import (  # noqa: E402
    endpoint_from_environment,
    Request,
    WireClient,
    WireEOFError,
    WireConnectionError,
    WireProtocolError,
    WireProtocolVersionError,
    WirePipelineRequestError,
    WireTimeoutError,
)


def frame(header: str, body: bytes = b"") -> bytes:
    raw = header.encode("ascii") + b"\n" + body
    return struct.pack(">I", len(raw)) + raw


class FakeSocket:
    def __init__(self, incoming):
        self.incoming = list(incoming)
        self.sent = bytearray()
        self.closed = False
        self.endpoint = None

    def settimeout(self, timeout):
        self.timeout = timeout

    def connect(self, endpoint):
        self.endpoint = endpoint

    def sendall(self, data):
        self.sent.extend(data)

    def recv(self, size):
        if not self.incoming:
            return b""
        value = self.incoming[0]
        if isinstance(value, BaseException):
            self.incoming.pop(0)
            raise value
        if len(value) <= size:
            self.incoming.pop(0)
            return value
        self.incoming[0] = value[size:]
        return value[:size]

    def close(self):
        self.closed = True


class FakeFactory:
    def __init__(self, connections):
        self.connections = [FakeSocket(incoming) for incoming in connections]
        self.used = []

    def __call__(self, family, kind):
        self.assert_family = family
        self.assert_kind = kind
        result = self.connections.pop(0)
        self.used.append(result)
        return result


class WireClientTest(unittest.TestCase):
    def client(self, connections):
        factory = FakeFactory(connections)
        return WireClient("/tmp/test.sock", socket_factory=factory), factory

    def test_single_request_and_body(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5 BUILD=20261002 120000")],
            [frame("T2 OK value" , b"\x00body")],
        ])
        response = client.send("MEM READ 10 4", body=b"payload")
        self.assertEqual((response.status, response.body), ("OK value", b"\x00body"))
        self.assertTrue(factory.used[0].closed)
        self.assertIn(b"T2 MEM READ 10 4\npayload", factory.used[1].sent)

    def test_pipeline_accepts_out_of_order_responses(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5") + frame("T3 OK peek", b"peek") + frame("T2 OK first", b"one")],
        ])
        responses = client.pipeline([Request("REGS"), Request("MEM PEEK 1000 100")])
        self.assertEqual(responses["T2"].body, b"one")
        self.assertEqual((responses["T3"].status, responses["T3"].body), ("OK peek", b"peek"))
        self.assertEqual(responses["T2"].request.command, "REGS")
        self.assertEqual(responses["T3"].request.command, "MEM PEEK 1000 100")
        self.assertEqual(len(factory.used), 1, "初回HELLOと同期pipelineは同じ接続")
        sent = bytes(factory.used[0].sent)
        self.assertLess(sent.index(b"T1 HELLO\n"), sent.index(b"T2 REGS\n"))
        self.assertLess(sent.index(b"T2 REGS\n"), sent.index(b"T3 MEM PEEK 1000 100\n"))

    def test_pipeline_reuses_verified_protocol_and_rechecks_after_error(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5") + frame("T2 OK")],
            [frame("T3 OK")],
            [frame("T4 OK PROTO=5") + frame("T5 OK")],
        ])
        self.assertEqual(client.pipeline([Request("REGS")])["T2"].status, "OK")
        self.assertEqual(client.pipeline([Request("REGS")])["T3"].status, "OK")
        self.assertNotIn(b"HELLO", factory.used[1].sent, "検証済み更新は1接続のみ")

        # 応答EOFはキャッシュを破棄する。次の更新は同一接続でHELLOから再開する。
        failing, failing_factory = self.client([
            [frame("T1 OK PROTO=5") + b"\x00\x00"],
            [frame("T3 OK PROTO=5") + frame("T4 OK")],
        ])
        with self.assertRaises(WireEOFError):
            failing.pipeline([Request("REGS")])
        self.assertEqual(failing.pipeline([Request("REGS")])["T4"].status, "OK")
        self.assertIn(b"T3 HELLO\n", failing_factory.used[1].sent)

    def test_pipeline_timeout_is_typed_and_rechecks_protocol(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5"), socket.timeout("busy")],
            [frame("T3 OK PROTO=5") + frame("T4 OK")],
        ])
        with self.assertRaises(WireTimeoutError):
            client.pipeline([Request("REGS")], timeout=0.5)
        self.assertEqual(client.pipeline([Request("REGS")])["T4"].status, "OK")
        self.assertIn(b"T3 HELLO\n", factory.used[1].sent)

    def test_pipeline_socket_error_is_a_typed_connection_error(self):
        client, _ = self.client([
            [frame("T1 OK PROTO=5"), ConnectionResetError("reset")],
        ])
        with self.assertRaises(WireConnectionError):
            client.pipeline([Request("REGS")])

    def test_eof_during_prefix_or_body_invalidates_protocol_cache(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5")],
            [b"\x00\x00"],
            [frame("T3 OK PROTO=5")],
            [struct.pack(">I", 8), b"T4 OK\n"],
        ])
        with self.assertRaises(WireEOFError):
            client.send("REGS")
        with self.assertRaises(WireEOFError):
            client.send("REGS")
        self.assertEqual(len(factory.used), 4, "EOF後はHELLOを再送する")

    def test_timeout_invalidates_protocol_cache(self):
        client, factory = self.client([
            [frame("T1 OK PROTO=5")],
            [socket.timeout("busy")],
            [frame("T3 OK PROTO=5")],
            [frame("T4 OK")],
        ])
        with self.assertRaises(TimeoutError):
            client.send("REGS", timeout=0.5)
        self.assertEqual(client.send("REGS").status, "OK")
        self.assertEqual(len(factory.used), 4)

    def test_connect_error_is_a_shared_transport_exception(self):
        class RefusingFactory:
            def __call__(self, family, kind):
                class RefusingSocket:
                    def settimeout(self, timeout):
                        pass

                    def connect(self, endpoint):
                        raise OSError("refused")

                    def close(self):
                        pass
                return RefusingSocket()

        client = WireClient("/tmp/test.sock", socket_factory=RefusingFactory())
        with self.assertRaises(WireConnectionError):
            client.send("REGS")

    def test_connect_timeout_is_distinct_from_connection_error(self):
        class TimeoutFactory:
            def __call__(self, family, kind):
                class TimeoutSocket:
                    def settimeout(self, timeout):
                        pass

                    def connect(self, endpoint):
                        raise socket.timeout("busy")

                    def close(self):
                        pass
                return TimeoutSocket()

        client = WireClient("/tmp/test.sock", socket_factory=TimeoutFactory())
        with self.assertRaises(WireTimeoutError):
            client.send("REGS")

    def test_endpoint_resolution_preserves_unix_and_windows_conventions(self):
        endpoint, use_tcp = endpoint_from_environment(
            platform="darwin", environ={"IX68K_MCP_SOCK": "/tmp/alternate.sock"}
        )
        self.assertEqual((endpoint, use_tcp), ("/tmp/alternate.sock", False))
        endpoint, use_tcp = endpoint_from_environment(
            platform="win32", environ={"IX68K_MCP_PORT": "26801"}
        )
        self.assertEqual((endpoint, use_tcp), (("127.0.0.1", 26801), True))

    def test_bad_response_tags_and_headers_are_rejected(self):
        cases = [
            frame("X unknown"),
            frame(" OK"),
            struct.pack(">I", len(b"T2 OK!")) + b"T2 OK!",
        ]
        for response in cases:
            with self.subTest(response=response):
                client, _ = self.client([[frame("T1 OK PROTO=5")], [response]])
                with self.assertRaises(WireProtocolError):
                    client.send("REGS")

        client, _ = self.client([
            [frame("T1 OK PROTO=5") + frame("T2 OK") + frame("T2 duplicate")],
        ])
        with self.assertRaises(WireProtocolError):
            client.pipeline([Request("REGS"), Request("BP LIST")])

    def test_proto_mismatch_sends_no_followup_command(self):
        client, factory = self.client([[frame("T1 OK PROTO=3 BUILD=old")]])
        with self.assertRaises(WireProtocolVersionError):
            client.send("REGS")
        self.assertEqual(len(factory.used), 1)

    def test_pipeline_rejects_wait_and_state_changing_commands_before_connect(self):
        for request in (Request("DBG WAIT 10"), Request("KEY WAIT 10"), Request("DBG CONT"),
                        Request("MEM READ 1000 100"), Request("MEM WRITE 100", b"x")):
            with self.subTest(request=request):
                client, factory = self.client([])
                with self.assertRaises(WirePipelineRequestError):
                    client.pipeline([request])
                self.assertEqual(factory.used, [])


if __name__ == "__main__":
    unittest.main()
