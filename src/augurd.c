/*
 * augurd: Augur's service, on the session bus as org.gemwm.Augur.
 * D-Bus starts it when a program first asks, and it goes when it's had
 * nothing to do for a while. See README.md for the interface.
 *
 * A request finds its profile and model, waits its turn in the profile's
 * queue (someone waiting to read an answer goes ahead of background work),
 * gets the key, and goes to the provider (provider.c); its text comes back
 * as Delta signals and then Done, to the program that asked alone. An
 * answer with a schema is checked (schema.c) and asked for once more if it
 * doesn't match.
 *
 * A request can bring tools. When the model calls them, the calls go to
 * the program as ToolCall signals, the request gives up its place with
 * the provider, and it waits for the program's ToolDone or ToolFailed for
 * each; then it's next in its profile's queue, and the model is asked
 * again with what the tools said.
 *
 * Classify asks questions about an input: of the profile's classifier
 * over the System One API (provider.c), or else of its chat model, the
 * questions made a schema (classify.c); the same answers either way.
 */
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include "classify.h"
#include "config.h"
#include "provider.h"
#include "schema.h"

#define BUS_NAME "org.gemwm.Augur"
#define OBJECT_PATH "/org/gemwm/Augur"
#define REQUEST_PATH OBJECT_PATH "/request"
#define IFACE "org.gemwm.Augur1"
#define REQUEST_IFACE IFACE ".Request"
#define ERROR_PREFIX IFACE ".Error."
#define IDLE_TIMEOUT 60     /* seconds with nothing to do, then exit */
#define MAX_ROUNDS 8        /* turns with tools, unless the request says */
#define RESERVED_TOOL "answer" /* provider.c's, for structured answers */

static const char introspection[] =
	"<node>"
	" <interface name='" IFACE "'>"
	"  <property name='Enabled' type='b' access='read'/>"
	"  <method name='Complete'>"
	"   <arg name='request' type='a{sv}' direction='in'/>"
	"   <arg name='handle' type='o' direction='out'/>"
	"  </method>"
	"  <method name='Ask'>"
	"   <arg name='request' type='a{sv}' direction='in'/>"
	"   <arg name='text' type='s' direction='out'/>"
	"   <arg name='info' type='a{sv}' direction='out'/>"
	"  </method>"
	"  <method name='Classify'>"
	"   <arg name='request' type='a{sv}' direction='in'/>"
	"   <arg name='answers' type='a{sv}' direction='out'/>"
	"   <arg name='info' type='a{sv}' direction='out'/>"
	"  </method>"
	"  <method name='Status'>"
	"   <arg name='status' type='a{sv}' direction='out'/>"
	"  </method>"
	"  <method name='ListProfiles'>"
	"   <arg name='profiles' type='a(sssas)' direction='out'/>"
	"  </method>"
	"  <method name='ListModels'>"
	"   <arg name='profile' type='s' direction='in'/>"
	"   <arg name='models' type='as' direction='out'/>"
	"  </method>"
	"  <method name='Usage'>"
	"   <arg name='query' type='a{sv}' direction='in'/>"
	"   <arg name='rows' type='aa{sv}' direction='out'/>"
	"  </method>"
	" </interface>"
	" <interface name='" REQUEST_IFACE "'>"
	"  <signal name='Delta'><arg name='text' type='s'/></signal>"
	"  <signal name='Done'>"
	"   <arg name='text' type='s'/><arg name='info' type='a{sv}'/>"
	"  </signal>"
	"  <signal name='Failed'>"
	"   <arg name='error' type='s'/><arg name='message' type='s'/>"
	"  </signal>"
	"  <signal name='ToolCall'>"
	"   <arg name='id' type='s'/><arg name='name' type='s'/>"
	"   <arg name='arguments' type='s'/>"
	"  </signal>"
	"  <method name='Cancel'/>"
	"  <method name='ToolDone'>"
	"   <arg name='id' type='s' direction='in'/>"
	"   <arg name='result' type='s' direction='in'/>"
	"  </method>"
	"  <method name='ToolFailed'>"
	"   <arg name='id' type='s' direction='in'/>"
	"   <arg name='message' type='s' direction='in'/>"
	"  </method>"
	" </interface>"
	"</node>";

enum augur_error {
	AUGUR_ERROR_DISABLED,
	AUGUR_ERROR_NO_PROFILE,
	AUGUR_ERROR_NO_MODEL,
	AUGUR_ERROR_AUTH,
	AUGUR_ERROR_RATE_LIMITED,
	AUGUR_ERROR_PROVIDER,
	AUGUR_ERROR_SCHEMA,
	AUGUR_ERROR_CANCELLED,
};

static const GDBusErrorEntry error_entries[] = {
	{ AUGUR_ERROR_DISABLED, ERROR_PREFIX "Disabled" },
	{ AUGUR_ERROR_NO_PROFILE, ERROR_PREFIX "NoProfile" },
	{ AUGUR_ERROR_NO_MODEL, ERROR_PREFIX "NoModel" },
	{ AUGUR_ERROR_AUTH, ERROR_PREFIX "Auth" },
	{ AUGUR_ERROR_RATE_LIMITED, ERROR_PREFIX "RateLimited" },
	{ AUGUR_ERROR_PROVIDER, ERROR_PREFIX "Provider" },
	{ AUGUR_ERROR_SCHEMA, ERROR_PREFIX "Schema" },
	{ AUGUR_ERROR_CANCELLED, ERROR_PREFIX "Cancelled" },
};

static GQuark augur_error_quark(void) {
	static gsize quark;
	g_dbus_error_register_error_domain("augur-error-quark", &quark,
		error_entries, G_N_ELEMENTS(error_entries));
	return (GQuark)quark;
}
#define AUGUR_ERROR augur_error_quark()

struct request {
	guint id;
	char *path;           /* NULL for Ask */
	guint registration;
	char *sender;
	GDBusMethodInvocation *ask;  /* Ask's or Classify's, to return to */
	bool classify;        /* Classify's: questions about an input */
	bool classifier;      /* ... answered by a classifier, not a chat model */
	char *input;
	JsonObject *questions;
	char *app_id;
	struct config *config;
	struct profile *profile;
	char *model;
	JsonArray *messages;
	JsonNode *schema;
	JsonArray *tools;     /* objects: name, description, schema; or NULL */
	JsonArray *turn;      /* the calls of the turn being answered */
	GHashTable *waiting;  /* their ids still to be answered by the program */
	GHashTable *results;  /* id -> its tool message */
	GPtrArray *called;    /* the names of the tools called, for the log */
	int rounds, max_rounds;
	guint next_call;      /* for calls the provider gave no id */
	gint64 max_tokens;
	double temperature;
	bool stream;
	bool interactive;     /* someone's waiting to read it */
	bool running;         /* has its place: out of the queue */
	bool sender_gone;     /* no one to tell */
	int attempts;
	gint64 input_tokens, output_tokens, cached_tokens;
	double cost;          /* dollars, over every round */
	bool cost_unknown;    /* a round's cost couldn't be known */
	GCancellable *cancel;
};

/* A profile's turn-taking. */
struct queue {
	int running;
	GQueue interactive, background;
};

static struct {
	GMainLoop *loop;
	GDBusConnection *bus;
	GDBusNodeInfo *info;
	struct config *config;
	char *config_error;
	char *config_file;
	char *config_stamp;    /* when GLib can't watch it: how it was */
	GFileMonitor *monitor;
	guint reload_id;
	bool disabled_by_env;
	bool enabled;
	SoupSession *soup;
	GHashTable *requests;  /* id -> struct request */
	GHashTable *queues;    /* profile name -> struct queue */
	GHashTable *keys;      /* profile name -> its key; "name\x01gw" its gateway's */
	guint next_id;
	int busy;              /* requests and listings under way */
	int idle_timeout;
	guint idle_id;
	bool owned;            /* the name was ours */
	int status;            /* to exit with */
} srv;

static void pump(const char *profile);
static gboolean start_soon(gpointer data);

/* ---- Staying, and going ---------------------------------------------------- */

static gboolean idle_exit(gpointer data) {
	srv.idle_id = 0;
	g_main_loop_quit(srv.loop);
	return G_SOURCE_REMOVE;
}

static void busy(int change) {
	srv.busy += change;
	g_clear_handle_id(&srv.idle_id, g_source_remove);
	if (srv.busy == 0 && srv.idle_timeout > 0) {
		srv.idle_id = g_timeout_add_seconds(srv.idle_timeout, idle_exit, NULL);
	}
}

/* ---- The configuration ------------------------------------------------------ */

static bool usable(void) {
	for (guint i = 0; i < srv.config->profiles->len; i++) {
		if (((struct profile *)srv.config->profiles->pdata[i])->problem == NULL) {
			return true;
		}
	}
	return false;
}

static void update_enabled(void) {
	bool enabled = srv.config->enabled && !srv.disabled_by_env && usable();
	if (enabled == srv.enabled) {
		return;
	}
	srv.enabled = enabled;
	if (srv.bus == NULL) {
		return;
	}
	GVariantBuilder changed;
	g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&changed, "{sv}", "Enabled",
		g_variant_new_boolean(enabled));
	g_dbus_connection_emit_signal(srv.bus, NULL, OBJECT_PATH,
		"org.freedesktop.DBus.Properties", "PropertiesChanged",
		g_variant_new("(sa{sv}as)", IFACE, &changed, NULL), NULL);
}

static void load_config(void) {
	char *error = NULL;
	struct config *c = config_load(srv.config_file, &error);
	config_unref(srv.config);
	srv.config = c;
	g_free(srv.config_error);
	srv.config_error = error;
	if (error != NULL) {
		g_warning("%s", error);
	}
	/* A key command may have changed with it. */
	g_hash_table_remove_all(srv.keys);
	update_enabled();
}

static gboolean reload(gpointer data) {
	srv.reload_id = 0;
	load_config();
	return G_SOURCE_REMOVE;
}

static void config_changed(GFileMonitor *m, GFile *file, GFile *other,
		GFileMonitorEvent event, gpointer data) {
	/* Editors write in steps: read it once they've finished. */
	g_clear_handle_id(&srv.reload_id, g_source_remove);
	srv.reload_id = g_timeout_add(200, reload, NULL);
}

/* The config's modification time, size and inode, as text ("" if it
 * isn't there): for looking at it where GLib can't watch files (it can't
 * on some FreeBSDs). */
static char *config_stamp(void) {
	GFile *f = g_file_new_for_path(srv.config_file);
	GFileInfo *i = g_file_query_info(f, G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_UNIX_INODE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
	char *stamp = i == NULL ? g_strdup("") : g_strdup_printf(
		"%" G_GUINT64_FORMAT ".%06u %" G_GOFFSET_FORMAT " %" G_GUINT64_FORMAT,
		g_file_info_get_attribute_uint64(i, G_FILE_ATTRIBUTE_TIME_MODIFIED),
		g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
		g_file_info_get_size(i),
		g_file_info_get_attribute_uint64(i, G_FILE_ATTRIBUTE_UNIX_INODE));
	g_clear_object(&i);
	g_object_unref(f);
	return stamp;
}

static gboolean config_looked_at(gpointer data) {
	char *stamp = config_stamp();
	if (strcmp(stamp, srv.config_stamp) != 0) {
		g_free(srv.config_stamp);
		srv.config_stamp = stamp;
		config_changed(NULL, NULL, NULL, 0, NULL);
	} else {
		g_free(stamp);
	}
	return G_SOURCE_CONTINUE;
}

/* ---- The log -------------------------------------------------------------- */

/* What was asked of whom, never the text: for what it's costing. */
static void log_request(struct request *r, const char *result) {
	char *dir = g_build_filename(g_get_user_state_dir(), "augur", NULL);
	char *path = g_build_filename(dir, "log", NULL);
	g_mkdir_with_parents(dir, 0700);
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	GDateTime *now = g_date_time_new_now_local();
	char *time = g_date_time_format_iso8601(now);
	g_date_time_unref(now);
	json_builder_set_member_name(b, "time");
	json_builder_add_string_value(b, time);
	json_builder_set_member_name(b, "app");
	json_builder_add_string_value(b, r->app_id);
	json_builder_set_member_name(b, "profile");
	json_builder_add_string_value(b, r->profile != NULL ? r->profile->name : "");
	json_builder_set_member_name(b, "model");
	json_builder_add_string_value(b, r->model != NULL ? r->model : "");
	json_builder_set_member_name(b, "input-tokens");
	json_builder_add_int_value(b, r->input_tokens);
	json_builder_set_member_name(b, "output-tokens");
	json_builder_add_int_value(b, r->output_tokens);
	json_builder_set_member_name(b, "cached-tokens");
	json_builder_add_int_value(b, r->cached_tokens);
	json_builder_set_member_name(b, "cost");
	if (r->cost_unknown) {
		json_builder_add_null_value(b);
	} else {
		json_builder_add_double_value(b, r->cost);
	}
	json_builder_set_member_name(b, "attempts");
	json_builder_add_int_value(b, r->attempts);
	if (r->classify) {
		/* Which questions, never what they asked or about what. */
		json_builder_set_member_name(b, "questions");
		json_builder_begin_array(b);
		GList *names = r->questions != NULL ? classify_names(r->questions) : NULL;
		for (GList *l = names; l != NULL; l = l->next) {
			json_builder_add_string_value(b, l->data);
		}
		g_list_free(names);
		json_builder_end_array(b);
	}
	if (r->tools != NULL) {
		/* Which tools, never what they were given or said. */
		json_builder_set_member_name(b, "rounds");
		json_builder_add_int_value(b, r->rounds);
		json_builder_set_member_name(b, "tools");
		json_builder_begin_array(b);
		for (guint i = 0; i < r->called->len; i++) {
			json_builder_add_string_value(b, r->called->pdata[i]);
		}
		json_builder_end_array(b);
	}
	json_builder_set_member_name(b, "result");
	json_builder_add_string_value(b, result);
	json_builder_end_object(b);
	JsonNode *root = json_builder_get_root(b);
	char *line = json_to_string(root, FALSE);
	FILE *f = fopen(path, "a");
	if (f != NULL) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	g_free(line);
	json_node_unref(root);
	g_object_unref(b);
	g_free(time);
	g_free(path);
	g_free(dir);
}

/* ---- Requests: the end ----------------------------------------------------- */

static struct queue *queue_of(const char *profile) {
	struct queue *q = g_hash_table_lookup(srv.queues, profile);
	if (q == NULL) {
		q = g_new0(struct queue, 1);
		g_queue_init(&q->interactive);
		g_queue_init(&q->background);
		g_hash_table_insert(srv.queues, g_strdup(profile), q);
	}
	return q;
}

static void request_free(struct request *r) {
	if (r->registration != 0) {
		g_dbus_connection_unregister_object(srv.bus, r->registration);
	}
	g_free(r->path);
	g_free(r->sender);
	g_free(r->app_id);
	g_free(r->model);
	g_free(r->input);
	if (r->questions != NULL) {
		json_object_unref(r->questions);
	}
	if (r->messages != NULL) {
		json_array_unref(r->messages);
	}
	if (r->schema != NULL) {
		json_node_unref(r->schema);
	}
	if (r->tools != NULL) {
		json_array_unref(r->tools);
	}
	if (r->turn != NULL) {
		json_array_unref(r->turn);
	}
	g_hash_table_unref(r->waiting);
	g_hash_table_unref(r->results);
	g_ptr_array_unref(r->called);
	g_clear_object(&r->cancel);
	config_unref(r->config);
	g_free(r);
}

/* It's over: out of the table, its place given up. */
static void request_end(struct request *r, const char *result) {
	log_request(r, result);
	char *profile = r->profile != NULL ? g_strdup(r->profile->name) : NULL;
	if (r->running && profile != NULL) {
		queue_of(profile)->running--;
	}
	g_hash_table_remove(srv.requests, GUINT_TO_POINTER(r->id));
	request_free(r);
	if (profile != NULL) {
		pump(profile);
		g_free(profile);
	}
	busy(-1);
}

static GVariant *info_of(struct request *r) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "profile",
		g_variant_new_string(r->profile->name));
	g_variant_builder_add(&b, "{sv}", "model", g_variant_new_string(r->model));
	if (r->input_tokens >= 0) {
		g_variant_builder_add(&b, "{sv}", "input-tokens",
			g_variant_new_int64(r->input_tokens));
	}
	if (r->output_tokens >= 0) {
		g_variant_builder_add(&b, "{sv}", "output-tokens",
			g_variant_new_int64(r->output_tokens));
	}
	if (r->cached_tokens >= 0) {
		g_variant_builder_add(&b, "{sv}", "cached-tokens",
			g_variant_new_int64(r->cached_tokens));
	}
	if (!r->cost_unknown) {
		g_variant_builder_add(&b, "{sv}", "cost", g_variant_new_double(r->cost));
	}
	g_variant_builder_add(&b, "{sv}", "attempts",
		g_variant_new_int32(r->attempts));
	g_variant_builder_add(&b, "{sv}", "rounds", g_variant_new_int32(r->rounds));
	if (r->classify) {
		g_variant_builder_add(&b, "{sv}", "calibrated",
			g_variant_new_boolean(r->classifier));
	} else {
		g_variant_builder_add(&b, "{sv}", "tool-calls",
			g_variant_new_int32(r->called->len));
	}
	return g_variant_builder_end(&b);
}

static void emit(struct request *r, const char *signal, GVariant *args) {
	if (r->sender_gone) {
		g_variant_unref(g_variant_ref_sink(args));
		return;
	}
	g_dbus_connection_emit_signal(srv.bus, r->sender, r->path, REQUEST_IFACE,
		signal, args, NULL);
}

static void fail(struct request *r, enum augur_error code, const char *message);

/* Classify's answer: Jev's, or the chat model's checked JSON. */
static void classified(struct request *r, const char *text) {
	char *why = NULL;
	GVariant *answers = r->classifier ?
		classify_answers_from_jev(r->questions, text, &why) :
		classify_answers_from_chat(r->questions, text);
	if (answers == NULL) {
		fail(r, AUGUR_ERROR_PROVIDER, why);
		g_free(why);
		return;
	}
	g_dbus_method_invocation_return_value(r->ask,
		g_variant_new("(@a{sv}@a{sv})", answers, info_of(r)));
	r->ask = NULL;
	request_end(r, "ok");
}

static void succeed(struct request *r, const char *text) {
	if (r->classify) {
		classified(r, text);
		return;
	}
	if (r->ask != NULL) {
		g_dbus_method_invocation_return_value(r->ask,
			g_variant_new("(s@a{sv})", text, info_of(r)));
		r->ask = NULL;
	} else {
		emit(r, "Done", g_variant_new("(s@a{sv})", text, info_of(r)));
	}
	request_end(r, "ok");
}

static void fail(struct request *r, enum augur_error code, const char *message) {
	const char *name = error_entries[code].dbus_error_name;
	if (r->ask != NULL) {
		g_dbus_method_invocation_return_error_literal(r->ask, AUGUR_ERROR, code,
			message);
		r->ask = NULL;
	} else {
		emit(r, "Failed", g_variant_new("(ss)", name, message));
	}
	request_end(r, name + strlen(ERROR_PREFIX));
}

/* ---- Requests: asking ------------------------------------------------------- */

static void ask_provider(struct request *r);

static void delta(const char *text, void *data) {
	struct request *r = data;
	if (r->stream && r->path != NULL) {
		emit(r, "Delta", g_variant_new("(s)", text));
	}
}

static enum augur_error error_for(enum call_error e) {
	switch (e) {
	case CALL_AUTH: return AUGUR_ERROR_AUTH;
	case CALL_RATE_LIMITED: return AUGUR_ERROR_RATE_LIMITED;
	case CALL_CANCELLED: return AUGUR_ERROR_CANCELLED;
	default: return AUGUR_ERROR_PROVIDER;
	}
}

static void add_message(struct request *r, const char *role,
		const char *content) {
	JsonObject *m = json_object_new();
	json_object_set_string_member(m, "role", role);
	json_object_set_string_member(m, "content", content);
	json_array_add_object_element(r->messages, m);
}

/* ---- Requests: tools ---------------------------------------------------------- */

static JsonObject *tool_of(struct request *r, const char *name) {
	for (guint i = 0; i < json_array_get_length(r->tools); i++) {
		JsonObject *t = json_array_get_object_element(r->tools, i);
		if (g_strcmp0(json_object_get_string_member(t, "name"), name) == 0) {
			return t;
		}
	}
	return NULL;
}

static void tool_result(struct request *r, const char *id, const char *content,
		bool error) {
	JsonObject *m = json_object_new();
	json_object_set_string_member(m, "role", "tool");
	json_object_set_string_member(m, "id", id);
	json_object_set_string_member(m, "content", content);
	json_object_set_boolean_member(m, "error", error);
	g_hash_table_insert(r->results, g_strdup(id), m);
}

/* Every call answered: what the tools said goes to the model, in the
 * order it called them, and the request is next in its queue. */
static void tools_answered(struct request *r) {
	for (guint i = 0; i < json_array_get_length(r->turn); i++) {
		const char *id = json_object_get_string_member(
			json_array_get_object_element(r->turn, i), "id");
		JsonObject *m = g_hash_table_lookup(r->results, id);
		json_array_add_object_element(r->messages, json_object_ref(m));
	}
	g_hash_table_remove_all(r->results);
	g_clear_pointer(&r->turn, json_array_unref);
	struct queue *q = queue_of(r->profile->name);
	g_queue_push_head(r->interactive ? &q->interactive : &q->background, r);
	g_idle_add(start_soon, g_strdup(r->profile->name));
}

/* The model's turn ended in calls. Each is checked against its tool's
 * schema: one that doesn't match, or names no tool, is answered here,
 * saying why; the rest go to the program. Meanwhile someone else can
 * have this request's place with the provider. */
static void tools_called(struct request *r, struct call_result *result) {
	JsonArray *calls = json_array_new();
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		NULL);
	for (guint i = 0; i < json_array_get_length(result->calls); i++) {
		JsonObject *c = json_object_ref(
			json_array_get_object_element(result->calls, i));
		const char *id = json_object_get_string_member(c, "id");
		if (id[0] == '\0' || g_hash_table_contains(seen, id)) {
			char *made = g_strdup_printf("call_%u", ++r->next_call);
			json_object_set_string_member(c, "id", made);
			g_free(made);
		}
		g_hash_table_add(seen, g_strdup(json_object_get_string_member(c, "id")));
		json_array_add_object_element(calls, c);
	}
	g_hash_table_unref(seen);
	JsonObject *m = json_object_new();
	json_object_set_string_member(m, "role", "assistant");
	json_object_set_string_member(m, "content", result->text);
	json_object_set_array_member(m, "calls", json_array_ref(calls));
	json_array_add_object_element(r->messages, m);
	r->turn = calls;

	for (guint i = 0; i < json_array_get_length(calls); i++) {
		JsonObject *c = json_array_get_object_element(calls, i);
		const char *id = json_object_get_string_member(c, "id");
		const char *name = json_object_get_string_member(c, "name");
		JsonObject *tool = tool_of(r, name);
		char *why = NULL;
		JsonNode *args = tool != NULL ?
			schema_parse_answer(json_object_get_string_member(c, "arguments"),
				&why) : NULL;
		if (tool == NULL) {
			why = g_strdup_printf("there's no tool \"%s\"", name);
		} else if (args != NULL && !schema_check(json_object_get_member(tool,
				"schema"), args, &why)) {
			char *w = g_strdup_printf("the arguments don't match the tool's "
				"schema: %s", why);
			g_free(why);
			why = w;
		}
		if (why != NULL) {
			tool_result(r, id, why, true);
		} else {
			char *json = json_to_string(args, FALSE);
			g_hash_table_add(r->waiting, g_strdup(id));
			g_ptr_array_add(r->called, g_strdup(name));
			emit(r, "ToolCall", g_variant_new("(sss)", id, name, json));
			g_free(json);
		}
		if (args != NULL) {
			json_node_unref(args);
		}
		g_free(why);
	}

	if (r->running) {
		r->running = false;
		queue_of(r->profile->name)->running--;
		g_idle_add(start_soon, g_strdup(r->profile->name));
	}
	if (g_hash_table_size(r->waiting) == 0) {
		tools_answered(r);
	}
}

/* ToolDone and ToolFailed. */
static void tool_answer(struct request *r, GVariant *params, bool error,
		GDBusMethodInvocation *inv) {
	const char *id, *content;
	g_variant_get(params, "(&s&s)", &id, &content);
	if (!g_hash_table_remove(r->waiting, id)) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "no call \"%s\" is waiting", id);
		return;
	}
	g_dbus_method_invocation_return_value(inv, NULL);
	tool_result(r, id, content, error);
	if (g_hash_table_size(r->waiting) == 0) {
		tools_answered(r);
	}
}

/* What a round cost: what the provider said, else its tokens at the
 * profile's price for the model; -1 if neither can be known. */
static double cost_of(struct request *r, struct call_result *result) {
	if (result->cost >= 0) {
		return result->cost;
	}
	if (result->input_tokens < 0 && result->output_tokens < 0 &&
			result->error != CALL_OK && result->error != CALL_CANCELLED) {
		return 0;   /* refused before it began: nothing used */
	}
	const struct price *p = g_hash_table_lookup(r->profile->prices, r->model);
	if (p == NULL || result->input_tokens < 0 || result->output_tokens < 0) {
		return -1;
	}
	gint64 cached = CLAMP(result->cached_tokens, 0, result->input_tokens);
	return ((result->input_tokens - cached) * p->input + cached * p->cached +
		result->output_tokens * p->output) / 1e6;
}

static void answered(struct call_result *result, void *data) {
	struct request *r = data;
	if (result->input_tokens >= 0) {
		r->input_tokens = MAX(r->input_tokens, 0) + result->input_tokens;
	}
	if (result->output_tokens >= 0) {
		r->output_tokens = MAX(r->output_tokens, 0) + result->output_tokens;
	}
	if (result->cached_tokens >= 0) {
		r->cached_tokens = MAX(r->cached_tokens, 0) + result->cached_tokens;
	}
	double cost = cost_of(r, result);
	if (cost < 0) {
		r->cost_unknown = true;
	} else {
		r->cost += cost;
	}
	if (result->error != CALL_OK) {
		if (result->error == CALL_AUTH) {
			g_hash_table_remove(srv.keys, r->profile->name); /* get it again */
		}
		fail(r, error_for(result->error), result->message);
		return;
	}
	if (result->calls != NULL && json_array_get_length(result->calls) > 0) {
		if (r->tools != NULL) {
			tools_called(r, result);
		} else {
			fail(r, AUGUR_ERROR_PROVIDER, "the model called a tool it wasn't given");
		}
		return;
	}
	if (r->schema == NULL) {
		succeed(r, result->text);
		return;
	}
	char *why = NULL;
	JsonNode *answer = schema_parse_answer(result->text, &why);
	if (answer != NULL && schema_check(r->schema, answer, &why)) {
		/* As JSON, without any code fence round it. */
		char *json = json_to_string(answer, FALSE);
		succeed(r, json);
		g_free(json);
	} else if (r->attempts < 2) {
		/* Once more, saying what was wrong. */
		r->attempts++;
		add_message(r, "assistant", result->text);
		char *again = g_strdup_printf("That doesn't match the JSON Schema: %s. "
			"Reply again with only JSON that matches it.", why);
		add_message(r, "user", again);
		g_free(again);
		ask_provider(r);
	} else {
		fail(r, AUGUR_ERROR_SCHEMA, why);
	}
	if (answer != NULL) {
		json_node_unref(answer);
	}
	g_free(why);
}

static void ask_provider(struct request *r) {
	r->attempts = MAX(r->attempts, 1);
	r->rounds++;
	struct call_spec spec = {
		.profile = r->profile,
		.model = r->model,
		.key = g_hash_table_lookup(srv.keys, r->profile->name),
		.messages = r->messages,
		.schema = r->schema,
		.tools = r->tools,
		/* The last turn: it answers with what it has. */
		.tools_off = r->tools != NULL && r->rounds >= r->max_rounds,
		.max_tokens = r->max_tokens,
		.temperature = r->temperature,
	};
	char *gw = g_strdup_printf("%s\x01gw", r->profile->name);
	spec.gateway_key = g_hash_table_lookup(srv.keys, gw);
	g_free(gw);
	if (r->classifier) {
		JsonNode *questions = classify_jev_questions(r->questions);
		call_systemone(srv.soup, &spec, r->input, questions, r->cancel, answered,
			r);
		json_node_unref(questions);
		return;
	}
	call_start(srv.soup, &spec, r->cancel, delta, answered, r);
}

/* ---- Requests: the key ------------------------------------------------------ */

struct key_job {
	struct request *r;
	char *slot;            /* where in srv.keys it goes */
	bool gateway;
};

static void key_ready(GObject *src, GAsyncResult *res, gpointer data);

/* Runs a key command; then key_ready. */
static void run_key_command(struct request *r, const char *command,
		const char *slot, bool gateway) {
	GError *err = NULL;
	const char *argv[] = { "/bin/sh", "-c", command, NULL };
	GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE, &err);
	if (p == NULL) {
		char *m = g_strdup_printf("couldn't run the key command: %s", err->message);
		g_error_free(err);
		fail(r, AUGUR_ERROR_AUTH, m);
		g_free(m);
		return;
	}
	struct key_job *job = g_new0(struct key_job, 1);
	job->r = r;
	job->slot = g_strdup(slot);
	job->gateway = gateway;
	g_subprocess_communicate_utf8_async(p, NULL, r->cancel, key_ready, job);
	g_object_unref(p);
}

/* The keys this request needs, then the provider. */
static void with_keys(struct request *r) {
	struct profile *p = r->profile;
	char *gw = g_strdup_printf("%s\x01gw", p->name);
	if (!g_hash_table_contains(srv.keys, p->name)) {
		const char *env = p->key_env != NULL ? g_getenv(p->key_env) : NULL;
		if (p->key_command != NULL) {
			run_key_command(r, p->key_command, p->name, false);
			g_free(gw);
			return;
		}
		if (p->key_env != NULL && env == NULL && p->needs_key) {
			char *m = g_strdup_printf("%s isn't set", p->key_env);
			fail(r, AUGUR_ERROR_AUTH, m);
			g_free(m);
			g_free(gw);
			return;
		}
		if (env != NULL) {
			g_hash_table_insert(srv.keys, g_strdup(p->name), g_strdup(env));
		}
	}
	if (p->gateway_key_command != NULL && !g_hash_table_contains(srv.keys, gw)) {
		run_key_command(r, p->gateway_key_command, gw, true);
		g_free(gw);
		return;
	}
	g_free(gw);
	ask_provider(r);
}

static void key_ready(GObject *src, GAsyncResult *res, gpointer data) {
	struct key_job *job = data;
	struct request *r = job->r;
	char *out = NULL, *errout = NULL;
	GError *err = NULL;
	bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out,
		&errout, &err);
	if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		fail(r, AUGUR_ERROR_CANCELLED, "cancelled");
	} else if (!ok || !g_subprocess_get_successful(G_SUBPROCESS(src)) ||
			out == NULL || g_strstrip(out)[0] == '\0') {
		char *m = g_strdup_printf("the %s command gave no key%s%s",
			job->gateway ? "gateway key" : "API key",
			errout != NULL && g_strstrip(errout)[0] ? ": " : "",
			errout != NULL ? errout : "");
		fail(r, AUGUR_ERROR_AUTH, m);
		g_free(m);
	} else {
		/* Only the first line: some commands print more. */
		char *nl = strchr(out, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		g_hash_table_insert(srv.keys, g_strdup(job->slot), g_strdup(out));
		with_keys(r);
	}
	g_clear_error(&err);
	g_free(out);
	g_free(errout);
	g_free(job->slot);
	g_free(job);
}

/* ---- Requests: turns --------------------------------------------------------- */

static void pump(const char *profile) {
	struct queue *q = queue_of(profile);
	for (;;) {
		struct request *r = g_queue_pop_head(&q->interactive);
		if (r == NULL) {
			r = g_queue_pop_head(&q->background);
		}
		if (r == NULL) {
			return;
		}
		if (q->running >= r->profile->max_concurrent) {
			g_queue_push_head(r->interactive ? &q->interactive : &q->background, r);
			return;
		}
		q->running++;
		r->running = true;
		with_keys(r);
	}
}

static gboolean start_soon(gpointer data) {
	char *profile = data;
	pump(profile);
	g_free(profile);
	return G_SOURCE_REMOVE;
}

/* ---- Requests: the beginning ---------------------------------------------------- */

static bool parse_messages(GVariant *v, JsonArray *out, GError **error) {
	GVariantIter it;
	GVariant *m;
	g_variant_iter_init(&it, v);
	while ((m = g_variant_iter_next_value(&it)) != NULL) {
		const char *role = NULL, *content = NULL;
		g_variant_lookup(m, "role", "&s", &role);
		g_variant_lookup(m, "content", "&s", &content);
		bool ok = role != NULL && content != NULL &&
			(strcmp(role, "system") == 0 || strcmp(role, "user") == 0 ||
				strcmp(role, "assistant") == 0);
		if (ok) {
			JsonObject *o = json_object_new();
			json_object_set_string_member(o, "role", role);
			json_object_set_string_member(o, "content", content);
			json_array_add_object_element(out, o);
		}
		g_variant_unref(m);
		if (!ok) {
			g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"each message needs a role (system, user or assistant) and content");
			return false;
		}
	}
	if (json_array_get_length(out) == 0) {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"there are no messages");
		return false;
	}
	return true;
}

/* A tool: a name both providers take, a description, and a JSON Schema
 * for its arguments, which they want to be an object. */
static bool parse_tool(GVariant *v, JsonArray *out, GError **error) {
	const char *name = NULL, *description = "", *schema = "{\"type\":\"object\"}";
	g_variant_lookup(v, "name", "&s", &name);
	g_variant_lookup(v, "description", "&s", &description);
	g_variant_lookup(v, "schema", "&s", &schema);
	if (name == NULL || !g_regex_match_simple("^[A-Za-z0-9_-]{1,64}$", name, 0,
			0)) {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"a tool needs a name: letters, digits, _ and -, up to 64");
		return false;
	}
	if (strcmp(name, RESERVED_TOOL) == 0) {
		g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"a tool can't be called \"%s\": Augur's is", RESERVED_TOOL);
		return false;
	}
	for (guint i = 0; i < json_array_get_length(out); i++) {
		if (strcmp(json_object_get_string_member(
				json_array_get_object_element(out, i), "name"), name) == 0) {
			g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"two tools are called \"%s\"", name);
			return false;
		}
	}
	char *why = NULL;
	JsonNode *s = schema_parse_answer(schema, &why);
	if (s == NULL || !JSON_NODE_HOLDS_OBJECT(s) ||
			g_strcmp0(json_object_get_string_member_with_default(
				json_node_get_object(s), "type", ""), "object") != 0) {
		g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"tool \"%s\"'s schema isn't a JSON Schema for an object%s%s", name,
			why != NULL ? ": " : "", why != NULL ? why : "");
		if (s != NULL) {
			json_node_unref(s);
		}
		g_free(why);
		return false;
	}
	JsonObject *t = json_object_new();
	json_object_set_string_member(t, "name", name);
	json_object_set_string_member(t, "description", description);
	json_object_set_member(t, "schema", s);
	json_array_add_object_element(out, t);
	return true;
}

static bool parse_tools(GVariant *v, JsonArray *out, GError **error) {
	GVariantIter it;
	GVariant *t;
	bool ok = true;
	g_variant_iter_init(&it, v);
	while (ok && (t = g_variant_iter_next_value(&it)) != NULL) {
		ok = parse_tool(t, out, error);
		g_variant_unref(t);
	}
	return ok;
}

/* Which profile and model, as README.md's "Which model" says. */
static bool resolve(struct request *r, const char *profile_name,
		const char *model, const char *tier, GError **error) {
	const struct app_settings *app = config_app(r->config, r->app_id);
	struct profile *p = profile_name != NULL ?
		config_profile(r->config, profile_name) :
		app != NULL && app->profile != NULL ? config_profile(r->config, app->profile) :
		config_default_profile(r->config);
	const char *asked = profile_name != NULL ? profile_name :
		app != NULL && app->profile != NULL ? app->profile : r->config->default_profile;
	if (p == NULL) {
		g_set_error(error, AUGUR_ERROR, AUGUR_ERROR_NO_PROFILE,
			asked != NULL ? "there's no profile \"%s\"" : "there's no profile%s",
			asked != NULL ? asked : "");
		return false;
	}
	if (p->problem != NULL) {
		g_set_error(error, AUGUR_ERROR, AUGUR_ERROR_NO_PROFILE,
			"profile \"%s\" can't be used: %s", p->name, p->problem);
		return false;
	}
	r->profile = p;
	const char *chosen = NULL;
	if (model != NULL) {
		chosen = model;
	} else if (tier != NULL) {
		chosen = g_hash_table_lookup(p->tiers, tier);
		if (chosen == NULL) {
			g_set_error(error, AUGUR_ERROR, AUGUR_ERROR_NO_MODEL,
				"profile \"%s\" has no tier \"%s\"", p->name, tier);
			return false;
		}
	} else if (app != NULL && app->model != NULL) {
		chosen = app->model;
	} else if (app != NULL && app->tier != NULL &&
			g_hash_table_lookup(p->tiers, app->tier) != NULL) {
		chosen = g_hash_table_lookup(p->tiers, app->tier);
	} else {
		chosen = p->model;
	}
	if (chosen == NULL) {
		g_set_error(error, AUGUR_ERROR, AUGUR_ERROR_NO_MODEL,
			"profile \"%s\" has no model; ask for one of its tiers", p->name);
		return false;
	}
	r->model = g_strdup(chosen);
	return true;
}

/* A request, before what it asks is read. */
static struct request *request_alloc(const char *sender) {
	struct request *r = g_new0(struct request, 1);
	r->config = config_ref(srv.config);
	r->sender = g_strdup(sender);
	r->input_tokens = r->output_tokens = r->cached_tokens = -1;
	r->temperature = -1;
	r->messages = json_array_new();
	r->waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	r->results = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)json_object_unref);
	r->called = g_ptr_array_new_with_free_func(g_free);
	r->max_rounds = MAX_ROUNDS;
	r->cancel = g_cancellable_new();
	return r;
}

/* It's in the table, and augurd stays while it's there. */
static void request_add(struct request *r) {
	r->id = ++srv.next_id;
	g_hash_table_insert(srv.requests, GUINT_TO_POINTER(r->id), r);
	busy(+1);
}

static struct request *request_new(GVariant *params, const char *sender,
		bool is_ask, GError **error) {
	if (!srv.enabled) {
		g_set_error_literal(error, AUGUR_ERROR, AUGUR_ERROR_DISABLED,
			"Augur is turned off");
		return NULL;
	}
	GVariant *req = g_variant_get_child_value(params, 0);
	struct request *r = request_alloc(sender);
	const char *app_id = NULL, *profile = NULL, *model = NULL, *tier = NULL;
	const char *schema = NULL;
	gboolean stream = !is_ask;
	guint32 max_tokens = 0, max_rounds = MAX_ROUNDS;
	double temperature = -1;
	g_variant_lookup(req, "app-id", "&s", &app_id);
	g_variant_lookup(req, "profile", "&s", &profile);
	g_variant_lookup(req, "model", "&s", &model);
	g_variant_lookup(req, "tier", "&s", &tier);
	g_variant_lookup(req, "schema", "&s", &schema);
	g_variant_lookup(req, "stream", "b", &stream);
	g_variant_lookup(req, "max-tokens", "u", &max_tokens);
	g_variant_lookup(req, "temperature", "d", &temperature);
	g_variant_lookup(req, "max-rounds", "u", &max_rounds);
	GVariant *messages = g_variant_lookup_value(req, "messages",
		G_VARIANT_TYPE("aa{sv}"));
	GVariant *tools = g_variant_lookup_value(req, "tools",
		G_VARIANT_TYPE("aa{sv}"));
	r->app_id = g_strdup(app_id != NULL ? app_id : "");
	r->stream = stream && !is_ask;
	r->max_tokens = max_tokens;
	r->temperature = temperature;
	bool ok = true;
	if (app_id == NULL || app_id[0] == '\0') {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"app-id is needed: who's asking");
		ok = false;
	} else if (messages == NULL) {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"messages is needed (aa{sv}: role and content)");
		ok = false;
	} else {
		ok = parse_messages(messages, r->messages, error);
	}
	if (ok && schema != NULL) {
		char *why = NULL;
		r->schema = schema_parse_answer(schema, &why);
		if (r->schema == NULL || !(JSON_NODE_HOLDS_OBJECT(r->schema) ||
				JSON_NODE_HOLDS_VALUE(r->schema))) {
			g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"the schema isn't a JSON Schema%s%s", why != NULL ? ": " : "",
				why != NULL ? why : "");
			ok = false;
		}
		g_free(why);
	}
	if (ok && tools != NULL) {
		if (is_ask) {
			g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"tools need Complete: Ask has no handle to call them on");
			ok = false;
		} else if (max_rounds < 1) {
			g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"max-rounds is at least 1");
			ok = false;
		} else {
			r->tools = json_array_new();
			ok = parse_tools(tools, r->tools, error);
		}
	}
	r->max_rounds = max_rounds;
	ok = ok && resolve(r, profile, model, tier, error);
	r->interactive = r->schema == NULL;
	if (messages != NULL) {
		g_variant_unref(messages);
	}
	if (tools != NULL) {
		g_variant_unref(tools);
	}
	g_variant_unref(req);
	if (!ok) {
		request_free(r);
		return NULL;
	}
	request_add(r);
	return r;
}

/* Classify's profile, and whether its classifier answers or its chat
 * model does: design/classify.md's "which profile". */
static bool resolve_classify(struct request *r, const char *profile_name,
		const char *model, GError **error) {
	const char *name = profile_name != NULL ? profile_name :
		config_classify_profile(r->config, r->app_id);
	struct profile *p = name != NULL ? config_profile(r->config, name) :
		config_default_profile(r->config);
	if (p != NULL && p->problem == NULL && p->classifier != NULL) {
		r->profile = p;
		r->classifier = true;
		r->model = g_strdup(model != NULL ? model : p->classifier);
		return true;
	}
	/* No classifier: the chat model, found as for Complete (and the errors
	 * for no profile, or a broken one, from there). */
	return resolve(r, p != NULL ? p->name : name, model, NULL, error);
}

static struct request *classify_new(GVariant *params, const char *sender,
		GError **error) {
	if (!srv.enabled) {
		g_set_error_literal(error, AUGUR_ERROR, AUGUR_ERROR_DISABLED,
			"Augur is turned off");
		return NULL;
	}
	GVariant *req = g_variant_get_child_value(params, 0);
	struct request *r = request_alloc(sender);
	r->classify = true;
	r->questions = json_object_new();
	const char *app_id = NULL, *input = NULL, *profile = NULL, *model = NULL;
	gboolean interactive = FALSE;
	g_variant_lookup(req, "app-id", "&s", &app_id);
	g_variant_lookup(req, "input", "&s", &input);
	g_variant_lookup(req, "profile", "&s", &profile);
	g_variant_lookup(req, "model", "&s", &model);
	g_variant_lookup(req, "interactive", "b", &interactive);
	GVariant *questions = g_variant_lookup_value(req, "questions",
		G_VARIANT_TYPE("a{sv}"));
	r->app_id = g_strdup(app_id != NULL ? app_id : "");
	r->input = g_strdup(input);
	r->interactive = interactive;
	bool ok = true;
	if (app_id == NULL || app_id[0] == '\0') {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"app-id is needed: who's asking");
		ok = false;
	} else if (input == NULL) {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"input is needed (s): what the questions are about");
		ok = false;
	} else if (questions == NULL) {
		g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"questions is needed (a{sv}: each by name, an a{sv})");
		ok = false;
	} else {
		ok = classify_parse(questions, r->questions, error);
	}
	ok = ok && resolve_classify(r, profile, model, error);
	if (ok && !r->classifier) {
		/* The questions as a schema, asked of the chat model. */
		r->schema = classify_schema(r->questions);
		char *prompt = classify_prompt(r->questions);
		add_message(r, "system", prompt);
		add_message(r, "user", r->input);
		g_free(prompt);
	}
	if (questions != NULL) {
		g_variant_unref(questions);
	}
	g_variant_unref(req);
	if (!ok) {
		request_free(r);
		return NULL;
	}
	request_add(r);
	return r;
}

/* After the reply's gone: the request waits its turn. */
static void enqueue(struct request *r) {
	struct queue *q = queue_of(r->profile->name);
	g_queue_push_tail(r->interactive ? &q->interactive : &q->background, r);
	g_idle_add(start_soon, g_strdup(r->profile->name));
}

static void cancel(struct request *r) {
	if (r->running) {
		g_cancellable_cancel(r->cancel);
		return;
	}
	/* Still waiting: out of the queue, and done. */
	struct queue *q = queue_of(r->profile->name);
	g_queue_remove(&q->interactive, r);
	g_queue_remove(&q->background, r);
	fail(r, AUGUR_ERROR_CANCELLED, "cancelled");
}

/* ---- D-Bus: a request's object ------------------------------------------------ */

static void request_method(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
	struct request *r = g_hash_table_lookup(srv.requests, data);
	if (r == NULL || g_strcmp0(sender, r->sender) != 0) {
		g_dbus_method_invocation_return_error_literal(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_ACCESS_DENIED, "not yours");
		return;
	}
	if (strcmp(method, "ToolDone") == 0 || strcmp(method, "ToolFailed") == 0) {
		tool_answer(r, params, strcmp(method, "ToolFailed") == 0, inv);
		return;
	}
	g_dbus_method_invocation_return_value(inv, NULL);
	cancel(r);
}

static const GDBusInterfaceVTable request_vtable = { request_method, NULL, NULL,
	{ 0 } };

/* ---- D-Bus: the service -------------------------------------------------------- */

static void return_gerror(GDBusMethodInvocation *inv, GError *error) {
	g_dbus_method_invocation_return_gerror(inv, error);
	g_error_free(error);
}

struct models_call {
	GDBusMethodInvocation *inv;
	struct config *config;
	char *profile;
};

static void models_listed(GPtrArray *models, const char *error, void *data) {
	struct models_call *m = data;
	if (models == NULL) {
		g_dbus_method_invocation_return_error_literal(m->inv, AUGUR_ERROR,
			AUGUR_ERROR_PROVIDER, error);
	} else {
		GVariantBuilder b;
		g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
		for (guint i = 0; i < models->len; i++) {
			g_variant_builder_add(&b, "s", models->pdata[i]);
		}
		g_dbus_method_invocation_return_value(m->inv, g_variant_new("(as)", &b));
	}
	config_unref(m->config);
	g_free(m->profile);
	g_free(m);
	busy(-1);
}

/* Listing needs only a key from the environment or an earlier request;
 * failing that, the profile's tiers and model are listed. */
static void list_models(GVariant *params, GDBusMethodInvocation *inv) {
	const char *name;
	g_variant_get(params, "(&s)", &name);
	struct profile *p = name[0] != '\0' ? config_profile(srv.config, name) :
		config_default_profile(srv.config);
	if (p == NULL || p->problem != NULL) {
		g_dbus_method_invocation_return_error(inv, AUGUR_ERROR,
			AUGUR_ERROR_NO_PROFILE, "no usable profile \"%s\"", name);
		return;
	}
	const char *key = g_hash_table_lookup(srv.keys, p->name);
	if (key == NULL && p->key_env != NULL) {
		key = g_getenv(p->key_env);
	}
	if (key == NULL && p->key_command != NULL) {
		char *out = NULL;
		if (g_spawn_command_line_sync(p->key_command, &out, NULL, NULL, NULL) &&
				out != NULL && g_strstrip(out)[0] != '\0') {
			char *nl = strchr(out, '\n');
			if (nl != NULL) {
				*nl = '\0';
			}
			g_hash_table_insert(srv.keys, g_strdup(p->name), g_strdup(out));
			key = g_hash_table_lookup(srv.keys, p->name);
		}
		g_free(out);
	}
	struct models_call *m = g_new0(struct models_call, 1);
	m->inv = inv;
	m->config = config_ref(srv.config);
	m->profile = g_strdup(p->name);
	busy(+1);
	call_list_models(srv.soup, p, key, models_listed, m);
}

/* ---- Usage: the log, added up ---------------------------------------------- */

struct usage_row {
	char *key[4];          /* app, profile, model, day: those grouped by */
	gint64 requests, input, output, cached, unpriced;
	double cost;
};

static const char *usage_keys[] = { "app", "profile", "model", "day" };

static void usage_row_free(gpointer data) {
	struct usage_row *row = data;
	for (int i = 0; i < 4; i++) {
		g_free(row->key[i]);
	}
	g_free(row);
}

static int usage_row_compare(gconstpointer a, gconstpointer b) {
	const struct usage_row *x = a, *y = b;
	for (int i = 0; i < 4; i++) {
		int c = g_strcmp0(x->key[i], y->key[i]);
		if (c != 0) {
			return c;
		}
	}
	return 0;
}

/* Usage(query) -> rows: the log's requests from since (x, seconds since
 * 1970) until before until, perhaps one app's, added up by what "by"
 * (as) names. */
static void usage(GVariant *params, GDBusMethodInvocation *inv) {
	GVariant *query = g_variant_get_child_value(params, 0);
	gint64 since = G_MININT64, until = G_MAXINT64;
	const char *app = NULL;
	const char **by = NULL;
	g_variant_lookup(query, "since", "x", &since);
	g_variant_lookup(query, "until", "x", &until);
	g_variant_lookup(query, "app-id", "&s", &app);
	g_variant_lookup(query, "by", "^a&s", &by);
	bool group[4] = { false };
	for (int i = 0; by != NULL && by[i] != NULL; i++) {
		bool known = false;
		for (int k = 0; k < 4; k++) {
			if (strcmp(by[i], usage_keys[k]) == 0) {
				group[k] = known = true;
			}
		}
		if (!known) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS,
				"usage can be by app, profile, model and day, not \"%s\"", by[i]);
			g_free(by);
			g_variant_unref(query);
			return;
		}
	}
	g_free(by);

	GHashTable *rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		usage_row_free);
	char *path = g_build_filename(g_get_user_state_dir(), "augur", "log", NULL);
	char *text = NULL;
	g_file_get_contents(path, &text, NULL, NULL);
	g_free(path);
	char **lines = g_strsplit(text != NULL ? text : "", "\n", -1);
	g_free(text);
	JsonParser *parser = json_parser_new();
	for (int i = 0; lines[i] != NULL; i++) {
		if (lines[i][0] == '\0' ||
				!json_parser_load_from_data(parser, lines[i], -1, NULL) ||
				!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
			continue;
		}
		JsonObject *o = json_node_get_object(json_parser_get_root(parser));
		GDateTime *t = g_date_time_new_from_iso8601(
			json_object_get_string_member_with_default(o, "time", ""), NULL);
		if (t == NULL) {
			continue;
		}
		gint64 when = g_date_time_to_unix(t);
		GDateTime *local = g_date_time_to_local(t);
		char *day = g_date_time_format(local, "%Y-%m-%d");
		g_date_time_unref(local);
		g_date_time_unref(t);
		const char *values[4] = {
			json_object_get_string_member_with_default(o, "app", ""),
			json_object_get_string_member_with_default(o, "profile", ""),
			json_object_get_string_member_with_default(o, "model", ""),
			day,
		};
		if (when < since || when >= until ||
				(app != NULL && strcmp(app, values[0]) != 0)) {
			g_free(day);
			continue;
		}
		GString *key = g_string_new(NULL);
		for (int k = 0; k < 4; k++) {
			g_string_append_printf(key, "%s\x01", group[k] ? values[k] : "");
		}
		struct usage_row *row = g_hash_table_lookup(rows, key->str);
		if (row == NULL) {
			row = g_new0(struct usage_row, 1);
			for (int k = 0; k < 4; k++) {
				row->key[k] = group[k] ? g_strdup(values[k]) : NULL;
			}
			g_hash_table_insert(rows, g_strdup(key->str), row);
		}
		g_string_free(key, TRUE);
		g_free(day);
		row->requests++;
		row->input += MAX(json_object_get_int_member_with_default(o,
			"input-tokens", 0), 0);
		row->output += MAX(json_object_get_int_member_with_default(o,
			"output-tokens", 0), 0);
		row->cached += MAX(json_object_get_int_member_with_default(o,
			"cached-tokens", 0), 0);
		/* Logged before costs were, or with no price: unpriced. */
		JsonNode *cost = json_object_get_member(o, "cost");
		if (cost != NULL && JSON_NODE_HOLDS_VALUE(cost)) {
			row->cost += json_node_get_double(cost);
		} else {
			row->unpriced++;
		}
	}
	g_object_unref(parser);
	g_strfreev(lines);

	GList *sorted = g_list_sort(g_hash_table_get_values(rows),
		usage_row_compare);
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
	for (GList *l = sorted; l != NULL; l = l->next) {
		struct usage_row *row = l->data;
		GVariantBuilder r;
		g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
		for (int k = 0; k < 4; k++) {
			if (row->key[k] != NULL) {
				g_variant_builder_add(&r, "{sv}", usage_keys[k],
					g_variant_new_string(row->key[k]));
			}
		}
		g_variant_builder_add(&r, "{sv}", "requests",
			g_variant_new_int64(row->requests));
		g_variant_builder_add(&r, "{sv}", "input-tokens",
			g_variant_new_int64(row->input));
		g_variant_builder_add(&r, "{sv}", "output-tokens",
			g_variant_new_int64(row->output));
		g_variant_builder_add(&r, "{sv}", "cached-tokens",
			g_variant_new_int64(row->cached));
		g_variant_builder_add(&r, "{sv}", "cost", g_variant_new_double(row->cost));
		g_variant_builder_add(&r, "{sv}", "unpriced",
			g_variant_new_int64(row->unpriced));
		g_variant_builder_add_value(&b, g_variant_builder_end(&r));
	}
	g_list_free(sorted);
	g_hash_table_unref(rows);
	g_variant_unref(query);
	g_dbus_method_invocation_return_value(inv, g_variant_new("(aa{sv})", &b));
}

static GVariant *status(void) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "enabled",
		g_variant_new_boolean(srv.enabled));
	g_variant_builder_add(&b, "{sv}", "config",
		g_variant_new_string(srv.config_file));
	struct profile *def = config_default_profile(srv.config);
	if (def != NULL) {
		g_variant_builder_add(&b, "{sv}", "default-profile",
			g_variant_new_string(def->name));
	}
	const char *why = NULL;
	if (srv.config_error != NULL) {
		why = srv.config_error;
	} else if (srv.disabled_by_env) {
		why = "AUGUR_DISABLED is set in Augur's environment (by the session)";
	} else if (!srv.config->enabled) {
		why = "turned off in the config ([augur] enabled = false)";
	} else if (srv.config->profiles->len == 0) {
		why = "no profiles in the config";
	} else if (!usable()) {
		why = "no profile can be used";
	}
	if (why != NULL) {
		g_variant_builder_add(&b, "{sv}", "problem", g_variant_new_string(why));
	}
	g_variant_builder_add(&b, "{sv}", "running",
		g_variant_new_uint32(g_hash_table_size(srv.requests)));
	return g_variant_builder_end(&b);
}

static GVariant *profiles(void) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(sssas)"));
	for (guint i = 0; i < srv.config->profiles->len; i++) {
		struct profile *p = srv.config->profiles->pdata[i];
		GVariantBuilder tiers;
		g_variant_builder_init(&tiers, G_VARIANT_TYPE("as"));
		GList *names = g_hash_table_get_keys(p->tiers);
		names = g_list_sort(names, (GCompareFunc)strcmp);
		for (GList *l = names; l != NULL; l = l->next) {
			g_variant_builder_add(&tiers, "s", l->data);
		}
		g_list_free(names);
		g_variant_builder_add(&b, "(sssas)", p->name,
			p->provider != NULL ? p->provider : "",
			p->problem != NULL ? p->problem : "", &tiers);
	}
	return g_variant_builder_end(&b);
}

static void method(GDBusConnection *bus, const char *sender, const char *path,
		const char *iface, const char *name, GVariant *params,
		GDBusMethodInvocation *inv, gpointer data) {
	GError *error = NULL;
	if (strcmp(name, "Complete") == 0 || strcmp(name, "Ask") == 0) {
		bool ask = strcmp(name, "Ask") == 0;
		struct request *r = request_new(params, sender, ask, &error);
		if (r == NULL) {
			return_gerror(inv, error);
			return;
		}
		if (ask) {
			r->ask = inv;
		} else {
			r->path = g_strdup_printf(REQUEST_PATH "/%u", r->id);
			r->registration = g_dbus_connection_register_object(srv.bus, r->path,
				g_dbus_node_info_lookup_interface(srv.info, REQUEST_IFACE),
				&request_vtable, GUINT_TO_POINTER(r->id), NULL, NULL);
			g_dbus_method_invocation_return_value(inv,
				g_variant_new("(o)", r->path));
		}
		enqueue(r);
	} else if (strcmp(name, "Classify") == 0) {
		struct request *r = classify_new(params, sender, &error);
		if (r == NULL) {
			return_gerror(inv, error);
			return;
		}
		r->ask = inv;
		enqueue(r);
	} else if (strcmp(name, "Status") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(@a{sv})", status()));
	} else if (strcmp(name, "ListProfiles") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(@a(sssas))", profiles()));
	} else if (strcmp(name, "ListModels") == 0) {
		list_models(params, inv);
	} else if (strcmp(name, "Usage") == 0) {
		usage(params, inv);
	}
}

static GVariant *get_property(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *name, GError **error,
		gpointer data) {
	return strcmp(name, "Enabled") == 0 ? g_variant_new_boolean(srv.enabled) :
		NULL;
}

static const GDBusInterfaceVTable vtable = { method, get_property, NULL,
	{ 0 } };

/* A program left the bus: what it asked for is no use to anyone now. */
static void owner_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	const char *name, *old, *new;
	g_variant_get(params, "(&s&s&s)", &name, &old, &new);
	if (name[0] != ':' || new[0] != '\0') {
		return;
	}
	GList *all = g_hash_table_get_values(srv.requests);
	for (GList *l = all; l != NULL; l = l->next) {
		struct request *r = l->data;
		if (strcmp(r->sender, name) == 0) {
			r->sender_gone = true;
			cancel(r);
		}
	}
	g_list_free(all);
}

static void bus_acquired(GDBusConnection *bus, const char *name, gpointer data) {
	srv.bus = bus;
	GError *error = NULL;
	if (g_dbus_connection_register_object(bus, OBJECT_PATH,
			g_dbus_node_info_lookup_interface(srv.info, IFACE), &vtable, NULL,
			NULL, &error) == 0) {
		g_printerr("augurd: %s\n", error->message);
		g_error_free(error);
		g_main_loop_quit(srv.loop);
		return;
	}
	g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus",
		NULL, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, NULL, NULL);
}

static void name_acquired(GDBusConnection *bus, const char *name,
		gpointer data) {
	srv.owned = true;
}

/* Never had it: another augurd has. Had it: the bus has gone. */
static void name_lost(GDBusConnection *bus, const char *name, gpointer data) {
	if (!srv.owned) {
		g_printerr("augurd: couldn't have %s on the session bus "
			"(is another augurd running?)\n", name);
		srv.status = 1;
	}
	g_main_loop_quit(srv.loop);
}

static gboolean on_signal(gpointer data) {
	g_main_loop_quit(srv.loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {
	gboolean stay = FALSE, version = FALSE;
	int idle = IDLE_TIMEOUT;
	char *config = NULL;
	GOptionEntry entries[] = {
		{ "no-exit", 0, 0, G_OPTION_ARG_NONE, &stay,
			"Keep running with nothing to do", NULL },
		{ "idle-timeout", 0, 0, G_OPTION_ARG_INT, &idle,
			"Seconds with nothing to do before exiting", "SECONDS" },
		{ "config", 0, 0, G_OPTION_ARG_FILENAME, &config,
			"Read this config instead of ~/.config/augur/config", "FILE" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &version, "Print the version",
			NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GOptionContext *ctx = g_option_context_new("- Augur's service");
	g_option_context_add_main_entries(ctx, entries, NULL);
	GError *error = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
		g_printerr("augurd: %s\n", error->message);
		return 2;
	}
	g_option_context_free(ctx);
	if (version) {
		printf("augurd %s\n", AUGUR_VERSION);
		return 0;
	}

	/* Watching the config is a local file's business: no gvfs. */
	g_setenv("GIO_USE_VFS", "local", TRUE);
	(void)augur_error_quark();
	srv.loop = g_main_loop_new(NULL, FALSE);
	srv.info = g_dbus_node_info_new_for_xml(introspection, NULL);
	srv.requests = g_hash_table_new(NULL, NULL);
	srv.queues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	srv.keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	srv.soup = soup_session_new_with_options("user-agent",
		"Augur/" AUGUR_VERSION, "timeout", 300, NULL);
	srv.idle_timeout = stay ? 0 : idle;
	const char *off = g_getenv("AUGUR_DISABLED");
	srv.disabled_by_env = off != NULL && off[0] != '\0' && strcmp(off, "0") != 0;
	srv.enabled = true; /* so the first load's result counts as a change */
	srv.config_file = config != NULL ? config : config_path();
	load_config();
	GFile *file = g_file_new_for_path(srv.config_file);
	GError *monitor_error = NULL;
	srv.monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL,
		&monitor_error);
	g_object_unref(file);
	/* Where GLib can't watch files it gives a monitor that looks every five
	 * seconds (GPollFileMonitor), which is too slow to call watching. */
	if (srv.monitor != NULL &&
			strcmp(G_OBJECT_TYPE_NAME(srv.monitor), "GPollFileMonitor") == 0) {
		g_clear_object(&srv.monitor);
		monitor_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"GLib can only look every five seconds");
	}
	if (srv.monitor != NULL) {
		g_signal_connect(srv.monitor, "changed", G_CALLBACK(config_changed), NULL);
	} else {
		/* Looked at twice a second instead: a stat, nothing more. */
		g_message("can't watch %s (%s); looking at it twice a second",
			srv.config_file, monitor_error->message);
		g_error_free(monitor_error);
		srv.config_stamp = config_stamp();
		g_timeout_add(500, config_looked_at, NULL);
	}

	guint owner = g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME,
		G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, bus_acquired, name_acquired,
		name_lost, NULL,
		NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	busy(0);
	g_main_loop_run(srv.loop);
	g_bus_unown_name(owner);
	return srv.status;
}
