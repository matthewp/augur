#include <string.h>
#include "classify.h"

#define MAX_OPTIONS 255     /* Jev's limit for a choice */

/* ---- The questions --------------------------------------------------------- */

static bool invalid(GError **error, const char *format, ...) G_GNUC_PRINTF(2, 3);

static bool invalid(GError **error, const char *format, ...) {
	va_list args;
	va_start(args, format);
	char *message = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, message);
	g_free(message);
	return false;
}

static bool parse_question(const char *name, GVariant *v, JsonObject *out,
		GError **error) {
	if (!g_regex_match_simple("^[A-Za-z0-9_-]{1,64}$", name, 0, 0)) {
		return invalid(error, "a question's name is letters, digits, _ and -, "
			"up to 64: not \"%s\"", name);
	}
	if (!g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}"))) {
		return invalid(error, "question \"%s\" isn't an a{sv}", name);
	}
	const char *type = NULL, *instructions = NULL;
	g_variant_lookup(v, "type", "&s", &type);
	g_variant_lookup(v, "instructions", "&s", &instructions);
	if (instructions == NULL || instructions[0] == '\0') {
		return invalid(error, "question \"%s\" needs instructions: the question",
			name);
	}
	JsonObject *q = json_object_new();
	json_object_set_string_member(q, "type", type != NULL ? type : "");
	json_object_set_string_member(q, "instructions", instructions);
	bool ok = true;
	if (g_strcmp0(type, "choice") == 0) {
		GVariant *options = g_variant_lookup_value(v, "options",
			G_VARIANT_TYPE("a{ss}"));
		gsize n = options != NULL ? g_variant_n_children(options) : 0;
		if (n < 2 || n > MAX_OPTIONS) {
			ok = invalid(error, "choice \"%s\" needs options (a{ss}: each a name "
				"and what it means), 2 to %d", name, MAX_OPTIONS);
		} else {
			JsonObject *o = json_object_new();
			GVariantIter it;
			const char *option, *description;
			g_variant_iter_init(&it, options);
			while (g_variant_iter_next(&it, "{&s&s}", &option, &description)) {
				json_object_set_string_member(o, option, description);
			}
			json_object_set_object_member(q, "options", o);
		}
		if (options != NULL) {
			g_variant_unref(options);
		}
	} else if (g_strcmp0(type, "yes-no") == 0) {
		const char *yes = NULL, *no = NULL;
		g_variant_lookup(v, "yes", "&s", &yes);
		g_variant_lookup(v, "no", "&s", &no);
		if (yes != NULL && yes[0] != '\0') {
			json_object_set_string_member(q, "yes", yes);
		}
		if (no != NULL && no[0] != '\0') {
			json_object_set_string_member(q, "no", no);
		}
	} else if (g_strcmp0(type, "score") == 0) {
		const char **levels = NULL;
		g_variant_lookup(v, "levels", "^a&s", &levels);
		if (levels == NULL || g_strv_length((char **)levels) < 2) {
			ok = invalid(error, "score \"%s\" needs levels (as: each described, "
				"lowest first), at least 2", name);
		} else {
			JsonArray *a = json_array_new();
			for (int i = 0; levels[i] != NULL; i++) {
				json_array_add_string_element(a, levels[i]);
			}
			json_object_set_array_member(q, "levels", a);
		}
		g_free(levels);
	} else {
		ok = invalid(error, "question \"%s\"'s type is choice, yes-no or score",
			name);
	}
	if (ok) {
		json_object_set_object_member(out, name, q);
	} else {
		json_object_unref(q);
	}
	return ok;
}

bool classify_parse(GVariant *questions, JsonObject *out, GError **error) {
	GVariantIter it;
	const char *name;
	GVariant *v;
	g_variant_iter_init(&it, questions);
	while (g_variant_iter_next(&it, "{&sv}", &name, &v)) {
		bool ok = parse_question(name, v, out, error);
		g_variant_unref(v);
		if (!ok) {
			return false;
		}
	}
	if (json_object_get_size(out) == 0) {
		return invalid(error, "there are no questions");
	}
	return true;
}

GList *classify_names(JsonObject *questions) {
	return json_object_get_members(questions);
}

static const char *type_of(JsonObject *q) {
	return json_object_get_string_member(q, "type");
}

static int levels_of(JsonObject *q) {
	return json_array_get_length(json_object_get_array_member(q, "levels"));
}

/* ---- Jev ------------------------------------------------------------------- */

JsonNode *classify_jev_questions(JsonObject *questions) {
	JsonObject *out = json_object_new();
	GList *names = classify_names(questions);
	for (GList *l = names; l != NULL; l = l->next) {
		JsonObject *q = json_object_get_object_member(questions, l->data);
		const char *type = type_of(q);
		JsonObject *j = json_object_new();
		json_object_set_string_member(j, "instructions",
			json_object_get_string_member(q, "instructions"));
		if (strcmp(type, "choice") == 0) {
			json_object_set_string_member(j, "type", "choice");
			json_object_set_member(j, "criteria",
				json_node_copy(json_object_get_member(q, "options")));
		} else if (strcmp(type, "yes-no") == 0) {
			json_object_set_string_member(j, "type", "noul");
			/* Jev takes both or neither: what's not said is the rest. */
			if (json_object_has_member(q, "yes") || json_object_has_member(q, "no")) {
				JsonObject *c = json_object_new();
				json_object_set_string_member(c, "true",
					json_object_get_string_member_with_default(q, "yes",
						"Anything else."));
				json_object_set_string_member(c, "false",
					json_object_get_string_member_with_default(q, "no",
						"Anything else."));
				json_object_set_object_member(j, "criteria", c);
			}
		} else {
			json_object_set_string_member(j, "type", "score");
			json_object_set_member(j, "criteria",
				json_node_copy(json_object_get_member(q, "levels")));
		}
		json_object_set_object_member(out, l->data, j);
	}
	g_list_free(names);
	JsonNode *node = json_node_init_object(json_node_alloc(), out);
	json_object_unref(out);
	return node;
}

static double number(JsonObject *o, const char *member, bool *ok) {
	JsonNode *n = json_object_get_member(o, member);
	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n) ||
			(json_node_get_value_type(n) != G_TYPE_DOUBLE &&
				json_node_get_value_type(n) != G_TYPE_INT64)) {
		*ok = false;
		return 0;
	}
	return json_node_get_double(n);
}

GVariant *classify_answers_from_jev(JsonObject *questions, const char *json,
		char **error) {
	JsonParser *parser = json_parser_new();
	if (!json_parser_load_from_data(parser, json, -1, NULL) ||
			!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		*error = g_strdup("the classifier's answers aren't a JSON object");
		g_object_unref(parser);
		return NULL;
	}
	JsonObject *answers = json_node_get_object(json_parser_get_root(parser));
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	GList *names = classify_names(questions);
	for (GList *l = names; l != NULL && *error == NULL; l = l->next) {
		const char *name = l->data;
		JsonObject *q = json_object_get_object_member(questions, name);
		JsonNode *an = json_object_get_member(answers, name);
		if (an == NULL || !JSON_NODE_HOLDS_OBJECT(an)) {
			*error = g_strdup_printf("the classifier didn't answer \"%s\"", name);
			break;
		}
		JsonObject *a = json_node_get_object(an);
		const char *type = type_of(q);
		bool ok = true;
		GVariantBuilder ab;
		g_variant_builder_init(&ab, G_VARIANT_TYPE("a{sv}"));
		if (strcmp(type, "yes-no") == 0) {
			double p = number(a, "noul", &ok);
			g_variant_builder_add(&ab, "{sv}", "probability", g_variant_new_double(p));
		} else {
			JsonNode *probs = json_object_get_member(a, "probabilities");
			JsonObject *po = probs != NULL && JSON_NODE_HOLDS_OBJECT(probs) ?
				json_node_get_object(probs) : NULL;
			double confidence = number(a, "confidence", &ok);
			if (strcmp(type, "choice") == 0) {
				const char *choice = json_object_get_string_member_with_default(a,
					"choice", NULL);
				JsonObject *options = json_object_get_object_member(q, "options");
				ok = ok && choice != NULL && json_object_has_member(options, choice);
				g_variant_builder_add(&ab, "{sv}", "choice",
					g_variant_new_string(choice != NULL ? choice : ""));
				GVariantBuilder pb;
				g_variant_builder_init(&pb, G_VARIANT_TYPE("a{sd}"));
				GList *opts = json_object_get_members(options);
				for (GList *o = opts; o != NULL; o = o->next) {
					bool has = po != NULL && json_object_has_member(po, o->data);
					double p = has ? number(po, o->data, &ok) : 0;
					g_variant_builder_add(&pb, "{sd}", (const char *)o->data, p);
				}
				g_list_free(opts);
				g_variant_builder_add(&ab, "{sv}", "probabilities",
					g_variant_builder_end(&pb));
			} else {
				double score = number(a, "score", &ok);
				g_variant_builder_add(&ab, "{sv}", "score", g_variant_new_double(score));
				GVariantBuilder pb;
				g_variant_builder_init(&pb, G_VARIANT_TYPE("ad"));
				for (int i = 0; i < levels_of(q); i++) {
					char key[16];
					g_snprintf(key, sizeof key, "%d", i);
					bool has = po != NULL && json_object_has_member(po, key);
					g_variant_builder_add(&pb, "d", has ? number(po, key, &ok) : 0);
				}
				g_variant_builder_add(&ab, "{sv}", "probabilities",
					g_variant_builder_end(&pb));
			}
			g_variant_builder_add(&ab, "{sv}", "confidence",
				g_variant_new_double(confidence));
		}
		GVariant *answer = g_variant_builder_end(&ab);
		if (!ok) {
			g_variant_unref(g_variant_ref_sink(answer));
			*error = g_strdup_printf("the classifier's answer to \"%s\" isn't one",
				name);
			break;
		}
		g_variant_builder_add(&b, "{sv}", name, answer);
	}
	g_list_free(names);
	g_object_unref(parser);
	if (*error != NULL) {
		g_variant_builder_clear(&b);
		return NULL;
	}
	return g_variant_builder_end(&b);
}

/* ---- A chat model instead ------------------------------------------------- */

JsonNode *classify_schema(JsonObject *questions) {
	JsonObject *props = json_object_new();
	JsonArray *required = json_array_new();
	GList *names = classify_names(questions);
	for (GList *l = names; l != NULL; l = l->next) {
		JsonObject *q = json_object_get_object_member(questions, l->data);
		const char *type = type_of(q);
		JsonObject *s = json_object_new();
		if (strcmp(type, "choice") == 0) {
			JsonArray *e = json_array_new();
			GList *opts = json_object_get_members(
				json_object_get_object_member(q, "options"));
			for (GList *o = opts; o != NULL; o = o->next) {
				json_array_add_string_element(e, o->data);
			}
			g_list_free(opts);
			json_object_set_array_member(s, "enum", e);
		} else if (strcmp(type, "yes-no") == 0) {
			json_object_set_string_member(s, "type", "boolean");
		} else {
			json_object_set_string_member(s, "type", "integer");
			json_object_set_int_member(s, "minimum", 0);
			json_object_set_int_member(s, "maximum", levels_of(q) - 1);
		}
		json_object_set_object_member(props, l->data, s);
		json_array_add_string_element(required, l->data);
	}
	g_list_free(names);
	JsonObject *schema = json_object_new();
	json_object_set_string_member(schema, "type", "object");
	json_object_set_object_member(schema, "properties", props);
	json_object_set_array_member(schema, "required", required);
	json_object_set_boolean_member(schema, "additionalProperties", FALSE);
	JsonNode *node = json_node_init_object(json_node_alloc(), schema);
	json_object_unref(schema);
	return node;
}

char *classify_prompt(JsonObject *questions) {
	GString *s = g_string_new("Answer these questions about the text the user "
		"gives you. Each answer is one of those the question allows, the one "
		"that fits the text best.\n");
	GList *names = classify_names(questions);
	for (GList *l = names; l != NULL; l = l->next) {
		JsonObject *q = json_object_get_object_member(questions, l->data);
		const char *type = type_of(q);
		g_string_append_printf(s, "\n%s: %s\n", (const char *)l->data,
			json_object_get_string_member(q, "instructions"));
		if (strcmp(type, "choice") == 0) {
			g_string_append(s, "The answer is one of these names:\n");
			JsonObject *options = json_object_get_object_member(q, "options");
			GList *opts = json_object_get_members(options);
			for (GList *o = opts; o != NULL; o = o->next) {
				g_string_append_printf(s, "- %s: %s\n", (const char *)o->data,
					json_object_get_string_member(options, o->data));
			}
			g_list_free(opts);
		} else if (strcmp(type, "yes-no") == 0) {
			g_string_append(s, "The answer is true (yes) or false (no).\n");
			if (json_object_has_member(q, "yes")) {
				g_string_append_printf(s, "- true: %s\n",
					json_object_get_string_member(q, "yes"));
			}
			if (json_object_has_member(q, "no")) {
				g_string_append_printf(s, "- false: %s\n",
					json_object_get_string_member(q, "no"));
			}
		} else {
			g_string_append(s, "The answer is the number of the level:\n");
			JsonArray *levels = json_object_get_array_member(q, "levels");
			for (guint i = 0; i < json_array_get_length(levels); i++) {
				g_string_append_printf(s, "- %u: %s\n", i,
					json_array_get_string_element(levels, i));
			}
		}
	}
	g_list_free(names);
	return g_string_free(s, FALSE);
}

GVariant *classify_answers_from_chat(JsonObject *questions, const char *json) {
	JsonParser *parser = json_parser_new();
	json_parser_load_from_data(parser, json, -1, NULL);
	JsonObject *answers = json_node_get_object(json_parser_get_root(parser));
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	GList *names = classify_names(questions);
	for (GList *l = names; l != NULL; l = l->next) {
		JsonObject *q = json_object_get_object_member(questions, l->data);
		const char *type = type_of(q);
		GVariantBuilder ab;
		g_variant_builder_init(&ab, G_VARIANT_TYPE("a{sv}"));
		if (strcmp(type, "choice") == 0) {
			g_variant_builder_add(&ab, "{sv}", "choice", g_variant_new_string(
				json_object_get_string_member(answers, l->data)));
		} else if (strcmp(type, "yes-no") == 0) {
			bool yes = json_object_get_boolean_member(answers, l->data);
			g_variant_builder_add(&ab, "{sv}", "probability",
				g_variant_new_double(yes ? 1 : 0));
		} else {
			g_variant_builder_add(&ab, "{sv}", "score", g_variant_new_double(
				json_object_get_int_member(answers, l->data)));
		}
		g_variant_builder_add(&b, "{sv}", (const char *)l->data,
			g_variant_builder_end(&ab));
	}
	g_list_free(names);
	g_object_unref(parser);
	return g_variant_builder_end(&b);
}
