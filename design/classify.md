# Classify: classifier models, and questions about text

Status: built. The README has the interface as it is; this has why.

Some models don't write: they read an input and answer questions about
it from a fixed set of answers, with probabilities. Typesafe's Jev is
the first of them. Programs want this all the time (is this mail a
bill? which team should get this ticket? how bad is this bug?), and
asking a chat model for it is slow, costs more, and gives an answer
with no idea how sure it is.

Augur gets a second kind of request, `Classify`, beside `Complete`. A
program asks its questions the same way whatever answers them: a
classifier where a profile has one, else the profile's chat model.

## Jev, and where it is

Jev takes a `state` (the input: text, or JSON) and named questions, and
answers all of them in one request, in about a tenth of a second. Its
probabilities are calibrated: 0.8 means the same thing from one
question to the next. It has three kinds of question:

- **choice**: one of up to 255 options, each a name and a description.
  The answer is the most likely, a `confidence` (how concentrated the
  probabilities are), and every option's probability.
- **noul** (yes or no): optionally with what counts as true and as
  false, both or neither (Augur fills in the one not given as "Anything
  else."). The answer is the probability that it's yes.
- **score**: levels, described lowest first. The answer is a `score`
  (each level's number times its probability, summed: 1.43 is between
  the second and third), a `confidence`, and each level's probability.

It isn't on chat completions anywhere. It's served as Typesafe's
"System One" API, `POST {base}/systemone`:

```json
{
  "model": "jev-1.13.0",
  "state": "the input",
  "questions": {
    "team": {
      "type": "choice",
      "instructions": "Which team should handle this?",
      "criteria": {"returns": "Exchanges, wrong or damaged items",
                   "billing": "Charges, invoices, payment problems"}
    },
    "human": {"type": "noul", "instructions": "Is the customer asking for a human?"},
    "severity": {"type": "score", "instructions": "How severe is it?",
                 "criteria": ["Cosmetic", "Degraded, with a workaround", "Blocking"]}
  }
}
```

and answers `{"answers": {"team": {...}, ...}, "usage": {...}}`.

| Where | Base | Model |
|-------|------|-------|
| Typesafe | `https://api.typesafe.ai/v1` | `jev-1.13.0`, `jev-latest` |
| OpenRouter | `https://openrouter.ai/api/v1` (its chat completions' too) | `typesafe/jev-1.13`, `~typesafe/jev-latest` |

OpenRouter also has an alpha "Decisions" API (`/api/alpha/decisions`);
System One is the one to build on. Other gateways (Cloudflare's,
Vercel's) are unknown.

## Configuration

A classifier isn't a provider of its own: it's a second API some
providers have. A profile names its classifier model, and `Classify`
goes to `{url}/systemone` with the profile's key:

```ini
[augur]
classify-profile = openrouter      # Classify's profile, unless the app's
                                   # section or the request says another

[profile openrouter]
provider = openrouter
model = deepseek-v4-flash
classifier = typesafe/jev-1.13

[profile typesafe]
provider = typesafe                # url https://api.typesafe.ai/v1
api-key-command = pass show typesafe
classifier = jev-1.13.0

[app org.example.Mail]
classify-profile = openrouter
```

| Key | Where | Meaning |
|-----|-------|---------|
| `classifier` | profile | a model speaking System One at `{url}/systemone` |
| `classify-profile` | `[augur]`, `[app ...]` | the profile for `Classify` when the request doesn't name one |

`provider = typesafe` is new, but only as a name for its `url` and key;
it has no chat model, so `Complete` to it is `Error.NoModel`. Any other
gateway that serves System One works through `openai-compatible` and
its `url`.

Which profile, first match wins: the request's, the `[app]` section's
`classify-profile`, `[augur] classify-profile`, `default-profile`.
Which model: the request's `model` (a classifier model, if the profile
has a classifier), else the profile's `classifier`,
else its chat model (below).

## The interface

```
interface org.gemwm.Augur1

  method Classify(request: a{sv}) -> (answers: a{sv}, info: a{sv})
```

A method that waits for its answer, unlike `Complete`: there's nothing
to stream, and a classifier answers in about the time a handle and its
signals would take. A chat model answering instead takes longer, but
the program can't tell and needn't care.

The request:

| Key | Type | Meaning |
|-----|------|---------|
| `app-id` | s | who's asking (required) |
| `input` | s | what the questions are about (required) |
| `questions` | a{sv} | by name (letters, digits, `_`, `-`): each an `a{sv}`, below (required, at least one) |
| `profile`, `model` | s | optional, as for `Complete` |
| `interactive` | b | someone's waiting for it: ahead of background work (false) |

A question:

| Key | Type | Meaning |
|-----|------|---------|
| `type` | s | `choice`, `yes-no` or `score` |
| `instructions` | s | the question |
| `options` | a{ss} | `choice`: each option's name and description (2 to 255) |
| `yes`, `no` | s | `yes-no`, optional: what counts as each |
| `levels` | as | `score`: each level described, lowest first (at least 2) |

`yes-no` is Jev's `noul`; otherwise the names are Jev's, or close.

An answer, by the question's name:

| Type | Keys |
|------|------|
| `choice` | `choice` (s), and from a classifier `confidence` (d) and `probabilities` (a{sd}) |
| `yes-no` | `probability` (d): that it's yes |
| `score` | `score` (d: 0 to the number of levels less one), and from a classifier `confidence` (d) and `probabilities` (ad, by level) |

`info`: `profile`, `model`, `calibrated` (b: the probabilities are a
classifier's), and `input-tokens` and `output-tokens` when said.

Errors are `Complete`'s, and `InvalidArgs` for a malformed question.

## Without a classifier

A profile with no `classifier` answers with its chat model. Augur makes
one JSON Schema of the questions (an enum of the options for each
choice, a boolean for each yes-no, an integer level for each score),
asks with the input and the questions in the prompt, and takes the
answer through the structured-answer path that's there: checked, and
asked for once more if it doesn't match.

A chat model's answer has no probabilities worth the name, so they
aren't made up. `choice` and `score` come without `confidence` and
`probabilities` (a score is a whole level), a `yes-no`'s `probability`
is 0 or 1, and `info`'s `calibrated` is false. A program that acts above
a threshold still works, only more coarsely.

## Inside augurd

- A request kind beside `Complete`'s, through the same queue: a
  `Classify` is background work unless the request says it's
  `interactive` (b), say for a tool call being checked as it's made.
- provider.c gets a System One call: the body above, its answer (one
  JSON reply, not streamed) turned into the answers. Retries on 429 and
  529 as Typesafe's own client does.
- The fallback is a `Complete`-like request made inside augurd, its
  schema built from the questions and its JSON answer turned into the
  same answers.
- The log: the app, profile, model, tokens, and the questions' names;
  never the input, the instructions or the answers.

## Example: a mail program's categories

A mail program sorts mail into categories (Bill, Newsletter, Shipping),
and a message can be in several. Categories overlap, so each is a
`yes-no` question, its description the instructions; all of a message's
go in one `Classify`. Above one probability (0.8) a category is
applied; between that and a lower one it's suggested, and the user's
choice joins their corrections. Without a classifier the same code gets
a chat model's yes or no for each.

## Open questions

- **Several inputs at once.** A mail program categorising a folder sends
  one request per message. Jev answers one state per request; an
  `inputs` (as) that Augur sends in parallel would save the program the
  bookkeeping, if it's needed.
- **JSON input.** Jev's state can be JSON, not only text. `input` as a
  string is enough until something needs more.
- **Other gateways.** Cloudflare's AI Gateway and Vercel's: whether
  they pass `/systemone` through, and with what URL.
- **Gating tool calls.** OpenRouter's cookbook uses Jev to approve tool
  calls. A program can already do that itself with `Classify` before it
  answers a `ToolCall`; Augur doing it is a later idea.

## Sources

- [Jev on OpenRouter](https://openrouter.ai/docs/guides/community/jev)
- [Jev 1.13 on OpenRouter](https://openrouter.ai/typesafe/jev-1.13)
- Typesafe's primitives: [choice](https://docs.typesafe.ai/primitives/choice),
  [noul](https://docs.typesafe.ai/primitives/noul),
  [score](https://docs.typesafe.ai/primitives/score)
- [Pipecat's Jev client](https://reference-server.pipecat.ai/en/latest/_modules/pipecat/classifiers/jev/client.html)
  (the request and reply, retries)
