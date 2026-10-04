/*
 * Checking an answer against a JSON Schema: the parts of it that
 * describe structured answers (type, properties, required,
 * additionalProperties, items, enum, const, anyOf, and the limits on
 * lengths, sizes and numbers). Anything else in a schema is let pass.
 */
#ifndef AUGUR_SCHEMA_H
#define AUGUR_SCHEMA_H

#include <json-glib/json-glib.h>
#include <stdbool.h>

/* Whether value matches schema; if not, *error says where and why. */
bool schema_check(JsonNode *schema, JsonNode *value, char **error);

/* An answer's JSON: the text parsed, after taking off a Markdown code
 * fence round it, which models add when asked for JSON in a prompt. NULL
 * with *error if it isn't JSON. */
JsonNode *schema_parse_answer(const char *text, char **error);

#endif
