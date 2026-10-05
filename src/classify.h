/*
 * Classify's questions: checked as they come over D-Bus, put as Jev asks
 * them (Typesafe's System One API) or as a JSON Schema and a prompt for a
 * chat model when there's no classifier, and the answers of either made
 * the same answers. See design/classify.md.
 */
#ifndef AUGUR_CLASSIFY_H
#define AUGUR_CLASSIFY_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdbool.h>

/* The questions (a{sv}: name -> a{sv}) as Augur keeps them, by name:
 * {type: choice|yes-no|score, instructions, options: {name: description},
 * yes, no, levels: [...]}. false, with InvalidArgs, if they're wrong. */
bool classify_parse(GVariant *questions, JsonObject *out, GError **error);

/* As Jev asks them: {name: {type, instructions, criteria}}. */
JsonNode *classify_jev_questions(JsonObject *questions);

/* For a chat model: a JSON Schema for all the answers, and the system
 * prompt that asks the questions. */
JsonNode *classify_schema(JsonObject *questions);
char *classify_prompt(JsonObject *questions);

/* The answers (a{sv}: name -> a{sv}) from Jev's (its reply's "answers",
 * as JSON text), or NULL and why. */
GVariant *classify_answers_from_jev(JsonObject *questions, const char *json,
	char **error);

/* The answers from a chat model's JSON, already checked against
 * classify_schema. */
GVariant *classify_answers_from_chat(JsonObject *questions, const char *json);

/* The questions' names, in order, for the log. */
GList *classify_names(JsonObject *questions);

#endif
