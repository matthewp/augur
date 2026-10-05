#include <string.h>
#include "provider.h"

#define ANTHROPIC_VERSION "2023-06-01"
#define ANTHROPIC_MAX_TOKENS 4096   /* it must be given */
#define TOOL "answer"               /* the tool a structured answer comes as */

/* A tool call as it's written. */
struct tool_call {
	char *id, *name;
	GString *arguments;
};

/* Anthropic: what the content block being written is. */
enum block { BLOCK_TEXT, BLOCK_ANSWER, BLOCK_CALL, BLOCK_OTHER };

struct call {
	SoupMessage *msg;
	GDataInputStream *lines;
	GCancellable *cancel;
	enum provider_kind kind;
	bool tool;            /* Anthropic: the answer is a tool's input */
	bool wrapped;         /* ... wrapped in {"answer": ...}, the schema not
	                       * being an object */
	bool answered;        /* ... and it was called */
	enum block block;
	GPtrArray *calls;     /* struct tool_call: the tools it's calling */
	int status;
	GString *text;        /* the answer so far */
	GString *body;        /* an error's body */
	char *stream_error;   /* an error sent in the stream */
	enum call_error stream_error_kind;
	gint64 input_tokens, output_tokens, cached_tokens;
	double cost;
	call_delta_fn delta;
	call_done_fn done;
	void *data;
};

/* ---- The request ---------------------------------------------------------- */

static const char *schema_prompt =
	"Reply with only JSON that matches this JSON Schema, and nothing else: "
	"no explanation, no code fence.\n\nSchema: ";

static void add_common(JsonBuilder *b, const struct call_spec *spec) {
	json_builder_set_member_name(b, "model");
	json_builder_add_string_value(b, spec->model);
	json_builder_set_member_name(b, "stream");
	json_builder_add_boolean_value(b, TRUE);
	if (spec->temperature >= 0) {
		json_builder_set_member_name(b, "temperature");
		json_builder_add_double_value(b, spec->temperature);
	}
}

/* The messages, as OpenAI's API takes them, with the schema added to the
 * first system message when it goes in the prompt (openai_body sees there
 * is one). */
static void add_openai_messages(JsonBuilder *b, const struct call_spec *spec,
		bool schema_in_prompt) {
	char *schema = schema_in_prompt && spec->schema != NULL ?
		json_to_string(spec->schema, FALSE) : NULL;
	bool system_seen = false;
	json_builder_set_member_name(b, "messages");
	json_builder_begin_array(b);
	guint n = json_array_get_length(spec->messages);
	for (guint i = 0; i < n; i++) {
		JsonObject *m = json_array_get_object_element(spec->messages, i);
		const char *role = json_object_get_string_member(m, "role");
		const char *content = json_object_get_string_member_with_default(m,
			"content", "");
		JsonArray *calls = json_object_has_member(m, "calls") ?
			json_object_get_array_member(m, "calls") : NULL;
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "role");
		json_builder_add_string_value(b, role);
		if (strcmp(role, "tool") == 0) {
			/* No word for a failure but its text. */
			json_builder_set_member_name(b, "tool_call_id");
			json_builder_add_string_value(b, json_object_get_string_member(m, "id"));
			json_builder_set_member_name(b, "content");
			if (json_object_get_boolean_member_with_default(m, "error", FALSE)) {
				char *e = g_strconcat("Error: ", content, NULL);
				json_builder_add_string_value(b, e);
				g_free(e);
			} else {
				json_builder_add_string_value(b, content);
			}
			json_builder_end_object(b);
			continue;
		}
		json_builder_set_member_name(b, "content");
		if (schema != NULL && !system_seen && strcmp(role, "system") == 0) {
			char *with = g_strconcat(content, "\n\n", schema_prompt, schema, NULL);
			json_builder_add_string_value(b, with);
			g_free(with);
			system_seen = true;
		} else if (calls != NULL && content[0] == '\0') {
			json_builder_add_null_value(b);
		} else {
			json_builder_add_string_value(b, content);
		}
		if (calls != NULL) {
			json_builder_set_member_name(b, "tool_calls");
			json_builder_begin_array(b);
			for (guint j = 0; j < json_array_get_length(calls); j++) {
				JsonObject *call = json_array_get_object_element(calls, j);
				json_builder_begin_object(b);
				json_builder_set_member_name(b, "id");
				json_builder_add_string_value(b,
					json_object_get_string_member(call, "id"));
				json_builder_set_member_name(b, "type");
				json_builder_add_string_value(b, "function");
				json_builder_set_member_name(b, "function");
				json_builder_begin_object(b);
				json_builder_set_member_name(b, "name");
				json_builder_add_string_value(b,
					json_object_get_string_member(call, "name"));
				json_builder_set_member_name(b, "arguments");
				json_builder_add_string_value(b,
					json_object_get_string_member(call, "arguments"));
				json_builder_end_object(b);
				json_builder_end_object(b);
			}
			json_builder_end_array(b);
		}
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	g_free(schema);
}

static JsonNode *openai_body(const struct call_spec *spec) {
	const struct profile *p = spec->profile;
	bool native = spec->schema != NULL && p->structured == STRUCTURED_NATIVE;
	JsonArray *messages = spec->messages;
	JsonArray *with_system = NULL;
	if (spec->schema != NULL && !native) {
		/* The schema goes in the system message; make one if there's none. */
		bool has = false;
		for (guint i = 0; i < json_array_get_length(messages) && !has; i++) {
			has = g_strcmp0(json_object_get_string_member(
				json_array_get_object_element(messages, i), "role"), "system") == 0;
		}
		if (!has) {
			with_system = json_array_new();
			JsonObject *sys = json_object_new();
			json_object_set_string_member(sys, "role", "system");
			json_object_set_string_member(sys, "content",
				"You answer in JSON.");
			json_array_add_object_element(with_system, sys);
			for (guint i = 0; i < json_array_get_length(messages); i++) {
				json_array_add_element(with_system,
					json_node_copy(json_array_get_element(messages, i)));
			}
		}
	}
	struct call_spec s = *spec;
	if (with_system != NULL) {
		s.messages = with_system;
	}
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	add_common(b, &s);
	add_openai_messages(b, &s, !native);
	if (p->stream_usage) {
		json_builder_set_member_name(b, "stream_options");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "include_usage");
		json_builder_add_boolean_value(b, TRUE);
		json_builder_end_object(b);
	}
	if (spec->max_tokens > 0) {
		/* OpenAI's own models want the new name; the rest the old. */
		json_builder_set_member_name(b, strcmp(p->provider, "openai") == 0 ?
			"max_completion_tokens" : "max_tokens");
		json_builder_add_int_value(b, spec->max_tokens);
	}
	if (native) {
		json_builder_set_member_name(b, "response_format");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "json_schema");
		json_builder_set_member_name(b, "json_schema");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, TOOL);
		json_builder_set_member_name(b, "schema");
		json_builder_add_value(b, json_node_copy(spec->schema));
		json_builder_end_object(b);
		json_builder_end_object(b);
	}
	if (spec->tools != NULL && json_array_get_length(spec->tools) > 0) {
		json_builder_set_member_name(b, "tools");
		json_builder_begin_array(b);
		for (guint i = 0; i < json_array_get_length(spec->tools); i++) {
			JsonObject *t = json_array_get_object_element(spec->tools, i);
			const char *description = json_object_get_string_member_with_default(t,
				"description", "");
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "type");
			json_builder_add_string_value(b, "function");
			json_builder_set_member_name(b, "function");
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "name");
			json_builder_add_string_value(b, json_object_get_string_member(t, "name"));
			if (description[0] != '\0') {
				json_builder_set_member_name(b, "description");
				json_builder_add_string_value(b, description);
			}
			json_builder_set_member_name(b, "parameters");
			json_builder_add_value(b, json_node_copy(
				json_object_get_member(t, "schema")));
			json_builder_end_object(b);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		if (spec->tools_off) {
			json_builder_set_member_name(b, "tool_choice");
			json_builder_add_string_value(b, "none");
		}
	}
	json_builder_end_object(b);
	JsonNode *root = json_builder_get_root(b);
	g_object_unref(b);
	if (with_system != NULL) {
		json_array_unref(with_system);
	}
	return root;
}

static bool schema_is_object(JsonNode *schema) {
	if (!JSON_NODE_HOLDS_OBJECT(schema)) {
		return false;
	}
	JsonObject *o = json_node_get_object(schema);
	return g_strcmp0(json_object_get_string_member_with_default(o, "type", ""),
		"object") == 0;
}

/* JSON text as a node, or NULL. */
static JsonNode *parse_json(const char *text) {
	JsonParser *parser = json_parser_new();
	JsonNode *node = NULL;
	if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL) &&
			json_parser_get_root(parser) != NULL) {
		node = json_node_copy(json_parser_get_root(parser));
	}
	g_object_unref(parser);
	return node;
}

static void add_anthropic_tool(JsonBuilder *b, const char *name,
		const char *description, JsonNode *schema) {
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, name);
	if (description != NULL && description[0] != '\0') {
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b, description);
	}
	json_builder_set_member_name(b, "input_schema");
	json_builder_add_value(b, json_node_copy(schema));
	json_builder_end_object(b);
}

/* An assistant message that called tools: its text, then the calls. */
static void add_anthropic_calls(JsonBuilder *b, const char *content,
		JsonArray *calls) {
	json_builder_begin_array(b);
	if (content[0] != '\0') {
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "text");
		json_builder_set_member_name(b, "text");
		json_builder_add_string_value(b, content);
		json_builder_end_object(b);
	}
	for (guint j = 0; j < json_array_get_length(calls); j++) {
		JsonObject *call = json_array_get_object_element(calls, j);
		JsonNode *input = parse_json(json_object_get_string_member(call,
			"arguments"));
		if (input == NULL || !JSON_NODE_HOLDS_OBJECT(input)) {
			if (input != NULL) {
				json_node_unref(input);
			}
			input = json_node_init_object(json_node_alloc(), json_object_new());
		}
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "tool_use");
		json_builder_set_member_name(b, "id");
		json_builder_add_string_value(b, json_object_get_string_member(call, "id"));
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, json_object_get_string_member(call,
			"name"));
		json_builder_set_member_name(b, "input");
		json_builder_add_value(b, input);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
}

/* Anthropic: system messages go apart, and a structured answer comes as
 * the input of a tool it must use, whose input must be an object: any
 * other schema is wrapped as {"answer": ...}. With a program's tools too,
 * it must use one of them or that one. The answers to a turn's calls go
 * together in one user message. */
static JsonNode *anthropic_body(const struct call_spec *spec, bool *tool,
		bool *wrapped) {
	const struct profile *p = spec->profile;
	*tool = spec->schema != NULL && p->structured == STRUCTURED_NATIVE;
	*wrapped = *tool && !schema_is_object(spec->schema);
	bool tools = spec->tools != NULL && json_array_get_length(spec->tools) > 0;
	char *schema_text = spec->schema != NULL && !*tool ?
		json_to_string(spec->schema, FALSE) : NULL;

	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	add_common(b, spec);
	json_builder_set_member_name(b, "max_tokens");
	json_builder_add_int_value(b, spec->max_tokens > 0 ? spec->max_tokens :
		ANTHROPIC_MAX_TOKENS);

	GString *system = g_string_new(NULL);
	json_builder_set_member_name(b, "messages");
	json_builder_begin_array(b);
	guint n = json_array_get_length(spec->messages);
	for (guint i = 0; i < n; i++) {
		JsonObject *m = json_array_get_object_element(spec->messages, i);
		const char *role = json_object_get_string_member(m, "role");
		const char *content = json_object_get_string_member_with_default(m,
			"content", "");
		if (strcmp(role, "system") == 0) {
			g_string_append_printf(system, "%s%s", system->len ? "\n\n" : "",
				content);
			continue;
		}
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "role");
		if (strcmp(role, "tool") == 0) {
			json_builder_add_string_value(b, "user");
			json_builder_set_member_name(b, "content");
			json_builder_begin_array(b);
			for (; i < n; i++) {
				JsonObject *t = json_array_get_object_element(spec->messages, i);
				if (g_strcmp0(json_object_get_string_member(t, "role"), "tool") != 0) {
					break;
				}
				json_builder_begin_object(b);
				json_builder_set_member_name(b, "type");
				json_builder_add_string_value(b, "tool_result");
				json_builder_set_member_name(b, "tool_use_id");
				json_builder_add_string_value(b, json_object_get_string_member(t, "id"));
				json_builder_set_member_name(b, "content");
				json_builder_add_string_value(b,
					json_object_get_string_member_with_default(t, "content", ""));
				if (json_object_get_boolean_member_with_default(t, "error", FALSE)) {
					json_builder_set_member_name(b, "is_error");
					json_builder_add_boolean_value(b, TRUE);
				}
				json_builder_end_object(b);
			}
			i--;
			json_builder_end_array(b);
		} else {
			json_builder_add_string_value(b, role);
			json_builder_set_member_name(b, "content");
			if (json_object_has_member(m, "calls")) {
				add_anthropic_calls(b, content,
					json_object_get_array_member(m, "calls"));
			} else {
				json_builder_add_string_value(b, content);
			}
		}
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	if (schema_text != NULL) {
		g_string_append_printf(system, "%s%s%s", system->len ? "\n\n" : "",
			schema_prompt, schema_text);
	}
	if (system->len > 0) {
		json_builder_set_member_name(b, "system");
		json_builder_add_string_value(b, system->str);
	}
	g_string_free(system, TRUE);
	g_free(schema_text);

	if (*tool || tools) {
		json_builder_set_member_name(b, "tools");
		json_builder_begin_array(b);
		for (guint i = 0; tools && i < json_array_get_length(spec->tools); i++) {
			JsonObject *t = json_array_get_object_element(spec->tools, i);
			add_anthropic_tool(b, json_object_get_string_member(t, "name"),
				json_object_get_string_member_with_default(t, "description", ""),
				json_object_get_member(t, "schema"));
		}
		if (*tool && *wrapped) {
			JsonObject *props = json_object_new();
			json_object_set_member(props, TOOL, json_node_copy(spec->schema));
			JsonArray *required = json_array_new();
			json_array_add_string_element(required, TOOL);
			JsonObject *o = json_object_new();
			json_object_set_string_member(o, "type", "object");
			json_object_set_object_member(o, "properties", props);
			json_object_set_array_member(o, "required", required);
			JsonNode *schema = json_node_init_object(json_node_alloc(), o);
			add_anthropic_tool(b, TOOL, "Give your answer.", schema);
			json_node_unref(schema);
			json_object_unref(o);
		} else if (*tool) {
			add_anthropic_tool(b, TOOL, "Give your answer.", spec->schema);
		}
		json_builder_end_array(b);
		/* Left out, it may call a tool or answer. */
		const char *choice = *tool && (!tools || spec->tools_off) ? "tool" :
			*tool ? "any" : spec->tools_off ? "none" : NULL;
		if (choice != NULL) {
			json_builder_set_member_name(b, "tool_choice");
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "type");
			json_builder_add_string_value(b, choice);
			if (strcmp(choice, "tool") == 0) {
				json_builder_set_member_name(b, "name");
				json_builder_add_string_value(b, TOOL);
			}
			json_builder_end_object(b);
		}
	}
	json_builder_end_object(b);
	JsonNode *root = json_builder_get_root(b);
	g_object_unref(b);
	return root;
}

static void set_headers(SoupMessage *msg, const struct call_spec *spec) {
	SoupMessageHeaders *h = soup_message_get_request_headers(msg);
	const struct profile *p = spec->profile;
	if (p->kind == PROVIDER_ANTHROPIC) {
		if (spec->key != NULL) {
			soup_message_headers_replace(h, "x-api-key", spec->key);
		}
		soup_message_headers_replace(h, "anthropic-version", ANTHROPIC_VERSION);
	} else if (spec->key != NULL) {
		char *bearer = g_strdup_printf("Bearer %s", spec->key);
		soup_message_headers_replace(h, "Authorization", bearer);
		g_free(bearer);
	}
	if (spec->gateway_key != NULL) {
		char *bearer = g_strdup_printf("Bearer %s", spec->gateway_key);
		soup_message_headers_replace(h, "cf-aig-authorization", bearer);
		g_free(bearer);
	}
	if (g_strcmp0(p->provider, "openrouter") == 0) {
		soup_message_headers_replace(h, "X-Title", "Augur");
	}
	soup_message_headers_replace(h, "Accept", "text/event-stream");
}

/* ---- The answer ----------------------------------------------------------- */

static void tool_call_free(gpointer data) {
	struct tool_call *t = data;
	g_free(t->id);
	g_free(t->name);
	g_string_free(t->arguments, TRUE);
	g_free(t);
}

static struct tool_call *add_call(struct call *c, const char *id,
		const char *name) {
	struct tool_call *t = g_new0(struct tool_call, 1);
	t->id = g_strdup(id != NULL ? id : "");
	t->name = g_strdup(name != NULL ? name : "");
	t->arguments = g_string_new(NULL);
	g_ptr_array_add(c->calls, t);
	return t;
}

/* The calls as call_result has them; an id is "" if none was given. */
static JsonArray *calls_of(struct call *c) {
	JsonArray *a = json_array_new();
	for (guint i = 0; i < c->calls->len; i++) {
		struct tool_call *t = c->calls->pdata[i];
		JsonObject *o = json_object_new();
		json_object_set_string_member(o, "id", t->id);
		json_object_set_string_member(o, "name", t->name);
		json_object_set_string_member(o, "arguments",
			t->arguments->len > 0 ? t->arguments->str : "{}");
		json_array_add_object_element(a, o);
	}
	return a;
}

static void finish(struct call *c, enum call_error error, char *message) {
	struct call_result r = { error, message, NULL, NULL, c->input_tokens,
		c->output_tokens, c->cached_tokens, c->cost };
	if (error == CALL_OK) {
		r.text = g_string_free(c->text, FALSE);
		c->text = NULL;
		/* Answering wins over any calls made with it. */
		if (c->calls->len > 0 && !c->answered) {
			r.calls = calls_of(c);
		}
		if (c->wrapped && c->answered) {
			/* {"answer": X} -> X */
			JsonParser *parser = json_parser_new();
			if (json_parser_load_from_data(parser, r.text, -1, NULL) &&
					JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
				JsonObject *o = json_node_get_object(json_parser_get_root(parser));
				JsonNode *inner = json_object_get_member(o, TOOL);
				if (inner != NULL) {
					g_free(r.text);
					r.text = json_to_string(inner, FALSE);
				}
			}
			g_object_unref(parser);
		}
	}
	c->done(&r, c->data);
	g_free(r.text);
	if (r.calls != NULL) {
		json_array_unref(r.calls);
	}
	g_ptr_array_unref(c->calls);
	g_free(message);
	if (c->text != NULL) {
		g_string_free(c->text, TRUE);
	}
	if (c->body != NULL) {
		g_string_free(c->body, TRUE);
	}
	g_clear_object(&c->lines);
	g_clear_object(&c->msg);
	g_clear_object(&c->cancel);
	g_free(c->stream_error);
	g_free(c);
}

static enum call_error error_kind(int status, const char *type) {
	if (status == 401 || status == 403 ||
			g_strcmp0(type, "authentication_error") == 0 ||
			g_strcmp0(type, "permission_error") == 0) {
		return CALL_AUTH;
	}
	if (status == 429 || g_strcmp0(type, "rate_limit_error") == 0) {
		return CALL_RATE_LIMITED;
	}
	return CALL_PROVIDER;
}

/* An error object's message (OpenAI's and Anthropic's look alike:
 * {"error": {"message": ..., "type": ...}}), or the text as it is. */
static char *error_message(const char *body, int status, char **type) {
	char *message = NULL;
	JsonParser *parser = json_parser_new();
	if (body != NULL && json_parser_load_from_data(parser, body, -1, NULL) &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		JsonObject *o = json_node_get_object(json_parser_get_root(parser));
		JsonNode *e = json_object_get_member(o, "error");
		if (e != NULL && JSON_NODE_HOLDS_OBJECT(e)) {
			JsonObject *eo = json_node_get_object(e);
			message = g_strdup(json_object_get_string_member_with_default(eo,
				"message", ""));
			if (type != NULL) {
				*type = g_strdup(json_object_get_string_member_with_default(eo,
					"type", ""));
			}
		} else if (e != NULL && JSON_NODE_HOLDS_VALUE(e)) {
			message = g_strdup(json_node_get_string(e));
		}
	}
	g_object_unref(parser);
	if (message == NULL || message[0] == '\0') {
		g_free(message);
		message = body != NULL && body[0] != '\0' ?
			g_strndup(body, 300) : g_strdup_printf("HTTP %d", status);
		g_strstrip(message);
	}
	return message;
}

static void add_text(struct call *c, const char *text) {
	if (text != NULL && text[0] != '\0') {
		g_string_append(c->text, text);
		c->delta(text, c->data);
	}
}

/* Calls come in pieces, each saying which call (index) it's part of: the
 * first with the id and name, then the arguments a bit at a time. */
static void openai_calls(struct call *c, JsonArray *calls) {
	for (guint i = 0; i < json_array_get_length(calls); i++) {
		JsonNode *n = json_array_get_element(calls, i);
		if (!JSON_NODE_HOLDS_OBJECT(n)) {
			continue;
		}
		JsonObject *piece = json_node_get_object(n);
		gint64 index = json_object_get_int_member_with_default(piece, "index", i);
		if (index < 0 || index > 128) {
			continue;
		}
		while (c->calls->len <= index) {
			add_call(c, NULL, NULL);
		}
		struct tool_call *t = c->calls->pdata[index];
		const char *id = json_object_get_string_member_with_default(piece, "id",
			NULL);
		if (id != NULL && id[0] != '\0') {
			g_free(t->id);
			t->id = g_strdup(id);
		}
		JsonNode *fn = json_object_get_member(piece, "function");
		if (fn == NULL || !JSON_NODE_HOLDS_OBJECT(fn)) {
			continue;
		}
		JsonObject *f = json_node_get_object(fn);
		const char *name = json_object_get_string_member_with_default(f, "name",
			NULL);
		if (name != NULL && name[0] != '\0') {
			g_free(t->name);
			t->name = g_strdup(name);
		}
		const char *args = json_object_get_string_member_with_default(f,
			"arguments", NULL);
		if (args != NULL) {
			g_string_append(t->arguments, args);
		}
	}
}

static void openai_event(struct call *c, JsonObject *o) {
	JsonNode *e = json_object_get_member(o, "error");
	if (e != NULL && c->stream_error == NULL) {
		char *json = json_to_string(json_object_get_member(o, "error"), FALSE);
		char *wrapped = g_strdup_printf("{\"error\":%s}", json);
		char *type = NULL;
		c->stream_error = error_message(wrapped, 0, &type);
		c->stream_error_kind = error_kind(0, type);
		g_free(type);
		g_free(wrapped);
		g_free(json);
		return;
	}
	JsonNode *choices = json_object_get_member(o, "choices");
	if (choices != NULL && JSON_NODE_HOLDS_ARRAY(choices) &&
			json_array_get_length(json_node_get_array(choices)) > 0) {
		JsonObject *choice = json_array_get_object_element(
			json_node_get_array(choices), 0);
		JsonNode *delta = json_object_get_member(choice, "delta");
		if (delta != NULL && JSON_NODE_HOLDS_OBJECT(delta)) {
			JsonObject *d = json_node_get_object(delta);
			JsonNode *content = json_object_get_member(d, "content");
			if (content != NULL && JSON_NODE_HOLDS_VALUE(content)) {
				add_text(c, json_node_get_string(content));
			}
			JsonNode *calls = json_object_get_member(d, "tool_calls");
			if (calls != NULL && JSON_NODE_HOLDS_ARRAY(calls)) {
				openai_calls(c, json_node_get_array(calls));
			}
		}
	}
	JsonNode *usage = json_object_get_member(o, "usage");
	if (usage != NULL && JSON_NODE_HOLDS_OBJECT(usage)) {
		JsonObject *u = json_node_get_object(usage);
		c->input_tokens = json_object_get_int_member_with_default(u,
			"prompt_tokens", c->input_tokens);
		c->output_tokens = json_object_get_int_member_with_default(u,
			"completion_tokens", c->output_tokens);
		JsonNode *details = json_object_get_member(u, "prompt_tokens_details");
		if (details != NULL && JSON_NODE_HOLDS_OBJECT(details)) {
			c->cached_tokens = json_object_get_int_member_with_default(
				json_node_get_object(details), "cached_tokens", c->cached_tokens);
		}
		/* OpenRouter's: what it charged. */
		JsonNode *cost = json_object_get_member(u, "cost");
		if (cost != NULL && JSON_NODE_HOLDS_VALUE(cost)) {
			c->cost = json_node_get_double(cost);
		}
	}
}

static void anthropic_event(struct call *c, JsonObject *o) {
	const char *type = json_object_get_string_member_with_default(o, "type", "");
	if (strcmp(type, "message_start") == 0) {
		JsonObject *m = json_object_get_object_member(o, "message");
		JsonObject *u = m != NULL && json_object_has_member(m, "usage") ?
			json_object_get_object_member(m, "usage") : NULL;
		if (u != NULL) {
			/* Anthropic counts what was read from and written to its cache
			 * apart from the rest; Augur counts them all as input, as
			 * OpenAI does, with what was read also as cached. */
			gint64 read = json_object_get_int_member_with_default(u,
				"cache_read_input_tokens", 0);
			c->input_tokens = json_object_get_int_member_with_default(u,
				"input_tokens", -1);
			if (c->input_tokens >= 0) {
				c->input_tokens += read + json_object_get_int_member_with_default(u,
					"cache_creation_input_tokens", 0);
				c->cached_tokens = read;
			}
		}
	} else if (strcmp(type, "content_block_start") == 0) {
		JsonObject *cb = json_object_has_member(o, "content_block") ?
			json_object_get_object_member(o, "content_block") : NULL;
		const char *bt = cb != NULL ?
			json_object_get_string_member_with_default(cb, "type", "") : "";
		const char *name = cb != NULL ?
			json_object_get_string_member_with_default(cb, "name", "") : "";
		if (strcmp(bt, "text") == 0) {
			c->block = BLOCK_TEXT;
		} else if (strcmp(bt, "tool_use") == 0 && c->tool &&
				strcmp(name, TOOL) == 0) {
			c->block = BLOCK_ANSWER;
			c->answered = true;
		} else if (strcmp(bt, "tool_use") == 0) {
			c->block = BLOCK_CALL;
			add_call(c, json_object_get_string_member_with_default(cb, "id", NULL),
				name);
		} else {
			c->block = BLOCK_OTHER;   /* thinking, say */
		}
	} else if (strcmp(type, "content_block_delta") == 0) {
		JsonObject *d = json_object_get_object_member(o, "delta");
		const char *dt = d != NULL ?
			json_object_get_string_member_with_default(d, "type", "") : "";
		const char *partial = strcmp(dt, "input_json_delta") == 0 ?
			json_object_get_string_member_with_default(d, "partial_json", "") : NULL;
		if (strcmp(dt, "text_delta") == 0 && c->block == BLOCK_TEXT && !c->tool) {
			add_text(c, json_object_get_string_member(d, "text"));
		} else if (partial != NULL && c->block == BLOCK_ANSWER) {
			add_text(c, partial);
		} else if (partial != NULL && c->block == BLOCK_CALL) {
			struct tool_call *t = c->calls->pdata[c->calls->len - 1];
			g_string_append(t->arguments, partial);
		}
	} else if (strcmp(type, "message_delta") == 0) {
		JsonObject *u = json_object_has_member(o, "usage") ?
			json_object_get_object_member(o, "usage") : NULL;
		if (u != NULL) {
			c->output_tokens = json_object_get_int_member_with_default(u,
				"output_tokens", c->output_tokens);
		}
	} else if (strcmp(type, "error") == 0 && c->stream_error == NULL) {
		JsonNode *node = json_node_init_object(json_node_alloc(), o);
		char *json = json_to_string(node, FALSE);
		json_node_unref(node);
		char *etype = NULL;
		c->stream_error = error_message(json, 0, &etype);
		c->stream_error_kind = error_kind(0, etype);
		g_free(etype);
		g_free(json);
	}
}

static void read_line(struct call *c);

static void line_read(GObject *src, GAsyncResult *res, gpointer data) {
	struct call *c = data;
	GError *err = NULL;
	gsize len;
	char *line = g_data_input_stream_read_line_finish_utf8(
		G_DATA_INPUT_STREAM(src), res, &len, &err);
	if (err != NULL) {
		bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		char *message = cancelled ? g_strdup("cancelled") : g_strdup(err->message);
		g_error_free(err);
		finish(c, cancelled ? CALL_CANCELLED : CALL_PROVIDER, message);
		return;
	}
	if (line == NULL) {
		/* The end. */
		if (c->body != NULL) {
			char *type = NULL;
			char *message = error_message(c->body->str, c->status, &type);
			enum call_error kind = error_kind(c->status, type);
			g_free(type);
			finish(c, kind, message);
		} else if (c->stream_error != NULL) {
			finish(c, c->stream_error_kind, g_strdup(c->stream_error));
		} else {
			finish(c, CALL_OK, NULL);
		}
		return;
	}
	if (c->body != NULL) {
		g_string_append_printf(c->body, "%s\n", line);
	} else if (g_str_has_prefix(line, "data:")) {
		const char *payload = line + 5;
		while (*payload == ' ') {
			payload++;
		}
		if (strcmp(payload, "[DONE]") != 0) {
			JsonParser *parser = json_parser_new();
			if (json_parser_load_from_data(parser, payload, -1, NULL) &&
					JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
				JsonObject *o = json_node_get_object(json_parser_get_root(parser));
				if (c->kind == PROVIDER_ANTHROPIC) {
					anthropic_event(c, o);
				} else {
					openai_event(c, o);
				}
			}
			g_object_unref(parser);
		}
	}
	g_free(line);
	read_line(c);
}

static void read_line(struct call *c) {
	g_data_input_stream_read_line_async(c->lines, G_PRIORITY_DEFAULT, c->cancel,
		line_read, c);
}

static void sent(GObject *src, GAsyncResult *res, gpointer data) {
	struct call *c = data;
	GError *err = NULL;
	GInputStream *in = soup_session_send_finish(SOUP_SESSION(src), res, &err);
	if (in == NULL) {
		bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		char *message = cancelled ? g_strdup("cancelled") : g_strdup(err->message);
		g_error_free(err);
		finish(c, cancelled ? CALL_CANCELLED : CALL_PROVIDER, message);
		return;
	}
	c->status = soup_message_get_status(c->msg);
	if (c->status < 200 || c->status >= 300) {
		c->body = g_string_new(NULL); /* read it all, for its message */
	}
	c->lines = g_data_input_stream_new(in);
	g_data_input_stream_set_newline_type(c->lines, G_DATA_STREAM_NEWLINE_TYPE_ANY);
	g_object_unref(in);
	read_line(c);
}

void call_start(SoupSession *session, const struct call_spec *spec,
		GCancellable *cancel, call_delta_fn delta, call_done_fn done, void *data) {
	struct call *c = g_new0(struct call, 1);
	c->kind = spec->profile->kind;
	c->text = g_string_new(NULL);
	c->calls = g_ptr_array_new_with_free_func(tool_call_free);
	c->input_tokens = c->output_tokens = c->cached_tokens = -1;
	c->cost = -1;
	c->delta = delta;
	c->done = done;
	c->data = data;
	c->cancel = cancel != NULL ? g_object_ref(cancel) : g_cancellable_new();
	JsonNode *body;
	char *url;
	if (c->kind == PROVIDER_ANTHROPIC) {
		body = anthropic_body(spec, &c->tool, &c->wrapped);
		url = g_strdup_printf("%s/messages", spec->profile->url);
	} else {
		body = openai_body(spec);
		url = g_strdup_printf("%s/chat/completions", spec->profile->url);
	}
	c->msg = soup_message_new(SOUP_METHOD_POST, url);
	g_free(url);
	if (c->msg == NULL) {
		json_node_unref(body);
		finish(c, CALL_PROVIDER, g_strdup_printf("the profile's url isn't one: %s",
			spec->profile->url));
		return;
	}
	char *json = json_to_string(body, FALSE);
	json_node_unref(body);
	GBytes *bytes = g_bytes_new_take(json, strlen(json));
	soup_message_set_request_body_from_bytes(c->msg, "application/json", bytes);
	g_bytes_unref(bytes);
	set_headers(c->msg, spec);
	soup_session_send_async(session, c->msg, G_PRIORITY_DEFAULT, c->cancel,
		sent, c);
}

/* ---- A classifier ------------------------------------------------------------ */

struct systemone {
	SoupMessage *msg;
	call_done_fn done;
	void *data;
};

static void systemone_read(GObject *src, GAsyncResult *res, gpointer data) {
	struct systemone *s = data;
	GError *err = NULL;
	GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res,
		&err);
	int status = soup_message_get_status(s->msg);
	gsize len = 0;
	const char *body = bytes != NULL ? g_bytes_get_data(bytes, &len) : NULL;
	char *text = body != NULL ? g_strndup(body, len) : NULL;
	struct call_result r = { CALL_OK, NULL, NULL, NULL, -1, -1, -1, -1 };
	JsonParser *parser = json_parser_new();
	if (bytes == NULL) {
		bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		r.error = cancelled ? CALL_CANCELLED : CALL_PROVIDER;
		r.message = g_strdup(cancelled ? "cancelled" : err->message);
	} else if (status < 200 || status >= 300) {
		char *type = NULL;
		r.message = error_message(text, status, &type);
		/* 529: overloaded, which is to say come back later. */
		r.error = status == 529 ? CALL_RATE_LIMITED : error_kind(status, type);
		g_free(type);
	} else if (!json_parser_load_from_data(parser, text, -1, NULL) ||
			!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)) ||
			!json_object_has_member(json_node_get_object(
				json_parser_get_root(parser)), "answers")) {
		r.error = CALL_PROVIDER;
		r.message = g_strdup("the classifier's reply has no answers");
	} else {
		JsonObject *o = json_node_get_object(json_parser_get_root(parser));
		r.text = json_to_string(json_object_get_member(o, "answers"), FALSE);
		JsonNode *usage = json_object_get_member(o, "usage");
		if (usage != NULL && JSON_NODE_HOLDS_OBJECT(usage)) {
			JsonObject *u = json_node_get_object(usage);
			r.input_tokens = json_object_get_int_member_with_default(u,
				"input_tokens", -1);
			r.output_tokens = json_object_get_int_member_with_default(u,
				"output_tokens", -1);
			JsonNode *cost = json_object_get_member(u, "cost");
			if (cost != NULL && JSON_NODE_HOLDS_VALUE(cost)) {
				r.cost = json_node_get_double(cost);
			}
		}
	}
	s->done(&r, s->data);
	g_free(r.text);
	g_free(r.message);
	g_object_unref(parser);
	g_free(text);
	if (bytes != NULL) {
		g_bytes_unref(bytes);
	}
	g_clear_error(&err);
	g_object_unref(s->msg);
	g_free(s);
}

void call_systemone(SoupSession *session, const struct call_spec *spec,
		const char *input, JsonNode *questions, GCancellable *cancel,
		call_done_fn done, void *data) {
	char *url = g_strdup_printf("%s/systemone", spec->profile->url);
	SoupMessage *msg = soup_message_new(SOUP_METHOD_POST, url);
	g_free(url);
	if (msg == NULL) {
		char *m = g_strdup_printf("the profile's url isn't one: %s",
			spec->profile->url);
		struct call_result r = { CALL_PROVIDER, m, NULL, NULL, -1, -1, -1, -1 };
		done(&r, data);
		g_free(m);
		return;
	}
	JsonObject *o = json_object_new();
	json_object_set_string_member(o, "model", spec->model);
	json_object_set_string_member(o, "state", input);
	json_object_set_member(o, "questions", json_node_copy(questions));
	JsonNode *root = json_node_init_object(json_node_alloc(), o);
	char *json = json_to_string(root, FALSE);
	json_node_unref(root);
	json_object_unref(o);
	GBytes *bytes = g_bytes_new_take(json, strlen(json));
	soup_message_set_request_body_from_bytes(msg, "application/json", bytes);
	g_bytes_unref(bytes);
	set_headers(msg, spec);
	soup_message_headers_replace(soup_message_get_request_headers(msg),
		"Accept", "application/json");
	struct systemone *s = g_new0(struct systemone, 1);
	s->msg = msg;
	s->done = done;
	s->data = data;
	soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, cancel,
		systemone_read, s);
}

/* ---- Models --------------------------------------------------------------- */

struct listing {
	models_fn done;
	void *data;
	SoupMessage *msg;
};

static void listed(GObject *src, GAsyncResult *res, gpointer data) {
	struct listing *l = data;
	GError *err = NULL;
	GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res,
		&err);
	int status = soup_message_get_status(l->msg);
	gsize len = 0;
	const char *body = bytes != NULL ? g_bytes_get_data(bytes, &len) : NULL;
	char *text = body != NULL ? g_strndup(body, len) : NULL;
	if (bytes == NULL || status < 200 || status >= 300) {
		char *message = err != NULL ? g_strdup(err->message) :
			error_message(text, status, NULL);
		l->done(NULL, message, l->data);
		g_free(message);
	} else {
		GPtrArray *models = g_ptr_array_new_with_free_func(g_free);
		JsonParser *parser = json_parser_new();
		if (json_parser_load_from_data(parser, text, -1, NULL) &&
				JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
			JsonObject *o = json_node_get_object(json_parser_get_root(parser));
			JsonNode *d = json_object_get_member(o, "data");
			JsonArray *list = d != NULL && JSON_NODE_HOLDS_ARRAY(d) ?
				json_node_get_array(d) : NULL;
			for (guint i = 0; list != NULL && i < json_array_get_length(list); i++) {
				JsonNode *m = json_array_get_element(list, i);
				if (JSON_NODE_HOLDS_OBJECT(m)) {
					const char *id = json_object_get_string_member_with_default(
						json_node_get_object(m), "id", NULL);
					if (id != NULL) {
						g_ptr_array_add(models, g_strdup(id));
					}
				}
			}
		}
		g_object_unref(parser);
		l->done(models, NULL, l->data);
		g_ptr_array_unref(models);
	}
	g_free(text);
	if (bytes != NULL) {
		g_bytes_unref(bytes);
	}
	g_clear_error(&err);
	g_object_unref(l->msg);
	g_free(l);
}

void call_list_models(SoupSession *session, const struct profile *p,
		const char *key, models_fn done, void *data) {
	char *url = g_strdup_printf("%s/models", p->url);
	SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
	g_free(url);
	if (msg == NULL) {
		done(NULL, "the profile's url isn't one", data);
		return;
	}
	struct call_spec spec = { .profile = p, .key = key };
	set_headers(msg, &spec);
	soup_message_headers_replace(soup_message_get_request_headers(msg),
		"Accept", "application/json");
	struct listing *l = g_new0(struct listing, 1);
	l->done = done;
	l->data = data;
	l->msg = msg;
	soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL,
		listed, l);
}
