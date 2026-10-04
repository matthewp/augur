#!/usr/bin/env python3
"""A pretend AI provider for Augur's tests: OpenAI's chat completions and
Anthropic's Messages API, streamed, answering by what the prompt says.

    mock_provider.py LOG    prints "PORT n" when it's listening; every
                            request goes into LOG as a line of JSON

What the last user message says decides the answer:
    rate     a 429
    slow     a long answer, slowly (to be cancelled)
    retry    with a schema: a wrong answer first, then a right one
    else     "Hello there!", or JSON when there's a schema
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = sys.argv[1]
OPENAI_KEY = "sk-test"
ANTHROPIC_KEY = "sk-ant-test"


def last_user(messages):
    for m in reversed(messages):
        if m.get("role") == "user":
            return m.get("content", "")
    return ""


def has_assistant(messages):
    return any(m.get("role") == "assistant" for m in messages)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def record(self, body):
        with open(LOG, "a") as f:
            f.write(json.dumps({
                "path": self.path,
                "authorization": self.headers.get("Authorization"),
                "x-api-key": self.headers.get("x-api-key"),
                "anthropic-version": self.headers.get("anthropic-version"),
                "cf-aig-authorization": self.headers.get("cf-aig-authorization"),
                "body": body,
            }) + "\n")

    def send_json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def start_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

    def event(self, obj, name=None):
        if name:
            self.wfile.write(f"event: {name}\n".encode())
        self.wfile.write(f"data: {json.dumps(obj)}\n\n".encode())
        self.wfile.flush()

    def do_GET(self):
        self.record(None)
        if self.path.endswith("/models"):
            self.send_json(200, {"data": [{"id": "model-a"}, {"id": "model-b"}]})
        else:
            self.send_json(404, {"error": {"message": "no such thing"}})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        self.record(body)
        try:
            if self.path.endswith("/chat/completions"):
                self.openai(body)
            elif self.path.endswith("/messages"):
                self.anthropic(body)
            else:
                self.send_json(404, {"error": {"message": "no such thing"}})
        except (BrokenPipeError, ConnectionResetError):
            pass  # a cancelled request: Augur hung up, as it should

    # ---- OpenAI ---------------------------------------------------------

    def openai(self, body):
        auth = self.headers.get("Authorization")
        if auth is not None and auth != f"Bearer {OPENAI_KEY}":
            self.send_json(401, {"error": {"message": "Incorrect API key",
                                           "type": "invalid_request_error"}})
            return
        messages = body.get("messages", [])
        prompt = last_user(messages)
        if "rate" in prompt:
            self.send_json(429, {"error": {"message": "Slow down",
                                           "type": "rate_limit_error"}})
            return
        system = " ".join(m["content"] for m in messages
                          if m.get("role") == "system")
        if "response_format" in body:
            wrong = "retry" in prompt and not has_assistant(messages)
            text = json.dumps({"categories": ["cats" if wrong else "bill"]})
            pieces = [text[:10], text[10:]]
        elif "JSON Schema" in system:
            pieces = ["```json\n", '{"categories": ["newsletter"]}', "\n```"]
        elif "slow" in prompt:
            pieces = ["word "] * 40
        else:
            pieces = ["Hello", " there", "!"]
        self.start_stream()
        for p in pieces:
            self.event({"choices": [{"delta": {"content": p}}]})
            if "slow" in prompt:
                time.sleep(0.25)
        if body.get("stream_options", {}).get("include_usage"):
            self.event({"choices": [], "usage": {"prompt_tokens": 11,
                                                 "completion_tokens": 3}})
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    # ---- Anthropic ------------------------------------------------------

    def anthropic(self, body):
        if self.headers.get("x-api-key") != ANTHROPIC_KEY or \
                self.headers.get("anthropic-version") is None:
            self.send_json(401, {"type": "error", "error": {
                "type": "authentication_error", "message": "invalid x-api-key"}})
            return
        messages = body.get("messages", [])
        prompt = last_user(messages)
        self.start_stream()
        self.event({"type": "message_start",
                    "message": {"usage": {"input_tokens": 21}}}, "message_start")
        tools = body.get("tools")
        if tools:
            schema = tools[0]["input_schema"]
            if "answer" in schema.get("properties", {}):
                text = json.dumps({"answer": 42})
            else:
                text = json.dumps({"categories": ["sports"]})
            self.event({"type": "content_block_start", "index": 0,
                        "content_block": {"type": "tool_use", "name": "answer"}})
            for i in range(0, len(text), 7):
                self.event({"type": "content_block_delta", "index": 0, "delta": {
                    "type": "input_json_delta", "partial_json": text[i:i + 7]}})
        else:
            self.event({"type": "content_block_start", "index": 0,
                        "content_block": {"type": "text", "text": ""}})
            for p in ["Hi", " from", " Claude"]:
                self.event({"type": "content_block_delta", "index": 0,
                            "delta": {"type": "text_delta", "text": p}})
        self.event({"type": "content_block_stop", "index": 0})
        self.event({"type": "message_delta", "delta": {},
                    "usage": {"output_tokens": 5}})
        self.event({"type": "message_stop"})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
print(f"PORT {server.server_address[1]}", flush=True)
server.serve_forever()
