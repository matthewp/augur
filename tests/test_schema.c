/* The schema checker against answers that match and answers that don't. */
#include <stdio.h>
#include <string.h>
#include "schema.h"

static int failures;

static JsonNode *parse(const char *json) {
	char *error = NULL;
	JsonNode *n = schema_parse_answer(json, &error);
	if (n == NULL) {
		fprintf(stderr, "can't parse %s: %s\n", json, error);
		g_free(error);
	}
	return n;
}

static void expect(const char *schema, const char *value, bool ok,
		const char *error_has) {
	JsonNode *s = parse(schema), *v = parse(value);
	char *error = NULL;
	bool got = s != NULL && v != NULL && schema_check(s, v, &error);
	bool pass = got == ok && (error_has == NULL ||
		(error != NULL && strstr(error, error_has) != NULL));
	if (!pass) {
		fprintf(stderr, "FAIL %s against %s: %s (%s)\n", value, schema,
			got ? "matched" : "didn't match", error ? error : "no error");
		failures++;
	}
	g_free(error);
	if (s != NULL) {
		json_node_unref(s);
	}
	if (v != NULL) {
		json_node_unref(v);
	}
}

int main(void) {
	const char *categories = "{\"type\": \"object\", \"properties\": {"
		"\"categories\": {\"type\": \"array\", \"items\": {\"enum\": "
		"[\"bill\", \"newsletter\", \"sports\"]}, \"uniqueItems\": true}},"
		"\"required\": [\"categories\"], \"additionalProperties\": false}";
	expect(categories, "{\"categories\": [\"bill\"]}", true, NULL);
	expect(categories, "{\"categories\": []}", true, NULL);
	expect(categories, "{\"categories\": [\"bill\", \"sports\"]}", true, NULL);
	expect(categories, "{\"categories\": [\"cats\"]}", false,
		"$.categories[0]");
	expect(categories, "{}", false, "\"categories\" is missing");
	expect(categories, "{\"categories\": [], \"why\": \"x\"}", false,
		"\"why\" isn't allowed");
	expect(categories, "{\"categories\": [\"bill\", \"bill\"]}", false,
		"the same");
	expect(categories, "[\"bill\"]", false, "should be object");

	expect("{\"type\": \"integer\"}", "3", true, NULL);
	expect("{\"type\": \"integer\"}", "3.5", false, NULL);
	expect("{\"type\": \"number\", \"minimum\": 0, \"maximum\": 1}", "0.5",
		true, NULL);
	expect("{\"type\": \"number\", \"maximum\": 1}", "2", false, "maximum");
	expect("{\"type\": [\"string\", \"null\"]}", "null", true, NULL);
	expect("{\"type\": \"string\", \"maxLength\": 3}", "\"abcd\"", false,
		"too long");
	expect("{\"anyOf\": [{\"type\": \"string\"}, {\"type\": \"integer\"}]}",
		"true", false, NULL);
	expect("{\"const\": \"yes\"}", "\"yes\"", true, NULL);
	expect("{\"type\": \"array\", \"minItems\": 1}", "[]", false, "at least");

	/* Models wrap JSON in a code fence when asked for it in a prompt. */
	JsonNode *n = schema_parse_answer("```json\n{\"a\": 1}\n```", NULL);
	if (n == NULL || !JSON_NODE_HOLDS_OBJECT(n)) {
		fprintf(stderr, "FAIL code fence\n");
		failures++;
	}
	if (n != NULL) {
		json_node_unref(n);
	}
	char *error = NULL;
	if (schema_parse_answer("not json", &error) != NULL || error == NULL) {
		fprintf(stderr, "FAIL not json\n");
		failures++;
	}
	g_free(error);

	if (failures == 0) {
		printf("schema: all passed\n");
	}
	return failures == 0 ? 0 : 1;
}
