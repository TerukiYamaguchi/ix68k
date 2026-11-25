import io
import os
import pty
import sys
import threading
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import aiq_bridge as bridge


def fake_llm(text):
    return lambda _instruction: text


class ParseRequestTest(unittest.TestCase):
    def test_ok(self):
        self.assertEqual(bridge.parse_request("ASK DIR を見せて".encode("cp932")), "DIR を見せて")

    def test_rejects_non_ask(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_request(b"HELLO world")

    def test_rejects_empty_argument(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_request(b"ASK  ")

    def test_rejects_bad_encoding(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_request(b"ASK abc\x81")


class ParseLlmOutputTest(unittest.TestCase):
    def test_plain_json(self):
        self.assertEqual(
            bridge.parse_llm_output('{"commands": ["DIR A:\\\\"]}'), ([], ["DIR A:\\"])
        )

    def test_strips_fence_and_think(self):
        text = '<think>考え中 {x}</think>\n```json\n{"commands": ["CLS", " "]}\n```'
        self.assertEqual(bridge.parse_llm_output(text), ([], ["CLS"]))

    def test_ignores_trailing_garbage(self):
        text = '{"commands": ["CLS"]}\n\n{"commands": ["CLS"]\n```'
        self.assertEqual(bridge.parse_llm_output(text), ([], ["CLS"]))

    def test_empty(self):
        self.assertEqual(bridge.parse_llm_output('{"commands": []}'), ([], []))

    def test_message_only(self):
        self.assertEqual(
            bridge.parse_llm_output('{"message": "COPY を使います。\\n例です"}'),
            (["COPY を使います。", "例です"], []),
        )

    def test_message_and_commands(self):
        lines, commands = bridge.parse_llm_output('{"message": "実行します", "commands": ["CLS"]}')
        self.assertEqual((lines, commands), (["実行します"], ["CLS"]))

    def test_rejects_non_json(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output("すみません")

    def test_rejects_unrelated_object(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output("{}")

    def test_rejects_non_string_message(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output('{"message": 1}')

    def test_rejects_control_char(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output('{"commands": ["DIR\\nDEL *.*"]}')

    def test_rejects_too_long(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output('{"commands": ["' + "A" * 256 + '"]}')

    def test_rejects_unencodable_command(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output('{"commands": ["ECHO 😀"]}')

    def test_rejects_non_list(self):
        with self.assertRaises(bridge.BridgeError):
            bridge.parse_llm_output('{"commands": "DIR"}')


class WrapMessageTest(unittest.TestCase):
    def test_wraps_at_byte_limit_on_char_boundary(self):
        lines = bridge.wrap_message("あ" * 40)  # 全角 1 文字 = 2 バイト
        self.assertEqual([len(line.encode("cp932")) for line in lines], [76, 4])

    def test_unencodable_becomes_question_mark_width(self):
        # 😀 は cp932 で ? 1 バイトに置換されるので、折り返し幅の計算は 1 バイト
        self.assertEqual(bridge.wrap_message("A😀B"), ["A😀B"])

    def test_truncates_long_message(self):
        lines = bridge.wrap_message("\n".join(f"行{i}" for i in range(30)))
        self.assertEqual(len(lines), bridge.MAX_MESSAGE_LINES + 1)
        self.assertEqual(lines[-1], bridge.MESSAGE_TRUNCATED_LINE)

    def test_strips_trailing_blank_lines(self):
        self.assertEqual(bridge.wrap_message("a\n\n"), ["a"])


class HandleLineTest(unittest.TestCase):
    def test_reply(self):
        reply = bridge.handle_line(b"ASK x", fake_llm('{"commands": ["CLS", "VER"]}'))
        self.assertEqual(reply, b"CMD CLS\r\nCMD VER\r\nEND\r\n")

    def test_reply_with_message(self):
        reply = bridge.handle_line(b"ASK x", fake_llm('{"message": "hi", "commands": ["CLS"]}'))
        self.assertEqual(reply, b"MSG hi\r\nCMD CLS\r\nEND\r\n")

    def test_message_with_unencodable_char_is_not_an_error(self):
        reply = bridge.handle_line(b"ASK x", fake_llm('{"message": "ok 😀"}'))
        self.assertEqual(reply, b"MSG ok ?\r\nEND\r\n")

    def test_error_has_no_end(self):
        reply = bridge.handle_line(b"junk", fake_llm("{}"))
        self.assertEqual(reply, b"ERR bad request\r\n")

    def test_llm_error_becomes_err(self):
        def failing(_):
            raise bridge.BridgeError("timeout")

        self.assertEqual(bridge.handle_line(b"ASK x", failing), b"ERR timeout\r\n")


class PostJsonTest(unittest.TestCase):
    def raise_http(self, code, body):
        error = urllib.error.HTTPError("http://x", code, "err", {}, io.BytesIO(body.encode()))
        return mock.patch("urllib.request.urlopen", side_effect=error)

    def test_context_overflow_is_distinguished(self):
        body = '{"error":"tokens to keep is greater than the context length"}'
        with self.raise_http(400, body), self.assertRaises(bridge.BridgeError) as caught:
            bridge.post_json("http://x", {}, {}, 1.0)
        self.assertEqual(str(caught.exception), "context too small")

    def test_other_http_error_keeps_code(self):
        with self.raise_http(401, "unauthorized"), self.assertRaises(bridge.BridgeError) as caught:
            bridge.post_json("http://x", {}, {}, 1.0)
        self.assertEqual(str(caught.exception), "api http 401")


class ClaudeLlmTest(unittest.TestCase):
    def test_text_blocks_are_joined_and_thinking_ignored(self):
        reply = {"stop_reason": "end_turn", "content": [
            {"type": "thinking", "thinking": ""},
            {"type": "text", "text": '{"commands":'},
            {"type": "text", "text": '["DIR"]}'}]}
        self.assertEqual(bridge.extract_claude_text(reply), '{"commands":["DIR"]}')

    def test_refusal_and_truncation_become_err(self):
        for stop_reason, reason in (("refusal", "refused"), ("max_tokens", "truncated")):
            with self.assertRaises(bridge.BridgeError) as caught:
                bridge.extract_claude_text({"stop_reason": stop_reason, "content": []})
            self.assertEqual(str(caught.exception), reason)

    def test_malformed_reply_is_bad_api_response(self):
        with self.assertRaises(bridge.BridgeError) as caught:
            bridge.extract_claude_text({"stop_reason": "end_turn"})
        self.assertEqual(str(caught.exception), "bad api response")

    def test_request_payload(self):
        sent = {}

        def fake_post(url, headers, payload, timeout):
            sent.update(url=url, headers=headers, payload=payload)
            return {"stop_reason": "end_turn", "content": [{"type": "text", "text": "ok"}]}

        with mock.patch.dict(os.environ, {"ANTHROPIC_API_KEY": "k"}), \
                mock.patch.object(bridge, "post_json", fake_post):
            ask = bridge.make_claude_llm("claude-sonnet-5-5", "sys", 5.0)
            self.assertEqual(ask("DIR"), "ok")
        self.assertEqual(sent["headers"]["x-api-key"], "k")
        self.assertEqual(sent["payload"]["output_config"], {"effort": "low"})
        self.assertNotIn("temperature", sent["payload"])
        self.assertEqual(sent["payload"]["messages"], [{"role": "user", "content": "DIR"}])


class CodexLlmTest(unittest.TestCase):
    def test_text_blocks_are_joined(self):
        reply = {"status": "completed", "output": [{"content": [
            {"type": "reasoning", "summary": []},
            {"type": "output_text", "text": '{"message":"","commands":'},
            {"type": "output_text", "text": '["DIR"]}'}]}]}
        self.assertEqual(bridge.extract_codex_text(reply), '{"message":"","commands":["DIR"]}')

    def test_refusal_and_truncation_become_err(self):
        replies = (
            ({"status": "completed", "output": [{"content": [{"type": "refusal"}]}]}, "refused"),
            ({"status": "incomplete", "incomplete_details": {"reason": "max_output_tokens"}, "output": []}, "truncated"),
        )
        for reply, reason in replies:
            with self.assertRaises(bridge.BridgeError) as caught:
                bridge.extract_codex_text(reply)
            self.assertEqual(str(caught.exception), reason)

    def test_malformed_reply_is_bad_api_response(self):
        with self.assertRaises(bridge.BridgeError) as caught:
            bridge.extract_codex_text({"status": "completed"})
        self.assertEqual(str(caught.exception), "bad api response")

    def test_request_payload(self):
        sent = {}

        def fake_post(url, headers, payload, timeout):
            sent.update(url=url, headers=headers, payload=payload)
            return {"status": "completed", "output": [{"content": [
                {"type": "output_text", "text": "ok"}]}]}

        with mock.patch.dict(os.environ, {"OPENAI_API_KEY": "k"}), \
                mock.patch.object(bridge, "post_json", fake_post):
            ask = bridge.make_codex_llm("gpt-5.3-codex", "sys", 5.0)
            self.assertEqual(ask("DIR"), "ok")
        self.assertEqual(sent["url"], bridge.OPENAI_RESPONSES_URL)
        self.assertEqual(sent["headers"]["Authorization"], "Bearer k")
        self.assertEqual(sent["payload"]["reasoning"], {"effort": "low"})
        self.assertEqual(sent["payload"]["text"]["format"]["schema"], bridge.AIQ_REPLY_SCHEMA)
        self.assertNotIn("temperature", sent["payload"])


class LmstudioLlmTest(unittest.TestCase):
    def test_length_finish_reason_becomes_truncated(self):
        reply = {"choices": [{"finish_reason": "length", "message": {"content": '{"message": "x'}}]}
        with self.assertRaises(bridge.BridgeError) as caught:
            bridge.extract_lmstudio_text(reply)
        self.assertEqual(str(caught.exception), "truncated")

    def test_normal_reply_and_malformed_reply(self):
        ok = {"choices": [{"finish_reason": "stop", "message": {"content": "hi"}}]}
        self.assertEqual(bridge.extract_lmstudio_text(ok), "hi")
        with self.assertRaises(bridge.BridgeError):
            bridge.extract_lmstudio_text({})

    def test_request_has_frequency_penalty(self):
        sent = {}

        def fake_post(url, headers, payload, timeout):
            sent.update(payload)
            return {"choices": [{"finish_reason": "stop", "message": {"content": "ok"}}]}

        with mock.patch.object(bridge, "post_json", fake_post):
            bridge.make_lmstudio_llm("m", "http://x/v1", "sys", 5.0)("q")
        self.assertEqual(sent["frequency_penalty"], bridge.LMSTUDIO_FREQUENCY_PENALTY)


class WritePacedTest(unittest.TestCase):
    def test_writes_one_line_at_a_time_with_interval(self):
        reply = b"MSG a\r\nCMD b\r\nEND\r\n"
        with mock.patch("os.write") as write, mock.patch("time.sleep") as sleep:
            bridge.write_paced(7, reply, 0.1)
        self.assertEqual([c.args for c in write.call_args_list],
                         [(7, b"MSG a\r\n"), (7, b"CMD b\r\n"), (7, b"END\r\n")])
        self.assertEqual(sleep.call_count, 2)


class ServeTest(unittest.TestCase):
    def test_roundtrip_over_pty(self):
        master, slave = pty.openpty()
        bridge_fd = os.open(os.ttyname(slave), os.O_RDWR | os.O_NOCTTY)
        import tty

        tty.setraw(bridge_fd)
        tty.setraw(master)
        thread = threading.Thread(
            target=bridge.serve,
            args=(bridge_fd, fake_llm('{"commands": ["DIR"]}')),
            daemon=True,
        )
        thread.start()
        os.write(master, b"ASK list\r\n")
        received = b""
        while not received.endswith(b"END\r\n"):
            received += os.read(master, 256)
        self.assertEqual(received, b"CMD DIR\r\nEND\r\n")

    def test_xon_xoff_prefix_is_ignored(self):
        master, slave = pty.openpty()
        bridge_fd = os.open(os.ttyname(slave), os.O_RDWR | os.O_NOCTTY)
        import tty

        tty.setraw(bridge_fd)
        tty.setraw(master)
        thread = threading.Thread(
            target=bridge.serve,
            args=(bridge_fd, fake_llm('{"commands": ["DIR"]}')),
            daemon=True,
        )
        thread.start()
        os.write(master, b"\x13\x11\x13\x11ASK li\x13st\r\n")
        received = b""
        while not received.endswith(b"END\r\n"):
            received += os.read(master, 256)
        self.assertEqual(received, b"CMD DIR\r\nEND\r\n")


if __name__ == "__main__":
    unittest.main()
