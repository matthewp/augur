/*
 * augur: Augur from the shell. Asks augurd over D-Bus, as any program
 * would.
 *
 *   augur status                    on or off, and why
 *   augur profiles                  the profiles, and what's wrong with any
 *   augur models [PROFILE]          the models a provider lists
 *   augur ask [OPTIONS] [PROMPT]    an answer, as it's written (the prompt
 *                                   from stdin if not given)
 */
#include <gio/gio.h>
#include <glib-unix.h>
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

/* ---- ask ------------------------------------------------------------------- */

static struct {
	GMainLoop *loop;
	char *handle;
	bool streamed;
	bool verbose;
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
	g_printerr("[%s, %s: %" G_GINT64_FORMAT " in, %" G_GINT64_FORMAT
		" out%s]\n", profile, model, in, out, attempts > 1 ? ", asked twice" : "");
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
	char *schema = NULL, *app_id = NULL;
	gboolean no_stream = FALSE, verbose = FALSE;
	GOptionEntry entries[] = {
		{ "profile", 'p', 0, G_OPTION_ARG_STRING, &profile, "Profile", "NAME" },
		{ "model", 'm', 0, G_OPTION_ARG_STRING, &model, "Model", "MODEL" },
		{ "tier", 't', 0, G_OPTION_ARG_STRING, &tier, "Tier", "TIER" },
		{ "system", 's', 0, G_OPTION_ARG_STRING, &system, "System prompt",
			"TEXT" },
		{ "schema", 0, 0, G_OPTION_ARG_STRING, &schema,
			"Answer in JSON matching this JSON Schema (or @FILE)", "SCHEMA" },
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
		"       augur ask [-p PROFILE] [-m MODEL] [-t TIER] [-s SYSTEM]\n"
		"                 [--schema JSON|@FILE] [--no-stream] [-v] [PROMPT...]\n"
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
	g_printerr("augur: no command \"%s\"\n", cmd);
	usage(stderr);
	return 2;
}
