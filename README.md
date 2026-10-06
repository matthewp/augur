# Augur

Augur is a session service that gives desktop applications AI without each
of them talking to AI providers. An application asks Augur over D-Bus; Augur
knows the providers (OpenAI, Anthropic, OpenRouter, Cloudflare AI Gateway,
Ollama...), the keys, which model to use, and how to get a well-formed
answer out of each of them.

## Building and trying it

It needs GLib 2.74 or later, json-glib and libsoup 3
(`pacman -S glib2 json-glib libsoup3`), and meson. The tests also need
python3 and `dbus-run-session` (from dbus); without them the service test
isn't run.

    meson setup build
    ninja -C build
    meson test -C build          # against a pretend provider: no keys, no cost
    sudo ninja -C build install  # augurd, augur, augur(1), and the D-Bus
                                 # and systemd user service files

Where there's systemd, D-Bus starts augurd through it, as the user
service `augurd.service` (`journalctl --user -u augurd` has what it
said); elsewhere D-Bus starts it itself. The unit goes where systemd's
pkg-config says, or `-Dsystemduserunitdir=DIR`; `no` leaves it out.

The session bus reads `.service` files when it starts, so after the first
install either log in again or tell it to look (else `augur` says "The name
is not activatable"):

    busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig

Debian 12 and 13 and Ubuntu 24.04 (amd64 and arm64) have `.deb`s on each
[release](https://github.com/matthewp/augur/releases). To build one
yourself, on the system it's for, from the top of the source (it installs
what it needs to build with apt, so as root, or in a container):

    build-aux/build-deb.sh OUTDIR

Then write `~/.config/augur/config` (below) and ask it something:

    augur status                 # on or off, and why
    augur profiles               # the profiles, and what's wrong with any
    augur models                 # what the default profile's provider lists
    augur ask "Say hello"        # the answer, as it's written
    augur ask -p anthropic -t smart -s "Be brief." "Why is the sky blue?"
    augur ask --schema '{"type":"object","properties":{"mood":{"enum":["happy","sad"]}},"required":["mood"]}' "I lost my keys"
    echo "a prompt from stdin" | augur ask -v    # -v: the model and tokens
    augur ask --tools tools.json "What's the weather in Paris?"
    augur usage                  # this month's requests, tokens and cost, by app
    augur usage --since week --by model,day
    augur classify -q questions.json "Your invoice is attached"

A tools file is a JSON array of `{name, description, schema, command}`: a
call's arguments go to the command on stdin, what it prints is the result,
and if it fails, what it printed to stderr is why.

## Names

| What            | Name                                |
|-----------------|-------------------------------------|
| Bus name        | `io.github.matthewp.Augur`          |
| Object          | `/io/github/matthewp/Augur`         |
| Interface       | `io.github.matthewp.Augur1`         |
| Program         | `augurd`                            |
| Command line    | `augur` (ask from the shell, list profiles, check config) |
| Config          | `~/.config/augur/config`            |

The interface carries a version (`Augur1`); an incompatible change is
`Augur2`, served alongside.

`augurd` is started by D-Bus when first asked (a `.service` file in
`/usr/share/dbus-1/services`) and exits after a minute with nothing to do.

## Configuration

```ini
[augur]
enabled = true
default-profile = cloud
classify-profile = openrouter       # Classify's, unless the app's says

[profile cloud]
provider = cloudflare
account = 0123abcd...
gateway = my-gateway
api-key-command = pass show cloudflare/ai
model = deepseek-v4-flash          # when nothing more particular is asked
tier.cheap = deepseek-v4-flash
tier.cheaper = llama-4-scout
tier.smart = claude-sonnet-5-5

[profile anthropic]
provider = anthropic
api-key-command = secret-tool lookup service anthropic
model = claude-haiku-4-5-20251001
tier.smart = claude-opus-5-5

[profile local]
provider = ollama
model = qwen3:8b

[profile openrouter]
provider = openrouter
api-key-command = pass show openrouter
model = deepseek/deepseek-v4-flash
classifier = typesafe/jev-1.13

# Per program, by app ID: which profile, and what it uses.
[app org.example.Mail]
profile = cloud
model = deepseek-v4-flash
classify-profile = openrouter
```

A profile's settings:

| Key | Meaning |
|-----|---------|
| `provider` | `openai`, `anthropic`, `openrouter`, `cloudflare`, `ollama`, `openai-compatible`, or `typesafe` (a classifier, and no chat model) |
| `url` | the API's base (with its `/v1`), for `openai-compatible`, or to point any provider elsewhere |
| `account`, `gateway` | Cloudflare: the account ID and gateway (`default` if not given); models are named `provider/model` there, e.g. `workers-ai/@cf/meta/llama-3.1-8b-instruct` |
| `api-key-command` | prints the key (its first line is used) |
| `api-key-env` | or: the variable holding it |
| `gateway-key-command` | Cloudflare: prints the token for an authenticated gateway |
| `model` | the model when nothing more particular is asked |
| `classifier` | a classifier model, which `Classify` asks over Typesafe's System One API at `{url}/systemone`: Typesafe's own (`jev-1.13.0`) and OpenRouter's (`typesafe/jev-1.13`) |
| `tier.NAME` | a model for the tier NAME |
| `structured-output` | `native` (the provider's own) or `prompt` (the schema in the prompt); the default suits the provider |
| `max-concurrent` | requests to it at once (4) |
| `stream-usage` | `true` to ask for token counts in a stream (`stream_options`); it's on for OpenAI, OpenRouter and Anthropic, and off for the rest, which may not take it. Without token counts, a request can't be priced |
| `price.MODEL` | what MODEL costs, in US dollars per million tokens: input and output, and cached input if it's cheaper (e.g. `3 15 0.3`) |

A `#` after a value, with a space before it, starts a comment.

Providers are a small set of adapters. Most providers and gateways speak
OpenAI's chat completions (`openai`, `openrouter`, `cloudflare`, `ollama`,
and `openai-compatible` with a `url` for anything else); Anthropic gets its
own adapter for its Messages API. A provider adapter turns a request into
the provider's HTTP, and its replies (whole or streamed) back.

**What it costs.** Augur counts every request's tokens, and its cost in
dollars: what the provider says it charged where it says (OpenRouter
does), else the tokens at the profile's `price.` for the model. A model
with neither is counted as unpriced, never guessed at. Prices change, so
Augur has none of its own: they're in your config, from the provider's
price list.

**Tiers** are names a profile gives to models, as many as you like (`cheap`,
`cheaper`, `smart`, `vision`...). They're shortcuts, not a ladder every
model has to fit on.

**Which model** answers a request, first match wins:

1. the model the request names
2. the tier the request names, looked up in the profile
3. the `[app ...]` section's model, or its tier
4. the profile's `model`

and the profile is the request's, else the app section's, else
`default-profile`. A program can therefore offer its own setting ("model
for categories") and it beats Augur's config; leaving it empty defers to
Augur's.

## Turning it off

Programs read the `Enabled` property and show their AI features only when
it's true. It's false when:

- `[augur] enabled = false`, or
- `AUGUR_DISABLED=1` is in Augur's environment, or
- no profile can be used (no config, or no key).

While off, every method fails with `io.github.matthewp.Augur1.Error.Disabled`.

## The D-Bus interface

```
interface io.github.matthewp.Augur1

  property Enabled: b                       (changes are signalled)

  # Start a request; the answer comes on the returned object.
  method Complete(request: a{sv}) -> (handle: o)

  # The same, waiting for the whole answer: the simple case.
  method Ask(request: a{sv}) -> (text: s, info: a{sv})

  # Questions about an input, answered: see "Classify" below.
  method Classify(request: a{sv}) -> (answers: a{sv}, info: a{sv})

  method Status() -> (status: a{sv})       # enabled, config, default-profile,
                                           # problem (why it's off), running
  method ListProfiles() -> (profiles: a(sssas))
                                    # name, provider, problem ("" if usable), tiers
  method ListModels(profile: s) -> (models: as)
                                    # what the provider lists ("": the default profile)
  method Usage(query: a{sv}) -> (rows: aa{sv})
                                    # the log, added up: see below

interface io.github.matthewp.Augur1.Request     (on each handle)

  signal Delta(text: s)                     # streamed text, as it comes
  signal Done(text: s, info: a{sv})         # the whole answer
  signal Failed(error: s, message: s)       # error: a D-Bus error name
  method Cancel()

  signal ToolCall(id: s, name: s, arguments: s)  # arguments: JSON matching
                                                 # the tool's schema
  method ToolDone(id: s, result: s)         # what the tool gives back
  method ToolFailed(id: s, message: s)      # the model is told it failed, and why
```

A request (`a{sv}`):

| Key        | Type      | Meaning |
|------------|-----------|---------|
| `app-id`   | s         | who's asking, for the `[app]` section and the log (required) |
| `messages` | aa{sv}    | `role` (`system`, `user`, `assistant`) and `content` (s) |
| `profile`  | s         | optional |
| `model`    | s         | optional |
| `tier`     | s         | optional |
| `schema`   | s         | a JSON Schema: the answer is JSON matching it |
| `stream`   | b         | send `Delta`s (default true for `Complete`) |
| `max-tokens` | u       | optional |
| `temperature` | d      | optional |
| `tools`    | aa{sv}    | tools the model may call (`Complete` only): `name` (s: letters, digits, `_` and `-`), `description` (s), `schema` (s: a JSON Schema for its arguments, an object; `{"type":"object"}` if not given) |
| `max-rounds` | u       | turns the model gets before it must answer without tools (8) |

`info` in `Done` and `Ask`: `profile` (s), `model` (s), `attempts` (i: 2
if a structured answer was asked for again), `rounds` (i: times the
model was asked), `tool-calls` (i); `input-tokens`, `output-tokens` and
`cached-tokens` (x: of the input, read from the provider's cache) over
every round, when the provider said; and `cost` (d, US dollars) when
it's known.

`Usage` adds up the log. Its query: `since` and `until` (x, seconds
since 1970; until isn't included), `app-id` (s), and `by` (as: any of
`app`, `profile`, `model`, `day`). Each row has what it's by, and
`requests`, `input-tokens`, `output-tokens`, `cached-tokens` (x), `cost`
(d: what's known) and `unpriced` (x: requests whose cost isn't).

Signals for a request go to the program that made it alone (D-Bus unicast
signals), never to the bus at large. Subscribe to them (sender
`io.github.matthewp.Augur`, interface `io.github.matthewp.Augur1.Request`)
before calling `Complete`, and keep the ones for the handle it returns: a
short answer can be done before the reply's been read. `augur ask` does
this; see `src/augur.c`. A handle is removed once its `Done` or
`Failed` has gone, or when the program that made it leaves the bus (which
cancels it).

With a `schema`, `Delta`s still come (the JSON as it's written, for a
program that wants to show progress) but only `Done`'s text is checked
against the schema: if the answer doesn't match, Augur asks again once,
saying what was wrong, then fails with `Error.Schema`.

### Tools

A program offers tools; Augur never runs anything itself. When the
model's turn ends in calls:

1. Each call's arguments are checked against its tool's schema. A call
   that doesn't match, or names no tool, is answered by Augur, saying
   what was wrong, and never reaches the program.
2. The rest come as `ToolCall` signals, in order, and the request gives up
   its place with the provider while it waits.
3. The program answers each with `ToolDone` or `ToolFailed`, in any
   order. There's no time limit: a tool may be waiting on the user ("send
   this?"). `Cancel` or leaving the bus ends it.
4. With every call answered, the request is next in its profile's queue,
   and the model gets the results in the order it made the calls.

`Delta`s carry the text of every turn ("Let me check your calendar.");
a `ToolCall` marks where a turn ends. `Done`'s text is the last turn's:
the answer. On the last of `max-rounds` turns the model is told to
answer without tools, with what it has.

With a `schema` too, the answer is still checked. On Anthropic, whose
structured answers come as a tool called `answer`, that tool joins the
program's and the model must call one of them each turn; `answer` can't
be the name of a program's tool.

Tools need a handle to call back on: `Ask` with `tools` is
`InvalidArgs`. The log has the tools called, by name, never their
arguments or results.

### Classify

Questions about an input, answered from fixed answers with how likely
each is. A classifier answers where the profile has one (`classifier`);
else the profile's chat model does, the questions made a JSON Schema and
asked as a structured answer. The program asks the same way either way.
`design/classify.md` has why, and Jev.

The profile is the request's, else the `[app]` section's
`classify-profile`, else `[augur] classify-profile`, else the default
profile. The model is the request's, else the profile's `classifier`,
else its chat model, as for `Complete`.

The request:

| Key | Type | Meaning |
|-----|------|---------|
| `app-id` | s | who's asking (required) |
| `input` | s | what the questions are about (required) |
| `questions` | a{sv} | by name (letters, digits, `_`, `-`), each an `a{sv}` (below; at least one) |
| `profile`, `model` | s | optional |
| `interactive` | b | someone's waiting for it: ahead of background work in the queue (default false) |

A question:

| Key | Type | Meaning |
|-----|------|---------|
| `type` | s | `choice`, `yes-no` or `score` |
| `instructions` | s | the question (required) |
| `options` | a{ss} | `choice`: each option's name and what it means (2 to 255) |
| `yes`, `no` | s | `yes-no`, optional: what counts as each |
| `levels` | as | `score`: each level described, lowest first (at least 2) |

The answers, by the question's name, each an `a{sv}`:

| Type | Keys |
|------|------|
| `choice` | `choice` (s); from a classifier also `confidence` (d) and `probabilities` (a{sd}: every option's) |
| `yes-no` | `probability` (d): that it's yes; 0 or 1 from a chat model |
| `score` | `score` (d: 0 to the levels less one; a chat model's a whole level); from a classifier also `confidence` (d) and `probabilities` (ad: each level's) |

`info`: `profile`, `model`, `calibrated` (b: a classifier's
probabilities, which mean the same from one question to the next),
the tokens and `cost`, as for `Complete`. A chat model's probabilities
aren't made up: it has none to give.

The log has the questions' names, never the input or the answers.

Errors, each `io.github.matthewp.Augur1.Error.` and: `Disabled`,
`NoProfile`, `NoModel`, `Auth` (the key was refused or couldn't be got),
`RateLimited`, `Provider` (anything else the provider said, with its
message), `Schema`, `Cancelled`. A malformed request is
`org.freedesktop.DBus.Error.InvalidArgs`.

## Inside augurd

- C, GLib/GIO for D-Bus and the main loop, libsoup 3 for HTTP, json-glib
  for JSON. Server-sent events (the streaming
  format both OpenAI's and Anthropic's APIs use) parsed as they arrive.
- A queue per profile, so a burst from one program (a mail program
  categorising a new folder) doesn't hold up an answer someone is waiting
  to read: requests without a schema go ahead of ones with one, and at
  most `max-concurrent` go to a provider at once.
- The config is read again when it changes; `Enabled` follows, signalled.
- Keys are fetched by running the command when first needed and kept in
  memory, never written anywhere.
- A log of what was asked of whom (app, profile, model, tokens, cost;
  not the text) in `~/.local/state/augur/log`, a line of JSON each, for
  "what's this costing me": `augur usage` and the `Usage` method add it
  up.

## Open questions

- **Images and files** in `content`: later, as parts (`a{sv}` with a
  `type`), when something needs them.
- **Tool use in the history.** A program continuing a conversation
  sends back text alone, so the model sees its earlier answers but not
  the calls behind them. `Done` could give the round's messages in
  Augur's own form, to be sent back as they are.
- **Tools by prompting** for models without tool use of their own, as the
  schema is for structured answers: the answer either a call or the
  final one. For now such a provider's error is `Error.Provider`.
- **Spending limits** per app or per day: worth having before anything runs
  unattended for long, more so with tools, where one request is several.
  The costs are counted now; a limit would refuse requests past it with
  an error of its own.
- **Model lists**: providers that list models make `ListModels` easy; for
  the rest, it's what the profile names.
