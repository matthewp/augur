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

Given tools, it says "Let me look." and calls the one the prompt names (or
the first) with {"city": "Paris"}, then answers "The tool said: ..." with
what the tools said (or, with a schema on Anthropic, answers through it).
The prompt can also say:
    twice    call it twice at once, for Paris and Rome
    badargs  call it first with arguments that don't fit
    loop     never stop calling it
Told it can't call tools (tool_choice none) it answers "No more tools".
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
        if m.get("role") == "user" and isinstance(m.get("content"), str):
            return m["content"]
    return ""


def tool_calls(prompt, tools, messages):
    """The calls to make: (name, arguments) pairs."""
    names = [t["name"] for t in tools]
    name = next((n for n in names if n in prompt), names[0])
    if "twice" in prompt:
        return [(name, {"city": "Paris"}), (name, {"city": "Rome"})]
    first = not any(m.get("role") == "assistant" for m in messages)
    if "badargs" in prompt and first:
        return [(name, {"bogus": True})]
    return [(name, {"city": "Paris"})]


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
        tools = [t["function"] for t in body.get("tools", [])]
        if tools:
            self.openai_tools(body, prompt, tools, messages)
            return
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

    def openai_tools(self, body, prompt, tools, messages):
        self.start_stream()
        results = []
        for m in reversed(messages):
            if m.get("role") != "tool":
                break
            results.insert(0, m["content"])
        if body.get("tool_choice") == "none":
            self.event({"choices": [{"delta": {"content": "No more tools"}}]})
        elif results and "loop" not in prompt:
            self.event({"choices": [{"delta": {
                "content": "The tool said: " + " | ".join(results)}}]})
        else:
            self.event({"choices": [{"delta": {"content": "Let me look. "}}]})
            turn = sum(1 for m in messages if m.get("role") == "assistant")
            for i, (name, args) in enumerate(tool_calls(prompt, tools, messages)):
                text = json.dumps(args)
                self.event({"choices": [{"delta": {"tool_calls": [{
                    "index": i, "id": f"call_{turn}_{i}", "type": "function",
                    "function": {"name": name, "arguments": ""}}]}}]})
                for j in range(0, len(text), 5):
                    self.event({"choices": [{"delta": {"tool_calls": [{
                        "index": i, "function": {"arguments": text[j:j + 5]}}]}}]})
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
        tools = body.get("tools") or []
        mine = [t for t in tools if t["name"] != "answer"]
        answer = [t for t in tools if t["name"] == "answer"]
        choice = body.get("tool_choice", {}).get("type")
        last = messages[-1].get("content") if messages else None
        results = [b for b in last if b.get("type") == "tool_result"] \
            if isinstance(last, list) else []
        if mine and choice not in ("tool", "none") and \
                (not results or "loop" in prompt or answer == []):
            if results and "loop" not in prompt:
                self.text_blocks(["The tool said: " + " | ".join(
                    ("Error: " if r.get("is_error") else "") + r["content"]
                    for r in results)])
            else:
                self.text_blocks(["Let me look. "])
                turn = sum(1 for m in messages if m.get("role") == "assistant")
                for i, (name, args) in enumerate(tool_calls(prompt, mine,
                                                            messages)):
                    self.event({"type": "content_block_start", "index": i + 1,
                                "content_block": {"type": "tool_use", "name": name,
                                                  "id": f"toolu_{turn}_{i}"}})
                    self.event({"type": "content_block_delta", "index": i + 1,
                                "delta": {"type": "input_json_delta",
                                          "partial_json": json.dumps(args)}})
                    self.event({"type": "content_block_stop", "index": i + 1})
            self.finish_anthropic()
            return
        if choice == "none":
            self.text_blocks(["No more tools"])
            self.finish_anthropic()
            return
        if answer:
            schema = answer[0]["input_schema"]
            if "answer" in schema.get("properties", {}):
                text = json.dumps({"answer": 42})
            else:
                text = json.dumps({"categories": ["sports"]})
            self.event({"type": "content_block_start", "index": 0,
                        "content_block": {"type": "tool_use", "name": "answer"}})
            for i in range(0, len(text), 7):
                self.event({"type": "content_block_delta", "index": 0, "delta": {
                    "type": "input_json_delta", "partial_json": text[i:i + 7]}})
            self.event({"type": "content_block_stop", "index": 0})
        else:
            self.text_blocks(["Hi", " from", " Claude"])
        self.finish_anthropic()

    def text_blocks(self, pieces):
        self.event({"type": "content_block_start", "index": 0,
                    "content_block": {"type": "text", "text": ""}})
        for p in pieces:
            self.event({"type": "content_block_delta", "index": 0,
                        "delta": {"type": "text_delta", "text": p}})
        self.event({"type": "content_block_stop", "index": 0})

    def finish_anthropic(self):
        self.event({"type": "message_delta", "delta": {},
                    "usage": {"output_tokens": 5}})
        self.event({"type": "message_stop"})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
print(f"PORT {server.server_address[1]}", flush=True)
server.serve_forever()
