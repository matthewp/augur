/*
 * Augur's configuration, ~/.config/augur/config: whether it's on, the
 * profiles (a provider, its key, its models and tiers), and per program
 * settings. See README.md.
 */
#ifndef AUGUR_CONFIG_H
#define AUGUR_CONFIG_H

#include <glib.h>
#include <stdbool.h>

enum provider_kind {
	PROVIDER_OPENAI,      /* OpenAI's chat completions, and the gateways */
	PROVIDER_ANTHROPIC,   /* Anthropic's Messages API */
};

/* How a profile gets structured (schema) answers. */
enum structured {
	STRUCTURED_NATIVE,    /* the provider's own: response_format, or a tool */
	STRUCTURED_PROMPT,    /* the schema in the prompt, the answer checked */
};

/* A model's price, in US dollars per million tokens. */
struct price {
	double input, output;
	double cached;            /* input read from the provider's cache */
};

struct profile {
	char *name;
	char *provider;           /* as written: openai, cloudflare, ... */
	enum provider_kind kind;
	char *url;                /* the API's base, e.g. https://api.openai.com/v1 */
	char *key_command;        /* prints the key */
	char *key_env;            /* or names a variable holding it */
	char *gateway_key_command; /* Cloudflare: an authenticated gateway's token */
	bool needs_key;
	char *model;              /* when nothing more particular is asked */
	GHashTable *tiers;        /* name -> model */
	GHashTable *prices;       /* model -> struct price */
	enum structured structured;
	bool stream_usage;        /* ask for token counts in a stream */
	int max_concurrent;
	char *problem;            /* why it can't be used, or NULL */
};

struct app_settings {
	char *profile, *model, *tier;
};

struct config {
	int refs;
	bool enabled;             /* [augur] enabled */
	char *default_profile;
	GPtrArray *profiles;      /* struct profile, in the file's order */
	GHashTable *apps;         /* app ID -> struct app_settings */
};

char *config_path(void);
/* The config at path; a missing file is an empty config. Requests hold a
 * reference, so a config read again doesn't take a profile from under one
 * that's running. */
struct config *config_load(const char *path, char **error);
struct config *config_ref(struct config *c);
void config_unref(struct config *c);

struct profile *config_profile(struct config *c, const char *name);
/* default_profile, or the first that can be used. */
struct profile *config_default_profile(struct config *c);
const struct app_settings *config_app(struct config *c, const char *app_id);

#endif
