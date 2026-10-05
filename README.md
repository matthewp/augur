# Augur

Augur is a session service that gives desktop applications AI without each
of them talking to AI providers. An application asks Augur over D-Bus; Augur
knows the providers (OpenAI, Anthropic, OpenRouter, Cloudflare AI Gateway,
Ollama...), the keys, which model to use, and how to get a well-formed
answer out of each of them.

GemWM uses it, but nothing in it knows about GemWM: any program on the
session bus can use it.

## Building and trying it

It needs GLib 2.74 or later, json-glib and libsoup 3
(`pacman -S glib2 json-glib libsoup3`), and meson:

    meson setup build
    ninja -C build
    meson test -C build          # against a pretend provider: no keys, no cost
    sudo ninja -C build install  # augurd, augur, and the D-Bus .service file

The session bus reads `.service` files when it starts, so after the first
install either log in again or tell it to look (else `augur` says "The name
is not activatable"):

    busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig

Then write `~/.config/augur/config` (below) and ask it something:

    augur status                 # on or off, and why
    augur profiles               # the profiles, and what's wrong with any
    augur models                 # what the default profile's provider lists
    augur ask "Say hello"        # the answer, as it's written
    augur ask -p anthropic -t smart -s "Be brief." "Why is the sky blue?"
    augur ask --schema '{"type":"object","properties":{"mood":{"enum":["happy","sad"]}},"required":["mood"]}' "I lost my keys"
    echo "a prompt from stdin" | augur ask -v    # -v: the model and tokens

## Why a service

- **One place for providers and keys.** Configured once, not per program;
  keys come from a command or the keyring, never from each app's settings.
- **Programs don't speak provider protocols.** They send messages and get
  text back. Augur does HTTP, each provider's format, streaming, retries and
  rate limits.
- **Structured answers that hold.** A program can give a JSON Schema and get
  back JSON that matches it, or an error, whatever the provider supports:
  its own structured output where it has it, else tool use, else prompting
  and checking (and asking again).
- **One switch.** Programs ask whether AI is on and show or hide their AI
  features to match.

Non-goals: Augur doesn't decide what's private. A program that sends text
to Augur has decided to; asking the user is the program's job (GemMail's
categories are something you turn on, for example). There are no per-app
permission prompts: every program runs as you and could read Augur's
config itself, so a prompt would only look like protection.

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

# Per program, by app ID: which profile, and what it uses.
[app org.gemwm.GemMail]
profile = cloud
model = deepseek-v4-flash
```

A profile's settings:

| Key | Meaning |
|-----|---------|
| `provider` | `openai`, `anthropic`, `openrouter`, `cloudflare`, `ollama`, or `openai-compatible` |
| `url` | the API's base (with its `/v1`), for `openai-compatible`, or to point any provider elsewhere |
| `account`, `gateway` | Cloudflare: the account ID and gateway (`default` if not given); models are named `provider/model` there, e.g. `workers-ai/@cf/meta/llama-3.1-8b-instruct` |
| `api-key-command` | prints the key (its first line is used) |
| `api-key-env` | or: the variable holding it |
| `gateway-key-command` | Cloudflare: prints the token for an authenticated gateway |
| `model` | the model when nothing more particular is asked |
| `tier.NAME` | a model for the tier NAME |
| `structured-output` | `native` (the provider's own) or `prompt` (the schema in the prompt); the default suits the provider |
| `max-concurrent` | requests to it at once (4) |

A `#` after a value, with a space before it, starts a comment.

Providers are a small set of adapters. Most providers and gateways speak
OpenAI's chat completions (`openai`, `openrouter`, `cloudflare`, `ollama`,
and `openai-compatible` with a `url` for anything else); Anthropic gets its
own adapter for its Messages API. A provider adapter turns a request into
the provider's HTTP, and its replies (whole or streamed) back.

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

GemWM's own switch is `[ai] enabled = false` in its config: `gemwm-session`
then puts `AUGUR_DISABLED=1` in the session's activation environment (it
already runs `dbus-update-activation-environment`), so Augur reports itself
off. Outside GemWM, Augur's own config is all there is.

While off, every method fails with `io.github.matthewp.Augur1.Error.Disabled`.

## The D-Bus interface

```
interface io.github.matthewp.Augur1

  property Enabled: b                       (changes are signalled)

  # Start a request; the answer comes on the returned object.
  method Complete(request: a{sv}) -> (handle: o)

  # The same, waiting for the whole answer: the simple case.
  method Ask(request: a{sv}) -> (text: s, info: a{sv})

  method Status() -> (status: a{sv})       # enabled, config, default-profile,
                                           # problem (why it's off), running
  method ListProfiles() -> (profiles: a(sssas))
                                    # name, provider, problem ("" if usable), tiers
  method ListModels(profile: s) -> (models: as)
                                    # what the provider lists ("": the default profile)

interface io.github.matthewp.Augur1.Request     (on each handle)

  signal Delta(text: s)                     # streamed text, as it comes
  signal Done(text: s, info: a{sv})         # the whole answer
  signal Failed(error: s, message: s)       # error: a D-Bus error name
  method Cancel()
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

`info` in `Done` and `Ask`: `profile` (s), `model` (s), `attempts` (i: 2
if a structured answer was asked for again), and `input-tokens` and
`output-tokens` (x) when the provider said.

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

Errors, each `io.github.matthewp.Augur1.Error.` and: `Disabled`,
`NoProfile`, `NoModel`, `Auth` (the key was refused or couldn't be got),
`RateLimited`, `Provider` (anything else the provider said, with its
message), `Schema`, `Cancelled`. A malformed request is
`org.freedesktop.DBus.Error.InvalidArgs`.

## Inside augurd

- C, GLib/GIO for D-Bus and the main loop, libsoup 3 for HTTP, json-glib
  for JSON. Server-sent events (the streaming
  format both OpenAI's and Anthropic's APIs use) parsed as they arrive.
- A queue per profile, so a burst from one program (GemMail categorising a
  new folder) doesn't hold up an answer someone is waiting to read:
  requests without a schema go ahead of ones with one, and at most
  `max-concurrent` go to a provider at once.
- The config is read again when it changes; `Enabled` follows, signalled.
- Keys are fetched by running the command when first needed and kept in
  memory, never written anywhere.
- A log of what was asked of whom (app, profile, model, tokens; not the
  text) in `~/.local/state/augur/log`, for "what's this costing me".

## First user: GemMail's categories (next)

Mail can be in several categories, like "Newsletter", "Bill", "Sports".
Categories are GemMail's; Augur only answers. (Labels, should GemMail have
them, are what you put on mail yourself; categories are what it works
out.)

- A few come built in, each a name and a description; you can change them
  and add your own. The descriptions are the point: they're what the model
  goes by ("Bill: asks you to pay something, or says a payment was taken").
- It's off until you turn it on (it sends your mail to the provider).
- New mail is categorised in the background: the sender, subject, and the
  first couple of thousand characters of the text, never attachments.
  Several messages go in one request.
- The request's schema allows only the category names, as a list (none is
  fine), per message.
- The answer is kept in GemMail's cache database with the model that gave
  it; changing the categories or the model categorises again, newest first.
- Putting a message in a category yourself, or taking it out, is kept and
  never overwritten, and the latest such corrections go into the prompt as
  examples.
- The folder list shows categories under the folders, as views across all
  of them.

## Open questions

- **Images and files** in `content`: later, as parts (`a{sv}` with a
  `type`), when something needs them.
- **Tools** (the model calling back into the program): later; not needed
  for anything planned.
- **Spending limits** per app or per day: worth having before anything runs
  unattended for long; the log is the start of it.
- **Model lists**: providers that list models make `ListModels` easy; for
  the rest, it's what the profile names.
