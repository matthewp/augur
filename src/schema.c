#include <math.h>
#include <string.h>
#include "schema.h"

static bool check(JsonNode *schema, JsonNode *value, const char *path,
	char **error);

static const char *type_of(JsonNode *v) {
	switch (json_node_get_node_type(v)) {
	case JSON_NODE_OBJECT: return "object";
	case JSON_NODE_ARRAY: return "array";
	case JSON_NODE_NULL: return "null";
	case JSON_NODE_VALUE:
		switch (json_node_get_value_type(v)) {
		case G_TYPE_STRING: return "string";
		case G_TYPE_BOOLEAN: return "boolean";
		case G_TYPE_INT64: return "integer";
		default: return "number";
		}
	}
	return "unknown";
}

static bool is_type(JsonNode *v, const char *type) {
	const char *t = type_of(v);
	if (strcmp(type, "number") == 0) {
		return strcmp(t, "number") == 0 || strcmp(t, "integer") == 0;
	}
	if (strcmp(type, "integer") == 0 && strcmp(t, "number") == 0) {
		double d = json_node_get_double(v);
		return d == floor(d);
	}
	return strcmp(t, type) == 0;
}

static double number(JsonNode *v) {
	return json_node_get_value_type(v) == G_TYPE_INT64 ?
		(double)json_node_get_int(v) : json_node_get_double(v);
}

static bool fail(char **error, const char *path, const char *format, ...)
		G_GNUC_PRINTF(3, 4);
static bool fail(char **error, const char *path, const char *format, ...) {
	if (error != NULL && *error == NULL) {
		va_list ap;
		va_start(ap, format);
		char *why = g_strdup_vprintf(format, ap);
		va_end(ap);
		*error = g_strdup_printf("%s: %s", path, why);
		g_free(why);
	}
	return false;
}

static char *shown(JsonNode *v) {
	char *s = json_to_string(v, FALSE);
	if (strlen(s) > 40) {
		strcpy(s + 37, "...");
	}
	return s;
}

static bool equal(JsonNode *a, JsonNode *b) {
	return json_node_equal(a, b);
}

static bool check_type(JsonObject *s, JsonNode *value, const char *path,
		char **error) {
	JsonNode *t = json_object_get_member(s, "type");
	if (t == NULL) {
		return true;
	}
	if (JSON_NODE_HOLDS_VALUE(t)) {
		const char *type = json_node_get_string(t);
		if (type != NULL && !is_type(value, type)) {
			return fail(error, path, "should be %s, not %s", type, type_of(value));
		}
		return true;
	}
	if (JSON_NODE_HOLDS_ARRAY(t)) {
		JsonArray *types = json_node_get_array(t);
		for (guint i = 0; i < json_array_get_length(types); i++) {
			const char *type = json_array_get_string_element(types, i);
			if (type != NULL && is_type(value, type)) {
				return true;
			}
		}
		return fail(error, path, "%s isn't one of the types allowed",
			type_of(value));
	}
	return true;
}

static bool check_object(JsonObject *s, JsonObject *o, const char *path,
		char **error) {
	JsonNode *req = json_object_get_member(s, "required");
	if (req != NULL && JSON_NODE_HOLDS_ARRAY(req)) {
		JsonArray *names = json_node_get_array(req);
		for (guint i = 0; i < json_array_get_length(names); i++) {
			const char *name = json_array_get_string_element(names, i);
			if (name != NULL && !json_object_has_member(o, name)) {
				return fail(error, path, "\"%s\" is missing", name);
			}
		}
	}
	JsonObject *props = json_object_has_member(s, "properties") ?
		json_object_get_object_member(s, "properties") : NULL;
	JsonNode *extra = json_object_get_member(s, "additionalProperties");
	GList *members = json_object_get_members(o);
	bool ok = true;
	for (GList *l = members; l != NULL && ok; l = l->next) {
		const char *name = l->data;
		char *sub = g_strdup_printf("%s.%s", path, name);
		JsonNode *v = json_object_get_member(o, name);
		if (props != NULL && json_object_has_member(props, name)) {
			ok = check(json_object_get_member(props, name), v, sub, error);
		} else if (extra != NULL && JSON_NODE_HOLDS_VALUE(extra) &&
				!json_node_get_boolean(extra)) {
			ok = fail(error, path, "\"%s\" isn't allowed", name);
		} else if (extra != NULL && JSON_NODE_HOLDS_OBJECT(extra)) {
			ok = check(extra, v, sub, error);
		}
		g_free(sub);
	}
	g_list_free(members);
	return ok;
}

static bool check_array(JsonObject *s, JsonArray *a, const char *path,
		char **error) {
	guint n = json_array_get_length(a);
	if (json_object_has_member(s, "minItems") &&
			n < (guint)json_object_get_int_member(s, "minItems")) {
		return fail(error, path, "needs at least %" G_GINT64_FORMAT " items",
			json_object_get_int_member(s, "minItems"));
	}
	if (json_object_has_member(s, "maxItems") &&
			n > (guint)json_object_get_int_member(s, "maxItems")) {
		return fail(error, path, "has more than %" G_GINT64_FORMAT " items",
			json_object_get_int_member(s, "maxItems"));
	}
	JsonNode *items = json_object_get_member(s, "items");
	if (items != NULL && JSON_NODE_HOLDS_OBJECT(items)) {
		for (guint i = 0; i < n; i++) {
			char *sub = g_strdup_printf("%s[%u]", path, i);
			bool ok = check(items, json_array_get_element(a, i), sub, error);
			g_free(sub);
			if (!ok) {
				return false;
			}
		}
	}
	if (json_object_has_member(s, "uniqueItems") &&
			json_object_get_boolean_member(s, "uniqueItems")) {
		for (guint i = 0; i < n; i++) {
			for (guint j = i + 1; j < n; j++) {
				if (equal(json_array_get_element(a, i), json_array_get_element(a, j))) {
					return fail(error, path, "items %u and %u are the same", i, j);
				}
			}
		}
	}
	return true;
}

static bool check(JsonNode *schema, JsonNode *value, const char *path,
		char **error) {
	if (schema == NULL || JSON_NODE_HOLDS_VALUE(schema)) {
		/* true (or a missing schema) allows anything; false nothing. */
		return schema == NULL || json_node_get_boolean(schema) ||
			fail(error, path, "isn't allowed");
	}
	if (!JSON_NODE_HOLDS_OBJECT(schema)) {
		return true;
	}
	JsonObject *s = json_node_get_object(schema);
	if (!check_type(s, value, path, error)) {
		return false;
	}
	JsonNode *c = json_object_get_member(s, "const");
	if (c != NULL && !equal(c, value)) {
		char *v = shown(value);
		bool ok = fail(error, path, "%s isn't the value it must be", v);
		g_free(v);
		return ok;
	}
	JsonNode *e = json_object_get_member(s, "enum");
	if (e != NULL && JSON_NODE_HOLDS_ARRAY(e)) {
		JsonArray *choices = json_node_get_array(e);
		bool found = false;
		for (guint i = 0; i < json_array_get_length(choices) && !found; i++) {
			found = equal(json_array_get_element(choices, i), value);
		}
		if (!found) {
			char *v = shown(value);
			char *allowed = json_to_string(e, FALSE);
			bool ok = fail(error, path, "%s isn't one of %s", v, allowed);
			g_free(allowed);
			g_free(v);
			return ok;
		}
	}
	JsonNode *any = json_object_get_member(s, "anyOf");
	if (any != NULL && JSON_NODE_HOLDS_ARRAY(any)) {
		JsonArray *options = json_node_get_array(any);
		bool matched = false;
		for (guint i = 0; i < json_array_get_length(options) && !matched; i++) {
			matched = check(json_array_get_element(options, i), value, path, NULL);
		}
		if (!matched) {
			return fail(error, path, "matches none of the shapes it may have");
		}
	}
	if (is_type(value, "string")) {
		glong n = g_utf8_strlen(json_node_get_string(value), -1);
		if (json_object_has_member(s, "minLength") &&
				n < json_object_get_int_member(s, "minLength")) {
			return fail(error, path, "is too short");
		}
		if (json_object_has_member(s, "maxLength") &&
				n > json_object_get_int_member(s, "maxLength")) {
			return fail(error, path, "is too long");
		}
	} else if (is_type(value, "number")) {
		double d = number(value);
		if (json_object_has_member(s, "minimum") &&
				d < json_object_get_double_member(s, "minimum")) {
			return fail(error, path, "is below the minimum");
		}
		if (json_object_has_member(s, "maximum") &&
				d > json_object_get_double_member(s, "maximum")) {
			return fail(error, path, "is above the maximum");
		}
	} else if (JSON_NODE_HOLDS_OBJECT(value)) {
		return check_object(s, json_node_get_object(value), path, error);
	} else if (JSON_NODE_HOLDS_ARRAY(value)) {
		return check_array(s, json_node_get_array(value), path, error);
	}
	return true;
}

bool schema_check(JsonNode *schema, JsonNode *value, char **error) {
	if (error != NULL) {
		*error = NULL;
	}
	return check(schema, value, "$", error);
}

JsonNode *schema_parse_answer(const char *text, char **error) {
	const char *start = text;
	while (g_ascii_isspace(*start)) {
		start++;
	}
	char *body;
	if (g_str_has_prefix(start, "```")) {
		/* ```json ... ``` */
		const char *nl = strchr(start, '\n');
		const char *end = nl != NULL ? strstr(nl, "```") : NULL;
		body = nl != NULL ? g_strndup(nl + 1, end != NULL ?
			(gsize)(end - nl - 1) : strlen(nl + 1)) : g_strdup("");
	} else {
		body = g_strdup(start);
	}
	JsonParser *parser = json_parser_new();
	GError *err = NULL;
	JsonNode *node = NULL;
	if (json_parser_load_from_data(parser, body, -1, &err) &&
			json_parser_get_root(parser) != NULL) {
		node = json_node_copy(json_parser_get_root(parser));
	} else if (error != NULL) {
		*error = g_strdup_printf("the answer isn't JSON (%s)",
			err != NULL ? err->message : "it's empty");
	}
	g_clear_error(&err);
	g_object_unref(parser);
	g_free(body);
	return node;
}
