/*
 * augur: Augur from the shell. Asks augurd over D-Bus, as any program
 * would.
 *
 *   augur status                    on or off, and why
 *   augur profiles                  the profiles, and what's wrong with any
 *   augur models [PROFILE]          the models a provider lists
 *   augur usage [OPTIONS]           requests, tokens and what they cost
 *   augur ask [OPTIONS] [PROMPT]    an answer, as it's written (the prompt
 *                                   from stdin if not given)
 *
 * With --tools FILE, ask offers the model tools, each a shell command: a
 * JSON array of {name, description, schema, command}. A call's arguments
 * (JSON) go to the command on stdin; what it prints is the result, and if
 * it fails, what it printed to stderr is why.
 */
#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BUS_NAME "io.github.matthewp.Augur"
#define OBJECT_PATH "/io/github/matthewp/Augur"
#define IFACE "io.github.matthewp.Augur1"
#define REQUEST_IFACE IFACE ".Request"

static GDBusConnection *bus;

static GVariant *call(const char *method, GVariant *args, const char *type,
		GError **error) {
	return g_dbus_connection_call_sync(bus, BUS_NAME, OBJECT_PATH, IFACE, method,
		args, G_VARIANT_TYPE(type), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
}

/* An error as a person would want it: Augur's own name, short. */
static void print_error(GError *error) {
	char *remote = g_dbus_error_get_remote_error(error);
	g_dbus_error_strip_remote_error(error);
	const char *name = remote != NULL ? strrchr(remote, '.') : NULL;
	if (name != NULL && remote != NULL &&
			g_str_has_prefix(remote, IFACE ".Error.")) {
		g_printerr("augur: %s: %s\n", name + 1, error->message);
	} else {
		g_printerr("augur: %s\n", error->message);
	}
	g_free(remote);
}

/* ---- status, profiles, models ------------------------------------------------ */

static int cmd_status(void) {
	GError *error = NULL;
	GVariant *r = call("Status", NULL, "(a{sv})", &error);
	if (r == NULL) {
		print_error(error);
		g_error_free(error);
		return 1;
	}
	GVariant *s = g_variant_get_child_value(r, 0);
	gboolean enabled = FALSE;
	const char *config = "", *problem = NULL, *def = NULL;
	g_variant_lookup(s, "enabled", "b", &enabled);
	g_variant_lookup(s, "config", "&s", &config);
	g_variant_lookup(s, "problem", "&s", &problem);
	g_variant_lookup(s, "default-profile", "&s", &def);
	printf("%s\n", enabled ? "on" : "off");
	printf("config: %s\n", config);
	if (def != NULL) {
		printf("default profile: %s\n", def);
	}
	if (problem != NULL) {
		printf("problem: %s\n", problem);
	}
	g_variant_unref(s);
	g_variant_unref(r);
	return enabled ? 0 : 1;
}

static int cmd_profiles(void) {
	GError *error = NULL;
	GVariant *r = call("ListProfiles", NULL, "(a(sssas))", &error);
	if (r == NULL) {
		print_error(error);
		g_error_free(error);
		return 1;
	}
	GVariantIter *it;
	const char *name, *provider, *problem;
	GVariantIter *tiers;
	g_variant_get(r, "(a(sssas))", &it);
	while (g_variant_iter_next(it, "(&s&s&sas)", &name, &provider, &problem,
			&tiers)) {
		printf("%s (%s)", name, provider);
		const char *tier;
		bool first = true;
		while (g_variant_iter_next(tiers, "&s", &tier)) {
			printf("%s%s", first ? "  tiers: " : ", ", tier);
			first = false;
		}
		if (problem[0] != '\0') {
			printf("  -- can't be used: %s", problem);
		}
		printf("\n");
		g_variant_iter_free(tiers);
	}
	g_variant_iter_free(it);
	g_variant_unref(r);
	return 0;
}

static int cmd_models(const char *profile) {
	GError *error = NULL;
	GVariant *r = call("ListModels", g_variant_new("(s)", profile ? profile : ""),
		"(as)", &error);
	if (r == NULL) {
		print_error(error);
		g_error_free(error);
		return 1;
	}
	GVariantIter *it;
	const char *model;
	g_variant_get(r, "(as)", &it);
	while (g_variant_iter_next(it, "&s", &model)) {
		printf("%s\n", model);
	}
	g_variant_iter_free(it);
	g_variant_unref(r);
	return 0;
}

/* Dollars: cents for what's over a dollar, else enough places to show it. */
static char *format_cost(double cost) {
	if (cost >= 1 || cost == 0) {
		return g_strdup_printf("$%.2f", cost);
	}
	char *s = g_strdup_printf("$%.6f", cost);
	size_t n = strlen(s);
	while (n > 5 && s[n - 1] == '0') {   /* $0.00 at least */
		s[--n] = '\0';
	}
	return s;
}

/* 1234567 as 1,234,567. */
static char *format_count(gint64 n) {
	char *digits = g_strdup_printf("%" G_GINT64_FORMAT, n);
	GString *s = g_string_new(NULL);
	size_t len = strlen(digits);
	for (size_t i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0 && digits[i - 1] != '-') {
			g_string_append_c(s, ',');
		}
		g_string_append_c(s, digits[i]);
	}
	g_free(digits);
	return g_string_free(s, FALSE);
}

/* ---- usage ----------------------------------------------------------------- */

/* A time as the shell says it: today, yesterday, week (the last seven
 * days), month (since the 1st), year, all, or a date (YYYY-MM-DD). */
static bool parse_when(const char *when, gint64 *out) {
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *today = g_date_time_new_local(g_date_time_get_year(now),
		g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
	GDateTime *t = NULL;
	int y, m, d;
	char extra;
	if (strcmp(when, "all") == 0) {
		*out = G_MININT64;
	} else if (strcmp(when, "today") == 0) {
		t = g_date_time_ref(today);
	} else if (strcmp(when, "yesterday") == 0) {
		t = g_date_time_add_days(today, -1);
	} else if (strcmp(when, "week") == 0) {
		t = g_date_time_add_days(today, -6);
	} else if (strcmp(when, "month") == 0) {
		t = g_date_time_new_local(g_date_time_get_year(now),
			g_date_time_get_month(now), 1, 0, 0, 0);
	} else if (strcmp(when, "year") == 0) {
		t = g_date_time_new_local(g_date_time_get_year(now), 1, 1, 0, 0, 0);
	} else if (sscanf(when, "%4d-%2d-%2d%c", &y, &m, &d, &extra) == 3) {
		t = g_date_time_new_local(y, m, d, 0, 0, 0);
	}
	bool ok = t != NULL || strcmp(when, "all") == 0;
	if (t != NULL) {
		*out = g_date_time_to_unix(t);
		g_date_time_unref(t);
	}
	g_date_time_unref(today);
	g_date_time_unref(now);
	return ok;
}

static int cmd_usage(int argc, char *argv[]) {
	char *since = NULL, *until = NULL, *by = NULL, *app = NULL;
	GOptionEntry entries[] = {
		{ "since", 0, 0, G_OPTION_ARG_STRING, &since,
			"From: today, yesterday, week, month (the default), year, all, "
			"or YYYY-MM-DD", "WHEN" },
		{ "until", 0, 0, G_OPTION_ARG_STRING, &until,
			"Up to (not including), the same way", "WHEN" },
		{ "by", 0, 0, G_OPTION_ARG_STRING, &by,
			"Added up by app (the default), profile, model or day; or several, "
			"with commas; or none", "KEYS" },
		{ "app", 0, 0, G_OPTION_ARG_STRING, &app, "Only this program's", "ID" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GOptionContext *ctx = g_option_context_new("usage");
	g_option_context_add_main_entries(ctx, entries, NULL);
	GError *error = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("augur: %s\n", error->message);
		return 2;
	}
	g_option_context_free(ctx);
	gint64 from, to = G_MAXINT64;
	if (!parse_when(since != NULL ? since : "month", &from) ||
			(until != NULL && !parse_when(until, &to))) {
		g_printerr("augur: a time is today, yesterday, week, month, year, all "
			"or YYYY-MM-DD\n");
		return 2;
	}
	char **keys = strcmp(by != NULL ? by : "app", "none") == 0 ?
		g_new0(char *, 1) : g_strsplit(by != NULL ? by : "app", ",", -1);

	GVariantBuilder q;
	g_variant_builder_init(&q, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&q, "{sv}", "since", g_variant_new_int64(from));
	g_variant_builder_add(&q, "{sv}", "until", g_variant_new_int64(to));
	g_variant_builder_add(&q, "{sv}", "by",
		g_variant_new_strv((const char *const *)keys, -1));
	if (app != NULL) {
		g_variant_builder_add(&q, "{sv}", "app-id", g_variant_new_string(app));
	}
	GVariant *r = call("Usage", g_variant_new("(a{sv})", &q), "(aa{sv})", &error);
	if (r == NULL) {
		print_error(error);
		g_error_free(error);
		g_strfreev(keys);
		return 1;
	}

	/* A table: the keys, then the numbers, and a total under several. */
	GVariant *rows = g_variant_get_child_value(r, 0);
	gsize n = g_variant_n_children(rows);
	int nkeys = g_strv_length(keys);
	GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
	gint64 total[5] = { 0 };   /* requests, input, output, cached, unpriced */
	double total_cost = 0;
	const char *heads[] = { "requests", "input", "output", "cached", "cost" };
	for (int k = 0; k < nkeys; k++) {
		g_ptr_array_add(cells, g_strdup(keys[k]));
	}
	for (int i = 0; i < 5; i++) {
		g_ptr_array_add(cells, g_strdup(heads[i]));
	}
	for (gsize i = 0; i <= n; i++) {
		bool is_total = i == n;
		if (is_total && n < 2) {
			break;
		}
		gint64 v[5] = { 0 };
		double cost = 0;
		if (is_total) {
			memcpy(v, total, sizeof v);
			cost = total_cost;
		} else {
			GVariant *row = g_variant_get_child_value(rows, i);
			g_variant_lookup(row, "requests", "x", &v[0]);
			g_variant_lookup(row, "input-tokens", "x", &v[1]);
			g_variant_lookup(row, "output-tokens", "x", &v[2]);
			g_variant_lookup(row, "cached-tokens", "x", &v[3]);
			g_variant_lookup(row, "unpriced", "x", &v[4]);
			g_variant_lookup(row, "cost", "d", &cost);
			for (int k = 0; k < nkeys; k++) {
				const char *s = "";
				g_variant_lookup(row, keys[k], "&s", &s);
				g_ptr_array_add(cells, g_strdup(s[0] != '\0' ? s : "-"));
			}
			g_variant_unref(row);
			for (int j = 0; j < 5; j++) {
				total[j] += v[j];
			}
			total_cost += cost;
		}
		for (int k = 0; is_total && k < nkeys; k++) {
			g_ptr_array_add(cells, g_strdup(k == 0 ? "total" : ""));
		}
		for (int j = 0; j < 4; j++) {
			g_ptr_array_add(cells, format_count(v[j]));
		}
		char *c = format_cost(cost);
		g_ptr_array_add(cells, v[4] > 0 ? g_strdup_printf("%s + %" G_GINT64_FORMAT
			" unpriced", c, v[4]) : g_strdup(c));
		g_free(c);
	}
	int ncols = nkeys + 5;
	guint nrows = cells->len / ncols;
	int *width = g_new0(int, ncols);
	for (guint i = 0; i < cells->len; i++) {
		width[i % ncols] = MAX(width[i % ncols],
			(int)g_utf8_strlen(cells->pdata[i], -1));
	}
	for (guint row = 0; n > 0 && row < nrows; row++) {
		for (int col = 0; col < ncols; col++) {
			const char *cell = cells->pdata[row * ncols + col];
			/* Keys to the left; numbers to the right; cost as it is. */
			if (col < nkeys) {
				printf("%-*s  ", width[col], cell);
			} else if (col < ncols - 1) {
				printf("%*s  ", width[col], cell);
			} else {
				printf("%s\n", cell);
			}
		}
	}
	if (n == 0) {
		printf("nothing asked\n");
	}
	g_free(width);
	g_ptr_array_unref(cells);
	g_variant_unref(rows);
	g_variant_unref(r);
	g_strfreev(keys);
	return 0;
}

/* ---- ask ------------------------------------------------------------------- */

static struct {
	GMainLoop *loop;
	char *handle;
	bool streamed;
	bool verbose;
	GHashTable *commands;   /* tool name -> its command */
	int status;
} ask;

static void print_info(GVariant *info) {
	if (!ask.verbose) {
		return;
	}
	const char *profile = "", *model = "";
	gint64 in = -1, out = -1;
	gint32 attempts = 1;
	g_variant_lookup(info, "profile", "&s", &profile);
	g_variant_lookup(info, "model", "&s", &model);
	g_variant_lookup(info, "input-tokens", "x", &in);
	g_variant_lookup(info, "output-tokens", "x", &out);
	g_variant_lookup(info, "attempts", "i", &attempts);
	gint32 rounds = 1, calls = 0;
	g_variant_lookup(info, "rounds", "i", &rounds);
	g_variant_lookup(info, "tool-calls", "i", &calls);
	char *tools = calls > 0 ? g_strdup_printf(", %d tool call%s in %d rounds",
		calls, calls == 1 ? "" : "s", rounds) : g_strdup("");
	double cost = -1;
	g_variant_lookup(info, "cost", "d", &cost);
	char *c = cost >= 0 ? format_cost(cost) : NULL;
	char *costs = c != NULL ? g_strdup_printf(", %s", c) : g_strdup("");
	g_printerr("[%s, %s: %" G_GINT64_FORMAT " in, %" G_GINT64_FORMAT
		" out%s%s%s]\n", profile, model, in, out, costs,
		attempts > 1 ? ", asked twice" : "", tools);
	g_free(tools);
	g_free(costs);
	g_free(c);
}

/* ---- ask: tools ------------------------------------------------------------ */

struct tool_run {
	char *id;
};

static void tool_ran(GObject *src, GAsyncResult *res, gpointer data) {
	struct tool_run *run = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
		&err, &error);
	const char *method = "ToolDone";
	char *content;
	if (!ok) {
		method = "ToolFailed";
		content = g_strdup(error->message);
	} else if (!g_subprocess_get_successful(G_SUBPROCESS(src))) {
		method = "ToolFailed";
		content = err != NULL && g_strstrip(err)[0] != '\0' ? g_strdup(err) :
			g_strdup_printf("it exited with %d",
				g_subprocess_get_exit_status(G_SUBPROCESS(src)));
	} else {
		content = g_strdup(out != NULL ? g_strchomp(out) : "");
	}
	if (ask.handle != NULL) {
		g_dbus_connection_call(bus, BUS_NAME, ask.handle, REQUEST_IFACE, method,
			g_variant_new("(ss)", run->id, content), NULL, G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, NULL, NULL);
	}
	g_clear_error(&error);
	g_free(content);
	g_free(out);
	g_free(err);
	g_free(run->id);
	g_free(run);
}

static void tool_call(const char *id, const char *name, const char *arguments) {
	if (ask.verbose) {
		g_printerr("[%s %s]\n", name, arguments);
	}
	const char *command = g_hash_table_lookup(ask.commands, name);
	GError *error = NULL;
	const char *argv[] = { "/bin/sh", "-c", command, NULL };
	GSubprocess *p = command == NULL ? NULL : g_subprocess_newv(argv,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	if (p == NULL) {
		g_dbus_connection_call(bus, BUS_NAME, ask.handle, REQUEST_IFACE,
			"ToolFailed", g_variant_new("(ss)", id, error != NULL ? error->message :
				"no such tool"), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
		g_clear_error(&error);
		return;
	}
	struct tool_run *run = g_new0(struct tool_run, 1);
	run->id = g_strdup(id);
	g_subprocess_communicate_utf8_async(p, arguments, NULL, tool_ran, run);
	g_object_unref(p);
}

/* The tools file as the request's tools, their commands kept. */
static GVariant *load_tools(const char *path) {
	GError *error = NULL;
	JsonParser *parser = json_parser_new();
	if (!json_parser_load_from_file(parser, path, &error)) {
		g_printerr("augur: %s\n", error->message);
		g_error_free(error);
		g_object_unref(parser);
		return NULL;
	}
	JsonNode *root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
		g_printerr("augur: %s isn't a JSON array of tools\n", path);
		g_object_unref(parser);
		return NULL;
	}
	JsonArray *a = json_node_get_array(root);
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
	for (guint i = 0; i < json_array_get_length(a); i++) {
		JsonNode *n = json_array_get_element(a, i);
		JsonObject *t = JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
		const char *name = t != NULL ?
			json_object_get_string_member_with_default(t, "name", NULL) : NULL;
		const char *command = t != NULL ?
			json_object_get_string_member_with_default(t, "command", NULL) : NULL;
		if (name == NULL || command == NULL) {
			g_printerr("augur: %s: each tool needs a name and a command\n", path);
			g_variant_builder_clear(&b);
			g_object_unref(parser);
			return NULL;
		}
		g_hash_table_insert(ask.commands, g_strdup(name), g_strdup(command));
		GVariantBuilder tool;
		g_variant_builder_init(&tool, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&tool, "{sv}", "name", g_variant_new_string(name));
		const char *description = json_object_get_string_member_with_default(t,
			"description", NULL);
		if (description != NULL) {
			g_variant_builder_add(&tool, "{sv}", "description",
				g_variant_new_string(description));
		}
		/* The schema as JSON, or a string of it. */
		JsonNode *schema = json_object_get_member(t, "schema");
		if (schema != NULL) {
			char *text = JSON_NODE_HOLDS_VALUE(schema) ?
				g_strdup(json_node_get_string(schema)) : json_to_string(schema, FALSE);
			g_variant_builder_add(&tool, "{sv}", "schema",
				g_variant_new_string(text != NULL ? text : ""));
			g_free(text);
		}
		g_variant_builder_add_value(&b, g_variant_builder_end(&tool));
	}
	g_object_unref(parser);
	return g_variant_builder_end(&b);
}

static void on_signal(GDBusConnection *c, const char *sender, const char *path,
		const char *iface, const char *signal, GVariant *params, gpointer data) {
	if (ask.handle == NULL || strcmp(path, ask.handle) != 0) {
		return;
	}
	if (strcmp(signal, "Delta") == 0) {
		const char *text;
		g_variant_get(params, "(&s)", &text);
		fputs(text, stdout);
		fflush(stdout);
		ask.streamed = true;
	} else if (strcmp(signal, "Done") == 0) {
		const char *text;
		GVariant *info;
		g_variant_get(params, "(&s@a{sv})", &text, &info);
		/* A structured answer is printed checked, not as it streamed. */
		if (!ask.streamed) {
			fputs(text, stdout);
		}
		printf("\n");
		print_info(info);
		g_variant_unref(info);
		g_main_loop_quit(ask.loop);
	} else if (strcmp(signal, "ToolCall") == 0) {
		const char *id, *name, *arguments;
		g_variant_get(params, "(&s&s&s)", &id, &name, &arguments);
		tool_call(id, name, arguments);
	} else if (strcmp(signal, "Failed") == 0) {
		const char *name, *message;
		g_variant_get(params, "(&s&s)", &name, &message);
		if (ask.streamed) {
			printf("\n");
		}
		const char *short_name = strrchr(name, '.');
		g_printerr("augur: %s: %s\n", short_name ? short_name + 1 : name, message);
		ask.status = 1;
		g_main_loop_quit(ask.loop);
	}
}

static gboolean interrupted(gpointer data) {
	if (ask.handle != NULL) {
		g_dbus_connection_call_sync(bus, BUS_NAME, ask.handle, REQUEST_IFACE,
			"Cancel", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	}
	ask.status = 130;
	g_main_loop_quit(ask.loop);
	return G_SOURCE_REMOVE;
}

static char *read_stdin(void) {
	GString *s = g_string_new(NULL);
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) {
		g_string_append_len(s, buf, n);
	}
	return g_string_free(s, FALSE);
}

static void add_message(GVariantBuilder *b, const char *role,
		const char *content) {
	GVariantBuilder m;
	g_variant_builder_init(&m, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&m, "{sv}", "role", g_variant_new_string(role));
	g_variant_builder_add(&m, "{sv}", "content", g_variant_new_string(content));
	g_variant_builder_add_value(b, g_variant_builder_end(&m));
}

static int cmd_ask(int argc, char *argv[]) {
	char *profile = NULL, *model = NULL, *tier = NULL, *system = NULL;
	char *schema = NULL, *app_id = NULL, *tools = NULL;
	int max_rounds = 0;
	gboolean no_stream = FALSE, verbose = FALSE;
	GOptionEntry entries[] = {
		{ "profile", 'p', 0, G_OPTION_ARG_STRING, &profile, "Profile", "NAME" },
		{ "model", 'm', 0, G_OPTION_ARG_STRING, &model, "Model", "MODEL" },
		{ "tier", 't', 0, G_OPTION_ARG_STRING, &tier, "Tier", "TIER" },
		{ "system", 's', 0, G_OPTION_ARG_STRING, &system, "System prompt",
			"TEXT" },
		{ "schema", 0, 0, G_OPTION_ARG_STRING, &schema,
			"Answer in JSON matching this JSON Schema (or @FILE)", "SCHEMA" },
		{ "tools", 0, 0, G_OPTION_ARG_FILENAME, &tools,
			"Offer the model tools, each a command, from a JSON file: "
			"[{name, description, schema, command}]", "FILE" },
		{ "max-rounds", 0, 0, G_OPTION_ARG_INT, &max_rounds,
			"Turns with tools before it must answer", "N" },
		{ "no-stream", 0, 0, G_OPTION_ARG_NONE, &no_stream,
			"Print the answer when it's all there", NULL },
		{ "app-id", 0, 0, G_OPTION_ARG_STRING, &app_id,
			"Ask as this program (for its [app] settings)", "ID" },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			"Say which model answered, and the tokens", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GOptionContext *ctx = g_option_context_new("ask [PROMPT...]");
	g_option_context_add_main_entries(ctx, entries, NULL);
	GError *error = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("augur: %s\n", error->message);
		return 2;
	}
	g_option_context_free(ctx);
	char *prompt = argc > 1 ? g_strjoinv(" ", argv + 1) : read_stdin();
	if (g_strstrip(prompt)[0] == '\0') {
		g_printerr("augur: nothing to ask\n");
		return 2;
	}
	if (schema != NULL && schema[0] == '@') {
		char *text = NULL;
		if (!g_file_get_contents(schema + 1, &text, NULL, &error)) {
			g_printerr("augur: %s\n", error->message);
			return 2;
		}
		g_free(schema);
		schema = text;
	}

	ask.commands = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GVariant *tool_list = NULL;
	if (tools != NULL && (tool_list = load_tools(tools)) == NULL) {
		return 2;
	}

	GVariantBuilder messages;
	g_variant_builder_init(&messages, G_VARIANT_TYPE("aa{sv}"));
	if (system != NULL) {
		add_message(&messages, "system", system);
	}
	add_message(&messages, "user", prompt);
	GVariantBuilder req;
	g_variant_builder_init(&req, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&req, "{sv}", "app-id",
		g_variant_new_string(app_id != NULL ? app_id : "augur"));
	g_variant_builder_add(&req, "{sv}", "messages",
		g_variant_builder_end(&messages));
	if (profile != NULL) {
		g_variant_builder_add(&req, "{sv}", "profile", g_variant_new_string(profile));
	}
	if (model != NULL) {
		g_variant_builder_add(&req, "{sv}", "model", g_variant_new_string(model));
	}
	if (tier != NULL) {
		g_variant_builder_add(&req, "{sv}", "tier", g_variant_new_string(tier));
	}
	if (schema != NULL) {
		g_variant_builder_add(&req, "{sv}", "schema", g_variant_new_string(schema));
	}
	if (tool_list != NULL) {
		g_variant_builder_add(&req, "{sv}", "tools", tool_list);
	}
	if (max_rounds > 0) {
		g_variant_builder_add(&req, "{sv}", "max-rounds",
			g_variant_new_uint32(max_rounds));
	}
	/* A structured answer's JSON as it's written isn't worth watching. */
	g_variant_builder_add(&req, "{sv}", "stream",
		g_variant_new_boolean(!no_stream && schema == NULL));

	ask.loop = g_main_loop_new(NULL, FALSE);
	ask.verbose = verbose;
	/* Listening first: a short answer can be done before Complete's reply
	 * has been read. */
	g_dbus_connection_signal_subscribe(bus, BUS_NAME, REQUEST_IFACE, NULL, NULL,
		NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, NULL, NULL);
	GVariant *r = call("Complete", g_variant_new("(a{sv})", &req), "(o)", &error);
	if (r == NULL) {
		print_error(error);
		g_error_free(error);
		return 1;
	}
	g_variant_get(r, "(o)", &ask.handle);
	g_variant_unref(r);
	g_unix_signal_add(SIGINT, interrupted, NULL);
	g_main_loop_run(ask.loop);
	return ask.status;
}

static void usage(FILE *to) {
	fputs("Usage: augur status\n"
		"       augur profiles\n"
		"       augur models [PROFILE]\n"
		"       augur usage [--since WHEN] [--until WHEN] [--by KEYS] [--app ID]\n"
		"       augur ask [-p PROFILE] [-m MODEL] [-t TIER] [-s SYSTEM]\n"
		"                 [--schema JSON|@FILE] [--tools FILE] [--max-rounds N]\n"
		"                 [--no-stream] [-v] [PROMPT...]\n"
		"\n"
		"Asks Augur's service (augurd) over D-Bus. The prompt is read from\n"
		"stdin when it isn't given.\n", to);
}

int main(int argc, char *argv[]) {
	if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		usage(argc < 2 ? stderr : stdout);
		return argc < 2 ? 2 : 0;
	}
	if (strcmp(argv[1], "--version") == 0) {
		printf("augur %s\n", AUGUR_VERSION);
		return 0;
	}
	GError *error = NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (bus == NULL) {
		g_printerr("augur: no session bus: %s\n", error->message);
		return 1;
	}
	const char *cmd = argv[1];
	if (strcmp(cmd, "status") == 0) {
		return cmd_status();
	}
	if (strcmp(cmd, "profiles") == 0) {
		return cmd_profiles();
	}
	if (strcmp(cmd, "models") == 0) {
		return cmd_models(argc > 2 ? argv[2] : NULL);
	}
	if (strcmp(cmd, "ask") == 0) {
		return cmd_ask(argc - 1, argv + 1);
	}
	if (strcmp(cmd, "usage") == 0) {
		return cmd_usage(argc - 1, argv + 1);
	}
	g_printerr("augur: no command \"%s\"\n", cmd);
	usage(stderr);
	return 2;
}
