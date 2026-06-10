#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define QSTAR_EXEC_MAX_ARGV 256
#define QSTAR_ACTION_TIMEOUT_SEC 30
#define QSTAR_TEST_TIMEOUT_SEC 5
#define QSTAR_RESPONSE_ARGV_BYTES 512
#define QSTAR_HASH_INIT 1469598103934665603ULL
#define QSTAR_HASH_PRIME 1099511628211ULL

struct qstar_build_ctx {
	FILE *out;
	const char *root_label;
	int explain_cache;
	int explain_only;
	int jobs;
	int schedule_trace;
	int action_timeout_sec;
	int cancelled;
	struct qstar_state_entry {
		char *id;
		char *key;
		char *output;
		char *status;
		char *kind;
		char *argv_key;
		char *env_key;
		char *input_key;
		char *depfile_key;
		char *profile_key;
	} *prev, *next;
	size_t prev_len, prev_cap, next_len, next_cap;
	size_t run_count, skip_count, fail_count;
	size_t scheduled_count;
	struct qstar_compile_record {
		char *directory;
		char *file;
		char *output;
		char *command;
	} *compiles;
	size_t compile_len, compile_cap;
};

struct qstar_action_material {
	char argv_key[32];
	char env_key[32];
	char input_key[32];
	char depfile_key[32];
	char profile_key[32];
};

struct qstar_prepared_action {
	const struct qstar_target *target;
	const struct qstar_resolved_toolchain *toolchain;
	char id[QSTAR_PATH_MAX];
	char kind[32];
	char key[32];
	char depfile[QSTAR_PATH_MAX];
	char source_path[QSTAR_PATH_MAX];
	int wants_depfile;
	struct qstar_action_material material;
	char *argv[QSTAR_EXEC_MAX_ARGV];
	size_t argc;
	struct qstar_string_list outputs;
};

struct qstar_running_action {
	struct qstar_prepared_action *action;
	pid_t pid;
	time_t start;
	size_t slot;
	char name[QSTAR_PATH_MAX];
	char action_log[QSTAR_PATH_MAX];
};

/** 테스트 harness에서만 action timeout을 짧게 낮춘다. */
static int
action_timeout_sec_from_env(void)
{
	const char *env;
	char *end;
	long value;

	env = getenv("QSTAR_TEST_ACTION_TIMEOUT_SEC");
	if (!env || !*env)
		return QSTAR_ACTION_TIMEOUT_SEC;
	errno = 0;
	value = strtol(env, &end, 10);
	if (errno || *end || value < 1 || value > QSTAR_ACTION_TIMEOUT_SEC)
		return QSTAR_ACTION_TIMEOUT_SEC;
	return (int)value;
}

/** 디렉터리를 parent까지 포함해 만든다. */
static int
mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/** 파일 path의 parent directory를 package root 기준으로 만든다. */
static int
mkdir_parent_under_root(const struct qstar_graph *graph, const char *rel)
{
	char full[QSTAR_PATH_MAX];
	char *slash;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".", rel, full,
	    sizeof(full)) < 0)
		return -1;
	slash = strrchr(full, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(full);
}

/** package root 기준 상대 path를 절대 실행 path로 바꾼다. */
static int
full_path_under_root(const struct qstar_graph *graph, const char *rel, char *dst,
    size_t dstlen)
{
	return qstar_path_join(graph->package_root ? graph->package_root : ".", rel, dst,
	    dstlen);
}

/** 파일 존재 여부를 검사한다. */
static int
path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/** 준비된 action argv storage를 해제한다. */
static void
prepared_action_free(struct qstar_prepared_action *action)
{
	size_t i;

	for (i = 0; i < action->argc; i++)
		free(action->argv[i]);
	qstar_string_list_free(&action->outputs);
	memset(action, 0, sizeof(*action));
}

/** 준비된 action argv에 소유 문자열을 추가한다. */
static int
prepared_action_push_argv(struct qstar_graph *graph, struct qstar_prepared_action *action,
    const char *s)
{
	if (action->argc + 1 >= QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: compile argv too long");
	action->argv[action->argc] = qstar_strdup(s);
	if (!action->argv[action->argc])
		return qstar_set_error(graph, "qstar: out of memory");
	action->argc++;
	action->argv[action->argc] = NULL;
	return 0;
}

/** PATH 또는 명시 path에서 실행 파일을 찾을 수 있는지 검사한다. */
static int
command_exists(const char *cmd)
{
	const char *path, *start, *end;
	char candidate[QSTAR_PATH_MAX];
	size_t n;

	if (!cmd || !*cmd)
		return 0;
	if (strchr(cmd, '/'))
		return access(cmd, X_OK) == 0;
	path = getenv("PATH");
	if (!path)
		return 0;
	for (start = path; ; start = end + 1) {
		end = strchr(start, ':');
		n = end ? (size_t)(end - start) : strlen(start);
		if (n == 0)
			snprintf(candidate, sizeof(candidate), "%s", cmd);
		else if (n + 1 + strlen(cmd) + 1 > sizeof(candidate))
			goto next;
		else
			snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)n, start, cmd);
		if (access(candidate, X_OK) == 0)
			return 1;
next:
		if (!end)
			break;
	}
	return 0;
}

/** 작은 FNV-1a hash에 문자열을 섞는다. */
static void
hash_str(unsigned long long *h, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	while (*p) {
		*h ^= (unsigned long long)*p++;
		*h *= QSTAR_HASH_PRIME;
	}
	*h ^= 0xffU;
	*h *= QSTAR_HASH_PRIME;
}

/** action key에 file metadata와 content hash를 함께 섞는다. */
static void
hash_file(unsigned long long *h, const struct qstar_graph *graph, const char *rel)
{
	char path[QSTAR_PATH_MAX];
	unsigned char buf[4096];
	struct stat st;
	FILE *f;
	size_t n;
	char meta[128];

	hash_str(h, rel);
	if (full_path_under_root(graph, rel, path, sizeof(path)) < 0 || stat(path, &st) < 0) {
		hash_str(h, "<missing>");
		return;
	}
	snprintf(meta, sizeof(meta), "size=%lld mtime=%lld", (long long)st.st_size,
	    (long long)st.st_mtime);
	hash_str(h, meta);
	f = fopen(path, "rb");
	if (!f) {
		hash_str(h, "<unreadable>");
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		size_t i;
		for (i = 0; i < n; i++) {
			*h ^= (unsigned long long)buf[i];
			*h *= QSTAR_HASH_PRIME;
		}
	}
	fclose(f);
}

/** action argv를 key material에 섞는다. */
static void
hash_argv(unsigned long long *h, char *const argv[])
{
	size_t i;

	for (i = 0; argv[i]; i++)
		hash_str(h, argv[i]);
}

/** whitelisted environment를 action key에 섞는다. */
static void
hash_env_whitelist(unsigned long long *h)
{
	const char *keys[] = {"PATH", "SDKROOT", "CPATH", "LIBRARY_PATH", NULL};
	size_t i;

	for (i = 0; keys[i]; i++) {
		hash_str(h, keys[i]);
		hash_str(h, getenv(keys[i]) ? getenv(keys[i]) : "");
	}
}

static void
format_key(unsigned long long h, char *dst, size_t dstlen);

/** whitelisted environment만 별도 digest로 만든다. */
static void
compute_env_key(char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;

	hash_env_whitelist(&h);
	format_key(h, dst, dstlen);
}

/** action key를 hex 문자열로 만든다. */
static void
format_key(unsigned long long h, char *dst, size_t dstlen)
{
	snprintf(dst, dstlen, "%016llx", h);
}

/** argv vector만 별도 digest로 만든다. */
static void
compute_argv_key(char *const argv[], char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;

	hash_argv(&h, argv);
	format_key(h, dst, dstlen);
}

/** package-relative input list만 별도 digest로 만든다. */
static void
compute_input_key(struct qstar_graph *graph, const struct qstar_string_list *inputs,
    char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	if (inputs) {
		for (i = 0; i < inputs->len; i++)
			hash_file(&h, graph, inputs->items[i]);
	}
	format_key(h, dst, dstlen);
}

/** profile/toolchain 선택만 별도 digest로 만든다. */
static void
compute_profile_key(struct qstar_graph *graph,
    const struct qstar_resolved_toolchain *toolchain, char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	hash_str(&h, graph->profile.name ? graph->profile.name : "default");
	hash_str(&h, graph->profile.target ? graph->profile.target : "host");
	hash_str(&h, graph->profile.freestanding ? graph->profile.freestanding : "false");
	hash_str(&h, graph->profile.arch ? graph->profile.arch : "");
	hash_str(&h, graph->profile.cpu ? graph->profile.cpu : "");
	hash_str(&h, graph->profile.abi ? graph->profile.abi : "");
	hash_str(&h, graph->profile.allow_absolute_tools ?
	    graph->profile.allow_absolute_tools : "false");
	for (i = 0; i < graph->profile.path_tools.len; i++)
		hash_str(&h, graph->profile.path_tools.items[i]);
	for (i = 0; i < graph->profile.tool_overrides.len; i++)
		hash_str(&h, graph->profile.tool_overrides.items[i]);
	if (toolchain) {
		hash_str(&h, toolchain->name);
		hash_str(&h, toolchain->target);
		hash_str(&h, toolchain->stdlib_policy);
		hash_str(&h, toolchain->cc);
		hash_str(&h, toolchain->cxx);
		hash_str(&h, toolchain->ar);
		hash_str(&h, toolchain->linker);
		hash_str(&h, toolchain->sysroot);
		hash_str(&h, toolchain->resource_dir);
		hash_str(&h, toolchain->response_files ? "rsp=on" : "rsp=off");
		hash_str(&h, toolchain->response_style);
	}
	format_key(h, dst, dstlen);
}

/** action key 공통 material을 계산한다. */
static void
compute_action_key(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *id, const char *kind,
    char *const argv[], const struct qstar_string_list *inputs,
    const struct qstar_string_list *depfile_inputs, const char *output, char *dst,
    size_t dstlen, struct qstar_action_material *material)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	if (material) {
		compute_argv_key(argv, material->argv_key, sizeof(material->argv_key));
		compute_env_key(material->env_key, sizeof(material->env_key));
		compute_input_key(graph, inputs, material->input_key,
		    sizeof(material->input_key));
		compute_input_key(graph, depfile_inputs, material->depfile_key,
		    sizeof(material->depfile_key));
		compute_profile_key(graph, toolchain, material->profile_key,
		    sizeof(material->profile_key));
	}
	hash_str(&h, id);
	hash_str(&h, kind);
	hash_str(&h, target ? target->label : "<generated>");
	hash_str(&h, output);
	hash_str(&h, graph->profile.name ? graph->profile.name : "default");
	hash_str(&h, graph->profile.target ? graph->profile.target : "host");
	hash_str(&h, graph->profile.freestanding ? graph->profile.freestanding : "false");
	hash_str(&h, graph->profile.arch ? graph->profile.arch : "");
	hash_str(&h, graph->profile.cpu ? graph->profile.cpu : "");
	hash_str(&h, graph->profile.abi ? graph->profile.abi : "");
	hash_str(&h, graph->profile.allow_absolute_tools ?
	    graph->profile.allow_absolute_tools : "false");
	for (i = 0; i < graph->profile.path_tools.len; i++)
		hash_str(&h, graph->profile.path_tools.items[i]);
	for (i = 0; i < graph->profile.tool_overrides.len; i++)
		hash_str(&h, graph->profile.tool_overrides.items[i]);
	if (toolchain) {
		hash_str(&h, toolchain->name);
		hash_str(&h, toolchain->target);
		hash_str(&h, toolchain->stdlib_policy);
		hash_str(&h, toolchain->cc);
		hash_str(&h, toolchain->cxx);
		hash_str(&h, toolchain->ar);
		hash_str(&h, toolchain->linker);
		hash_str(&h, toolchain->sysroot);
		hash_str(&h, toolchain->resource_dir);
		hash_str(&h, toolchain->response_files ? "rsp=on" : "rsp=off");
		hash_str(&h, toolchain->response_style);
	}
	hash_argv(&h, argv);
	if (inputs) {
		for (i = 0; i < inputs->len; i++)
			hash_file(&h, graph, inputs->items[i]);
	}
	hash_env_whitelist(&h);
	format_key(h, dst, dstlen);
}

/** action id를 log filename으로 쓸 수 있게 정규화한다. */
static void
action_log_name(const char *id, char *dst, size_t dstlen)
{
	qstar_mangle_label(id, dst, dstlen);
}

/** shell replay/compile database에서 argv item 하나를 안전하게 quoting한다. */
static void
write_shell_arg(FILE *f, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");
	int simple;

	simple = *p != '\0';
	for (; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '/' ||
		    *p == ':' || *p == '=' || *p == '+' || *p == ','))
			simple = 0;
	}
	if (simple) {
		fputs(s, f);
		return;
	}
	fputc('\'', f);
	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '\'')
			fputs("'\\''", f);
		else
			fputc(*p, f);
	}
	fputc('\'', f);
}

/** Windows/MSVC response file에 들어갈 argv item을 double-quote 규칙으로 쓴다. */
static void
write_windows_response_arg(FILE *f, const char *s)
{
	const unsigned char *p;
	size_t backslashes;
	int quote;

	p = (const unsigned char *)(s ? s : "");
	quote = *p == '\0';
	for (; *p; p++) {
		if (isspace(*p) || *p == '"' || *p == '\\')
			quote = 1;
	}
	if (!quote) {
		fputs(s, f);
		return;
	}
	fputc('"', f);
	backslashes = 0;
	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '\\') {
			backslashes++;
			continue;
		}
		if (*p == '"') {
			while (backslashes--)
				fputs("\\\\", f);
			fputs("\\\"", f);
			continue;
		}
		while (backslashes--)
			fputc('\\', f);
		fputc(*p, f);
	}
	while (backslashes--)
		fputs("\\\\", f);
	fputc('"', f);
}

/** response file style에 맞춰 argv item 하나를 쓴다. */
static void
write_response_arg(FILE *f, const char *s, const char *style)
{
	if (style && (strcmp(style, "windows") == 0 || strcmp(style, "msvc") == 0))
		write_windows_response_arg(f, s);
	else
		write_shell_arg(f, s);
}

/** argv가 response file로 내릴 만큼 긴지 계산한다. */
static int
argv_needs_response_file(char *const argv[])
{
	size_t i, bytes;

	bytes = 0;
	for (i = 0; argv[i]; i++)
		bytes += strlen(argv[i]) + 1;
	return bytes >= QSTAR_RESPONSE_ARGV_BYTES || i > 48;
}

/** response file content digest를 deterministic FNV-1a 값으로 계산한다. */
static unsigned long long
response_file_digest(const char *id, char *const argv[], const char *style)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	hash_str(&h, id);
	hash_str(&h, style);
	for (i = 1; argv[i]; i++) {
		hash_str(&h, argv[i]);
		hash_str(&h, "\n");
	}
	return h;
}

/** compiler/linker response file을 package-local path에 쓴다. */
static int
prepare_response_file(struct qstar_graph *graph, const char *id,
    const struct qstar_resolved_toolchain *toolchain, char *const argv[], char *rsp_arg,
    size_t rsp_arg_len, FILE *out)
{
	char dir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	char full[QSTAR_PATH_MAX];
	const char *style;
	unsigned long long digest;
	FILE *f;
	size_t i;

	if (!argv_needs_response_file(argv))
		return 0;
	if (!toolchain || !toolchain->response_files) {
		fprintf(out, "response_file id=%s mode=unsupported capability=off\n", id);
		return 0;
	}
	style = toolchain->response_style[0] ? toolchain->response_style : "posix";
	if (full_path_under_root(graph, ".qstar/rsp", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0)
		return qstar_set_error(graph, "qstar: could not create response file dir");
	action_log_name(id, name, sizeof(name));
	if (snprintf(rel, sizeof(rel), ".qstar/rsp/%s.rsp", name) >= (int)sizeof(rel) ||
	    full_path_under_root(graph, rel, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: response file path too long");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write response file");
	for (i = 1; argv[i]; i++) {
		write_response_arg(f, argv[i], style);
		fputc('\n', f);
	}
	fclose(f);
	if (snprintf(rsp_arg, rsp_arg_len, "@%s", rel) >= (int)rsp_arg_len)
		return qstar_set_error(graph, "qstar: response file arg too long");
	digest = response_file_digest(id, argv, style);
	fprintf(out, "response_file id=%s path=%s mode=real style=%s digest=%016llx\n",
	    id, rel, style, digest);
	return 1;
}

/** argv 배열을 action log에 deterministic하게 기록한다. */
static void
write_action_log(const char *path, char *const argv[], int exit_code)
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "qstar-action-log v2\nexit=%d\n", exit_code);
	for (i = 0; argv[i]; i++)
		;
	fprintf(f, "argc=%zu\n", i);
	for (i = 0; argv[i]; i++) {
		fprintf(f, "argv[%zu]=", i);
		write_shell_arg(f, argv[i]);
		fputc('\n', f);
	}
	fputs("argv=", f);
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
	fputs("\nargv_shell=", f);
	for (i = 0; argv[i]; i++) {
		if (i)
			fputc(' ', f);
		write_shell_arg(f, argv[i]);
	}
	fputc('\n', f);
	fclose(f);
}

/** skipped action도 log index에 남긴다. */
static void
write_skip_log(const char *path, char *const argv[])
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (!f)
		return;
	fputs("qstar-action-log v2\nexit=skip\n", f);
	for (i = 0; argv[i]; i++)
		;
	fprintf(f, "argc=%zu\n", i);
	for (i = 0; argv[i]; i++) {
		fprintf(f, "argv[%zu]=", i);
		write_shell_arg(f, argv[i]);
		fputc('\n', f);
	}
	fputs("argv=", f);
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
	fputs("\nargv_shell=", f);
	for (i = 0; argv[i]; i++) {
		if (i)
			fputc(' ', f);
		write_shell_arg(f, argv[i]);
	}
	fputc('\n', f);
	fclose(f);
}

/** 실패한 action을 직접 재현할 수 있는 argv 파일을 갱신한다. */
static void
write_failure_replay(const struct qstar_graph *graph, const char *id,
    const struct qstar_resolved_toolchain *toolchain, char *const argv[])
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;
	unsigned long long digest;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs/last-failure.replay", path, sizeof(path)) < 0)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# qstar failure replay v2\ncd %s\n",
	    graph->package_root ? graph->package_root : ".");
	digest = QSTAR_HASH_INIT;
	for (i = 0; argv[i]; i++)
		hash_str(&digest, argv[i]);
	fprintf(f, "argv_digest=%016llx\n", digest);
	if (toolchain && toolchain->response_files && argv_needs_response_file(argv)) {
		char name[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
		const char *style = toolchain->response_style[0] ?
		    toolchain->response_style : "posix";
		action_log_name(id, name, sizeof(name));
		snprintf(rel, sizeof(rel), ".qstar/rsp/%s.rsp", name);
		fprintf(f, "response_file path=%s style=%s digest=%016llx\n", rel, style,
		    response_file_digest(id, argv, style));
	} else {
		fputs("response_file path=<none> style=none digest=<none>\n", f);
	}
	for (i = 0; argv[i]; i++) {
		if (i)
			fputc(' ', f);
		write_shell_arg(f, argv[i]);
	}
	fputc('\n', f);
	fclose(f);
}

/** JSON string에 들어갈 최소 escape를 출력한다. */
static void
json_string(FILE *f, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	fputc('"', f);
	while (*p) {
		if (*p == '"' || *p == '\\')
			fprintf(f, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", f);
		else if (*p == '\r')
			fputs("\\r", f);
		else if (*p == '\t')
			fputs("\\t", f);
		else
			fputc(*p, f);
		p++;
	}
	fputc('"', f);
}

/** action state entry 동적 배열에 record를 추가한다. */
static int
state_push(struct qstar_build_ctx *ctx, int next, const char *id, const char *key,
    const char *output, const char *status, const char *kind,
    const struct qstar_action_material *material)
{
	struct qstar_state_entry **items;
	size_t *len, *cap;
	struct qstar_state_entry *p;
	size_t ncap;

	items = next ? &ctx->next : &ctx->prev;
	len = next ? &ctx->next_len : &ctx->prev_len;
	cap = next ? &ctx->next_cap : &ctx->prev_cap;
	if (*len == *cap) {
		ncap = *cap ? *cap * 2 : 16;
		p = realloc(*items, ncap * sizeof((*items)[0]));
		if (!p)
			return -1;
		*items = p;
		*cap = ncap;
	}
	p = &(*items)[(*len)++];
	p->id = qstar_strdup(id);
	p->key = qstar_strdup(key);
	p->output = qstar_strdup(output);
	p->status = qstar_strdup(status);
	p->kind = qstar_strdup(kind ? kind : "");
	p->argv_key = qstar_strdup(material ? material->argv_key : "");
	p->env_key = qstar_strdup(material ? material->env_key : "");
	p->input_key = qstar_strdup(material ? material->input_key : "");
	p->depfile_key = qstar_strdup(material ? material->depfile_key : "");
	p->profile_key = qstar_strdup(material ? material->profile_key : "");
	return p->id && p->key && p->output && p->status && p->kind &&
	    p->argv_key && p->env_key && p->input_key && p->depfile_key &&
	    p->profile_key ? 0 : -1;
}

/** state entry 배열을 해제한다. */
static void
state_free(struct qstar_state_entry *entries, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		free(entries[i].id);
		free(entries[i].key);
		free(entries[i].output);
		free(entries[i].status);
		free(entries[i].kind);
		free(entries[i].argv_key);
		free(entries[i].env_key);
		free(entries[i].input_key);
		free(entries[i].depfile_key);
		free(entries[i].profile_key);
	}
	free(entries);
}

/** 이전 state에서 action id를 찾는다. */
static const struct qstar_state_entry *
state_find(const struct qstar_build_ctx *ctx, const char *id)
{
	size_t i;

	for (i = 0; i < ctx->prev_len; i++) {
		if (strcmp(ctx->prev[i].id, id) == 0)
			return &ctx->prev[i];
	}
	return NULL;
}

/** next state에 이미 기록된 action key/material을 성공 후 갱신한다. */
static int
state_update_material(struct qstar_build_ctx *ctx, const char *id, const char *key,
    const struct qstar_action_material *material)
{
	struct qstar_state_entry *entry;
	size_t i;
	char *copy;

	for (i = 0; i < ctx->next_len; i++) {
		if (strcmp(ctx->next[i].id, id) != 0)
			continue;
		entry = &ctx->next[i];
		copy = qstar_strdup(key);
		if (!copy)
			return -1;
		free(entry->key);
		entry->key = copy;
		if (!material)
			return 0;
#define REPLACE_FIELD(field, value) \
	do { \
		copy = qstar_strdup(value); \
		if (!copy) \
			return -1; \
		free(entry->field); \
		entry->field = copy; \
	} while (0)
		REPLACE_FIELD(argv_key, material->argv_key);
		REPLACE_FIELD(env_key, material->env_key);
		REPLACE_FIELD(input_key, material->input_key);
		REPLACE_FIELD(depfile_key, material->depfile_key);
		REPLACE_FIELD(profile_key, material->profile_key);
#undef REPLACE_FIELD
		return 0;
	}
	return 0;
}

/** QStar가 쓴 flat JSON object line에서 string field 하나를 뽑는다. */
static void
json_field_copy(const char *line, const char *field, char *dst, size_t dstlen)
{
	char pat[64];
	const char *p, *end;
	size_t n;

	dst[0] = '\0';
	snprintf(pat, sizeof(pat), "\"%s\":\"", field);
	p = strstr(line, pat);
	if (!p)
		return;
	p += strlen(pat);
	end = strchr(p, '"');
	if (!end)
		return;
	n = (size_t)(end - p);
	if (n >= dstlen)
		n = dstlen - 1;
	memcpy(dst, p, n);
	dst[n] = '\0';
}

/** 매우 작은 actions.json reader: QStar가 쓴 one-object-per-line만 읽는다. */
static int
state_load(struct qstar_graph *graph, struct qstar_build_ctx *ctx)
{
	char path[QSTAR_PATH_MAX], line[8192];
	char id[QSTAR_PATH_MAX], key[64], output[QSTAR_PATH_MAX], status[64], kind[64];
	struct qstar_action_material material;
	FILE *f;

	if (full_path_under_root(graph, ".qstar/state/actions.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: state path too long");
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		memset(&material, 0, sizeof(material));
		json_field_copy(line, "id", id, sizeof(id));
		json_field_copy(line, "key", key, sizeof(key));
		json_field_copy(line, "output", output, sizeof(output));
		json_field_copy(line, "status", status, sizeof(status));
		json_field_copy(line, "kind", kind, sizeof(kind));
		json_field_copy(line, "argv_key", material.argv_key,
		    sizeof(material.argv_key));
		json_field_copy(line, "env_key", material.env_key, sizeof(material.env_key));
		json_field_copy(line, "input_key", material.input_key,
		    sizeof(material.input_key));
		json_field_copy(line, "depfile_key", material.depfile_key,
		    sizeof(material.depfile_key));
		json_field_copy(line, "profile_key", material.profile_key,
		    sizeof(material.profile_key));
		if (!id[0] || !key[0] || !output[0] || !status[0])
			continue;
		if (state_push(ctx, 0, id, key, output, status, kind, &material) < 0) {
			fclose(f);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	fclose(f);
	return 0;
}

/** 현재 action state를 deterministic JSON으로 쓴다. */
static int
state_write(struct qstar_graph *graph, const struct qstar_build_ctx *ctx)
{
	char dir[QSTAR_PATH_MAX], path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (full_path_under_root(graph, ".qstar/state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_root(graph, ".qstar/state/actions.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: could not create state dir");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write action state");
	fputs("[\n", f);
	for (i = 0; i < ctx->next_len; i++) {
		fputs("  {\"id\":", f);
		json_string(f, ctx->next[i].id);
		fputs(",\"key\":", f);
		json_string(f, ctx->next[i].key);
		fputs(",\"output\":", f);
		json_string(f, ctx->next[i].output);
		fputs(",\"status\":", f);
		json_string(f, ctx->next[i].status);
		fputs(",\"kind\":", f);
		json_string(f, ctx->next[i].kind);
		fputs(",\"argv_key\":", f);
		json_string(f, ctx->next[i].argv_key);
		fputs(",\"env_key\":", f);
		json_string(f, ctx->next[i].env_key);
		fputs(",\"input_key\":", f);
		json_string(f, ctx->next[i].input_key);
		fputs(",\"depfile_key\":", f);
		json_string(f, ctx->next[i].depfile_key);
		fputs(",\"profile_key\":", f);
		json_string(f, ctx->next[i].profile_key);
		fputs("}", f);
		fputs(i + 1 == ctx->next_len ? "\n" : ",\n", f);
	}
	fputs("]\n", f);
	fclose(f);
	if (rename(tmp, path) < 0)
		return qstar_set_error(graph, "qstar: could not commit action state");
	return 0;
}

/** JSON string list를 compact array로 출력한다. */
static void
json_string_list(FILE *f, const struct qstar_string_list *list)
{
	size_t i;

	fputc('[', f);
	for (i = 0; i < list->len; i++) {
		if (i)
			fputc(',', f);
		json_string(f, list->items[i]);
	}
	fputc(']', f);
}

/** generated action output artifact metadata를 graph snapshot JSON에 쓴다. */
static void
json_generated_output_artifacts(FILE *f, const struct qstar_genrule *genrule)
{
	char identity[QSTAR_PATH_MAX];
	size_t i;

	fputc('[', f);
	for (i = 0; i < genrule->outputs.len; i++) {
		if (i)
			fputc(',', f);
		if (qstar_genrule_output_identity(genrule, i, identity,
		    sizeof(identity)) < 0)
			snprintf(identity, sizeof(identity), "<too-long>");
		fputs("{\"path\":", f);
		json_string(f, genrule->outputs.items[i]);
		fputs(",\"group\":", f);
		json_string(f, qstar_genrule_output_group(genrule, i));
		fputs(",\"format\":", f);
		json_string(f, qstar_genrule_output_format(genrule, i));
		fputs(",\"address\":", f);
		json_string(f, qstar_genrule_output_address(genrule, i));
		fputs(",\"layout\":", f);
		json_string(f, qstar_genrule_output_layout(genrule, i));
		fputs(",\"identity\":", f);
		json_string(f, identity);
		fputc('}', f);
	}
	fputc(']', f);
}

/** 현재 평가된 target/generated graph snapshot을 state에 저장한다. */
static int
graph_snapshot_write(struct qstar_graph *graph)
{
	char dir[QSTAR_PATH_MAX], path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (full_path_under_root(graph, ".qstar/state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_root(graph, ".qstar/state/graph.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: could not create graph snapshot dir");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write graph snapshot");
	fputs("{\"schema\":\"qstar-graph-snapshot-v1\",\"package_root\":", f);
	json_string(f, graph->package_root ? graph->package_root : ".");
	fputs(",\"profile\":{", f);
	fputs("\"name\":", f);
	json_string(f, graph->profile.name ? graph->profile.name : "default");
	fputs(",\"target\":", f);
	json_string(f, graph->profile.target ? graph->profile.target : "host");
	fputs(",\"toolchain\":", f);
	json_string(f, graph->profile.toolchain ? graph->profile.toolchain : "default");
	fputs("},\"targets\":[\n", f);
	for (i = 0; i < graph->len; i++) {
		if (i)
			fputs(",\n", f);
		fputs("  {\"label\":", f);
		json_string(f, graph->targets[i].label);
		fputs(",\"kind\":", f);
		json_string(f, graph->targets[i].kind);
		fputs(",\"artifact_name\":", f);
		json_string(f, graph->targets[i].artifact_name &&
		    *graph->targets[i].artifact_name ? graph->targets[i].artifact_name : "");
		fputs(",\"origin\":", f);
		json_string(f, graph->targets[i].origin_file);
		fprintf(f, ",\"line\":%d,\"sources\":", graph->targets[i].origin_line);
		json_string_list(f, &graph->targets[i].sources);
		fputs(",\"deps\":", f);
		json_string_list(f, &graph->targets[i].deps);
		fputs(",\"private_deps\":", f);
		json_string_list(f, &graph->targets[i].private_deps);
		fputc('}', f);
	}
	fputs("\n],\"generated_actions\":[\n", f);
	for (i = 0; i < graph->genrule_len; i++) {
		if (i)
			fputs(",\n", f);
		fputs("  {\"label\":", f);
		json_string(f, graph->genrules[i].label);
		fputs(",\"config_header\":", f);
		fputs(graph->genrules[i].config_header ? "true" : "false", f);
		fputs(",\"outputs\":", f);
		json_string_list(f, &graph->genrules[i].outputs);
		fputs(",\"output_artifacts\":", f);
		json_generated_output_artifacts(f, &graph->genrules[i]);
		fputc('}', f);
	}
	fputs("\n]}\n", f);
	fclose(f);
	if (rename(tmp, path) < 0)
		return qstar_set_error(graph, "qstar: could not commit graph snapshot");
	return 0;
}

/** 마지막 build 성공/실패 summary를 state에 저장한다. */
static int
build_summary_write(struct qstar_graph *graph, const struct qstar_build_ctx *ctx,
    const char *status)
{
	char dir[QSTAR_PATH_MAX], path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;

	if (full_path_under_root(graph, ".qstar/state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_root(graph, ".qstar/state/last-summary.json", path,
	    sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: could not create build summary dir");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write build summary");
	fputs("{\"schema\":\"qstar-build-summary-v1\",\"root\":", f);
	json_string(f, ctx->root_label);
	fputs(",\"status\":", f);
	json_string(f, status);
	fprintf(f,
	    ",\"run\":%zu,\"skip\":%zu,\"fail\":%zu,\"scheduled\":%zu,\"jobs\":%d,\"cancelled\":%s}\n",
	    ctx->run_count, ctx->skip_count, ctx->fail_count, ctx->scheduled_count,
	    ctx->jobs, ctx->cancelled ? "true" : "false");
	fclose(f);
	if (rename(tmp, path) < 0)
		return qstar_set_error(graph, "qstar: could not commit build summary");
	return 0;
}

/** compile_commands.json command string용으로 argv를 join한다. */
static char *
join_argv(char *const argv[])
{
	size_t i, j, len, used;
	char *s;
	const char *p;
	int simple;

	len = 1;
	for (i = 0; argv[i]; i++)
		len += strlen(argv[i]) * 4 + 4;
	s = malloc(len);
	if (!s)
		return NULL;
	used = 0;
	for (i = 0; argv[i]; i++) {
		if (i)
			s[used++] = ' ';
		simple = argv[i][0] != '\0';
		for (p = argv[i]; *p; p++) {
			if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' ||
			    *p == '.' || *p == '/' || *p == ':' || *p == '=' ||
			    *p == '+' || *p == ','))
				simple = 0;
		}
		if (simple) {
			for (p = argv[i]; *p; p++)
				s[used++] = *p;
			continue;
		}
		s[used++] = '\'';
		for (j = 0; argv[i][j]; j++) {
			if (argv[i][j] == '\'') {
				memcpy(s + used, "'\\''", 4);
				used += 4;
			} else {
				s[used++] = argv[i][j];
			}
		}
		s[used++] = '\'';
	}
	s[used] = '\0';
	return s;
}

/** compile_commands record를 build context에 추가한다. */
static int
compile_db_push(struct qstar_build_ctx *ctx, const char *directory, const char *file,
    const char *output, char *const argv[])
{
	struct qstar_compile_record *p;
	char *command;
	size_t cap;

	if (ctx->compile_len == ctx->compile_cap) {
		cap = ctx->compile_cap ? ctx->compile_cap * 2 : 16;
		p = realloc(ctx->compiles, cap * sizeof(ctx->compiles[0]));
		if (!p)
			return -1;
		ctx->compiles = p;
		ctx->compile_cap = cap;
	}
	command = join_argv(argv);
	if (!command)
		return -1;
	p = &ctx->compiles[ctx->compile_len++];
	p->directory = qstar_strdup(directory);
	p->file = qstar_strdup(file);
	p->output = qstar_strdup(output);
	p->command = command;
	return p->directory && p->file && p->output;
}

/** compile_commands record 저장소를 해제한다. */
static void
compile_db_free(struct qstar_compile_record *records, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		free(records[i].directory);
		free(records[i].file);
		free(records[i].output);
		free(records[i].command);
	}
	free(records);
}

/** build에서 수집한 compile_commands.json을 package root에 쓴다. */
static int
compile_db_write(struct qstar_graph *graph, const struct qstar_build_ctx *ctx)
{
	char path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (full_path_under_root(graph, "compile_commands.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: compile_commands path too long");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write compile_commands.json");
	fputs("[\n", f);
	for (i = 0; i < ctx->compile_len; i++) {
		fputs("  {\"directory\":", f);
		json_string(f, ctx->compiles[i].directory);
		fputs(",\"file\":", f);
		json_string(f, ctx->compiles[i].file);
		fputs(",\"output\":", f);
		json_string(f, ctx->compiles[i].output);
		fputs(",\"command\":", f);
		json_string(f, ctx->compiles[i].command);
		fputs("}", f);
		fputs(i + 1 == ctx->compile_len ? "\n" : ",\n", f);
	}
	fputs("]\n", f);
	fclose(f);
	if (rename(tmp, path) < 0)
		return qstar_set_error(graph, "qstar: could not commit compile_commands.json");
	return 0;
}

/** action output들이 모두 존재하는지 확인한다. */
static int
outputs_exist(struct qstar_graph *graph, const struct qstar_string_list *outputs)
{
	char full[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < outputs->len; i++) {
		if (full_path_under_root(graph, outputs->items[i], full, sizeof(full)) < 0 ||
		    !path_exists(full))
			return 0;
	}
	return outputs->len > 0;
}

/** 이전 state와 새 material을 비교해 cache miss 이유를 안정적으로 분류한다. */
static const char *
cache_reason(struct qstar_graph *graph, const struct qstar_state_entry *prev,
    const char *key, const struct qstar_string_list *outputs,
    const struct qstar_action_material *material)
{
	if (!prev)
		return "no-previous-state";
	if (strcmp(prev->key, key) == 0 && !outputs_exist(graph, outputs))
		return "output-missing";
	if (strcmp(prev->key, key) == 0)
		return "output-check";
	if (material) {
		if (prev->argv_key && *prev->argv_key &&
		    strcmp(prev->argv_key, material->argv_key) != 0)
			return "argv-changed";
		if (prev->env_key && *prev->env_key &&
		    strcmp(prev->env_key, material->env_key) != 0)
			return "env-changed";
		if (prev->depfile_key && *prev->depfile_key &&
		    strcmp(prev->depfile_key, material->depfile_key) != 0)
			return "depfile-changed";
		if (prev->input_key && *prev->input_key &&
		    strcmp(prev->input_key, material->input_key) != 0)
			return "input-changed";
		if (prev->profile_key && *prev->profile_key &&
		    strcmp(prev->profile_key, material->profile_key) != 0)
			return "profile-changed";
	}
	return "key-changed";
}

/** 빈 stdout/stderr log 파일을 만든다. */
static void
write_empty_log_file(const char *path)
{
	FILE *f;

	f = fopen(path, "w");
	if (f)
		fclose(f);
}

/** stdout/stderr를 log 파일에 연결해 package root 안에서 argv를 실행하거나 cache skip한다. */
static int
run_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const char *id, const char *kind, const char *key,
    const struct qstar_string_list *outputs, char *const argv[],
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_action_material *material)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX];
	char stderr_path[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	char rsp_arg[QSTAR_PATH_MAX];
	char *exec_argv[3];
	char *const *child_argv;
	const struct qstar_state_entry *prev;
	pid_t pid;
	int status, fdout, fderr, exit_code, use_rsp;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create action log dir");
	action_log_name(id, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, name);
	snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
	snprintf(child_stdout_path, sizeof(child_stdout_path), ".qstar/logs/%s.stdout", name);
	snprintf(child_stderr_path, sizeof(child_stderr_path), ".qstar/logs/%s.stderr", name);
	prev = state_find(ctx, id);
	ctx->scheduled_count++;
	if (ctx->schedule_trace)
		fprintf(ctx->out,
		    "schedule_action id=%s kind=%s slot=0 jobs=%d state=ready\n",
		    id, kind, ctx->jobs);
	if (ctx->explain_only) {
		fprintf(ctx->out,
		    "cache_action id=%s kind=%s status=%s reason=%s key=%s previous=%s\n",
		    id, kind,
		    prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, outputs) ?
		    "skip" : "run",
		    cache_reason(graph, prev, key, outputs, material),
		    key, prev ? prev->key : "<none>");
		return 0;
	}
	if (prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, outputs)) {
		fprintf(ctx->out,
		    "build_action id=%s status=skip reason=cache-hit stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
		    id, name, name);
		write_skip_log(action_log, argv);
		ctx->skip_count++;
		return state_push(ctx, 1, id, key, outputs->items[0], "skip", kind,
		    material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	fprintf(ctx->out,
	    "build_action id=%s status=run timeout_sec=%d stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    id, ctx->action_timeout_sec, name, name);
	if (ctx->explain_cache)
		fprintf(ctx->out, "cache_miss id=%s reason=%s key=%s previous=%s\n",
		    id, cache_reason(graph, prev, key, outputs, material), key,
		    prev ? prev->key : "<none>");
	use_rsp = prepare_response_file(graph, id, toolchain, argv, rsp_arg, sizeof(rsp_arg),
	    ctx->out);
	if (use_rsp < 0)
		return -1;
	child_argv = argv;
	if (use_rsp) {
		exec_argv[0] = argv[0];
		exec_argv[1] = rsp_arg;
		exec_argv[2] = NULL;
		child_argv = exec_argv;
	}
	pid = fork();
	if (pid < 0)
		return qstar_set_error(graph, "qstar: fork failed");
	if (pid == 0) {
		if (chdir(graph->package_root ? graph->package_root : ".") < 0)
			_exit(127);
		fdout = open(child_stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fderr = open(child_stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fdout < 0 || fderr < 0 || dup2(fdout, 1) < 0 || dup2(fderr, 2) < 0)
			_exit(127);
		close(fdout);
		close(fderr);
		execvp(child_argv[0], child_argv);
		_exit(127);
	}
	{
		time_t start;
		pid_t waited;

		start = time(NULL);
		for (;;) {
			waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid)
				break;
			if (waited < 0)
				return qstar_set_error(graph, "qstar: waitpid failed");
			if (time(NULL) - start >= ctx->action_timeout_sec) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				write_action_log(action_log, argv, 124);
				write_failure_replay(graph, id, toolchain, argv);
				ctx->fail_count++;
				ctx->cancelled = 1;
				fprintf(ctx->out,
				    "build_action id=%s status=timeout timeout_sec=%d\n",
				    id, ctx->action_timeout_sec);
				return qstar_set_error_origin(graph,
				    target ? target->origin_file : "",
				    target ? target->origin_line : 0, "action",
				    target ? target->label : id,
				    "qstar: action '%s' timed out after %d seconds; replay=.qstar/logs/last-failure.replay",
				    id, ctx->action_timeout_sec);
			}
			sleep(1);
		}
	}
	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	write_action_log(action_log, argv, exit_code);
	if (exit_code != 0) {
		fprintf(ctx->out, "build_action id=%s status=fail exit=%d\n", id, exit_code);
		write_failure_replay(graph, id, toolchain, argv);
		ctx->fail_count++;
		ctx->cancelled = 1;
		return qstar_set_error_origin(graph, target ? target->origin_file : "",
		    target ? target->origin_line : 0, "action", target ? target->label : id,
		    "qstar: action '%s' failed with status %d; replay=.qstar/logs/last-failure.replay",
		    id, exit_code);
	}
	ctx->run_count++;
	if (state_push(ctx, 1, id, key, outputs->items[0], "run", kind, material) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** graph에서 canonical label target을 찾는다. */
static const struct qstar_target *
find_target(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return &graph->targets[i];
	}
	return NULL;
}

/** 임시 string list에 중복 없이 문자열을 추가한다. */
static int
push_unique_tmp(struct qstar_string_list *list, const char *s)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], s) == 0)
			return 0;
	}
	return qstar_string_list_push(list, s);
}

/** public/interface include dirs를 public dependency graph를 따라 수집한다. */
static int
collect_public_includes_rec(const struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_string_list *out, int include_self)
{
	const struct qstar_target *dep;
	size_t i;

	if (include_self) {
		for (i = 0; i < target->public_include_dirs.len; i++) {
			if (push_unique_tmp(out, target->public_include_dirs.items[i]) < 0)
				return -1;
		}
		for (i = 0; i < target->interface_include_dirs.len; i++) {
			if (push_unique_tmp(out, target->interface_include_dirs.items[i]) < 0)
				return -1;
		}
	}
	for (i = 0; i < target->deps.len; i++) {
		dep = find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		if (collect_public_includes_rec(graph, dep, out, 1) < 0)
			return -1;
	}
	return 0;
}

/** target compile argv에 들어갈 self/private/public dependency include dirs를 수집한다. */
static int
collect_compile_include_dirs(const struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_string_list *out)
{
	size_t i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < target->include_dirs.len; i++) {
		if (push_unique_tmp(out, target->include_dirs.items[i]) < 0)
			return -1;
	}
	if (collect_public_includes_rec(graph, target, out, 0) < 0)
		return -1;
	return 0;
}

/** profile freestanding 값을 boolean으로 해석한다. */
static int
profile_is_freestanding(const struct qstar_graph *graph)
{
	const char *v;

	v = graph->profile.freestanding;
	return v && *v && strcmp(v, "false") != 0 && strcmp(v, "0") != 0 &&
	    strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

/** profile arch가 없을 때 target triple에서 arch 판단에 쓸 문자열을 고른다. */
static const char *
profile_arch_hint(const struct qstar_graph *graph)
{
	return graph->profile.arch && *graph->profile.arch ? graph->profile.arch :
	    graph->profile.target && *graph->profile.target ? graph->profile.target : "";
}

/** freestanding/cpu/abi profile에서 자동 compile option 개수를 계산한다. */
static size_t
profile_compile_option_count(const struct qstar_graph *graph)
{
	size_t n;
	const char *arch;

	n = 0;
	if (profile_is_freestanding(graph)) {
		n += 3;
		arch = profile_arch_hint(graph);
		if (strstr(arch, "x86_64") || strstr(arch, "amd64"))
			n++;
		if (strstr(arch, "aarch64") || strstr(arch, "arm64"))
			n++;
	}
	if (graph->profile.cpu && *graph->profile.cpu)
		n++;
	if (graph->profile.abi && *graph->profile.abi)
		n++;
	return n;
}

/** freestanding/cpu/abi profile에서 자동 compile option을 argv에 추가한다. */
static int
append_profile_compile_options(struct qstar_graph *graph, struct qstar_prepared_action *action)
{
	char arg[QSTAR_PATH_MAX];
	const char *arch;

	if (profile_is_freestanding(graph)) {
		if (prepared_action_push_argv(graph, action, "-ffreestanding") < 0 ||
		    prepared_action_push_argv(graph, action, "-fno-builtin") < 0 ||
		    prepared_action_push_argv(graph, action, "-fno-stack-protector") < 0)
			return -1;
		arch = profile_arch_hint(graph);
		if ((strstr(arch, "x86_64") || strstr(arch, "amd64")) &&
		    prepared_action_push_argv(graph, action, "-mno-red-zone") < 0)
			return -1;
		if ((strstr(arch, "aarch64") || strstr(arch, "arm64")) &&
		    prepared_action_push_argv(graph, action, "-mgeneral-regs-only") < 0)
			return -1;
	}
	if (graph->profile.cpu && *graph->profile.cpu) {
		snprintf(arg, sizeof(arg), "-mcpu=%s", graph->profile.cpu);
		if (prepared_action_push_argv(graph, action, arg) < 0)
			return -1;
	}
	if (graph->profile.abi && *graph->profile.abi) {
		snprintf(arg, sizeof(arg), "-mabi=%s", graph->profile.abi);
		if (prepared_action_push_argv(graph, action, arg) < 0)
			return -1;
	}
	return 0;
}

/** source language가 assembler 계열인지 확인한다. */
static int
source_is_asm(const struct qstar_source_info *source)
{
	return strcmp(source->language, "asm") == 0 ||
	    strcmp(source->language, "asm-cpp") == 0;
}

/** ASM source가 C preprocessor를 거쳐야 하는지 확인한다. */
static int
source_uses_asm_preprocessor(const struct qstar_target *target,
    const struct qstar_source_info *source)
{
	return strcmp(source->language, "asm-cpp") == 0 ||
	    (strcmp(source->language, "asm") == 0 && target->asm_preprocess);
}

/** generated output list가 target 파일 입력 list에 소비되는지 검사한다. */
static int
generated_output_in_list(const struct qstar_genrule *genrule, const struct qstar_string_list *list)
{
	size_t i, j;

	for (i = 0; i < genrule->outputs.len; i++) {
		for (j = 0; j < list->len; j++) {
			if (strcmp(genrule->outputs.items[i], list->items[j]) == 0)
				return 1;
		}
	}
	return 0;
}

/** target이 generated action output을 source/header로 소비하는지 검사한다. */
static int
target_consumes_genrule(const struct qstar_target *target, const struct qstar_genrule *genrule)
{
	return generated_output_in_list(genrule, &target->sources) ||
	    generated_output_in_list(genrule, &target->public_headers) ||
	    generated_output_in_list(genrule, &target->private_headers);
}

/** config header include guard에 안전한 identifier 조각을 만든다. */
static void
config_guard_name(const char *output, char *dst, size_t dstlen)
{
	size_t i;
	unsigned char c;

	if (!dstlen)
		return;
	snprintf(dst, dstlen, "QSTAR_CONFIG_");
	for (i = strlen(dst); *output && i + 1 < dstlen; output++) {
		c = (unsigned char)*output;
		dst[i++] = isalnum(c) ? (char)toupper(c) : '_';
	}
	dst[i] = '\0';
}

/** qstar.configure_file define 항목 하나를 C preprocessor line으로 출력한다. */
static void
write_config_define(FILE *f, const char *def)
{
	const char *eq;

	eq = strchr(def, '=');
	if (eq)
		fprintf(f, "#define %.*s %s\n", (int)(eq - def), def, eq + 1);
	else
		fprintf(f, "#define %s 1\n", def);
}

/** qstar.configure_file output 파일을 deterministic하게 생성한다. */
static int
write_config_header(struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	char full[QSTAR_PATH_MAX], guard[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (genrule->outputs.len != 1)
		return qstar_set_error_origin(graph, genrule->origin_file, genrule->origin_line,
		    "outputs", genrule->label,
		    "qstar: config header '%s' must have exactly one output",
		    genrule->label);
	if (mkdir_parent_under_root(graph, genrule->outputs.items[0]) < 0 ||
	    full_path_under_root(graph, genrule->outputs.items[0], full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: could not create config header output");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error_origin(graph, genrule->origin_file, genrule->origin_line,
		    "outputs", genrule->label,
		    "qstar: could not write config header '%s'",
		    genrule->outputs.items[0]);
	config_guard_name(genrule->outputs.items[0], guard, sizeof(guard));
	fprintf(f, "/* generated by qstar.configure_file: %s */\n", genrule->label);
	fprintf(f, "#ifndef %s\n#define %s\n", guard, guard);
	for (i = 0; i < genrule->args.len; i++)
		write_config_define(f, genrule->args.items[i]);
	fprintf(f, "#endif /* %s */\n", guard);
	fclose(f);
	return 0;
}

/** qstar.configure_file을 external process 없이 action/cache model에 맞춰 실행한다. */
static int
run_config_header_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule, const char *id,
    const char *key, char *const argv[], const struct qstar_action_material *material)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX];
	char stderr_path[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	const struct qstar_state_entry *prev;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create action log dir");
	action_log_name(id, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, name);
	snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
	prev = state_find(ctx, id);
	ctx->scheduled_count++;
	if (ctx->schedule_trace)
		fprintf(ctx->out,
		    "schedule_action id=%s kind=generate slot=0 jobs=%d state=ready\n",
		    id, ctx->jobs);
	if (ctx->explain_only) {
		fprintf(ctx->out,
		    "cache_action id=%s kind=generate status=%s reason=%s key=%s previous=%s\n",
		    id,
		    prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, &genrule->outputs) ?
		    "skip" : "run",
		    cache_reason(graph, prev, key, &genrule->outputs, material),
		    key, prev ? prev->key : "<none>");
		return 0;
	}
	if (prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, &genrule->outputs)) {
		fprintf(ctx->out,
		    "build_action id=%s status=skip reason=cache-hit stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
		    id, name, name);
		write_skip_log(action_log, argv);
		ctx->skip_count++;
		return state_push(ctx, 1, id, key, genrule->outputs.items[0], "skip",
		    "generate", material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	fprintf(ctx->out,
	    "build_action id=%s status=run timeout_sec=internal stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    id, name, name);
	if (ctx->explain_cache)
		fprintf(ctx->out, "cache_miss id=%s reason=%s key=%s previous=%s\n",
		    id, cache_reason(graph, prev, key, &genrule->outputs, material), key,
		    prev ? prev->key : "<none>");
	if (write_config_header(graph, genrule) < 0) {
		write_failure_replay(graph, id, NULL, argv);
		ctx->fail_count++;
		ctx->cancelled = 1;
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "action", target->label, "%s", graph->error);
	}
	write_empty_log_file(stdout_path);
	write_empty_log_file(stderr_path);
	write_action_log(action_log, argv, 0);
	ctx->run_count++;
	return state_push(ctx, 1, id, key, genrule->outputs.items[0], "run",
	    "generate", material) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

/** generated action argv가 package root 밖 path를 직접 참조하는지 검사한다. */
static int
generated_arg_escapes_package(const char *arg)
{
	return arg && (arg[0] == '/' || strcmp(arg, "..") == 0 ||
	    strncmp(arg, "../", 3) == 0 || strstr(arg, "/../") ||
	    strstr(arg, "=../") || strstr(arg, ":../"));
}

/** qstar.target_file placeholder를 target artifact path로 해석한다. */
static int
resolve_target_file_token(struct qstar_graph *graph, const char *arg, char *dst,
    size_t dstlen)
{
	const char *prefix = "<qstar-target-file:";
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX];
	size_t n;

	if (strncmp(arg, prefix, strlen(prefix)) != 0) {
		if (snprintf(dst, dstlen, "%s", arg) >= (int)dstlen)
			return qstar_set_error(graph, "qstar: command argv item is too long");
		return 0;
	}
	n = strlen(arg);
	if (n <= strlen(prefix) + 1 || arg[n - 1] != '>')
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (n - strlen(prefix) >= sizeof(label))
		return qstar_set_error(graph, "qstar: target_file label is too long");
	memcpy(label, arg + strlen(prefix), n - strlen(prefix) - 1);
	label[n - strlen(prefix) - 1] = '\0';
	target = find_target(graph, label);
	if (!target) {
		genrule = qstar_graph_find_genrule(graph, label);
		if (genrule && genrule->outputs.len > 0) {
			if (snprintf(dst, dstlen, "%s", genrule->outputs.items[0]) >=
			    (int)dstlen)
				return qstar_set_error(graph,
				    "qstar: target_file generated output path is too long");
			return 0;
		}
		return qstar_set_error(graph, "qstar: target_file references unknown target '%s'",
		    label);
	}
	if (qstar_graph_artifact_output_path(graph, target, dst, dstlen) < 0)
		return qstar_set_error(graph, "qstar: target_file artifact path is too long");
	return 0;
}

/** command list item 하나를 실행 argv용 소유 문자열로 복사한다. */
static int
push_resolved_command_argv(struct qstar_graph *graph, char **argv, size_t *argc,
    const char *arg)
{
	char resolved[QSTAR_PATH_MAX];

	if (*argc + 1 >= QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: command argv too long");
	if (resolve_target_file_token(graph, arg, resolved, sizeof(resolved)) < 0)
		return -1;
	argv[*argc] = qstar_strdup(resolved);
	if (!argv[*argc])
		return qstar_set_error(graph, "qstar: out of memory");
	(*argc)++;
	argv[*argc] = NULL;
	return 0;
}

/** 소유 argv storage를 해제한다. */
static void
free_owned_argv(char **argv, size_t argc)
{
	size_t i;

	for (i = 0; i < argc; i++)
		free(argv[i]);
}

/** generated action 하나를 external tool policy와 cache key에 맞춰 실행한다. */
static int
run_one_generated_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule)
{
	char id[QSTAR_PATH_MAX], key[32], output_identity[QSTAR_PATH_MAX];
	char resolved_tool[QSTAR_PATH_MAX], tool_mode[64], tool_error[QSTAR_PATH_MAX];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	struct qstar_string_list inputs;
	struct qstar_resolved_toolchain toolchain;
	struct qstar_action_material material;
	size_t argc, argi, inputi;

	if (!genrule->config_header &&
	    qstar_profile_resolve_command_tool(graph, genrule->tool, resolved_tool,
	    sizeof(resolved_tool), tool_mode, sizeof(tool_mode), tool_error,
	    sizeof(tool_error)) < 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "command", genrule->label, "%s",
		    tool_error);
	if (genrule->config_header) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "builtin");
	}
	if (genrule->args.len + 2 > QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: generated action argv too long");
	for (argc = 0; argc < genrule->args.len; argc++) {
		if (generated_arg_escapes_package(genrule->args.items[argc]))
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "args", genrule->label,
			    "qstar: generated action arg '%s' escapes package root",
			    genrule->args.items[argc]);
	}
	for (argc = 0; argc < genrule->outputs.len; argc++) {
		if (mkdir_parent_under_root(graph, genrule->outputs.items[argc]) < 0)
			return qstar_set_error(graph,
			    "qstar: could not create generated output directory");
	}
	if (qstar_genrule_output_identity_list(genrule, output_identity,
	    sizeof(output_identity)) < 0)
		return qstar_set_error(graph, "qstar: generated output identity is too long");
	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	argc = 0;
	if (push_resolved_command_argv(graph, argv, &argc, resolved_tool) < 0)
		return -1;
	for (argi = 0; argi < genrule->args.len; argi++) {
		if (push_resolved_command_argv(graph, argv, &argc,
		    genrule->args.items[argi]) < 0) {
			free_owned_argv(argv, argc);
			return -1;
		}
	}
	memset(&inputs, 0, sizeof(inputs));
	for (inputi = 0; inputi < genrule->inputs.len; inputi++) {
		if (qstar_string_list_push(&inputs, genrule->inputs.items[inputi]) < 0) {
			qstar_string_list_free(&inputs);
			free_owned_argv(argv, argc);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	if (!genrule->config_header &&
	    qstar_profile_tool_mode_is_package_input(tool_mode) &&
	    qstar_string_list_push(&inputs, resolved_tool) < 0) {
		qstar_string_list_free(&inputs);
		free_owned_argv(argv, argc);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	memset(&toolchain, 0, sizeof(toolchain));
	snprintf(toolchain.name, sizeof(toolchain.name), "%s",
	    target ? target->toolchain : "custom");
	snprintf(toolchain.target, sizeof(toolchain.target), "%s",
	    graph->profile.target ? graph->profile.target : "host");
	compute_action_key(graph, target, &toolchain, id, "generate", argv,
	    &inputs, NULL, output_identity, key, sizeof(key), &material);
	qstar_string_list_free(&inputs);
	fprintf(ctx->out,
	    "generated_sandbox id=%s inputs=package-root outputs=generated-only cwd=package-root network=disabled tool=%s tool_mode=%s resolved_tool=%s output_identity=%s\n",
	    genrule->label, genrule->tool, tool_mode, resolved_tool, output_identity);
	if (genrule->config_header) {
		if (run_config_header_action(graph, ctx, target, genrule, id, key,
		    argv, &material) < 0) {
			free_owned_argv(argv, argc);
			return -1;
		}
	} else if (run_action(graph, ctx, target, id, "generate", key,
	    &genrule->outputs, argv, NULL, &material) < 0) {
		free_owned_argv(argv, argc);
		return -1;
	}
	free_owned_argv(argv, argc);
	return 0;
}

/** target이 소비하는 generated action을 package-local tool policy로 실행한다. */
static int
run_generated_actions(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (!target_consumes_genrule(target, &graph->genrules[i]))
			continue;
		if (run_one_generated_action(graph, ctx, target, &graph->genrules[i]) < 0)
			return -1;
	}
	return 0;
}

/** run target stdout log에 marker 문자열이 들어 있는지 확인한다. */
static int
stdout_contains_marker(struct qstar_graph *graph, const char *id, const char *marker)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX];
	FILE *f;
	char buf[4096];

	if (!marker || !*marker)
		return 1;
	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0)
		return 0;
	action_log_name(id, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, name);
	f = fopen(stdout_path, "rb");
	if (!f)
		return 0;
	while (fgets(buf, sizeof(buf), f)) {
		if (strstr(buf, marker)) {
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

/** run target cache stamp를 package-local output으로 만든다. */
static int
write_run_stamp(struct qstar_graph *graph, const char *stamp)
{
	char full[QSTAR_PATH_MAX];
	FILE *f;

	if (mkdir_parent_under_root(graph, stamp) < 0 ||
	    full_path_under_root(graph, stamp, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: could not create run stamp directory");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write run stamp");
	fputs("qstar run_target stamp\n", f);
	fclose(f);
	return 0;
}

/** qstar.run_target command를 dependency build 이후 실행한다. */
static int
run_target_command(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target)
{
	struct qstar_string_list inputs, outputs;
	struct qstar_action_material material;
	char *argv[QSTAR_EXEC_MAX_ARGV];
	char id[QSTAR_PATH_MAX], stamp[QSTAR_PATH_MAX], key[32], artifact[QSTAR_PATH_MAX];
	char owner[QSTAR_PATH_MAX];
	size_t argc, i;
	int old_timeout, rc;
	const struct qstar_target *dep;

	if (target->run_command.len == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "command", target->label,
		    "qstar: run_target '%s' requires command = qstar.cli { ... }",
		    target->label);
	argc = 0;
	for (i = 0; i < target->run_command.len; i++) {
		if (push_resolved_command_argv(graph, argv, &argc,
		    target->run_command.items[i]) < 0) {
			free_owned_argv(argv, argc);
			return -1;
		}
	}
	memset(&inputs, 0, sizeof(inputs));
	memset(&outputs, 0, sizeof(outputs));
	for (i = 0; i < target->deps.len; i++) {
		dep = find_target(graph, target->deps.items[i]);
		if (!dep || qstar_graph_artifact_output_path(graph, dep, artifact, sizeof(artifact)) < 0)
			continue;
		if (qstar_string_list_push(&inputs, artifact) < 0) {
			qstar_string_list_free(&inputs);
			free_owned_argv(argv, argc);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	qstar_mangle_label(target->label, owner, sizeof(owner));
	snprintf(stamp, sizeof(stamp), ".qstar/out/%s/run.stamp", owner);
	if (qstar_string_list_push(&outputs, stamp) < 0) {
		qstar_string_list_free(&inputs);
		free_owned_argv(argv, argc);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	snprintf(id, sizeof(id), "%s:run:0", target->label);
	compute_action_key(graph, target, NULL, id, "run", argv, &inputs, NULL, stamp,
	    key, sizeof(key), &material);
	fprintf(ctx->out,
	    "run_target label=%s command=argv timeout_sec=%d marker=%s\n",
	    target->label, target->run_timeout_sec ? target->run_timeout_sec :
	    ctx->action_timeout_sec,
	    target->run_marker && *target->run_marker ? target->run_marker : "<none>");
	old_timeout = ctx->action_timeout_sec;
	if (target->run_timeout_sec > 0)
		ctx->action_timeout_sec = target->run_timeout_sec;
	rc = run_action(graph, ctx, target, id, "run", key, &outputs, argv, NULL, &material);
	ctx->action_timeout_sec = old_timeout;
	if (rc == 0 && write_run_stamp(graph, stamp) < 0)
		rc = -1;
	if (rc == 0 && !stdout_contains_marker(graph, id, target->run_marker))
		rc = qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "marker", target->label,
		    "qstar: run_target '%s' stdout did not contain marker '%s'",
		    target->label, target->run_marker);
	if (rc == 0 && target->run_marker && *target->run_marker)
		fprintf(ctx->out, "run_marker label=%s status=matched marker=%s\n",
		    target->label, target->run_marker);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&outputs);
	free_owned_argv(argv, argc);
	return rc;
}

/** compile action key에 target header inputs를 포함한다. */
static int
push_target_header_inputs(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_string_list *inputs)
{
	size_t i;

	for (i = 0; i < target->public_headers.len; i++) {
		if (qstar_string_list_push(inputs, target->public_headers.items[i]) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	for (i = 0; i < target->private_headers.len; i++) {
		if (qstar_string_list_push(inputs, target->private_headers.items[i]) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

static int
string_list_contains(const struct qstar_string_list *list, const char *s)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], s) == 0)
			return 1;
	}
	return 0;
}

/** depfile token을 compile action input list에 추가한다. */
static int
push_depfile_token(struct qstar_graph *graph, struct qstar_string_list *inputs,
    const char *token)
{
	char full[QSTAR_PATH_MAX];

	if (!token[0] || !qstar_path_is_package_relative(token))
		return 0;
	if (full_path_under_root(graph, token, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: depfile input path too long");
	if (!path_exists(full))
		return qstar_set_error(graph,
		    "qstar: depfile-discovered header '%s' is missing", token);
	if (string_list_contains(inputs, token))
		return 0;
	if (qstar_string_list_push(inputs, token) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** compiler depfile을 읽어 discovered header input을 action key에 추가한다. */
static int
push_depfile_inputs(struct qstar_graph *graph, const char *depfile,
    struct qstar_string_list *inputs)
{
	char full[QSTAR_PATH_MAX], token[QSTAR_PATH_MAX];
	FILE *f;
	int c, seen_colon, n;
	size_t len;

	if (full_path_under_root(graph, depfile, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: depfile path too long");
	f = fopen(full, "rb");
	if (!f)
		return 0;
	seen_colon = 0;
	len = 0;
	while ((c = fgetc(f)) != EOF) {
		if (!seen_colon) {
			if (c == ':')
				seen_colon = 1;
			continue;
		}
		if (c == '\\') {
			n = fgetc(f);
			if (n == '\n' || n == '\r')
				continue;
			if (n == EOF)
				break;
			c = n;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (len) {
				token[len] = '\0';
				if (push_depfile_token(graph, inputs, token) < 0) {
					fclose(f);
					return -1;
				}
				len = 0;
			}
			continue;
		}
		if (len + 1 < sizeof(token))
			token[len++] = (char)c;
	}
	if (len) {
		token[len] = '\0';
		if (push_depfile_token(graph, inputs, token) < 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

/** compile source 종류와 toolchain 조합을 검증한다. */
static int
validate_compile_source(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const struct qstar_source_info *source,
    size_t index)
{
	if (source->header_input)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: header source '%s' must be listed as public_headers/private_headers",
		    target->sources.items[index]);
	if (strcmp(source->language, "cxx-module") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: C++ modules are not supported in QStar local executor v1");
	if (strcmp(source->language, "c") != 0 && strcmp(source->language, "cxx") != 0 &&
	    strcmp(source->language, "cale") != 0 && !source_is_asm(source))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: local executor does not support source '%s' with language '%s'",
		    target->sources.items[index], source->language);
	if (source_is_asm(source) &&
	    (strcmp(toolchain->name, "cale") == 0 || strcmp(toolchain->name, "cale-sol") == 0))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: assembler source '%s' requires host or clang toolchain",
		    target->sources.items[index]);
	if (strcmp(source->language, "cale") == 0 &&
	    strcmp(toolchain->name, "cale") != 0 &&
	    strcmp(toolchain->name, "cale-sol") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: Cale source '%s' requires toolchain=cale",
		    target->sources.items[index]);
	if (strcmp(source->language, "cxx") == 0 &&
	    (strcmp(toolchain->name, "cale") == 0 || strcmp(toolchain->name, "cale-sol") == 0))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: C++ source '%s' requires host or clang toolchain",
		    target->sources.items[index]);
	if ((strcmp(toolchain->name, "cale") == 0 ||
	    strcmp(toolchain->name, "cale-sol") == 0) && !command_exists(toolchain->cale))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: Cale compiler '%s' not found for source '%s'",
		    toolchain->cale, target->sources.items[index]);
	if (strcmp(source->language, "cxx") == 0 && !command_exists(toolchain->cxx))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: C++ compiler '%s' not found for source '%s'",
		    toolchain->cxx, target->sources.items[index]);
	if (source_is_asm(source) && !command_exists(toolchain->cc))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: assembler compiler driver '%s' not found for source '%s'",
		    toolchain->cc, target->sources.items[index]);
	return 0;
}

/** compile action 실행 후 depfile이 필요한 경우 생성 여부를 확인한다. */
static int
check_compile_depfile(struct qstar_graph *graph, const struct qstar_prepared_action *action)
{
	char full_depfile[QSTAR_PATH_MAX];

	if (!action->wants_depfile)
		return 0;
	if (full_path_under_root(graph, action->depfile, full_depfile,
	    sizeof(full_depfile)) < 0 || !path_exists(full_depfile))
		return qstar_set_error(graph, "qstar: compiler did not produce depfile '%s'",
		    action->depfile);
	return 0;
}

/** compile 성공 후 depfile-discovered input까지 포함한 최종 action key를 state에 반영한다. */
static int
refresh_compile_state_after_depfile(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    struct qstar_prepared_action *action)
{
	struct qstar_string_list inputs, dep_inputs;
	size_t i;

	memset(&inputs, 0, sizeof(inputs));
	memset(&dep_inputs, 0, sizeof(dep_inputs));
	if (qstar_string_list_push(&inputs, action->source_path) < 0) {
		qstar_string_list_free(&inputs);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	if (push_target_header_inputs(graph, target, &inputs) < 0) {
		qstar_string_list_free(&inputs);
		return -1;
	}
	if (action->wants_depfile &&
	    push_depfile_inputs(graph, action->depfile, &dep_inputs) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&dep_inputs);
		return -1;
	}
	for (i = 0; i < dep_inputs.len; i++) {
		if (qstar_string_list_push(&inputs, dep_inputs.items[i]) < 0) {
			qstar_string_list_free(&inputs);
			qstar_string_list_free(&dep_inputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	compute_action_key(graph, target, toolchain, action->id, "compile", action->argv,
	    &inputs, &dep_inputs, action->outputs.items[0], action->key,
	    sizeof(action->key), &action->material);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&dep_inputs);
	if (state_update_material(ctx, action->id, action->key, &action->material) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** C/C++/Cale/ASM source 하나를 compile action으로 준비한다. */
static int
prepare_compile_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    size_t index, struct qstar_prepared_action *action)
{
	struct qstar_source_info source;
	char object[QSTAR_PATH_MAX], target_arg[QSTAR_PATH_MAX], std_arg[128];
	char sysroot_arg[QSTAR_PATH_MAX];
	const char *compiler;
	struct qstar_string_list inputs, dep_inputs, outputs, includes;
	int cross, wants_depfile, is_asm, is_cale, is_cxx;
	size_t i;

	memset(action, 0, sizeof(*action));
	action->target = target;
	action->toolchain = toolchain;
	snprintf(action->kind, sizeof(action->kind), "compile");
	snprintf(action->source_path, sizeof(action->source_path), "%s",
	    target->sources.items[index]);
	qstar_source_classify(target->sources.items[index], &source);
	if (validate_compile_source(graph, target, toolchain, &source, index) < 0)
		return -1;
	if (qstar_object_output_path(target, index, object, sizeof(object)) < 0 ||
	    qstar_depfile_output_path(target, index, action->depfile,
	    sizeof(action->depfile)) < 0 ||
	    mkdir_parent_under_root(graph, object) < 0)
		return qstar_set_error(graph, "qstar: could not create object output directory");
	memset(&includes, 0, sizeof(includes));
	if (collect_compile_include_dirs(graph, target, &includes) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	is_asm = source_is_asm(&source);
	is_cale = strcmp(source.language, "cale") == 0;
	is_cxx = strcmp(source.language, "cxx") == 0;
	wants_depfile = (strcmp(source.language, "c") == 0 || is_cxx ||
	    source_uses_asm_preprocessor(target, &source)) &&
	    strcmp(toolchain->name, "cale") != 0 && strcmp(toolchain->name, "cale-sol") != 0;
	action->wants_depfile = wants_depfile;
	if (graph->profile.include_dirs.len * 2 +
	    (is_asm ? target->asm_include_dirs.len * 2 : includes.len * 2) +
	    (is_asm ? 0 : target->system_include_dirs.len * 2) +
	    (is_asm ? target->asm_compile_options.len :
	    is_cxx ? target->cxxflags.len : is_cale ? 0 : target->cflags.len) +
	    profile_compile_option_count(graph) +
	    18 > QSTAR_EXEC_MAX_ARGV) {
		qstar_string_list_free(&includes);
		return qstar_set_error(graph, "qstar: compile argv too long");
	}
	snprintf(action->id, sizeof(action->id), "%s:compile:%zu", target->label, index);
	snprintf(target_arg, sizeof(target_arg), "--target=%s", toolchain->target);
	snprintf(std_arg, sizeof(std_arg), "-std=%s", target->cxx_standard);
	snprintf(sysroot_arg, sizeof(sysroot_arg), "--sysroot=%s", toolchain->sysroot);
	compiler = is_cale ? toolchain->cale :
	    is_cxx ? toolchain->cxx : toolchain->cc;
	cross = (strcmp(toolchain->name, "clang") == 0 ||
	    strcmp(toolchain->name, "cale") == 0 ||
	    strcmp(toolchain->name, "cale-sol") == 0) &&
	    strcmp(toolchain->target, "host") != 0;
	if (prepared_action_push_argv(graph, action, compiler) < 0)
		goto fail;
	if (cross && prepared_action_push_argv(graph, action, target_arg) < 0)
		goto fail;
	if (toolchain->sysroot[0] &&
	    prepared_action_push_argv(graph, action, sysroot_arg) < 0)
		goto fail;
	if (toolchain->resource_dir[0]) {
		if (prepared_action_push_argv(graph, action, "-resource-dir") < 0 ||
		    prepared_action_push_argv(graph, action, toolchain->resource_dir) < 0)
			goto fail;
	}
	if (is_asm) {
		if (prepared_action_push_argv(graph, action, "-x") < 0 ||
		    prepared_action_push_argv(graph, action,
		    source_uses_asm_preprocessor(target, &source) ?
		    "assembler-with-cpp" : "assembler") < 0)
			goto fail;
	}
	if (prepared_action_push_argv(graph, action, "-c") < 0 ||
	    prepared_action_push_argv(graph, action, target->sources.items[index]) < 0 ||
	    prepared_action_push_argv(graph, action, "-o") < 0 ||
	    prepared_action_push_argv(graph, action, object) < 0)
		goto fail;
	if (wants_depfile) {
		if (prepared_action_push_argv(graph, action, "-MMD") < 0 ||
		    prepared_action_push_argv(graph, action, "-MF") < 0 ||
		    prepared_action_push_argv(graph, action, action->depfile) < 0)
			goto fail;
	}
	if (is_cxx && target->cxx_standard[0] &&
	    prepared_action_push_argv(graph, action, std_arg) < 0)
		goto fail;
	for (i = 0; !is_asm && !is_cxx && !is_cale && i < target->cflags.len; i++) {
		if (prepared_action_push_argv(graph, action, target->cflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_cxx && i < target->cxxflags.len; i++) {
		if (prepared_action_push_argv(graph, action, target->cxxflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_asm && i < target->asm_compile_options.len; i++) {
		if (prepared_action_push_argv(graph, action,
		    target->asm_compile_options.items[i]) < 0)
			goto fail;
	}
	if (!is_cale && append_profile_compile_options(graph, action) < 0)
		goto fail;
	for (i = 0; i < graph->profile.include_dirs.len; i++) {
		if (prepared_action_push_argv(graph, action, "-I") < 0 ||
		    prepared_action_push_argv(graph, action,
		    graph->profile.include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !is_asm && i < includes.len; i++) {
		if (prepared_action_push_argv(graph, action, "-I") < 0 ||
		    prepared_action_push_argv(graph, action, includes.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_asm && i < target->asm_include_dirs.len; i++) {
		if (prepared_action_push_argv(graph, action, "-I") < 0 ||
		    prepared_action_push_argv(graph, action,
		    target->asm_include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !is_asm && i < target->system_include_dirs.len; i++) {
		if (prepared_action_push_argv(graph, action, "-isystem") < 0 ||
		    prepared_action_push_argv(graph, action,
		    target->system_include_dirs.items[i]) < 0)
			goto fail;
	}
	if (compile_db_push(ctx, graph->package_root ? graph->package_root : ".",
	    target->sources.items[index], object, action->argv) < 0) {
		goto oom;
	}
	memset(&inputs, 0, sizeof(inputs));
	memset(&dep_inputs, 0, sizeof(dep_inputs));
	memset(&outputs, 0, sizeof(outputs));
	if (qstar_string_list_push(&inputs, target->sources.items[index]) < 0 ||
	    qstar_string_list_push(&outputs, object) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&dep_inputs);
		qstar_string_list_free(&outputs);
		qstar_string_list_free(&includes);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	if (push_target_header_inputs(graph, target, &inputs) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&dep_inputs);
		qstar_string_list_free(&outputs);
		qstar_string_list_free(&includes);
		return -1;
	}
	if (wants_depfile && push_depfile_inputs(graph, action->depfile, &dep_inputs) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&dep_inputs);
		qstar_string_list_free(&outputs);
		qstar_string_list_free(&includes);
		return -1;
	}
	for (i = 0; i < dep_inputs.len; i++) {
		if (qstar_string_list_push(&inputs, dep_inputs.items[i]) < 0) {
			qstar_string_list_free(&inputs);
			qstar_string_list_free(&dep_inputs);
			qstar_string_list_free(&outputs);
			qstar_string_list_free(&includes);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	compute_action_key(graph, target, toolchain, action->id, "compile", action->argv,
	    &inputs, &dep_inputs, object, action->key, sizeof(action->key),
	    &action->material);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&dep_inputs);
	action->outputs = outputs;
	qstar_string_list_free(&includes);
	return 0;
oom:
	qstar_string_list_free(&includes);
	return qstar_set_error(graph, "qstar: out of memory");
fail:
	qstar_string_list_free(&includes);
	prepared_action_free(action);
	return -1;
}

/** C/C++/Cale/ASM source 하나를 object로 compile한다. */
static int
run_compile(struct qstar_graph *graph, struct qstar_build_ctx *ctx, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, size_t index)
{
	struct qstar_prepared_action action;
	int rc;

	if (prepare_compile_action(graph, ctx, target, toolchain, index, &action) < 0)
		return -1;
	rc = run_action(graph, ctx, target, action.id, action.kind, action.key,
	    &action.outputs, action.argv, toolchain, &action.material);
	if (rc == 0)
		rc = check_compile_depfile(graph, &action);
	if (rc == 0)
		rc = refresh_compile_state_after_depfile(graph, ctx, target, toolchain,
		    &action);
	prepared_action_free(&action);
	return rc;
}

/** 준비된 compile action을 async child process로 시작하거나 cache-hit skip 처리한다. */
static int
start_prepared_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    struct qstar_prepared_action *action, struct qstar_running_action *running,
    size_t slot, size_t queue_index)
{
	char logdir[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX], stderr_path[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	char rsp_arg[QSTAR_PATH_MAX];
	char *exec_argv[3];
	char *const *child_argv;
	const struct qstar_state_entry *prev;
	pid_t pid;
	int fdout, fderr, use_rsp;

	fprintf(ctx->out,
	    "parallel_event target=%s event=queue id=%s order=%zu slot=%zu state=ready retry=no\n",
	    action->target->label, action->id, queue_index, slot);
	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create action log dir");
	action_log_name(action->id, running->name, sizeof(running->name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, running->name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, running->name);
	snprintf(running->action_log, sizeof(running->action_log), "%s/%s.log", logdir,
	    running->name);
	snprintf(child_stdout_path, sizeof(child_stdout_path), ".qstar/logs/%s.stdout",
	    running->name);
	snprintf(child_stderr_path, sizeof(child_stderr_path), ".qstar/logs/%s.stderr",
	    running->name);
	prev = state_find(ctx, action->id);
	ctx->scheduled_count++;
	fprintf(ctx->out, "schedule_action id=%s kind=%s slot=%zu jobs=%d state=ready\n",
	    action->id, action->kind, slot, ctx->jobs);
	if (prev && strcmp(prev->key, action->key) == 0 &&
	    outputs_exist(graph, &action->outputs)) {
		fprintf(ctx->out,
		    "build_action id=%s status=skip reason=cache-hit stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
		    action->id, running->name, running->name);
		write_skip_log(running->action_log, action->argv);
		ctx->skip_count++;
		fprintf(ctx->out,
		    "parallel_event target=%s event=skip id=%s slot=%zu state=cache-hit retry=no\n",
		    action->target->label, action->id, slot);
		return state_push(ctx, 1, action->id, action->key, action->outputs.items[0],
		    "skip", action->kind, &action->material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	fprintf(ctx->out,
	    "build_action id=%s status=run timeout_sec=%d stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    action->id, ctx->action_timeout_sec, running->name, running->name);
	if (ctx->explain_cache)
		fprintf(ctx->out, "cache_miss id=%s reason=%s key=%s previous=%s\n",
		    action->id, cache_reason(graph, prev, action->key, &action->outputs,
		    &action->material), action->key, prev ? prev->key : "<none>");
	use_rsp = prepare_response_file(graph, action->id, action->toolchain, action->argv,
	    rsp_arg, sizeof(rsp_arg), ctx->out);
	if (use_rsp < 0)
		return -1;
	child_argv = action->argv;
	if (use_rsp) {
		exec_argv[0] = action->argv[0];
		exec_argv[1] = rsp_arg;
		exec_argv[2] = NULL;
		child_argv = exec_argv;
	}
	pid = fork();
	if (pid < 0)
		return qstar_set_error(graph, "qstar: fork failed");
	if (pid == 0) {
		if (chdir(graph->package_root ? graph->package_root : ".") < 0)
			_exit(127);
		fdout = open(child_stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fderr = open(child_stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fdout < 0 || fderr < 0 || dup2(fdout, 1) < 0 || dup2(fderr, 2) < 0)
			_exit(127);
		close(fdout);
		close(fderr);
		execvp(child_argv[0], child_argv);
		_exit(127);
	}
	running->action = action;
	running->pid = pid;
	running->start = time(NULL);
	running->slot = slot;
	fprintf(ctx->out, "schedule_action id=%s slot=%zu pid=%ld state=started\n",
	    action->id, slot, (long)pid);
	fprintf(ctx->out,
	    "parallel_event target=%s event=start id=%s slot=%zu pid=%ld state=running retry=on-failure\n",
	    action->target->label, action->id, slot, (long)pid);
	return 1;
}

/** async compile action 하나의 종료 상태를 반영한다. */
static int
finish_running_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    struct qstar_running_action *running, int status)
{
	struct qstar_prepared_action *action = running->action;
	int exit_code;

	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	write_action_log(running->action_log, action->argv, exit_code);
	if (exit_code != 0) {
		fprintf(ctx->out, "build_action id=%s status=fail exit=%d\n",
		    action->id, exit_code);
		fprintf(ctx->out,
		    "parallel_event target=%s event=fail id=%s slot=%zu exit=%d state=failed retry=next-build cancel=active\n",
		    action->target->label, action->id, running->slot, exit_code);
		write_failure_replay(graph, action->id, action->toolchain, action->argv);
		ctx->fail_count++;
		ctx->cancelled = 1;
		return qstar_set_error_origin(graph, action->target->origin_file,
		    action->target->origin_line, "action", action->target->label,
		    "qstar: action '%s' failed with status %d; replay=.qstar/logs/last-failure.replay",
		    action->id, exit_code);
	}
	ctx->run_count++;
	if (state_push(ctx, 1, action->id, action->key, action->outputs.items[0],
	    "run", action->kind, &action->material) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	fprintf(ctx->out, "build_action id=%s status=done exit=0\n", action->id);
	fprintf(ctx->out, "schedule_action id=%s slot=%zu state=finished\n", action->id,
	    running->slot);
	fprintf(ctx->out,
	    "parallel_event target=%s event=finish id=%s slot=%zu state=success retry=no\n",
	    action->target->label, action->id, running->slot);
	return 0;
}

/** 실패한 병렬 batch의 아직 실행 중인 child process를 정리한다. */
static void
cancel_running_actions(struct qstar_build_ctx *ctx, struct qstar_running_action *running,
    size_t jobs)
{
	size_t i;
	int status;

	for (i = 0; i < jobs; i++) {
		if (running[i].pid <= 0)
			continue;
		kill(running[i].pid, SIGKILL);
		waitpid(running[i].pid, &status, 0);
		fprintf(ctx->out,
		    "build_action id=%s status=cancelled reason=parallel-failure retry=next-build\n",
		    running[i].action->id);
		fprintf(ctx->out, "schedule_action id=%s slot=%zu state=cancelled\n",
		    running[i].action->id, running[i].slot);
		fprintf(ctx->out,
		    "parallel_event target=%s event=cancel id=%s slot=%zu state=cancelled reason=parallel-failure retry=next-build\n",
		    running[i].action->target->label, running[i].action->id,
		    running[i].slot);
	}
}

/** 병렬 compile batch에서 비어 있는 scheduler slot을 찾는다. */
static size_t
find_free_slot(const struct qstar_running_action *running, size_t jobs)
{
	size_t i;

	for (i = 0; i < jobs; i++) {
		if (running[i].pid <= 0)
			return i;
	}
	return jobs;
}

/** 같은 target 안의 independent compile action을 job limit에 맞춰 병렬 실행한다. */
static int
run_compile_parallel(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	struct qstar_prepared_action *actions;
	struct qstar_running_action *running;
	size_t i, next, active, jobs, slot;
	int rc, status, progressed;
	pid_t waited;

	if (target->sources.len == 0)
		return 0;
	jobs = ctx->jobs > 1 ? (size_t)ctx->jobs : 1;
	actions = calloc(target->sources.len, sizeof(actions[0]));
	running = calloc(jobs, sizeof(running[0]));
	if (!actions || !running) {
		free(actions);
		free(running);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	for (i = 0; i < target->sources.len; i++) {
		if (prepare_compile_action(graph, ctx, target, toolchain, i, &actions[i]) < 0) {
			while (i > 0)
				prepared_action_free(&actions[--i]);
			free(actions);
			free(running);
			return -1;
		}
	}
	next = 0;
	active = 0;
	rc = 0;
	fprintf(ctx->out,
	    "parallel_batch target=%s jobs=%zu total=%zu policy=fifo fairness=source-order cancel=kill-active retry=next-build\n",
	    target->label, jobs, target->sources.len);
	while (next < target->sources.len || active > 0) {
		while (next < target->sources.len && active < jobs) {
			slot = find_free_slot(running, jobs);
			if (slot >= jobs) {
				rc = qstar_set_error(graph, "qstar: no free parallel slot");
				goto fail;
			}
			fprintf(ctx->out,
			    "parallel_slot target=%s slot=%zu state=assign action=%s queue=%zu\n",
			    target->label, slot, actions[next].id, next);
			rc = start_prepared_action(graph, ctx, &actions[next],
			    &running[slot], slot, next);
			if (rc < 0)
				goto fail;
			next++;
			if (rc == 1)
				active++;
		}
		if (active == 0)
			continue;
		progressed = 0;
		for (i = 0; i < jobs; i++) {
			if (running[i].pid <= 0)
				continue;
			waited = waitpid(running[i].pid, &status, WNOHANG);
			if (waited == 0) {
				if (time(NULL) - running[i].start >= ctx->action_timeout_sec) {
					kill(running[i].pid, SIGKILL);
					waitpid(running[i].pid, &status, 0);
					running[i].pid = 0;
					write_action_log(running[i].action_log,
					    running[i].action->argv, 124);
					write_failure_replay(graph, running[i].action->id,
					    running[i].action->toolchain,
					    running[i].action->argv);
					ctx->fail_count++;
					ctx->cancelled = 1;
					fprintf(ctx->out,
					    "build_action id=%s status=timeout timeout_sec=%d\n",
					    running[i].action->id, ctx->action_timeout_sec);
					fprintf(ctx->out,
					    "parallel_event target=%s event=timeout id=%s slot=%zu state=timeout retry=next-build cancel=active\n",
					    running[i].action->target->label,
					    running[i].action->id, running[i].slot);
					rc = qstar_set_error_origin(graph,
					    running[i].action->target->origin_file,
					    running[i].action->target->origin_line, "action",
					    running[i].action->target->label,
					    "qstar: action '%s' timed out after %d seconds; replay=.qstar/logs/last-failure.replay",
					    running[i].action->id, ctx->action_timeout_sec);
					goto fail;
				}
				continue;
			}
			if (waited < 0) {
				rc = qstar_set_error(graph, "qstar: waitpid failed");
				goto fail;
			}
			running[i].pid = 0;
			rc = finish_running_action(graph, ctx, &running[i], status);
			running[i].action = NULL;
			active--;
			if (rc < 0)
				goto fail;
			progressed = 1;
			break;
		}
		if (!progressed)
			sleep(1);
	}
	for (i = 0; i < target->sources.len; i++) {
		if (check_compile_depfile(graph, &actions[i]) < 0) {
			rc = -1;
			break;
		}
		if (refresh_compile_state_after_depfile(graph, ctx, target, toolchain,
		    &actions[i]) < 0) {
			rc = -1;
			break;
		}
	}
fail:
	if (rc < 0 && active > 0)
		cancel_running_actions(ctx, running, jobs);
	for (i = 0; i < target->sources.len; i++)
		prepared_action_free(&actions[i]);
	free(actions);
	free(running);
	return rc;
}

/** argv tail에 소유 문자열을 추가한다. */
static int
append_owned_argv(struct qstar_graph *graph, char **argv, size_t *argc, const char *s)
{
	if (*argc + 1 >= QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: final argv too long");
	argv[(*argc)++] = qstar_strdup(s);
	return argv[*argc - 1] ? 0 : qstar_set_error(graph, "qstar: out of memory");
}

/** artifact list에 이미 추가한 target label인지 검사한다. */
static int
seen_label(const struct qstar_string_list *seen, const char *label)
{
	size_t i;

	for (i = 0; i < seen->len; i++) {
		if (strcmp(seen->items[i], label) == 0)
			return 1;
	}
	return 0;
}

/** dependency artifact를 dependent-before-dependency order로 재귀 추가한다. */
static int
append_dep_artifact_rec(struct qstar_graph *graph, const struct qstar_target *dep,
    char **argv, size_t *argc, struct qstar_string_list *seen)
{
	const struct qstar_target *next;
	char artifact[QSTAR_PATH_MAX];
	size_t i;

	if (seen_label(seen, dep->label))
		return 0;
	if (qstar_string_list_push(seen, dep->label) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (qstar_graph_artifact_output_path(graph, dep, artifact, sizeof(artifact)) < 0)
		return qstar_set_error(graph, "qstar: dependency artifact path too long");
	if (append_owned_argv(graph, argv, argc, artifact) < 0)
		return -1;
	for (i = 0; i < dep->deps.len; i++) {
		if (dep->deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->deps.items[i]);
		if (next && append_dep_artifact_rec(graph, next, argv, argc, seen) < 0)
			return -1;
	}
	for (i = 0; i < dep->private_deps.len; i++) {
		if (dep->private_deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->private_deps.items[i]);
		if (next && append_dep_artifact_rec(graph, next, argv, argc, seen) < 0)
			return -1;
	}
	return 0;
}

/** target의 transitive dependency artifact를 final argv 뒤에 붙인다. */
static int
append_dep_artifacts(struct qstar_graph *graph, const struct qstar_target *target,
    char **argv, size_t *argc)
{
	const struct qstar_target *dep;
	struct qstar_string_list seen;
	size_t i;
	int rc;

	memset(&seen, 0, sizeof(seen));
	for (i = 0; i < target->deps.len; i++) {
		if (target->deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		rc = append_dep_artifact_rec(graph, dep, argv, argc, &seen);
		if (rc < 0) {
			qstar_string_list_free(&seen);
			return rc;
		}
	}
	for (i = 0; i < target->private_deps.len; i++) {
		if (target->private_deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->private_deps.items[i]);
		if (!dep)
			continue;
		rc = append_dep_artifact_rec(graph, dep, argv, argc, &seen);
		if (rc < 0) {
			qstar_string_list_free(&seen);
			return rc;
		}
	}
	qstar_string_list_free(&seen);
	return 0;
}

/** append_dep_artifacts에서 복사한 argv tail을 해제한다. */
static void
free_dep_artifacts(char **argv, size_t first, size_t argc)
{
	while (first < argc)
		free(argv[first++]);
}

static int
target_is_darwin(const char *target)
{
	return !target || strcmp(target, "host") == 0 || strstr(target, "apple") ||
	    strstr(target, "darwin") || strstr(target, "macos");
}

static int
target_is_windows(const char *target)
{
	return target && (strstr(target, "windows") || strstr(target, "mingw") ||
	    strstr(target, "msvc"));
}

/** target source list에 C++ compile input이 있는지 확인한다. */
static int
target_has_cxx_source(const struct qstar_target *target)
{
	struct qstar_source_info source;
	size_t i;

	for (i = 0; i < target->sources.len; i++) {
		if (qstar_source_classify(target->sources.items[i], &source) == 0 &&
		    strcmp(source.language, "cxx") == 0)
			return 1;
	}
	return 0;
}

/** MSVC target에서 clang-cl style driver가 link flag boundary를 필요로 하는지 본다. */
static int
toolchain_needs_msvc_link_boundary(const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_target *target)
{
	const char *tool;

	if (!toolchain || !target || !strstr(toolchain->target, "msvc"))
		return 0;
	tool = target_has_cxx_source(target) ? toolchain->cxx : toolchain->linker;
	return strstr(tool, "clang-cl") != NULL || strstr(tool, "cl.exe") != NULL;
}

/** lld-link/link.exe 계열 linker의 output option spelling을 확인한다. */
static int
toolchain_uses_msvc_out_arg(const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_target *target)
{
	const char *tool;

	if (!toolchain || !target)
		return 0;
	tool = target_has_cxx_source(target) ? toolchain->cxx : toolchain->linker;
	return strstr(tool, "lld-link") != NULL || strstr(tool, "link.exe") != NULL;
}

/** target linker_script가 있으면 우선하고 없으면 profile linker_script를 쓴다. */
static const char *
effective_linker_script(const struct qstar_graph *graph, const struct qstar_target *target)
{
	return target->linker_script && *target->linker_script ? target->linker_script :
	    graph->profile.linker_script && *graph->profile.linker_script ?
	    graph->profile.linker_script : NULL;
}

/** target/profile link policy를 argv에 추가한다. */
static int
append_link_policy_flags(struct qstar_graph *graph, const struct qstar_target *target,
    char **argv, size_t *argc)
{
	char arg[QSTAR_PATH_MAX];
	const char *script;
	size_t i;

	for (i = 0; i < graph->profile.link_options.len; i++) {
		if (append_owned_argv(graph, argv, argc, graph->profile.link_options.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < target->link_options.len; i++) {
		if (append_owned_argv(graph, argv, argc, target->link_options.items[i]) < 0)
			return -1;
	}
	script = effective_linker_script(graph, target);
	if (script) {
		if (append_owned_argv(graph, argv, argc, "-T") < 0 ||
		    append_owned_argv(graph, argv, argc, script) < 0)
			return -1;
	}
	for (i = 0; i < graph->profile.defsyms.len; i++) {
		snprintf(arg, sizeof(arg), "--defsym=%s", graph->profile.defsyms.items[i]);
		if (append_owned_argv(graph, argv, argc, arg) < 0)
			return -1;
	}
	for (i = 0; i < target->defsyms.len; i++) {
		snprintf(arg, sizeof(arg), "--defsym=%s", target->defsyms.items[i]);
		if (append_owned_argv(graph, argv, argc, arg) < 0)
			return -1;
	}
	return 0;
}

/** linker_script가 package-relative이면 action input으로 추적한다. */
static int
push_linker_script_input(struct qstar_graph *graph, const char *script,
    struct qstar_string_list *inputs)
{
	if (!script || !*script || !qstar_path_is_package_relative(script))
		return 0;
	if (qstar_string_list_push(inputs, script) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** system lib/lib_dir/framework link flags를 target profile별 spelling으로 추가한다. */
static int
append_system_link_flags(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, char **argv, size_t *argc)
{
	char flag[QSTAR_PATH_MAX];
	size_t i;
	int windows;

	windows = target_is_windows(toolchain->target);
	for (i = 0; i < graph->profile.lib_dirs.len; i++) {
		if (windows)
			snprintf(flag, sizeof(flag), "/LIBPATH:%s", graph->profile.lib_dirs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-L%s", graph->profile.lib_dirs.items[i]);
		if (append_owned_argv(graph, argv, argc, flag) < 0)
			return -1;
	}
	for (i = 0; i < target->lib_dirs.len; i++) {
		if (windows)
			snprintf(flag, sizeof(flag), "/LIBPATH:%s", target->lib_dirs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-L%s", target->lib_dirs.items[i]);
		if (append_owned_argv(graph, argv, argc, flag) < 0)
			return -1;
	}
	for (i = 0; i < target->libs.len; i++) {
		if (windows)
			snprintf(flag, sizeof(flag), "%s.lib", target->libs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-l%s", target->libs.items[i]);
		if (append_owned_argv(graph, argv, argc, flag) < 0)
			return -1;
	}
	if (target->frameworks.len && !target_is_darwin(toolchain->target))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "frameworks", target->label,
		    "qstar: frameworks are supported only for Darwin-like targets");
	for (i = 0; i < target->frameworks.len; i++) {
		if (append_owned_argv(graph, argv, argc, "-framework") < 0 ||
		    append_owned_argv(graph, argv, argc, target->frameworks.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** target final archive/link action을 실행한다. */
static int
run_final(struct qstar_graph *graph, struct qstar_build_ctx *ctx, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	char artifact[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], key[32];
	char sysroot_arg[QSTAR_PATH_MAX];
	char out_arg[QSTAR_PATH_MAX];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	struct qstar_string_list inputs, outputs;
	struct qstar_action_material material;
	size_t argc, dep_first, owned_first, i;
	int rc;

	if (strcmp(target->kind, "sharedlib") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: sharedlib targets are plan-only in local executor v1");
	if (!qstar_target_rule_lookup(target->kind) ||
	    !qstar_target_rule_lookup(target->kind)->local_executor_supported)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: local executor does not support target kind '%s'",
		    target->kind);
	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    mkdir_parent_under_root(graph, artifact) < 0)
		return qstar_set_error(graph, "qstar: could not create artifact output directory");
	argc = 0;
	if (strcmp(target->kind, "staticlib") == 0) {
		snprintf(id, sizeof(id), "%s:archive:0", target->label);
		argv[argc++] = (char *)toolchain->ar;
		argv[argc++] = "rcs";
		argv[argc++] = artifact;
	} else {
		snprintf(id, sizeof(id), "%s:link:0", target->label);
		argv[argc++] = (char *)(target_has_cxx_source(target) ?
		    toolchain->cxx : toolchain->linker);
		if (toolchain->sysroot[0]) {
			snprintf(sysroot_arg, sizeof(sysroot_arg), "--sysroot=%s",
			    toolchain->sysroot);
			argv[argc++] = sysroot_arg;
		}
		if (toolchain_uses_msvc_out_arg(toolchain, target)) {
			snprintf(out_arg, sizeof(out_arg), "/out:%s", artifact);
			argv[argc++] = out_arg;
		} else {
			argv[argc++] = "-o";
			argv[argc++] = artifact;
		}
	}
	owned_first = argc;
	if (strcmp(target->kind, "staticlib") != 0 &&
	    append_link_policy_flags(graph, target, argv, &argc) < 0) {
		free_dep_artifacts(argv, owned_first, argc);
		return -1;
	}
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_object_output_path(target, i, object, sizeof(object)) < 0)
			return qstar_set_error(graph, "qstar: object output path too long");
		argv[argc++] = qstar_strdup(object);
		if (!argv[argc - 1])
			return qstar_set_error(graph, "qstar: out of memory");
	}
	dep_first = argc;
	if (append_dep_artifacts(graph, target, argv, &argc) < 0) {
		free_dep_artifacts(argv, owned_first, dep_first);
		return -1;
	}
	if (strcmp(target->kind, "staticlib") != 0 &&
	    toolchain_needs_msvc_link_boundary(toolchain, target) &&
	    append_owned_argv(graph, argv, &argc, "/link") < 0) {
		free_dep_artifacts(argv, owned_first, argc);
		return -1;
	}
	if (strcmp(target->kind, "staticlib") != 0 &&
	    append_system_link_flags(graph, target, toolchain, argv, &argc) < 0) {
		free_dep_artifacts(argv, owned_first, argc);
		return -1;
	}
	argv[argc] = NULL;
	memset(&inputs, 0, sizeof(inputs));
	memset(&outputs, 0, sizeof(outputs));
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_object_output_path(target, i, object, sizeof(object)) < 0)
			return qstar_set_error(graph, "qstar: object output path too long");
		if (qstar_string_list_push(&inputs, object) < 0) {
			qstar_string_list_free(&inputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	for (i = dep_first; i < argc; i++) {
		if (qstar_string_list_push(&inputs, argv[i]) < 0) {
			qstar_string_list_free(&inputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	if (push_linker_script_input(graph, effective_linker_script(graph, target), &inputs) < 0) {
		qstar_string_list_free(&inputs);
		return -1;
	}
	if (qstar_string_list_push(&outputs, artifact) < 0) {
		qstar_string_list_free(&inputs);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	compute_action_key(graph, target, toolchain, id,
	    strcmp(target->kind, "staticlib") == 0 ? "archive" : "link", argv, &inputs,
	    NULL, artifact, key, sizeof(key), &material);
	rc = run_action(graph, ctx, target, id,
	    strcmp(target->kind, "staticlib") == 0 ? "archive" : "link", key, &outputs,
	    argv, toolchain, &material);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&outputs);
	free_dep_artifacts(argv, owned_first, argc);
	return rc;
}

/** closure target 하나를 local executor v1 policy로 실행한다. */
static int
build_target(struct qstar_graph *graph, const struct qstar_target *target, size_t order,
    void *user)
{
	struct qstar_build_ctx *ctx = user;
	struct qstar_resolved_toolchain toolchain;
	size_t i;

	fprintf(ctx->out, "build_target %s order=%zu kind=%s\n", target->label, order,
	    target->kind);
	fprintf(ctx->out,
	    "action_dag target=%s order=%zu parallel=%s reason=deterministic-v3 failure=stop-on-first-failure timeout_sec=%d\n",
	    target->label, order,
	    ctx->jobs > 1 && target->sources.len > 1 ? "compile-process-v2" : "no",
	    ctx->action_timeout_sec);
	if (strcmp(target->kind, "sharedlib") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: sharedlib targets are plan-only in local executor v1");
	if (strcmp(target->kind, "run_target") == 0)
		return run_target_command(graph, ctx, target);
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	fprintf(ctx->out,
	    "resolved_toolchain owner=%s toolchain=%s target=%s cc=%s cxx=%s ar=%s linker=%s\n",
	    target->label, toolchain.name, toolchain.target, toolchain.cc, toolchain.cxx,
	    toolchain.ar, toolchain.linker);
	if (run_generated_actions(graph, ctx, target) < 0)
		return -1;
	if (ctx->jobs > 1 && target->sources.len > 1) {
		fprintf(ctx->out,
		    "parallel_compile target=%s jobs=%d sources=%zu mode=process-v2 failure=cancel-active fairness=fifo\n",
		    target->label, ctx->jobs, target->sources.len);
		if (run_compile_parallel(graph, ctx, target, &toolchain) < 0)
			return -1;
	} else {
		for (i = 0; i < target->sources.len; i++) {
			if (run_compile(graph, ctx, target, &toolchain, i) < 0)
				return -1;
		}
	}
	return run_final(graph, ctx, target, &toolchain);
}

/** 직접 선택된 generated action label을 local executor로 실행한다. */
static int
build_direct_generated_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_genrule *genrule)
{
	fprintf(ctx->out, "build_generated_action %s order=0 kind=custom_target\n",
	    genrule->label);
	fprintf(ctx->out,
	    "action_dag target=%s order=0 parallel=no reason=direct-generated-action failure=stop-on-first-failure timeout_sec=%d\n",
	    genrule->label, ctx->action_timeout_sec);
	return run_one_generated_action(graph, ctx, NULL, genrule);
}

/** build context가 소유한 동적 저장소를 해제한다. */
static void
build_ctx_free(struct qstar_build_ctx *ctx)
{
	state_free(ctx->prev, ctx->prev_len);
	state_free(ctx->next, ctx->next_len);
	compile_db_free(ctx->compiles, ctx->compile_len);
	memset(ctx, 0, sizeof(*ctx));
}

/** QStar local executor에 option을 적용해 제한된 build action을 실행한다. */
int
qstar_graph_build_with_options(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out)
{
	struct qstar_build_ctx ctx;
	const struct qstar_genrule *genrule;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.out = out;
	ctx.root_label = label && *label ? label : "<all>";
	ctx.explain_cache = options ? options->explain_cache : 0;
	ctx.jobs = options && options->jobs > 0 ? options->jobs : 1;
	ctx.schedule_trace = options ? options->schedule_trace : 0;
	ctx.action_timeout_sec = action_timeout_sec_from_env();
	if (state_load(graph, &ctx) < 0) {
		build_ctx_free(&ctx);
		return -1;
	}
	fputs("qstar build v2\n", out);
	fprintf(out, "root %s\n", ctx.root_label);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	fprintf(out,
	    "executor-policy version=v3 parallel=%s jobs=%d active=%s failure=stop-on-first-failure action_timeout_sec=%d cancel=kill-process-and-stop-queue\n",
	    ctx.jobs > 1 ? "optional" : "no", ctx.jobs,
	    ctx.jobs > 1 ? "compile-process-v2" : "serial", ctx.action_timeout_sec);
	rc = graph_snapshot_write(graph);
	genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	if (rc == 0 && genrule)
		rc = build_direct_generated_action(graph, &ctx, genrule);
	else if (rc == 0)
		rc = qstar_graph_visit_closure(graph, label, build_target, &ctx);
	if (rc == 0)
		rc = state_write(graph, &ctx);
	if (rc == 0)
		rc = compile_db_write(graph, &ctx);
	if (build_summary_write(graph, &ctx, rc == 0 ? "success" : "failure") < 0 &&
	    rc == 0)
		rc = -1;
	if (rc == 0)
		fprintf(out, "status ok run=%zu skip=%zu fail=%zu\n",
		    ctx.run_count, ctx.skip_count, ctx.fail_count);
	else {
		if (ctx.cancelled)
			fprintf(out,
			    "cancel_propagation policy=stop-on-first-failure pending=not-started scheduled=%zu\n",
			    ctx.scheduled_count);
		fprintf(out, "status fail run=%zu skip=%zu fail=%zu\n",
		    ctx.run_count, ctx.skip_count, ctx.fail_count);
	}
	build_ctx_free(&ctx);
	return rc;
}

/** QStar local executor v1로 제한된 build action을 실행한다. */
int
qstar_graph_build(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_build_options options;

	memset(&options, 0, sizeof(options));
	options.jobs = 1;
	return qstar_graph_build_with_options(graph, label, &options, out);
}

/** QStar action cache 기준으로 rebuild 이유를 설명한다. */
int
qstar_graph_why_rebuild(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_build_ctx ctx;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.out = out;
	ctx.root_label = label && *label ? label : "<all>";
	ctx.explain_only = 1;
	if (state_load(graph, &ctx) < 0) {
		build_ctx_free(&ctx);
		return -1;
	}
	fputs("qstar why-rebuild v1\n", out);
	fprintf(out, "root %s\n", ctx.root_label);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	rc = qstar_graph_visit_closure(graph, label, build_target, &ctx);
	if (rc == 0)
		fputs("status ok\n", out);
	build_ctx_free(&ctx);
	return rc;
}

/** file 또는 directory tree를 지운다. */
static int
remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *ent;
	char child[QSTAR_PATH_MAX];

	if (lstat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	dir = opendir(path);
	if (!dir)
		return -1;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
		    (int)sizeof(child)) {
			closedir(dir);
			return -1;
		}
		if (remove_tree(child) < 0) {
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);
	return rmdir(path);
}

/** target 하나의 .qstar/out directory를 지운다. */
static int
clean_target_output(struct qstar_graph *graph, const struct qstar_target *target)
{
	char owner[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];

	qstar_mangle_label(target->label, owner, sizeof(owner));
	if (snprintf(rel, sizeof(rel), ".qstar/out/%s", owner) >= (int)sizeof(rel) ||
	    full_path_under_root(graph, rel, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: clean path too long");
	return remove_tree(full) < 0 ? qstar_set_error(graph, "qstar: clean failed for '%s'",
	    rel) : 0;
}

/** clean target closure callback이다. */
static int
clean_target_cb(struct qstar_graph *graph, const struct qstar_target *target, size_t order,
    void *user)
{
	FILE *out = user;

	(void)order;
	fprintf(out, "clean_target %s\n", target->label);
	return clean_target_output(graph, target);
}

/** QStar local build output을 전체 또는 target 단위로 지운다. */
int
qstar_graph_clean(struct qstar_graph *graph, const char *label, FILE *out)
{
	char full[QSTAR_PATH_MAX];

	fputs("qstar clean v1\n", out);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	if (label && *label) {
		fprintf(out, "root %s\n", label);
		if (qstar_graph_visit_closure(graph, label, clean_target_cb, out) < 0)
			return -1;
	} else {
		if (full_path_under_root(graph, ".qstar", full, sizeof(full)) < 0 ||
		    remove_tree(full) < 0)
			return qstar_set_error(graph, "qstar: clean failed for '.qstar'");
		if (full_path_under_root(graph, "compile_commands.json", full, sizeof(full)) < 0 ||
		    remove_tree(full) < 0)
			return qstar_set_error(graph, "qstar: clean failed for compile_commands.json");
		fputs("clean_all .qstar compile_commands.json\n", out);
	}
	fputs("status ok\n", out);
	return 0;
}

/** test target artifact를 제한된 runner로 실행한다. */
static int
run_test_artifact(struct qstar_graph *graph, const struct qstar_target *target, FILE *out)
{
	char artifact[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], logdir[QSTAR_PATH_MAX];
	char name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX], stderr_path[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	pid_t pid;
	time_t start;
	int status, fdout, fderr;

	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    full_path_under_root(graph, artifact, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: test artifact path too long");
	if (!path_exists(full))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "test", target->label,
		    "qstar: test artifact '%s' is missing; run qstar build first",
		    artifact);
	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create test log dir");
	action_log_name(target->label, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.test.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.test.stderr", logdir, name);
	snprintf(child_stdout_path, sizeof(child_stdout_path), ".qstar/logs/%s.test.stdout", name);
	snprintf(child_stderr_path, sizeof(child_stderr_path), ".qstar/logs/%s.test.stderr", name);
	fprintf(out,
	    "test_run label=%s artifact=%s stdout=.qstar/logs/%s.test.stdout stderr=.qstar/logs/%s.test.stderr\n",
	    target->label, artifact, name, name);
	pid = fork();
	if (pid < 0)
		return qstar_set_error(graph, "qstar: fork failed");
	if (pid == 0) {
		if (chdir(graph->package_root ? graph->package_root : ".") < 0)
			_exit(127);
		fdout = open(child_stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fderr = open(child_stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fdout < 0 || fderr < 0 || dup2(fdout, 1) < 0 || dup2(fderr, 2) < 0)
			_exit(127);
		close(fdout);
		close(fderr);
		execl(artifact, artifact, (char *)NULL);
		_exit(127);
	}
	start = time(NULL);
	for (;;) {
		pid_t waited;

		waited = waitpid(pid, &status, WNOHANG);
		if (waited == pid)
			break;
		if (waited < 0)
			return qstar_set_error(graph, "qstar: waitpid failed");
		if (time(NULL) - start >= QSTAR_TEST_TIMEOUT_SEC) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			fprintf(out, "test_result label=%s status=timeout timeout_sec=%d\n",
			    target->label, QSTAR_TEST_TIMEOUT_SEC);
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "test", target->label,
			    "qstar: test '%s' timed out", target->label);
		}
		sleep(1);
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		fprintf(out, "test_result label=%s status=pass exit=0\n", target->label);
		return 0;
	}
	if (WIFEXITED(status)) {
		fprintf(out, "test_result label=%s status=fail exit=%d\n",
		    target->label, WEXITSTATUS(status));
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "test", target->label,
		    "qstar: test '%s' failed with status %d", target->label,
		    WEXITSTATUS(status));
	}
	fprintf(out, "test_result label=%s status=signal signal=%d\n",
	    target->label, WTERMSIG(status));
	return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
	    "test", target->label, "qstar: test '%s' terminated by signal %d",
	    target->label, WTERMSIG(status));
}

/** 단일 test target을 build 후 실행한다. */
static int
test_one_target(struct qstar_graph *graph, const struct qstar_target *target, FILE *out)
{
	if (!qstar_target_has_executable_artifact(target) || strcmp(target->kind, "test") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: target '%s' is not a test target", target->label);
	if (qstar_graph_build(graph, target->label, out) < 0)
		return -1;
	return run_test_artifact(graph, target, out);
}

/** QStar test target을 build한 뒤 제한된 runner로 실행한다. */
int
qstar_graph_test(struct qstar_graph *graph, const char *label, FILE *out)
{
	const struct qstar_target *target;
	size_t i, ran;

	fputs("qstar test v1\n", out);
	fprintf(out, "root %s\n", label && *label ? label : "//...");
	if (label && *label && strcmp(label, "//...") != 0) {
		target = find_target(graph, label);
		if (!target)
			return qstar_set_error(graph, "qstar: unknown target label '%s'", label);
		if (test_one_target(graph, target, out) < 0)
			return -1;
		fputs("status ok\n", out);
		return 0;
	}
	ran = 0;
	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].kind, "test") != 0)
			continue;
		ran++;
		if (test_one_target(graph, &graph->targets[i], out) < 0)
			return -1;
	}
	if (ran == 0)
		return qstar_set_error(graph, "qstar: no test targets found");
	fputs("status ok\n", out);
	return 0;
}

/** 파일을 byte-for-byte 복사한다. */
static int
copy_file_to_path(const char *src, const char *dst)
{
	FILE *in, *out;
	unsigned char buf[8192];
	size_t n;

	in = fopen(src, "rb");
	if (!in)
		return -1;
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -1;
		}
	}
	fclose(in);
	fclose(out);
	return 0;
}

/** install destination parent directory를 만든다. */
static int
mkdir_parent_absolute(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *slash;

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(tmp);
}

struct qstar_install_ctx {
	const struct qstar_install_options *options;
	FILE *out;
	FILE *manifest;
	char manifest_path[QSTAR_PATH_MAX];
	char manifest_tmp[QSTAR_PATH_MAX];
	size_t manifest_len;
};

/** package-local install manifest v2를 쓰기 시작한다. */
static int
install_manifest_begin(struct qstar_graph *graph, struct qstar_install_ctx *ctx,
    const struct qstar_install_options *options, FILE *out)
{
	char dir[QSTAR_PATH_MAX];

	memset(ctx, 0, sizeof(*ctx));
	ctx->options = options;
	ctx->out = out;
	if (full_path_under_root(graph, ".qstar/install", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_root(graph, ".qstar/install/manifest.json",
	    ctx->manifest_path, sizeof(ctx->manifest_path)) < 0)
		return qstar_set_error(graph, "qstar: could not create install manifest dir");
	snprintf(ctx->manifest_tmp, sizeof(ctx->manifest_tmp), "%s.tmp", ctx->manifest_path);
	ctx->manifest = fopen(ctx->manifest_tmp, "w");
	if (!ctx->manifest)
		return qstar_set_error(graph, "qstar: could not write install manifest");
	fputs("{\n  \"schema\":\"qstar-install-manifest-v2\",\n  \"prefix\":", ctx->manifest);
	json_string(ctx->manifest, options->prefix);
	fputs(",\n  \"mode\":", ctx->manifest);
	json_string(ctx->manifest, options->dry_run ? "dry-run" : "copy");
	fputs(",\n  \"entries\":[\n", ctx->manifest);
	fprintf(out, "install_manifest .qstar/install/manifest.json\n");
	return 0;
}

/** install manifest에 artifact/header record 하나를 추가한다. */
static void
install_manifest_record(struct qstar_install_ctx *ctx, const char *target_label,
    const char *role, const char *src_rel, const char *dst)
{
	if (!ctx->manifest)
		return;
	if (ctx->manifest_len)
		fputs(",\n", ctx->manifest);
	fputs("    {\"target\":", ctx->manifest);
	json_string(ctx->manifest, target_label);
	fputs(",\"role\":", ctx->manifest);
	json_string(ctx->manifest, role);
	fputs(",\"src\":", ctx->manifest);
	json_string(ctx->manifest, src_rel);
	fputs(",\"dst\":", ctx->manifest);
	json_string(ctx->manifest, dst);
	fputs(",\"mode\":", ctx->manifest);
	json_string(ctx->manifest, ctx->options->dry_run ? "dry-run" : "copy");
	fputs("}", ctx->manifest);
	ctx->manifest_len++;
}

/** install manifest v2를 완료하고 skeleton export metadata를 남긴다. */
static int
install_manifest_end(struct qstar_graph *graph, struct qstar_install_ctx *ctx, int ok)
{
	if (!ctx->manifest)
		return 0;
	fputs("\n  ],\n  \"exports\":{\"cmake_config\":\"deferred\"},\n  \"status\":",
	    ctx->manifest);
	json_string(ctx->manifest, ok ? "ok" : "fail");
	fputs("\n}\n", ctx->manifest);
	if (fclose(ctx->manifest) != 0) {
		ctx->manifest = NULL;
		return qstar_set_error(graph, "qstar: could not close install manifest");
	}
	ctx->manifest = NULL;
	if (rename(ctx->manifest_tmp, ctx->manifest_path) < 0)
		return qstar_set_error(graph, "qstar: could not commit install manifest");
	return 0;
}

/** public header source path를 prefix install path로 변환한다. */
static int
install_header_dst(const struct qstar_target *target, const char *prefix, const char *header,
    char *dst, size_t dstlen)
{
	const char *rel;
	char package[QSTAR_PATH_MAX], package_include[QSTAR_PATH_MAX];

	rel = strncmp(header, "include/", 8) == 0 ? header + 8 : header;
	if (qstar_label_package_path(target->label, package, sizeof(package)) == 0 &&
	    package[0] &&
	    snprintf(package_include, sizeof(package_include), "%s/include/", package) <
	    (int)sizeof(package_include) &&
	    strncmp(header, package_include, strlen(package_include)) == 0)
		rel = header + strlen(package_include);
	return snprintf(dst, dstlen, "%s/include/%s", prefix, rel) < (int)dstlen ? 0 : -1;
}

/** single file install 또는 dry-run line을 수행한다. */
static int
install_file(struct qstar_graph *graph, struct qstar_install_ctx *ctx,
    const struct qstar_target *target, const char *src_rel, const char *dst, const char *role)
{
	char src[QSTAR_PATH_MAX];
	const char *diff_action;

	diff_action = ctx->options->dry_run ? (path_exists(dst) ? "would-overwrite" :
	    "would-create") : "copy";
	fprintf(ctx->out, "install_file src=%s dst=%s mode=%s role=%s\n", src_rel, dst,
	    ctx->options->dry_run ? "dry-run" : "copy", role);
	fprintf(ctx->out, "install_diff dst=%s action=%s\n", dst, diff_action);
	install_manifest_record(ctx, target->label, role, src_rel, dst);
	if (ctx->options->dry_run)
		return 0;
	if (full_path_under_root(graph, src_rel, src, sizeof(src)) < 0 || !path_exists(src))
		return qstar_set_error(graph, "qstar: install source '%s' is missing", src_rel);
	if (mkdir_parent_absolute(dst) < 0 || copy_file_to_path(src, dst) < 0)
		return qstar_set_error(graph, "qstar: failed to install '%s' to '%s'",
		    src_rel, dst);
	return 0;
}

/** target 하나의 installable artifact/header를 설치한다. */
static int
install_one_target(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_install_ctx *ctx)
{
	char artifact[QSTAR_PATH_MAX], dst[QSTAR_PATH_MAX];
	const char *role;
	size_t i;

	if (!qstar_target_is_installable(target))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: target '%s' is not installable", target->label);
	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0)
		return qstar_set_error(graph, "qstar: install artifact path too long");
	if (strcmp(target->kind, "exe") == 0) {
		role = "exe";
		snprintf(dst, sizeof(dst), "%s/bin/%s", ctx->options->prefix, target->name);
	} else {
		role = "staticlib";
		snprintf(dst, sizeof(dst), "%s/lib/lib%s.a", ctx->options->prefix,
		    target->name);
	}
	if (install_file(graph, ctx, target, artifact, dst, role) < 0)
		return -1;
	for (i = 0; i < target->public_headers.len; i++) {
		if (install_header_dst(target, ctx->options->prefix,
		    target->public_headers.items[i], dst, sizeof(dst)) < 0)
			return qstar_set_error(graph, "qstar: install header path too long");
		if (install_file(graph, ctx, target, target->public_headers.items[i], dst,
		    "header") < 0)
			return -1;
	}
	return 0;
}

/** QStar installable artifact와 public header를 prefix 아래 배치한다. */
int
qstar_graph_install(struct qstar_graph *graph, const char *label,
    const struct qstar_install_options *options, FILE *out)
{
	const struct qstar_target *target;
	struct qstar_install_ctx ctx;
	size_t i, n;
	int rc;

	if (!options || !options->prefix || !*options->prefix)
		return qstar_set_error(graph, "qstar: install requires --prefix");
	fputs("qstar install v2\n", out);
	fprintf(out, "prefix %s\n", options->prefix);
	fprintf(out, "mode %s\n", options->dry_run ? "dry-run" : "copy");
	if (install_manifest_begin(graph, &ctx, options, out) < 0)
		return -1;
	rc = 0;
	if (label && *label) {
		target = find_target(graph, label);
		if (!target)
			rc = qstar_set_error(graph, "qstar: unknown target label '%s'", label);
		else if (install_one_target(graph, target, &ctx) < 0)
			rc = -1;
		if (install_manifest_end(graph, &ctx, rc == 0) < 0 && rc == 0)
			rc = -1;
		if (rc == 0)
			fputs("status ok\n", out);
		return rc;
	}
	n = 0;
	for (i = 0; i < graph->len; i++) {
		if (!qstar_target_is_installable(&graph->targets[i]))
			continue;
		n++;
		if (install_one_target(graph, &graph->targets[i], &ctx) < 0) {
			rc = -1;
			break;
		}
	}
	if (n == 0)
		rc = qstar_set_error(graph, "qstar: no installable targets found");
	if (install_manifest_end(graph, &ctx, rc == 0) < 0 && rc == 0)
		rc = -1;
	if (rc == 0)
		fputs("status ok\n", out);
	return rc;
}

struct qstar_stage_ctx {
	const struct qstar_stage_options *options;
	const struct qstar_stage *stage;
	FILE *out;
	FILE *manifest;
	char manifest_path[QSTAR_PATH_MAX];
	char manifest_tmp[QSTAR_PATH_MAX];
	char root_rel[QSTAR_PATH_MAX];
	size_t manifest_len;
};

/** stage source token에서 target_file label을 추출한다. */
static int
stage_target_file_label(const char *src, char *label, size_t labellen)
{
	const char *prefix = "<qstar-target-file:";
	size_t n, payload;

	if (strncmp(src, prefix, strlen(prefix)) != 0)
		return 0;
	n = strlen(src);
	if (n <= strlen(prefix) + 1 || src[n - 1] != '>')
		return -1;
	payload = n - strlen(prefix) - 1;
	if (payload + 1 > labellen)
		return -1;
	memcpy(label, src + strlen(prefix), payload);
	label[payload] = '\0';
	return 1;
}

/** 두 파일이 byte-for-byte 동일한지 확인한다. */
static int
file_content_equal(const char *a, const char *b)
{
	FILE *fa, *fb;
	unsigned char ba[8192], bb[8192];
	size_t na, nb;

	fa = fopen(a, "rb");
	if (!fa)
		return 0;
	fb = fopen(b, "rb");
	if (!fb) {
		fclose(fa);
		return 0;
	}
	do {
		na = fread(ba, 1, sizeof(ba), fa);
		nb = fread(bb, 1, sizeof(bb), fb);
		if (na != nb || memcmp(ba, bb, na) != 0) {
			fclose(fa);
			fclose(fb);
			return 0;
		}
	} while (na > 0);
	fclose(fa);
	fclose(fb);
	return 1;
}

/** stage manifest v1을 쓰기 시작한다. */
static int
stage_manifest_begin(struct qstar_graph *graph, struct qstar_stage_ctx *ctx,
    const struct qstar_stage *stage, const struct qstar_stage_options *options, FILE *out)
{
	char dir[QSTAR_PATH_MAX], owner[QSTAR_PATH_MAX];

	memset(ctx, 0, sizeof(*ctx));
	ctx->options = options;
	ctx->stage = stage;
	ctx->out = out;
	if (options && options->root && *options->root)
		snprintf(ctx->root_rel, sizeof(ctx->root_rel), "%s", options->root);
	else
		snprintf(ctx->root_rel, sizeof(ctx->root_rel), "%s", stage->root);
	qstar_mangle_label(stage->label, owner, sizeof(owner));
	if (snprintf(dir, sizeof(dir), ".qstar/stage/%s", owner) >= (int)sizeof(dir) ||
	    mkdir_parent_under_root(graph, ".qstar/stage/.keep") < 0 ||
	    full_path_under_root(graph, dir, ctx->manifest_path, sizeof(ctx->manifest_path)) < 0 ||
	    mkdir_p(ctx->manifest_path) < 0)
		return qstar_set_error(graph, "qstar: could not create stage manifest dir");
	if (snprintf(dir, sizeof(dir), ".qstar/stage/%s/manifest.json", owner) >=
	    (int)sizeof(dir) ||
	    full_path_under_root(graph, dir, ctx->manifest_path,
	    sizeof(ctx->manifest_path)) < 0)
		return qstar_set_error(graph, "qstar: stage manifest path too long");
	snprintf(ctx->manifest_tmp, sizeof(ctx->manifest_tmp), "%s.tmp",
	    ctx->manifest_path);
	ctx->manifest = fopen(ctx->manifest_tmp, "w");
	if (!ctx->manifest)
		return qstar_set_error(graph, "qstar: could not write stage manifest");
	fputs("{\n  \"schema\":\"qstar-stage-manifest-v1\",\n  \"label\":", ctx->manifest);
	json_string(ctx->manifest, stage->label);
	fputs(",\n  \"root\":", ctx->manifest);
	json_string(ctx->manifest, ctx->root_rel);
	fputs(",\n  \"mode\":", ctx->manifest);
	json_string(ctx->manifest, options && options->dry_run ? "dry-run" : "copy");
	fputs(",\n  \"entries\":[\n", ctx->manifest);
	fprintf(out, "stage_manifest .qstar/stage/%s/manifest.json\n", owner);
	return 0;
}

/** stage manifest에 copy/diff record 하나를 추가한다. */
static void
stage_manifest_record(struct qstar_stage_ctx *ctx, const char *src_rel,
    const char *dst_rel, const char *action)
{
	if (!ctx->manifest)
		return;
	if (ctx->manifest_len)
		fputs(",\n", ctx->manifest);
	fputs("    {\"src\":", ctx->manifest);
	json_string(ctx->manifest, src_rel);
	fputs(",\"dst\":", ctx->manifest);
	json_string(ctx->manifest, dst_rel);
	fputs(",\"action\":", ctx->manifest);
	json_string(ctx->manifest, action);
	fputs("}", ctx->manifest);
	ctx->manifest_len++;
}

/** stage manifest v1을 완료한다. */
static int
stage_manifest_end(struct qstar_graph *graph, struct qstar_stage_ctx *ctx, int ok)
{
	if (!ctx->manifest)
		return 0;
	fputs("\n  ],\n  \"status\":", ctx->manifest);
	json_string(ctx->manifest, ok ? "ok" : "fail");
	fputs("\n}\n", ctx->manifest);
	if (fclose(ctx->manifest) != 0) {
		ctx->manifest = NULL;
		return qstar_set_error(graph, "qstar: could not close stage manifest");
	}
	ctx->manifest = NULL;
	if (rename(ctx->manifest_tmp, ctx->manifest_path) < 0)
		return qstar_set_error(graph, "qstar: could not commit stage manifest");
	return 0;
}

/** stage source가 build artifact이면 dry-run이 아닌 경우 먼저 빌드한다. */
static int
stage_build_source_dependency(struct qstar_graph *graph, const char *src, int dry_run,
    FILE *out)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = stage_target_file_label(src, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed stage target_file source");
	if (dry_run)
		return 0;
	if (rc == 1)
		return qstar_graph_build(graph, label, out);
	owner = qstar_graph_find_output_owner(graph, src);
	if (owner)
		return qstar_graph_build(graph, owner->label, out);
	return 0;
}

/** stage source token을 package-relative source path로 해석한다. */
static int
stage_resolve_src(struct qstar_graph *graph, const char *src, char *dst, size_t dstlen)
{
	return resolve_target_file_token(graph, src, dst, dstlen);
}

/** single stage copy 또는 dry-run diff line을 수행한다. */
static int
stage_file(struct qstar_graph *graph, struct qstar_stage_ctx *ctx, const char *src_token,
    const char *dst_rel)
{
	char src_rel[QSTAR_PATH_MAX], src_full[QSTAR_PATH_MAX], dst_stage_rel[QSTAR_PATH_MAX];
	char dst_full[QSTAR_PATH_MAX];
	const char *action;
	int exists;

	if (stage_build_source_dependency(graph, src_token,
	    ctx->options && ctx->options->dry_run, ctx->out) < 0)
		return -1;
	if (stage_resolve_src(graph, src_token, src_rel, sizeof(src_rel)) < 0)
		return -1;
	if (qstar_path_join(ctx->root_rel, dst_rel, dst_stage_rel,
	    sizeof(dst_stage_rel)) < 0)
		return qstar_set_error(graph, "qstar: stage destination path too long");
	if (full_path_under_root(graph, src_rel, src_full, sizeof(src_full)) < 0 ||
	    full_path_under_root(graph, dst_stage_rel, dst_full, sizeof(dst_full)) < 0)
		return qstar_set_error(graph, "qstar: stage file path too long");
	exists = path_exists(dst_full);
	if (exists && file_content_equal(src_full, dst_full))
		action = "unchanged";
	else if (exists)
		action = "would-update";
	else
		action = "would-create";
	fprintf(ctx->out, "stage_file src=%s dst=%s mode=%s\n", src_rel, dst_stage_rel,
	    ctx->options && ctx->options->dry_run ? "dry-run" : "copy");
	fprintf(ctx->out, "stage_diff dst=%s action=%s\n", dst_stage_rel, action);
	stage_manifest_record(ctx, src_rel, dst_stage_rel, action);
	if (ctx->options && ctx->options->dry_run)
		return 0;
	if (!path_exists(src_full))
		return qstar_set_error(graph, "qstar: stage source '%s' is missing", src_rel);
	if (mkdir_parent_under_root(graph, dst_stage_rel) < 0 ||
	    copy_file_to_path(src_full, dst_full) < 0)
		return qstar_set_error(graph, "qstar: failed to stage '%s' to '%s'",
		    src_rel, dst_stage_rel);
	return 0;
}

/** QStar boot/package staging rule을 package-local root 아래 copy-only로 실행한다. */
int
qstar_graph_stage(struct qstar_graph *graph, const char *label,
    const struct qstar_stage_options *options, FILE *out)
{
	const struct qstar_stage *stage;
	struct qstar_stage_ctx ctx;
	size_t i;
	int rc;

	if (!label || !*label)
		return qstar_set_error(graph, "qstar: stage requires a stage label");
	stage = qstar_graph_find_stage(graph, label);
	if (!stage)
		return qstar_set_error(graph, "qstar: unknown stage label '%s'", label);
	if (options && options->root && *options->root &&
	    !qstar_path_is_package_relative(options->root))
		return qstar_set_error_origin(graph, stage->origin_file, stage->origin_line,
		    "root", stage->label,
		    "qstar: stage override root '%s' must be package-relative",
		    options->root);
	fputs("qstar stage v1\n", out);
	fprintf(out, "root %s\n", stage->label);
	fprintf(out, "stage-root %s\n", options && options->root && *options->root ?
	    options->root : stage->root);
	fprintf(out, "mode %s\n", options && options->dry_run ? "dry-run" : "copy");
	if (stage_manifest_begin(graph, &ctx, stage, options, out) < 0)
		return -1;
	rc = 0;
	for (i = 0; i < stage->srcs.len; i++) {
		if (stage_file(graph, &ctx, stage->srcs.items[i],
		    stage->dsts.items[i]) < 0) {
			rc = -1;
			break;
		}
	}
	if (stage_manifest_end(graph, &ctx, rc == 0) < 0 && rc == 0)
		rc = -1;
	if (rc == 0)
		fputs("status ok\n", out);
	return rc;
}

/** qsort용 string pointer compare다. */
static int
cmp_string_ptr(const void *a, const void *b)
{
	const char *const *sa = a;
	const char *const *sb = b;

	return strcmp(*sa, *sb);
}

/** log filename list에 문자열을 추가한다. */
static int
push_name(char ***items, size_t *len, size_t *cap, const char *name)
{
	char **p;
	size_t ncap;

	if (*len == *cap) {
		ncap = *cap ? *cap * 2 : 16;
		p = realloc(*items, ncap * sizeof((*items)[0]));
		if (!p)
			return -1;
		*items = p;
		*cap = ncap;
	}
	(*items)[*len] = qstar_strdup(name);
	return (*items)[(*len)++] ? 0 : -1;
}

/** QStar action log path 목록을 target 기준으로 출력한다. */
int
qstar_graph_log(struct qstar_graph *graph, const char *label, FILE *out)
{
	char dirpath[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX], owner[QSTAR_PATH_MAX];
	char **names = NULL;
	size_t len = 0, cap = 0, i;
	DIR *dir;
	struct dirent *ent;
	const char *needle;

	if (!label || !*label)
		return qstar_set_error(graph, "qstar: log requires a target label");
	qstar_mangle_label(label, owner, sizeof(owner));
	snprintf(prefix, sizeof(prefix), "%s_", owner);
	needle = prefix;
	if (full_path_under_root(graph, ".qstar/logs", dirpath, sizeof(dirpath)) < 0)
		return qstar_set_error(graph, "qstar: log path too long");
	dir = opendir(dirpath);
	fputs("qstar log v1\n", out);
	fprintf(out, "root %s\n", label);
	if (!dir) {
		fputs("status ok\n", out);
		return 0;
	}
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, needle, strlen(needle)) == 0 &&
		    push_name(&names, &len, &cap, ent->d_name) < 0) {
			closedir(dir);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	closedir(dir);
	qsort(names, len, sizeof(names[0]), cmp_string_ptr);
	for (i = 0; i < len; i++) {
		fprintf(out, "log_file .qstar/logs/%s\n", names[i]);
		free(names[i]);
	}
	free(names);
	fputs("status ok\n", out);
	return 0;
}

/** 마지막 실패 replay 파일을 출력한다. */
int
qstar_graph_last_failure(struct qstar_graph *graph, FILE *out)
{
	char path[QSTAR_PATH_MAX], line[4096];
	FILE *f;

	if (full_path_under_root(graph, ".qstar/logs/last-failure.replay", path,
	    sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: replay path too long");
	fputs("qstar last-failure v1\n", out);
	f = fopen(path, "r");
	if (!f) {
		fputs("replay <none>\nstatus ok\n", out);
		return 0;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, out);
	fclose(f);
	fputs("status ok\n", out);
	return 0;
}

/** action id를 package-local log path로 변환한다. */
static int
action_log_path_for_id(struct qstar_graph *graph, const char *action_id, char *rel,
    size_t rel_len, char *full, size_t full_len)
{
	char name[QSTAR_PATH_MAX];

	if (!action_id || !*action_id)
		return qstar_set_error(graph, "qstar: action id is required");
	action_log_name(action_id, name, sizeof(name));
	if (snprintf(rel, rel_len, ".qstar/logs/%s.log", name) >= (int)rel_len ||
	    full_path_under_root(graph, rel, full, full_len) < 0)
		return qstar_set_error(graph, "qstar: action log path too long");
	return 0;
}

/** action id에 대응하는 deterministic action log를 출력한다. */
int
qstar_graph_action_log(struct qstar_graph *graph, const char *action_id, FILE *out)
{
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], line[4096];
	FILE *f;

	if (action_log_path_for_id(graph, action_id, rel, sizeof(rel), full,
	    sizeof(full)) < 0)
		return -1;
	f = fopen(full, "r");
	if (!f)
		return qstar_set_error(graph, "qstar: action log '%s' does not exist", rel);
	fputs("qstar action-log v1\n", out);
	fprintf(out, "action %s\n", action_id);
	fprintf(out, "log %s\n", rel);
	while (fgets(line, sizeof(line), f))
		fputs(line, out);
	fclose(f);
	fputs("status ok\n", out);
	return 0;
}

/** action id에 대응하는 shell replay command를 출력한다. */
int
qstar_graph_replay_action(struct qstar_graph *graph, const char *action_id, FILE *out)
{
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], line[8192], command[8192];
	FILE *f;

	if (action_log_path_for_id(graph, action_id, rel, sizeof(rel), full,
	    sizeof(full)) < 0)
		return -1;
	f = fopen(full, "r");
	if (!f)
		return qstar_set_error(graph, "qstar: action log '%s' does not exist", rel);
	command[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "argv_shell=", 11) == 0) {
			snprintf(command, sizeof(command), "%s", line + 11);
			break;
		}
	}
	fclose(f);
	if (!command[0])
		return qstar_set_error(graph, "qstar: action log '%s' has no replay argv",
		    rel);
	fputs("qstar replay v1\n", out);
	fprintf(out, "action %s\n", action_id);
	fprintf(out, "log %s\n", rel);
	fprintf(out, "cd %s\n", graph->package_root ? graph->package_root : ".");
	fputs(command, out);
	if (command[strlen(command) - 1] != '\n')
		fputc('\n', out);
	fputs("status ok\n", out);
	return 0;
}
