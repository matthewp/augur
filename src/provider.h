/*
 * Talking to a provider: one request as its HTTP, and its streamed answer
 * (server-sent events) back as text. Two kinds: OpenAI's chat completions,
 * which most providers and gateways speak, and Anthropic's Messages API.
 */
#ifndef AUGUR_PROVIDER_H
#define AUGUR_PROVIDER_H

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include "config.h"

enum call_error {
	CALL_OK,
	CALL_AUTH,          /* the key was refused */
	CALL_RATE_LIMITED,
	CALL_PROVIDER,      /* anything else the provider (or the network) said */
	CALL_CANCELLED,
};

struct call_spec {
	const struct profile *profile;
	const char *model;
	const char *key;          /* may be NULL */
	const char *gateway_key;  /* Cloudflare's, may be NULL */
	JsonArray *messages;      /* objects: role, content; see below */
	JsonNode *schema;         /* NULL: text */
	JsonArray *tools;         /* objects: name, description, schema; may be NULL */
	bool tools_off;           /* there are tools, but answer without them */
	gint64 max_tokens;        /* 0: the provider's default */
	double temperature;       /* < 0: the provider's default */
};

struct call_result {
	enum call_error error;
	char *message;            /* the error's, for people */
	char *text;               /* the whole answer, or the text before calls */
	JsonArray *calls;         /* the tools it called (objects: id, name,
	                           * arguments, as JSON text), or NULL */
	gint64 input_tokens, output_tokens; /* -1 if not said */
	gint64 cached_tokens;     /* of the input, read from a cache; -1 */
	double cost;              /* dollars, if the provider said; else -1 */
};

/* Besides system, user and assistant messages (role, content), a
 * conversation with tools has, in Augur's own form, which each provider's
 * adapter turns into its own:
 *
 *   {role: assistant, content, calls: [{id, name, arguments}]}
 *   {role: tool, id, content, error: bool}     the answer to a call
 */

typedef void (*call_delta_fn)(const char *text, void *data);
typedef void (*call_done_fn)(struct call_result *result, void *data);

/* Starts it; delta gets the text as it comes, then done gets it all (or
 * what went wrong), once. The spec isn't needed after this returns. */
void call_start(SoupSession *session, const struct call_spec *spec,
	GCancellable *cancel, call_delta_fn delta, call_done_fn done, void *data);

/* The models a provider lists (char *), or NULL and why. */
typedef void (*models_fn)(GPtrArray *models, const char *error, void *data);
void call_list_models(SoupSession *session, const struct profile *p,
	const char *key, models_fn done, void *data);

#endif
