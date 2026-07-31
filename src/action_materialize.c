#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if QSTAR_PLATFORM_WINDOWS
#include <direct.h>
#endif

#define QSTAR_ACTION_HASH_INIT 1469598103934665603ULL
#define QSTAR_ACTION_HASH_PRIME 1099511628211ULL
#define QSTAR_RESPONSE_ARGV_BYTES 512U
#define QSTAR_RESPONSE_ARGC 48U
#define QSTAR_WINDOWS_COMMAND_LIMIT 32767U
#define QSTAR_FALLBACK_COMMAND_LIMIT 131072U
#define QSTAR_POSIX_COMMAND_HEADROOM 2048U

#if !QSTAR_PLATFORM_WINDOWS
extern char **environ;
#endif

static void
hash_bytes(unsigned long long *hash, const char *value)
{
	const unsigned char *p;

	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		*hash ^= (unsigned long long)*p;
		*hash *= QSTAR_ACTION_HASH_PRIME;
	}
	*hash ^= 0xffU;
	*hash *= QSTAR_ACTION_HASH_PRIME;
}

void
qstar_argv_digest(const struct qstar_argv *argv, char *dst, size_t dstlen)
{
	unsigned long long hash = QSTAR_ACTION_HASH_INIT;
	size_t i;

	for (i = 0; argv && i < argv->len; i++)
		hash_bytes(&hash, argv->items[i]);
	snprintf(dst, dstlen, "%016llx", hash);
}

int
qstar_action_needs_response_file(const struct qstar_argv *logical)
{
	return logical &&
	    (logical->bytes >= QSTAR_RESPONSE_ARGV_BYTES ||
	    logical->len > QSTAR_RESPONSE_ARGC);
}

static unsigned long long
response_digest_char(unsigned long long hash, unsigned char value)
{
	hash ^= (unsigned long long)value;
	hash *= QSTAR_ACTION_HASH_PRIME;
	return hash;
}

static unsigned long long
response_digest_text(unsigned long long hash, const char *value)
{
	const unsigned char *p;

	for (p = (const unsigned char *)(value ? value : ""); *p; p++)
		hash = response_digest_char(hash, *p);
	return hash;
}

static unsigned long long
response_digest_shell_arg(unsigned long long hash, const char *value)
{
	const unsigned char *p;
	int simple;

	p = (const unsigned char *)(value ? value : "");
	simple = *p != '\0';
	for (; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
		    *p == '/' || *p == ':' || *p == '=' || *p == '+' ||
		    *p == ','))
			simple = 0;
	}
	if (simple)
		return response_digest_text(hash, value);
	hash = response_digest_char(hash, '\'');
	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		if (*p == '\'')
			hash = response_digest_text(hash, "'\\''");
		else
			hash = response_digest_char(hash, *p);
	}
	return response_digest_char(hash, '\'');
}

static unsigned long long
response_digest_windows_arg(unsigned long long hash, const char *value)
{
	const unsigned char *p;
	size_t backslashes;
	int quote;

	p = (const unsigned char *)(value ? value : "");
	quote = *p == '\0';
	for (; *p; p++) {
		if (isspace(*p) || *p == '"' || *p == '\\')
			quote = 1;
	}
	if (!quote)
		return response_digest_text(hash, value);
	hash = response_digest_char(hash, '"');
	backslashes = 0;
	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		if (*p == '\\') {
			backslashes++;
			continue;
		}
		if (*p == '"') {
			while (backslashes > 0) {
				hash = response_digest_text(hash, "\\\\");
				backslashes--;
			}
			hash = response_digest_text(hash, "\\\"");
			continue;
		}
		while (backslashes > 0) {
			hash = response_digest_char(hash, '\\');
			backslashes--;
		}
		hash = response_digest_char(hash, *p);
	}
	while (backslashes > 0) {
		hash = response_digest_text(hash, "\\\\");
		backslashes--;
	}
	return response_digest_char(hash, '"');
}

static unsigned long long
response_digest(const struct qstar_argv *logical, const char *style)
{
	unsigned long long hash;
	size_t i;

	hash = QSTAR_ACTION_HASH_INIT;
	for (i = 1; logical && i < logical->len; i++) {
		if (style && (strcmp(style, "windows") == 0 ||
		    strcmp(style, "msvc") == 0))
			hash = response_digest_windows_arg(hash, logical->items[i]);
		else
			hash = response_digest_shell_arg(hash, logical->items[i]);
		hash = response_digest_char(hash, '\n');
	}
	return hash;
}

static void
write_shell_arg(FILE *file, const char *value)
{
	const unsigned char *p;
	int simple;

	p = (const unsigned char *)(value ? value : "");
	simple = *p != '\0';
	for (; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
		    *p == '/' || *p == ':' || *p == '=' || *p == '+' ||
		    *p == ','))
			simple = 0;
	}
	if (simple) {
		fputs(value, file);
		return;
	}
	fputc('\'', file);
	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		if (*p == '\'')
			fputs("'\\''", file);
		else
			fputc(*p, file);
	}
	fputc('\'', file);
}

static void
write_windows_arg(FILE *file, const char *value)
{
	const unsigned char *p;
	size_t backslashes;
	int quote;

	p = (const unsigned char *)(value ? value : "");
	quote = *p == '\0';
	for (; *p; p++) {
		if (isspace(*p) || *p == '"' || *p == '\\')
			quote = 1;
	}
	if (!quote) {
		fputs(value, file);
		return;
	}
	fputc('"', file);
	backslashes = 0;
	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		if (*p == '\\') {
			backslashes++;
			continue;
		}
		if (*p == '"') {
			while (backslashes > 0) {
				fputs("\\\\", file);
				backslashes--;
			}
			fputs("\\\"", file);
			continue;
		}
		while (backslashes > 0) {
			fputc('\\', file);
			backslashes--;
		}
		fputc(*p, file);
	}
	while (backslashes > 0) {
		fputs("\\\\", file);
		backslashes--;
	}
	fputc('"', file);
}

static void
write_response_arg(FILE *file, const char *value, const char *style)
{
	if (style && (strcmp(style, "windows") == 0 ||
	    strcmp(style, "msvc") == 0))
		write_windows_arg(file, value);
	else
		write_shell_arg(file, value);
}

static int
mkdir_one(const char *path)
{
#if QSTAR_PLATFORM_WINDOWS
	if (_mkdir(path) == 0 || errno == EEXIST)
#else
	if (mkdir(path, 0777) == 0 || errno == EEXIST)
#endif
		return 0;
	return -1;
}

static int
mkdir_p(const char *path)
{
	char current[QSTAR_PATH_MAX];
	size_t i, len;

	len = strlen(path);
	if (len == 0 || len >= sizeof(current))
		return -1;
	memcpy(current, path, len + 1);
	for (i = 1; i < len; i++) {
		if (current[i] != '/' && current[i] != '\\')
			continue;
		current[i] = '\0';
		if (current[0] && mkdir_one(current) < 0)
			return -1;
		current[i] = '/';
	}
	return mkdir_one(current);
}

static int
write_response_file(struct qstar_graph *graph,
    const struct qstar_argv *logical,
    const struct qstar_materialized_command *command)
{
	char full[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];
	const char *root;
	FILE *file;
	size_t i;

	root = graph->package_root && *graph->package_root ?
	    graph->package_root : ".";
	if (qstar_path_join(root, command->response_file, full,
	    sizeof(full)) < 0 ||
	    qstar_dirname(full, parent, sizeof(parent)) < 0 ||
	    mkdir_p(parent) < 0)
		return qstar_set_error(graph,
		    "qstar: could not create response file directory");
	file = fopen(full, "w");
	if (!file)
		return qstar_set_error(graph,
		    "qstar: could not write response file '%s'",
		    command->response_file);
	for (i = 1; i < logical->len; i++) {
		write_response_arg(file, logical->items[i],
		    command->response_style);
		fputc('\n', file);
	}
	if (fclose(file) != 0)
		return qstar_set_error(graph,
		    "qstar: could not close response file '%s'",
		    command->response_file);
	return 0;
}

static size_t
environment_bytes(const struct qstar_string_list *env)
{
	size_t bytes, i, n;
#if !QSTAR_PLATFORM_WINDOWS
	char **item;
#endif

	bytes = 0;
#if !QSTAR_PLATFORM_WINDOWS
	for (item = environ; item && *item; item++) {
		n = strlen(*item) + 1;
		if (bytes > SIZE_MAX - n)
			return SIZE_MAX;
		bytes += n;
	}
#endif
	for (i = 0; env && i < env->len; i++) {
		n = strlen(env->items[i]) + 1;
		if (bytes > SIZE_MAX - n)
			return SIZE_MAX;
		bytes += n;
	}
	return bytes;
}

static int
validate_full_command_limit(struct qstar_graph *graph, const char *action_id,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_argv *logical, const struct qstar_string_list *env,
    size_t *host_limit)
{
	char *windows_line;
	const char *toolset;
	size_t env_bytes, pointer_bytes, required, limit;
	long arg_max;

	toolset = toolchain && toolchain->toolset[0] ?
	    toolchain->toolset : "<default>";
	if (toolchain && qstar_platform_is_windows(toolchain->platform)) {
		limit = QSTAR_WINDOWS_COMMAND_LIMIT;
		windows_line = malloc(limit + 1);
		if (!windows_line)
			return qstar_set_error(graph, "qstar: out of memory");
		if (qstar_platform_windows_command_line_from_argv(
		    qstar_argv_data((struct qstar_argv *)logical), windows_line,
		    limit + 1) < 0) {
			free(windows_line);
			*host_limit = limit;
			return qstar_set_error(graph,
			    "qstar: final action '%s' requires %zu argv items and "
			    "%zu bytes, but response files are unavailable under "
			    "toolset '%s' "
			    "and the host command limit is %zu bytes",
			    action_id, logical->len, logical->bytes, toolset, limit);
		}
		free(windows_line);
		*host_limit = limit;
		return 0;
	}
	arg_max = sysconf(_SC_ARG_MAX);
	limit = arg_max > 0 ? (size_t)arg_max : QSTAR_FALLBACK_COMMAND_LIMIT;
	env_bytes = environment_bytes(env);
	if (logical->len == SIZE_MAX ||
	    logical->len + 1 > SIZE_MAX / sizeof(char *))
		pointer_bytes = SIZE_MAX;
	else
		pointer_bytes = (logical->len + 1) * sizeof(char *);
	if (env_bytes == SIZE_MAX || pointer_bytes == SIZE_MAX ||
	    logical->bytes > SIZE_MAX - pointer_bytes ||
	    env_bytes > SIZE_MAX - logical->bytes - pointer_bytes)
		required = SIZE_MAX;
	else
		required = logical->bytes + pointer_bytes + env_bytes;
	*host_limit = limit;
	if (limit > QSTAR_POSIX_COMMAND_HEADROOM &&
	    required <= limit - QSTAR_POSIX_COMMAND_HEADROOM)
		return 0;
	return qstar_set_error(graph,
	    "qstar: final action '%s' requires %zu argv items and %zu bytes, "
	    "but response files are unavailable under toolset '%s' and the host command "
	    "limit is %zu bytes",
	    action_id, logical->len, logical->bytes, toolset, limit);
}

int
qstar_action_materialize(struct qstar_graph *graph, const char *action_id,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_argv *logical, const struct qstar_string_list *env,
    int write_response, struct qstar_materialized_command *command)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], rsp_arg[QSTAR_PATH_MAX];
	unsigned long long digest;
	const char *style;
	int needs_response, enabled;

	if (!graph || !action_id || !logical || logical->len == 0 || !command)
		return qstar_set_error(graph,
		    "qstar: action materialization requires a non-empty logical argv");
	memset(command, 0, sizeof(*command));
	command->logical_argc = logical->len;
	command->logical_bytes = logical->bytes;
	qstar_argv_digest(logical, command->logical_argv_digest,
	    sizeof(command->logical_argv_digest));
	needs_response = qstar_action_needs_response_file(logical);
	command->needs_response_file = needs_response;
	enabled = toolchain && toolchain->response_files;
	if (!needs_response || !enabled) {
		if (qstar_argv_clone(&command->exec_argv, logical) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
		command->exec_argc = command->exec_argv.len;
		if (!enabled && validate_full_command_limit(graph, action_id,
		    toolchain, logical, env, &command->host_limit) < 0) {
			qstar_materialized_command_free(command);
			return -1;
		}
		return 0;
	}
	style = toolchain->response_style[0] ?
	    toolchain->response_style : "posix";
	snprintf(command->response_style, sizeof(command->response_style),
	    "%s", style);
	qstar_mangle_label(action_id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "rsp/%s.rsp", name) >=
	    (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, command->response_file,
	    sizeof(command->response_file)) < 0)
		return qstar_set_error(graph, "qstar: response file path too long");
	digest = response_digest(logical, style);
	snprintf(command->response_digest, sizeof(command->response_digest),
	    "%016llx", digest);
	if (write_response &&
	    write_response_file(graph, logical, command) < 0)
		return -1;
	if (snprintf(rsp_arg, sizeof(rsp_arg), "@%s",
	    command->response_file) >= (int)sizeof(rsp_arg) ||
	    qstar_argv_push(&command->exec_argv, logical->items[0]) < 0 ||
	    qstar_argv_push(&command->exec_argv, rsp_arg) < 0) {
		qstar_materialized_command_free(command);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	command->exec_argc = command->exec_argv.len;
	command->uses_response_file = 1;
	return 0;
}

int
qstar_action_materialize_raw(struct qstar_graph *graph,
    const char *action_id, const struct qstar_resolved_toolchain *toolchain,
    char *const logical_argv[], const struct qstar_string_list *env,
    int write_response, struct qstar_materialized_command *command)
{
	struct qstar_argv logical;
	size_t n;

	memset(&logical, 0, sizeof(logical));
	logical.items = (char **)logical_argv;
	for (n = 0; logical_argv && logical_argv[n]; n++) {
		if (logical.bytes > SIZE_MAX - strlen(logical_argv[n]) - 1)
			return qstar_set_error(graph,
			    "qstar: logical argv byte count overflow");
		logical.bytes += strlen(logical_argv[n]) + 1;
	}
	logical.len = n;
	logical.cap = n;
	return qstar_action_materialize(graph, action_id, toolchain, &logical,
	    env, write_response, command);
}

void
qstar_materialized_command_free(struct qstar_materialized_command *command)
{
	if (!command)
		return;
	qstar_argv_free(&command->exec_argv);
	memset(command, 0, sizeof(*command));
}
