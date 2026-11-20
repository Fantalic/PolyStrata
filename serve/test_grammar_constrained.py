"""Exercise grammar-constrained decoding through the HTTP layer:
json_schema -> GBNF compilation, explicit grammar field, batch rejection, status flag."""
import json
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest import mock

from serve.frontend import ChatTemplate
from serve import structured
from serve.server import ByteTokenizer, MockEngine, Service, serve


def object_schema(properties):
    return {"type": "object", "properties": properties, "required": list(properties), "additionalProperties": False}


TEXT = {"type": "string"}

SIMPLE_SCHEMA = object_schema({
    "name": TEXT, "age": {"type": "integer", "minimum": 0},
})
FORMAT_JSON_SCHEMA = {"type": "json_schema", "json_schema": {"name": "person", "strict": True, "schema": SIMPLE_SCHEMA}}


class GrammarConstrained(unittest.TestCase):
    """Grammar-constrained decoding tests via the HTTP layer."""

    def request(self, path, body=None, headers=None):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode() if body is not None else None,
                                      headers={"Content-Type": "application/json", **(headers or {})})
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read().decode()
            ct = response.headers.get("Content-Type", "")
            return response.status, data if "event-stream" in ct else json.loads(data)

    def setUp(self):
        self.tok = ByteTokenizer()
        self.engine = MockEngine(self.tok, '{"name":"Alice","age":30}', max_context=16384)
        self.svc = Service(self.engine, self.tok, ChatTemplate(Path(__file__).parent / "chat_template.jinja"))
        self.httpd = serve(self.svc, port=0)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def chat(self, body=None, **kwargs):
        if body is None:
            body = {"messages": [{"role": "system", "content": "Write a film plan."},
                                 {"role": "user", "content": "Photobooth. 15 seconds."}],
                    "reasoning_effort": "none", **kwargs}
        return self.request("/v1/chat/completions", body)

    def _capture_sampling(self):
        """Patch MockEngine.generate to capture the sampling dict, then restore."""
        self._captured_sampling = {}
        original_generate = self.engine.generate

        def capturing_generate(ids, max_new, sampling, cancel, embeddings=None):
            self._captured_sampling = dict(sampling)
            return original_generate(ids, max_new, sampling, cancel, embeddings)

        return mock.patch.object(self.engine, "generate", side_effect=capturing_generate)

    def test_json_schema_compiles_to_grammar(self):
        """A json_schema response_format produces a sampling dict with grammar_hex."""
        with self._capture_sampling():
            code, reply = self.chat(response_format=FORMAT_JSON_SCHEMA)
        self.assertEqual(code, 200)
        self.assertIn("grammar_hex", self._captured_sampling)
        hex_val = self._captured_sampling["grammar_hex"]
        # Verify it decodes back to valid GBNF text
        gbnf_bytes = bytes.fromhex(hex_val).decode("utf-8")
        self.assertTrue(len(gbnf_bytes) > 0)
        # The compiled grammar should contain typical GBNF rules
        self.assertIn("root", gbnf_bytes)

    def test_explicit_grammar_field_hex(self):
        """An explicit grammar field is hex-encoded and passed to the engine."""
        # "root ::= \"ok\"\n" hex-encoded
        grammar_text = 'root ::= "ok"\n'
        grammar_hex = grammar_text.encode("utf-8").hex()
        with self._capture_sampling():
            code, reply = self.chat(grammar=grammar_text)
        self.assertEqual(code, 200)
        self.assertIn("grammar_hex", self._captured_sampling)
        self.assertEqual(self._captured_sampling["grammar_hex"], grammar_hex)

    def test_unsupported_schema_returns_400(self):
        """A schema the validator accepts but the grammar compiler rejects
        (unanchored pattern under an object root) returns 400."""
        fmt = {"type": "json_schema", "json_schema": {"name": "bad", "schema": {
            "type": "object",
            "properties": {"x": {"type": "string", "pattern": "abc"}},
            "required": ["x"],
        }}}
        code, reply = self.chat(response_format=fmt)
        self.assertEqual(code, 400)
        self.assertIn("error", reply)
        self.assertIn("grammar", reply["error"]["message"].lower())

    def test_text_format_no_grammar(self):
        """response_format type text produces no grammar key in the sampling dict."""
        with self._capture_sampling():
            code, reply = self.chat(response_format={"type": "text"})
        self.assertEqual(code, 200)
        self.assertNotIn("grammar_hex", self._captured_sampling)

    def test_json_object_grammar(self):
        """json_object response_format produces a grammar_hex in the sampling dict."""
        fmt = {"type": "json_object"}
        with self._capture_sampling():
            code, reply = self.chat(response_format=fmt)
        self.assertEqual(code, 200)
        self.assertIn("grammar_hex", self._captured_sampling)
        hex_val = self._captured_sampling["grammar_hex"]
        gbnf = bytes.fromhex(hex_val).decode("utf-8")
        self.assertIn("root", gbnf)

    def test_v1_status_constrained_decoding_false(self):
        """v1/status shows constrained_decoding: False when the mock engine has no grammar flag."""
        req = urllib.request.Request(self.base + "/v1/status")
        with urllib.request.urlopen(req, timeout=10) as response:
            status = json.loads(response.read().decode())
        self.assertIn("structured_output", status)
        self.assertFalse(status["structured_output"]["constrained_decoding"])

    def test_v1_status_constrained_decoding_true_when_grammar_present(self):
        """v1/status shows constrained_decoding: True when the engine exposes grammar=1."""
        # Temporarily set grammar=1 on the engine
        self.engine.grammar = 1
        try:
            req = urllib.request.Request(self.base + "/v1/status")
            with urllib.request.urlopen(req, timeout=10) as response:
                status = json.loads(response.read().decode())
            self.assertTrue(status["structured_output"]["constrained_decoding"])
        finally:
            self.engine.grammar = 0


if __name__ == "__main__":
    unittest.main()
