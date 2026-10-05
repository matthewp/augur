#include <string.h>
#include "config.h"

char *config_path(void) {
	return g_build_filename(g_get_user_config_dir(), "augur", "config", NULL);
}

/* What each provider is, where it is, and how it does things. */
static const struct known {
	const char *name;
	enum provider_kind kind;
	const char *url;          /* NULL: the profile must say */
	bool needs_key;
	enum structured structured;
	bool stream_usage;
} known[] = {
	{ "openai", PROVIDER_OPENAI, "https://api.openai.com/v1", true,
		STRUCTURED_NATIVE, true },
	{ "openrouter", PROVIDER_OPENAI, "https://openrouter.ai/api/v1", true,
		STRUCTURED_NATIVE, true },
	{ "cloudflare", PROVIDER_OPENAI, NULL, false, STRUCTURED_PROMPT, false },
	{ "ollama", PROVIDER_OPENAI, "http://localhost:11434/v1", false,
		STRUCTURED_NATIVE, false },
	{ "openai-compatible", PROVIDER_OPENAI, NULL, false, STRUCTURED_PROMPT,
		false },
	{ "anthropic", PROVIDER_ANTHROPIC, "https://api.anthropic.com/v1", true,
		STRUCTURED_NATIVE, true },
	/* A classifier, Jev, and no chat model. */
	{ "typesafe", PROVIDER_OPENAI, "https://api.typesafe.ai/v1", true,
		STRUCTURED_PROMPT, false },
};

static void profile_free(gpointer p) {
	struct profile *pr = p;
	g_free(pr->name);
	g_free(pr->provider);
	g_free(pr->url);
	g_free(pr->key_command);
	g_free(pr->key_env);
	g_free(pr->gateway_key_command);
	g_free(pr->model);
	g_hash_table_unref(pr->tiers);
	g_hash_table_unref(pr->prices);
	g_free(pr->classifier);
	g_free(pr->problem);
	g_free(pr);
}

static void app_free(gpointer p) {
	struct app_settings *a = p;
	g_free(a->profile);
	g_free(a->model);
	g_free(a->tier);
	g_free(a->classify_profile);
	g_free(a);
}

/* A value with any trailing "# comment" taken off and trimmed, or NULL. */
static char *value(GKeyFile *kf, const char *group, const char *key) {
	char *v = g_key_file_get_string(kf, group, key, NULL);
	if (v == NULL) {
		return NULL;
	}
	/* GKeyFile has no comments after values; the README's examples do. */
	for (char *p = v; *p != '\0'; p++) {
		if (*p == '#' && (p == v || p[-1] == ' ' || p[-1] == '\t')) {
			*p = '\0';
			break;
		}
	}
	g_strstrip(v);
	if (v[0] == '\0') {
		g_free(v);
		return NULL;
	}
	return v;
}

/* "IN OUT [CACHED]": dollars per million tokens. Cached input, not
 * given, is priced as input: more than it costs, never less. */
static struct price *parse_price(const char *v) {
	char **parts = g_strsplit_set(v, " \t", -1);
	double n[3];
	int count = 0;
	bool ok = true;
	for (int i = 0; parts[i] != NULL && ok; i++) {
		if (parts[i][0] == '\0') {
			continue;
		}
		char *end;
		double d = g_ascii_strtod(parts[i], &end);
		ok = count < 3 && *end == '\0' && end != parts[i] && d >= 0;
		if (ok) {
			n[count++] = d;
		}
	}
	g_strfreev(parts);
	if (!ok || count < 2) {
		return NULL;
	}
	struct price *p = g_new(struct price, 1);
	p->input = n[0];
	p->output = n[1];
	p->cached = count == 3 ? n[2] : n[0];
	return p;
}

static struct profile *profile_load(GKeyFile *kf, const char *group,
		const char *name) {
	struct profile *p = g_new0(struct profile, 1);
	p->name = g_strdup(name);
	p->tiers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	p->prices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	char *bad_price = NULL;
	p->provider = value(kf, group, "provider");
	p->model = value(kf, group, "model");
	p->classifier = value(kf, group, "classifier");
	p->key_command = value(kf, group, "api-key-command");
	p->key_env = value(kf, group, "api-key-env");
	p->gateway_key_command = value(kf, group, "gateway-key-command");
	char *url = value(kf, group, "url");
	char *max = value(kf, group, "max-concurrent");
	p->max_concurrent = max != NULL ? atoi(max) : 0;
	p->max_concurrent = p->max_concurrent > 0 ? p->max_concurrent : 4;
	g_free(max);

	gsize n = 0;
	char **keys = g_key_file_get_keys(kf, group, &n, NULL);
	for (gsize i = 0; i < n; i++) {
		if (g_str_has_prefix(keys[i], "tier.") && keys[i][5] != '\0') {
			char *model = value(kf, group, keys[i]);
			if (model != NULL) {
				g_hash_table_insert(p->tiers, g_strdup(keys[i] + 5), model);
			}
		} else if (g_str_has_prefix(keys[i], "price.") && keys[i][6] != '\0') {
			char *v = value(kf, group, keys[i]);
			struct price *price = v != NULL ? parse_price(v) : NULL;
			if (price != NULL) {
				g_hash_table_insert(p->prices, g_strdup(keys[i] + 6), price);
			} else if (bad_price == NULL) {
				bad_price = g_strdup(keys[i]);
			}
			g_free(v);
		}
	}
	g_strfreev(keys);

	const struct known *k = NULL;
	for (size_t i = 0; p->provider != NULL && i < G_N_ELEMENTS(known); i++) {
		if (strcmp(known[i].name, p->provider) == 0) {
			k = &known[i];
		}
	}
	if (k == NULL) {
		p->problem = p->provider == NULL ? g_strdup("no provider") :
			g_strdup_printf("unknown provider \"%s\"", p->provider);
		g_free(url);
		g_free(bad_price);
		return p;
	}
	p->kind = k->kind;
	p->needs_key = k->needs_key;
	p->structured = k->structured;
	p->stream_usage = k->stream_usage;
	if (url != NULL) {
		p->url = url;
	} else if (strcmp(k->name, "cloudflare") == 0) {
		char *account = value(kf, group, "account");
		char *gateway = value(kf, group, "gateway");
		if (account != NULL) {
			p->url = g_strdup_printf(
				"https://gateway.ai.cloudflare.com/v1/%s/%s/compat", account,
				gateway != NULL ? gateway : "default");
		}
		g_free(account);
		g_free(gateway);
	} else if (k->url != NULL) {
		p->url = g_strdup(k->url);
	}
	/* A trailing slash would make "//chat/completions". */
	if (p->url != NULL && g_str_has_suffix(p->url, "/")) {
		p->url[strlen(p->url) - 1] = '\0';
	}

	/* Token counts in a stream are asked for with a field that not every
	 * OpenAI-compatible server takes; a profile can say its does. */
	char *usage = value(kf, group, "stream-usage");
	if (usage != NULL) {
		p->stream_usage = g_ascii_strcasecmp(usage, "true") == 0 ||
			g_ascii_strcasecmp(usage, "yes") == 0 || strcmp(usage, "1") == 0;
		g_free(usage);
	}

	char *structured = value(kf, group, "structured-output");
	if (structured != NULL) {
		p->structured = strcmp(structured, "prompt") == 0 ? STRUCTURED_PROMPT :
			STRUCTURED_NATIVE;
		g_free(structured);
	}

	if (p->url == NULL) {
		p->problem = g_strdup(strcmp(k->name, "cloudflare") == 0 ?
			"no account (Cloudflare's account ID)" : "no url");
	} else if (p->model == NULL && g_hash_table_size(p->tiers) == 0 &&
			p->classifier == NULL) {
		p->problem = g_strdup("no model");
	} else if (p->needs_key && p->key_command == NULL && p->key_env == NULL) {
		p->problem = g_strdup("no api-key-command or api-key-env");
	} else if (bad_price != NULL) {
		p->problem = g_strdup_printf("%s isn't two or three numbers: "
			"dollars per million input and output (and cached input) tokens",
			bad_price);
	}
	g_free(bad_price);
	return p;
}

struct config *config_load(const char *path, char **error) {
	struct config *c = g_new0(struct config, 1);
	c->refs = 1;
	c->enabled = true;
	c->profiles = g_ptr_array_new_with_free_func(profile_free);
	c->apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, app_free);

	GKeyFile *kf = g_key_file_new();
	GError *err = NULL;
	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
		if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
				error != NULL) {
			*error = g_strdup_printf("%s: %s", path, err->message);
		}
		g_error_free(err);
		g_key_file_free(kf);
		return c;
	}
	char *enabled = value(kf, "augur", "enabled");
	if (enabled != NULL) {
		c->enabled = g_ascii_strcasecmp(enabled, "false") != 0 &&
			g_ascii_strcasecmp(enabled, "no") != 0 && strcmp(enabled, "0") != 0;
		g_free(enabled);
	}
	c->default_profile = value(kf, "augur", "default-profile");
	c->classify_profile = value(kf, "augur", "classify-profile");

	char **groups = g_key_file_get_groups(kf, NULL);
	for (int i = 0; groups[i] != NULL; i++) {
		const char *g = groups[i];
		if (g_str_has_prefix(g, "profile ")) {
			char *name = g_strstrip(g_strdup(g + 8));
			g_ptr_array_add(c->profiles, profile_load(kf, g, name));
			g_free(name);
		} else if (g_str_has_prefix(g, "app ")) {
			struct app_settings *a = g_new0(struct app_settings, 1);
			a->profile = value(kf, g, "profile");
			a->model = value(kf, g, "model");
			a->tier = value(kf, g, "tier");
			a->classify_profile = value(kf, g, "classify-profile");
			char *id = g_strstrip(g_strdup(g + 4));
			g_hash_table_replace(c->apps, id, a);
		}
	}
	g_strfreev(groups);
	g_key_file_free(kf);
	return c;
}

struct config *config_ref(struct config *c) {
	c->refs++;
	return c;
}

void config_unref(struct config *c) {
	if (c == NULL || --c->refs > 0) {
		return;
	}
	g_free(c->default_profile);
	g_free(c->classify_profile);
	g_ptr_array_unref(c->profiles);
	g_hash_table_unref(c->apps);
	g_free(c);
}

struct profile *config_profile(struct config *c, const char *name) {
	for (guint i = 0; name != NULL && i < c->profiles->len; i++) {
		struct profile *p = c->profiles->pdata[i];
		if (strcmp(p->name, name) == 0) {
			return p;
		}
	}
	return NULL;
}

struct profile *config_default_profile(struct config *c) {
	if (c->default_profile != NULL) {
		return config_profile(c, c->default_profile);
	}
	for (guint i = 0; i < c->profiles->len; i++) {
		struct profile *p = c->profiles->pdata[i];
		if (p->problem == NULL) {
			return p;
		}
	}
	return NULL;
}

const struct app_settings *config_app(struct config *c, const char *app_id) {
	return app_id != NULL ? g_hash_table_lookup(c->apps, app_id) : NULL;
}

const char *config_classify_profile(struct config *c, const char *app_id) {
	const struct app_settings *a = config_app(c, app_id);
	return a != NULL && a->classify_profile != NULL ? a->classify_profile :
		c->classify_profile;
}
