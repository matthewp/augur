#!/bin/sh
# augurd and augur together, on a session bus of their own, against
# mock_provider.py: nothing here talks to a real provider.
#
#   test_service.sh AUGURD AUGUR MOCK_PROVIDER
set -u

if [ -z "${AUGUR_TEST_BUS:-}" ]; then
	exec env AUGUR_TEST_BUS=1 dbus-run-session -- sh "$0" "$@"
fi

AUGURD=$1
AUGUR=$2
MOCK=$3
TMP=$(mktemp -d)
export XDG_CONFIG_HOME="$TMP/config" XDG_STATE_HOME="$TMP/state"
mkdir -p "$XDG_CONFIG_HOME/augur"
CONFIG="$XDG_CONFIG_HOME/augur/config"
LOG="$XDG_STATE_HOME/augur/log"
failures=0

cleanup() {
	[ -n "${DAEMON:-}" ] && kill "$DAEMON" 2>/dev/null
	[ -n "${PROVIDER:-}" ] && kill "$PROVIDER" 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT

pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; failures=$((failures + 1)); }

# expect NAME EXPECTED ACTUAL: the same text
expect() {
	if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: wanted [$2], got [$3]"; fi
}
# contains NAME TEXT HAYSTACK
contains() {
	case "$3" in *"$2"*) pass "$1" ;; *) fail "$1: no [$2] in [$3]" ;; esac
}
# The last request the provider got, as JSON.
last_request() { tail -n 1 "$TMP/requests"; }
field() { python3 -c "import json,sys; r=json.loads(sys.stdin.read()); print($1)"; }

python3 "$MOCK" "$TMP/requests" > "$TMP/port" &
PROVIDER=$!
for _ in $(seq 50); do grep -q PORT "$TMP/port" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/PORT //p' "$TMP/port")
URL="http://127.0.0.1:$PORT/v1"

cat > "$CONFIG" <<EOF
[augur]
default-profile = main

[profile main]
provider = openai
url = $URL
api-key-command = echo sk-test
model = model-main
tier.fast = model-fast

[profile plain]
provider = openai-compatible
url = $URL
model = model-plain

[profile claude]
provider = anthropic
url = $URL
api-key-command = echo sk-ant-test
model = claude-test

[profile wrongkey]
provider = openai
url = $URL
api-key-command = echo sk-nope
model = model-main

[profile broken]
provider = anthropic
model = claude-test

[profile cf]
provider = cloudflare
account = acc123
gateway = gw
model = workers-ai/@cf/meta/llama

[app test.app]
model = app-model
EOF

start_daemon() {
	"$AUGURD" --no-exit &
	DAEMON=$!
	for _ in $(seq 50); do "$AUGUR" status > /dev/null 2>&1 && return; sleep 0.1; done
	"$AUGUR" status > /dev/null 2>&1 || true
}
start_daemon

# ---- On, and the profiles -------------------------------------------------
expect "status is on" "on" "$("$AUGUR" status | head -n 1)"
profiles=$("$AUGUR" profiles)
contains "profiles: main has its tier" "main (openai)  tiers: fast" "$profiles"
contains "profiles: a broken one says why" "broken (anthropic)  -- can't be used: no api-key-command or api-key-env" "$profiles"
contains "profiles: cloudflare from its account" "cf (cloudflare)" "$profiles"
case "$profiles" in *"cf (cloudflare)  --"*) fail "cloudflare profile usable" ;; *) pass "cloudflare profile usable" ;; esac

# ---- Text ----------------------------------------------------------------
expect "streamed answer" "Hello there!" "$("$AUGUR" ask hi)"
expect "the key from its command" "Bearer sk-test" "$(last_request | field 'r["authorization"]')"
expect "default profile's model" "model-main" "$(last_request | field 'r["body"]["model"]')"
info=$("$AUGUR" ask -v hi 2>&1 >/dev/null)
contains "tokens counted" "11 in, 3 out" "$info"
expect "whole answer, not streamed" "Hello there!" "$("$AUGUR" ask --no-stream hi)"
expect "prompt from stdin" "Hello there!" "$(echo hi | "$AUGUR" ask)"
"$AUGUR" ask -s "Be brief." hi > /dev/null
expect "system prompt sent" "Be brief." "$(last_request | field 'r["body"]["messages"][0]["content"]')"

# ---- Which model ---------------------------------------------------------
"$AUGUR" ask -t fast hi > /dev/null
expect "a tier's model" "model-fast" "$(last_request | field 'r["body"]["model"]')"
"$AUGUR" ask --app-id test.app hi > /dev/null
expect "the app's model" "app-model" "$(last_request | field 'r["body"]["model"]')"
"$AUGUR" ask --app-id test.app -m asked-model hi > /dev/null
expect "the asked model beats the app's" "asked-model" "$(last_request | field 'r["body"]["model"]')"
out=$("$AUGUR" ask -t nope hi 2>&1)
contains "an unknown tier" "NoModel: profile \"main\" has no tier \"nope\"" "$out"
out=$("$AUGUR" ask -p nowhere hi 2>&1)
contains "an unknown profile" "NoProfile" "$out"
out=$("$AUGUR" ask -p broken hi 2>&1)
contains "a broken profile" "can't be used" "$out"

# ---- Structured answers ---------------------------------------------------
SCHEMA='{"type":"object","properties":{"categories":{"type":"array","items":{"enum":["bill","newsletter","sports"]}}},"required":["categories"],"additionalProperties":false}'
expect "native schema" '{"categories":["bill"]}' "$("$AUGUR" ask --schema "$SCHEMA" categorise)"
expect "schema went as response_format" "json_schema" "$(last_request | field 'r["body"]["response_format"]["type"]')"
out=$("$AUGUR" ask -v --schema "$SCHEMA" retry 2>&1)
contains "a wrong answer is asked again" '{"categories":["bill"]}' "$out"
contains "... and it says so" "asked twice" "$out"
contains "the second ask says what was wrong" "doesn't match the JSON Schema" "$(last_request)"
expect "schema in the prompt, fence taken off" '{"categories":["newsletter"]}' "$("$AUGUR" ask -p plain --schema "$SCHEMA" categorise)"
contains "the prompt has the schema" "JSON Schema" "$(last_request | field 'r["body"]["messages"][0]["content"]')"

# ---- Anthropic ------------------------------------------------------------
expect "anthropic text" "Hi from Claude" "$("$AUGUR" ask -p claude hi)"
expect "anthropic's key header" "sk-ant-test" "$(last_request | field 'r["x-api-key"]')"
expect "anthropic's max_tokens given" "4096" "$(last_request | field 'r["body"]["max_tokens"]')"
expect "anthropic schema as a tool" '{"categories":["sports"]}' "$("$AUGUR" ask -p claude --schema "$SCHEMA" categorise)"
expect "the tool is required" "answer" "$(last_request | field 'r["body"]["tool_choice"]["name"]')"
expect "a non-object schema, wrapped and unwrapped" "42" "$("$AUGUR" ask -p claude --schema '{"type":"integer"}' number)"

# ---- Tools ------------------------------------------------------------------
# The weather tool says what it was given, and notes that it ran.
cat > "$TMP/tools.json" <<EOF
[
  {"name": "weather", "description": "The weather in a city",
   "schema": {"type": "object", "properties": {"city": {"type": "string"}},
              "required": ["city"], "additionalProperties": false},
   "command": "tee -a $TMP/ran"},
  {"name": "broken", "schema": {"type": "object"},
   "command": "echo it broke >&2; exit 3"},
  {"name": "slowtool", "schema": {"type": "object"}, "command": "sleep 5"}
]
EOF
T="--tools $TMP/tools.json"
expect "a tool's result goes back to the model" 'The tool said: {"city":"Paris"}' "$("$AUGUR" ask $T --no-stream weather)"
expect "the tools went to the provider" "weather" "$(last_request | field 'r["body"]["tools"][0]["function"]["name"]')"
expect "the result answers its call" "call_0_0" "$(last_request | field 'r["body"]["messages"][-1]["tool_call_id"]')"
expect "the call is in the history" "weather" "$(last_request | field 'r["body"]["messages"][-2]["tool_calls"][0]["function"]["name"]')"
expect "streamed: every turn's text" 'Let me look. The tool said: {"city":"Paris"}' "$("$AUGUR" ask $T weather)"
info=$("$AUGUR" ask -v $T --no-stream weather 2>&1 >/dev/null)
contains "-v counts the calls" "1 tool call in 2 rounds" "$info"
expect "two calls at once, answered in order" 'The tool said: {"city":"Paris"} | {"city":"Rome"}' "$("$AUGUR" ask $T --no-stream twice weather)"
rm -f "$TMP/ran"
out=$("$AUGUR" ask $T --no-stream badargs weather)
contains "arguments that don't fit go back to the model" "Error: the arguments don't match" "$out"
expect "... and the tool isn't run for them" "" "$(cat "$TMP/ran" 2>/dev/null)"
expect "a tool that fails" "The tool said: Error: it broke" "$("$AUGUR" ask $T --no-stream broken)"
out=$("$AUGUR" ask -v $T --max-rounds 3 --no-stream loop weather 2>&1)
contains "out of rounds, it answers without tools" "No more tools" "$out"
contains "... after max-rounds" "2 tool calls in 3 rounds" "$out"
expect "... told not to call any" "none" "$(last_request | field 'r["body"]["tool_choice"]')"

expect "anthropic tools" 'The tool said: {"city":"Paris"}' "$("$AUGUR" ask -p claude $T --no-stream weather)"
expect "anthropic: the results go as a user message" "toolu_0_0" "$(last_request | field 'r["body"]["messages"][-1]["content"][0]["tool_use_id"]')"
expect "anthropic: the call's input is JSON" "Paris" "$(last_request | field 'r["body"]["messages"][-2]["content"][1]["input"]["city"]')"
expect "anthropic: a failed tool says so" "The tool said: Error: it broke" "$("$AUGUR" ask -p claude $T --no-stream broken)"
expect "anthropic: is_error" "True" "$(last_request | field 'r["body"]["messages"][-1]["content"][0]["is_error"]')"
expect "anthropic: tools and a schema" '{"categories":["sports"]}' "$("$AUGUR" ask -p claude $T --schema "$SCHEMA" weather)"
expect "... any tool, the answer one included" "any" "$(head -n -1 "$TMP/requests" | tail -n 1 | field 'r["body"]["tool_choice"]["type"]')"

echo '[{"name": "answer", "command": "true"}]' > "$TMP/answer.json"
out=$("$AUGUR" ask --tools "$TMP/answer.json" hi 2>&1)
contains "the answer tool is Augur's" "can't be called \"answer\"" "$out"
echo '[{"name": "t", "schema": {"type": "string"}, "command": "true"}]' > "$TMP/string.json"
out=$("$AUGUR" ask --tools "$TMP/string.json" hi 2>&1)
contains "a tool's schema is for an object" "isn't a JSON Schema for an object" "$out"
out=$(gdbus call --session --dest io.github.matthewp.Augur --object-path /io/github/matthewp/Augur \
	--method io.github.matthewp.Augur1.Ask \
	"{'app-id': <'t'>, 'messages': <[{'role': <'user'>, 'content': <'hi'>}]>, 'tools': <[{'name': <'t'>}]>}" 2>&1)
contains "tools need Complete" "tools need Complete" "$out"
"$AUGUR" ask $T slowtool > /dev/null 2>&1 &
ASKER=$!
sleep 1
kill -INT "$ASKER"
wait "$ASKER"
sleep 0.5
contains "cancelling while a tool runs" '"tools":["slowtool"],"result":"Cancelled"' "$(tail -n 1 "$LOG")"
contains "the log names the tools" '"rounds":2,"tools":["weather"]' "$(cat "$LOG")"

# ---- Errors -----------------------------------------------------------------
out=$("$AUGUR" ask rate 2>&1); status=$?
contains "rate limited" "RateLimited: Slow down" "$out"
expect "... and fails" "1" "$status"
out=$("$AUGUR" ask -p wrongkey hi 2>&1)
contains "a refused key" "Auth: Incorrect API key" "$out"
out=$("$AUGUR" ask --schema '{"type":' hi 2>&1)
contains "a bad schema" "isn't a JSON Schema" "$out"

# ---- Models -------------------------------------------------------------------
expect "models listed" "model-a model-b" "$("$AUGUR" models main | tr '\n' ' ' | sed 's/ $//')"

# ---- Cancelling ------------------------------------------------------------------
"$AUGUR" ask slow > "$TMP/slow1" 2>&1 &
ASKER=$!
sleep 1
kill -INT "$ASKER"
wait "$ASKER"
sleep 0.5
contains "Ctrl+C cancels" '"result":"Cancelled"' "$(tail -n 1 "$LOG")"
"$AUGUR" ask slow > /dev/null 2>&1 &
ASKER=$!
sleep 1
kill -KILL "$ASKER"
wait "$ASKER" 2>/dev/null
sleep 0.5
contains "a program leaving cancels its requests" '"result":"Cancelled"' "$(tail -n 1 "$LOG")"

# ---- The log ----------------------------------------------------------------------
contains "the log has the app and model" '"app":"test.app","profile":"main","model":"app-model"' "$(cat "$LOG")"
case "$(cat "$LOG")" in *"Hello"*|*"categorise"*|*"Paris"*) fail "the log has no text" ;; *) pass "the log has no text" ;; esac

# ---- Turning it off -----------------------------------------------------------------
sed -i 's/^default-profile = main/default-profile = main\nenabled = false/' "$CONFIG"
sleep 1
expect "the config turns it off" "off" "$("$AUGUR" status | head -n 1)"
out=$("$AUGUR" ask hi 2>&1)
contains "asking while off" "Disabled" "$out"
sed -i '/^enabled = false/d' "$CONFIG"
sleep 1
expect "and on again" "on" "$("$AUGUR" status | head -n 1)"

kill "$DAEMON"; wait "$DAEMON" 2>/dev/null
AUGUR_DISABLED=1 "$AUGURD" --no-exit &
DAEMON=$!
for _ in $(seq 50); do "$AUGUR" status > /dev/null 2>&1; [ $? -le 1 ] && "$AUGUR" status 2>/dev/null | grep -q . && break; sleep 0.1; done
status_out=$("$AUGUR" status)
expect "AUGUR_DISABLED turns it off" "off" "$(echo "$status_out" | head -n 1)"
contains "... and says why" "AUGUR_DISABLED" "$status_out"

if [ "$failures" -eq 0 ]; then
	echo "service: all passed"
	exit 0
fi
echo "service: $failures failed"
exit 1
