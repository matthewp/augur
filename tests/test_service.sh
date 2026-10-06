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
price.model-main = 1 2

[profile plain]
provider = openai-compatible
url = $URL
model = model-plain

[profile claude]
provider = anthropic
url = $URL
api-key-command = echo sk-ant-test
model = claude-test
price.claude-test = 3 15 0.3

[profile router]
provider = openrouter
url = $URL
api-key-command = echo sk-test
model = router-model
classifier = typesafe/jev-1.13

[profile ts]
provider = typesafe
url = $URL
api-key-command = echo sk-test
classifier = jev-1.13.0
price.jev-1.13.0 = 0.042 0

[app classify.app]
classify-profile = ts

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
out=$(gdbus call --session --dest org.gemwm.Augur --object-path /org/gemwm/Augur \
	--method org.gemwm.Augur1.Ask \
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

# ---- Usage and spend ----------------------------------------------------------
U="--app-id usage.test"
out=$("$AUGUR" ask -v $U hi 2>&1 >/dev/null)
contains "a price from the config" '11 in, 3 out, $0.000017]' "$out"
"$AUGUR" ask $U hi > /dev/null
out=$("$AUGUR" ask -v $U -p router hi 2>&1 >/dev/null)
contains "the cost OpenRouter says" '$0.0005]' "$out"
out=$("$AUGUR" ask -v $U -m unpriced-model hi 2>&1 >/dev/null)
case "$out" in *'$'*) fail "no price, no cost: $out" ;; *) pass "no price, no cost" ;; esac
out=$("$AUGUR" ask -v $U -p claude hi 2>&1 >/dev/null)
contains "anthropic: cache reads count as input, at their own price" '25 in, 5 out, $0.000139]' "$out"
contains "the log has the cost" '"cached-tokens":4,"cost":0.0001392' "$(tail -n 1 "$LOG")"
usage=$("$AUGUR" usage --app usage.test --by model --since today | tr -s ' ')
contains "usage by model" 'model-main 2 22 6 0 $0.000034' "$usage"
contains "... OpenRouter's" 'router-model 1 11 3 0 $0.0005' "$usage"
contains "... unpriced said so" '$0.00 + 1 unpriced' "$usage"
"$AUGUR" ask -p plain hi > /dev/null
expect "no token counts asked of a compatible server" "False" "$(last_request | field '"stream_options" in r["body"]')"
printf '\n[profile counted]\nprovider = openai-compatible\nurl = %s\nmodel = m\nstream-usage = true\nprice.m = 1 2\n' "$URL" >> "$CONFIG"
sleep 1
out=$("$AUGUR" ask -v -p counted hi 2>&1 >/dev/null)
contains "... unless the profile says it takes them" '11 in, 3 out, $0.000017]' "$out"
contains "... a total" 'total 5 69 17 4 $0.000673 + 1 unpriced' "$usage"
expect "usage for no one" "nothing asked" "$("$AUGUR" usage --app no.one)"
expect "usage before it all" "nothing asked" "$("$AUGUR" usage --until 2001-01-01 --since all)"
out=$("$AUGUR" usage --by colour 2>&1)
contains "usage by something unknown" "not \"colour\"" "$out"
sed -i 's/^price.model-main = 1 2$/price.model-main = 1 two/' "$CONFIG"
sleep 1
contains "a bad price says so" "price.model-main isn't two or three numbers" "$("$AUGUR" profiles)"
sed -i 's/^price.model-main = 1 two$/price.model-main = 1 2/' "$CONFIG"
sleep 1

# ---- Classify -------------------------------------------------------------------
cat > "$TMP/questions.json" <<'EOF'
{
  "team": {"type": "choice", "instructions": "Which team should handle this?",
           "options": {"returns": "Exchanges, wrong or damaged items",
                       "billing": "Charges, invoices, payment problems"}},
  "bill": {"type": "yes-no", "instructions": "Is it a bill?",
           "yes": "Asks you to pay something", "no": "Anything else"},
  "severity": {"type": "score", "instructions": "How bad is it?",
               "levels": ["Cosmetic", "Degraded", "Blocking"]},
  "letter": {"type": "yes-no", "instructions": "Is it a newsletter?",
             "yes": "Sent to many readers"}
}
EOF
Q="--questions $TMP/questions.json"
out=$("$AUGUR" classify $Q -p ts "Your bill is due")
expect "a classifier's answers" "team: returns (0.70)
bill: 0.93
severity: 1.43 (confidence 0.35)
letter: 0.93" "$out"
expect "only yes said: no is the rest" "Sent to many readers / Anything else." "$(last_request | field 'r["body"]["questions"]["letter"]["criteria"]["true"] + " / " + r["body"]["questions"]["letter"]["criteria"]["false"]')"
expect "... asked of System One" "/v1/systemone" "$(last_request | field 'r["path"]')"
expect "... with its classifier" "jev-1.13.0" "$(last_request | field 'r["body"]["model"]')"
expect "... about the input" "Your bill is due" "$(last_request | field 'r["body"]["state"]')"
expect "yes-no is Jev's noul" "noul Asks you to pay something" "$(last_request | field 'r["body"]["questions"]["bill"]["type"] + " " + r["body"]["questions"]["bill"]["criteria"]["true"]')"
expect "a choice's options are its criteria" "Charges, invoices, payment problems" "$(last_request | field 'r["body"]["questions"]["team"]["criteria"]["billing"]')"
expect "a score's levels are its criteria" "Blocking" "$(last_request | field 'r["body"]["questions"]["severity"]["criteria"][2]')"
json=$("$AUGUR" classify $Q -p ts --json "Your bill is due")
expect "every option's probability" "0.3" "$(echo "$json" | field 'round(r["team"]["probabilities"]["billing"], 2)')"
expect "every level's" "[0.0, 0.57, 0.43]" "$(echo "$json" | field 'r["severity"]["probabilities"]')"
out=$("$AUGUR" classify -v $Q -p ts "hello" 2>&1 >/dev/null)
contains "-v: a classifier answered" "[ts, jev-1.13.0: a classifier, 40 in, \$0.000002]" "$out"
out=$("$AUGUR" classify -v $Q -p router "hello" 2>&1)
contains "OpenRouter's classifier" "[router, typesafe/jev-1.13: a classifier, 40 in, \$0.00002]" "$out"
contains "... no bill" "bill: 0.04" "$out"

out=$("$AUGUR" classify -v $Q "Your bill is due" 2>&1)
contains "no classifier: the chat model" "[main, model-main: a chat model, uncalibrated, 11 in" "$out"
contains "... a choice without probabilities" "team: returns
" "$out"
contains "... yes or no as 1 or 0" "bill: 1.00" "$out"
contains "... a score a whole level" "severity: 2" "$out"
expect "... asked with the questions as a schema" "['returns', 'billing']" "$(last_request | field 'r["body"]["response_format"]["json_schema"]["schema"]["properties"]["team"]["enum"]')"
contains "... and in the prompt" "- billing: Charges, invoices, payment problems" "$(last_request | field 'r["body"]["messages"][0]["content"]')"
expect "... the input as the user's" "Your bill is due" "$(last_request | field 'r["body"]["messages"][1]["content"]')"
out=$("$AUGUR" classify $Q -p claude "Your bill is due")
contains "anthropic: no classifier, the answer tool" "bill: 1.00" "$out"
expect "... the questions as its schema" "boolean" "$(last_request | field '[t for t in r["body"]["tools"] if t["name"] == "answer"][0]["input_schema"]["properties"]["bill"]["type"]')"
out=$("$AUGUR" classify $Q --app-id classify.app "hi")
contains "an app's classify-profile" "team: returns (0.70)" "$out"

out=$("$AUGUR" classify $Q -p ts "overloaded" 2>&1)
contains "an overloaded classifier" "RateLimited" "$out"
echo '{"x": {"type": "colour", "instructions": "?"}}' > "$TMP/badq.json"
out=$("$AUGUR" classify -q "$TMP/badq.json" hi 2>&1)
contains "a question of no type" "type is choice, yes-no or score" "$out"
echo '{"x": {"type": "choice", "instructions": "?", "options": {"a": "A"}}}' > "$TMP/badq.json"
out=$("$AUGUR" classify -q "$TMP/badq.json" hi 2>&1)
contains "a choice of one" "needs options" "$out"
echo '{}' > "$TMP/badq.json"
out=$("$AUGUR" classify -q "$TMP/badq.json" hi 2>&1)
contains "no questions" "there are no questions" "$out"
out=$("$AUGUR" ask -p ts hi 2>&1)
contains "a classifier's profile has no chat model" "NoModel" "$out"
contains "the log names the questions" '"questions":["team","bill","severity","letter"]' "$(cat "$LOG")"
case "$(cat "$LOG")" in *"Your bill"*) fail "the log has no input" ;; *) pass "the log has no input" ;; esac

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
