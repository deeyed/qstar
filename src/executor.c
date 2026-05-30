#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define QSTAR_EXEC_MAX_ARGV 256
#define QSTAR_HASH_INIT 1469598103934665603ULL
#define QSTAR_HASH_PRIME 1099511628211ULL

struct qstar_build_ctx {
	FILE *out;
	const char *root_label;
	int explain_cache;
	int explain_only;
	struct qstar_state_entry {
		char *id;
		char *key;
		char *output;
		char *status;
	} *prev, *next;
	size_t prev_len, prev_cap, next_len, next_cap;
	struct qstar_compile_record {
		char *directory;
		char *file;
		char *output;
		char *command;
	} *compiles;
	size_t compile_len, compile_cap;
};

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

/** action key를 hex 문자열로 만든다. */
static void
format_key(unsigned long long h, char *dst, size_t dstlen)
{
	snprintf(dst, dstlen, "%016llx", h);
}

/** action key 공통 material을 계산한다. */
static void
compute_action_key(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *id, const char *kind,
    char *const argv[], const struct qstar_string_list *inputs, const char *output,
    char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	hash_str(&h, id);
	hash_str(&h, kind);
	hash_str(&h, target ? target->label : "<generated>");
	hash_str(&h, output);
	hash_str(&h, graph->profile.name ? graph->profile.name : "default");
	hash_str(&h, graph->profile.target ? graph->profile.target : "host");
	if (toolchain) {
		hash_str(&h, toolchain->name);
		hash_str(&h, toolchain->target);
		hash_str(&h, toolchain->stdlib_policy);
		hash_str(&h, toolchain->cc);
		hash_str(&h, toolchain->ar);
		hash_str(&h, toolchain->linker);
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

/** argv 배열을 action log에 deterministic하게 기록한다. */
static void
write_action_log(const char *path, char *const argv[], int exit_code)
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "exit=%d\nargv=", exit_code);
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
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
	fputs("exit=skip\nargv=", f);
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
	fputc('\n', f);
	fclose(f);
}

/** 실패한 action을 직접 재현할 수 있는 argv 파일을 갱신한다. */
static void
write_failure_replay(const struct qstar_graph *graph, char *const argv[])
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    ".qstar/logs/last-failure.replay", path, sizeof(path)) < 0)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "cd %s\n", graph->package_root ? graph->package_root : ".");
	for (i = 0; argv[i]; i++)
		fprintf(f, "%s%s", i ? " " : "", argv[i]);
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
    const char *output, const char *status)
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
	return p->id && p->key && p->output && p->status ? 0 : -1;
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

/** 매우 작은 actions.json reader: QStar가 쓴 one-object-per-line만 읽는다. */
static int
state_load(struct qstar_graph *graph, struct qstar_build_ctx *ctx)
{
	char path[QSTAR_PATH_MAX], line[8192], *id, *key, *output, *status;
	FILE *f;

	if (full_path_under_root(graph, ".qstar/state/actions.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: state path too long");
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		id = strstr(line, "\"id\":\"");
		key = strstr(line, "\"key\":\"");
		output = strstr(line, "\"output\":\"");
		status = strstr(line, "\"status\":\"");
		if (!id || !key || !output || !status)
			continue;
		id += 6;
		key += 7;
		output += 10;
		status += 10;
		strchr(id, '"') ? *strchr(id, '"') = '\0' : 0;
		strchr(key, '"') ? *strchr(key, '"') = '\0' : 0;
		strchr(output, '"') ? *strchr(output, '"') = '\0' : 0;
		strchr(status, '"') ? *strchr(status, '"') = '\0' : 0;
		if (state_push(ctx, 0, id, key, output, status) < 0) {
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
		fputs("}", f);
		fputs(i + 1 == ctx->next_len ? "\n" : ",\n", f);
	}
	fputs("]\n", f);
	fclose(f);
	if (rename(tmp, path) < 0)
		return qstar_set_error(graph, "qstar: could not commit action state");
	return 0;
}

/** compile_commands.json command string용으로 argv를 join한다. */
static char *
join_argv(char *const argv[])
{
	size_t i, len;
	char *s;

	len = 1;
	for (i = 0; argv[i]; i++)
		len += strlen(argv[i]) + 1;
	s = malloc(len);
	if (!s)
		return NULL;
	s[0] = '\0';
	for (i = 0; argv[i]; i++) {
		if (i)
			strcat(s, " ");
		strcat(s, argv[i]);
	}
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
    const struct qstar_string_list *outputs, char *const argv[])
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX];
	char stderr_path[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	const struct qstar_state_entry *prev;
	pid_t pid;
	int status, fdout, fderr, exit_code;

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
	if (ctx->explain_only) {
		fprintf(ctx->out,
		    "cache_action id=%s kind=%s status=%s reason=%s key=%s previous=%s\n",
		    id, kind,
		    prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, outputs) ?
		    "skip" : "run",
		    prev ? (strcmp(prev->key, key) == 0 ? "output-check" : "key-changed") :
		    "no-previous-state",
		    key, prev ? prev->key : "<none>");
		return 0;
	}
	if (prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, outputs)) {
		fprintf(ctx->out,
		    "build_action id=%s status=skip reason=cache-hit stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
		    id, name, name);
		write_skip_log(action_log, argv);
		return state_push(ctx, 1, id, key, outputs->items[0], "skip") < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	fprintf(ctx->out,
	    "build_action id=%s status=run stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    id, name, name);
	if (ctx->explain_cache)
		fprintf(ctx->out, "cache_miss id=%s key=%s previous=%s\n",
		    id, key, prev ? prev->key : "<none>");
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
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return qstar_set_error(graph, "qstar: waitpid failed");
	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	write_action_log(action_log, argv, exit_code);
	if (exit_code != 0) {
		fprintf(ctx->out, "build_action id=%s status=fail exit=%d\n", id, exit_code);
		write_failure_replay(graph, argv);
		return qstar_set_error_origin(graph, target ? target->origin_file : "",
		    target ? target->origin_line : 0, "action", target ? target->label : id,
		    "qstar: action '%s' failed with status %d; replay=.qstar/logs/last-failure.replay",
		    id, exit_code);
	}
	if (state_push(ctx, 1, id, key, outputs->items[0], "run") < 0)
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

/** qstar.config_header define 항목 하나를 C preprocessor line으로 출력한다. */
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

/** qstar.config_header output 파일을 deterministic하게 생성한다. */
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
	fprintf(f, "/* generated by qstar.config_header: %s */\n", genrule->label);
	fprintf(f, "#ifndef %s\n#define %s\n", guard, guard);
	for (i = 0; i < genrule->args.len; i++)
		write_config_define(f, genrule->args.items[i]);
	fprintf(f, "#endif /* %s */\n", guard);
	fclose(f);
	return 0;
}

/** qstar.config_header를 external process 없이 action/cache model에 맞춰 실행한다. */
static int
run_config_header_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule, const char *id,
    const char *key, char *const argv[])
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
	if (ctx->explain_only) {
		fprintf(ctx->out,
		    "cache_action id=%s kind=generate status=%s reason=%s key=%s previous=%s\n",
		    id,
		    prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, &genrule->outputs) ?
		    "skip" : "run",
		    prev ? (strcmp(prev->key, key) == 0 ? "output-check" : "key-changed") :
		    "no-previous-state",
		    key, prev ? prev->key : "<none>");
		return 0;
	}
	if (prev && strcmp(prev->key, key) == 0 && outputs_exist(graph, &genrule->outputs)) {
		fprintf(ctx->out,
		    "build_action id=%s status=skip reason=cache-hit stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
		    id, name, name);
		write_skip_log(action_log, argv);
		return state_push(ctx, 1, id, key, genrule->outputs.items[0], "skip") < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	fprintf(ctx->out,
	    "build_action id=%s status=run stdout=.qstar/logs/%s.stdout stderr=.qstar/logs/%s.stderr\n",
	    id, name, name);
	if (ctx->explain_cache)
		fprintf(ctx->out, "cache_miss id=%s key=%s previous=%s\n",
		    id, key, prev ? prev->key : "<none>");
	if (write_config_header(graph, genrule) < 0) {
		write_failure_replay(graph, argv);
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "action", target->label, "%s", graph->error);
	}
	write_empty_log_file(stdout_path);
	write_empty_log_file(stderr_path);
	write_action_log(action_log, argv, 0);
	return state_push(ctx, 1, id, key, genrule->outputs.items[0], "run") < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

/** target이 소비하는 generated action을 package-local tool policy로 실행한다. */
static int
run_generated_actions(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	char id[QSTAR_PATH_MAX], key[32];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	struct qstar_string_list inputs;
	struct qstar_resolved_toolchain toolchain;
	size_t i, argc;

	for (i = 0; i < graph->genrule_len; i++) {
		genrule = &graph->genrules[i];
		if (!target_consumes_genrule(target, genrule))
			continue;
		if (!genrule->config_header && !qstar_path_is_package_relative(genrule->tool))
			return qstar_set_error(graph,
			    "qstar: generated action tool '%s' must be package-relative",
			    genrule->tool);
		if (genrule->args.len + 2 > QSTAR_EXEC_MAX_ARGV)
			return qstar_set_error(graph, "qstar: generated action argv too long");
		for (argc = 0; argc < genrule->outputs.len; argc++) {
			if (mkdir_parent_under_root(graph, genrule->outputs.items[argc]) < 0)
				return qstar_set_error(graph,
				    "qstar: could not create generated output directory");
		}
		snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
		argv[0] = genrule->tool;
		for (argc = 0; argc < genrule->args.len; argc++)
			argv[argc + 1] = genrule->args.items[argc];
		argv[genrule->args.len + 1] = NULL;
		memset(&inputs, 0, sizeof(inputs));
		for (argc = 0; argc < genrule->inputs.len; argc++) {
			if (qstar_string_list_push(&inputs, genrule->inputs.items[argc]) < 0) {
				qstar_string_list_free(&inputs);
				return qstar_set_error(graph, "qstar: out of memory");
			}
		}
		if (!genrule->config_header && qstar_string_list_push(&inputs, genrule->tool) < 0) {
			qstar_string_list_free(&inputs);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		memset(&toolchain, 0, sizeof(toolchain));
		snprintf(toolchain.name, sizeof(toolchain.name), "%s", target->toolchain);
		snprintf(toolchain.target, sizeof(toolchain.target), "%s",
		    graph->profile.target ? graph->profile.target : "host");
		compute_action_key(graph, target, &toolchain, id, "generate", argv,
		    &inputs, genrule->outputs.items[0], key, sizeof(key));
		qstar_string_list_free(&inputs);
		if (genrule->config_header) {
			if (run_config_header_action(graph, ctx, target, genrule, id, key, argv) < 0)
				return -1;
		} else if (run_action(graph, ctx, target, id, "generate", key,
		    &genrule->outputs, argv) < 0) {
			return -1;
		}
	}
	return 0;
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

/** compile source 종류와 toolchain 조합을 검증한다. */
static int
validate_compile_source(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const struct qstar_source_info *source,
    size_t index)
{
	if (strcmp(source->language, "header") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: header source '%s' must be listed as public_headers/private_headers",
		    target->sources.items[index]);
	if (strcmp(source->language, "c") != 0 && strcmp(source->language, "cale") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: local executor does not support source '%s' with language '%s'",
		    target->sources.items[index], source->language);
	if (strcmp(source->language, "cale") == 0 &&
	    strcmp(toolchain->name, "cale") != 0 &&
	    strcmp(toolchain->name, "cale-sol") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: Cale source '%s' requires toolchain=cale",
		    target->sources.items[index]);
	if ((strcmp(toolchain->name, "cale") == 0 ||
	    strcmp(toolchain->name, "cale-sol") == 0) && !command_exists(toolchain->cale))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: Cale compiler '%s' not found for source '%s'",
		    toolchain->cale, target->sources.items[index]);
	return 0;
}

/** C 또는 Cale source 하나를 object로 compile한다. */
static int
run_compile(struct qstar_graph *graph, struct qstar_build_ctx *ctx, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, size_t index)
{
	struct qstar_source_info source;
	char object[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], target_arg[QSTAR_PATH_MAX], key[32];
	const char *compiler;
	char *argv[QSTAR_EXEC_MAX_ARGV];
	struct qstar_string_list inputs, outputs;
	int cross;
	size_t argc, i;

	qstar_source_classify(target->sources.items[index], &source);
	if (validate_compile_source(graph, target, toolchain, &source, index) < 0)
		return -1;
	if (qstar_object_output_path(target, index, object, sizeof(object)) < 0 ||
	    mkdir_parent_under_root(graph, object) < 0)
		return qstar_set_error(graph, "qstar: could not create object output directory");
	if (target->include_dirs.len * 2 + target->system_include_dirs.len * 2 + 8 >
	    QSTAR_EXEC_MAX_ARGV)
		return qstar_set_error(graph, "qstar: compile argv too long");
	snprintf(id, sizeof(id), "%s:compile:%zu", target->label, index);
	snprintf(target_arg, sizeof(target_arg), "--target=%s", toolchain->target);
	compiler = strcmp(source.language, "cale") == 0 ? toolchain->cale : toolchain->cc;
	cross = (strcmp(toolchain->name, "clang") == 0 ||
	    strcmp(toolchain->name, "cale") == 0 ||
	    strcmp(toolchain->name, "cale-sol") == 0) &&
	    strcmp(toolchain->target, "host") != 0;
	argc = 0;
	argv[argc++] = (char *)compiler;
	if (cross)
		argv[argc++] = target_arg;
	argv[argc++] = "-c";
	argv[argc++] = target->sources.items[index];
	argv[argc++] = "-o";
	argv[argc++] = object;
	for (i = 0; i < target->include_dirs.len; i++) {
		argv[argc++] = "-I";
		argv[argc++] = target->include_dirs.items[i];
	}
	for (i = 0; i < target->system_include_dirs.len; i++) {
		argv[argc++] = "-isystem";
		argv[argc++] = target->system_include_dirs.items[i];
	}
	argv[argc] = NULL;
	if (compile_db_push(ctx, graph->package_root ? graph->package_root : ".",
	    target->sources.items[index], object, argv) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	memset(&inputs, 0, sizeof(inputs));
	memset(&outputs, 0, sizeof(outputs));
	if (qstar_string_list_push(&inputs, target->sources.items[index]) < 0 ||
	    qstar_string_list_push(&outputs, object) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&outputs);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	if (push_target_header_inputs(graph, target, &inputs) < 0) {
		qstar_string_list_free(&inputs);
		qstar_string_list_free(&outputs);
		return -1;
	}
	compute_action_key(graph, target, toolchain, id, "compile", argv, &inputs, object, key,
	    sizeof(key));
	qstar_string_list_free(&inputs);
	return run_action(graph, ctx, target, id, "compile", key, &outputs, argv) < 0 ?
	    (qstar_string_list_free(&outputs), -1) :
	    (qstar_string_list_free(&outputs), 0);
}

/** target의 direct dependency artifact를 final argv 뒤에 붙인다. */
static int
append_dep_artifacts(struct qstar_graph *graph, const struct qstar_target *target,
    char **argv, size_t *argc)
{
	const struct qstar_target *dep;
	char artifact[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < target->deps.len; i++) {
		if (target->deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		if (qstar_artifact_output_path(dep, artifact, sizeof(artifact)) < 0)
			return qstar_set_error(graph, "qstar: dependency artifact path too long");
		argv[(*argc)++] = qstar_strdup(artifact);
		if (!argv[*argc - 1])
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

/** append_dep_artifacts에서 복사한 argv tail을 해제한다. */
static void
free_dep_artifacts(char **argv, size_t first, size_t argc)
{
	while (first < argc)
		free(argv[first++]);
}

/** target final archive/link action을 실행한다. */
static int
run_final(struct qstar_graph *graph, struct qstar_build_ctx *ctx, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	char artifact[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], key[32];
	char *argv[QSTAR_EXEC_MAX_ARGV];
	struct qstar_string_list inputs, outputs;
	size_t argc, dep_first, i;
	int rc;

	if (strcmp(target->kind, "exe") != 0 && strcmp(target->kind, "staticlib") != 0)
		return qstar_set_error(graph,
		    "qstar: local executor v1 only supports exe/staticlib targets");
	if (qstar_artifact_output_path(target, artifact, sizeof(artifact)) < 0 ||
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
		argv[argc++] = (char *)toolchain->linker;
		argv[argc++] = "-o";
		argv[argc++] = artifact;
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
		free_dep_artifacts(argv, 3, dep_first);
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
	if (qstar_string_list_push(&outputs, artifact) < 0) {
		qstar_string_list_free(&inputs);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	compute_action_key(graph, target, toolchain, id,
	    strcmp(target->kind, "staticlib") == 0 ? "archive" : "link", argv, &inputs,
	    artifact, key, sizeof(key));
	rc = run_action(graph, ctx, target, id,
	    strcmp(target->kind, "staticlib") == 0 ? "archive" : "link", key, &outputs, argv);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&outputs);
	free_dep_artifacts(argv, 3, argc);
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
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	fprintf(ctx->out, "resolved_toolchain owner=%s toolchain=%s target=%s cc=%s ar=%s linker=%s\n",
	    target->label, toolchain.name, toolchain.target, toolchain.cc, toolchain.ar,
	    toolchain.linker);
	if (run_generated_actions(graph, ctx, target) < 0)
		return -1;
	for (i = 0; i < target->sources.len; i++) {
		if (run_compile(graph, ctx, target, &toolchain, i) < 0)
			return -1;
	}
	return run_final(graph, ctx, target, &toolchain);
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
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.out = out;
	ctx.root_label = label && *label ? label : "<all>";
	ctx.explain_cache = options ? options->explain_cache : 0;
	if (state_load(graph, &ctx) < 0) {
		build_ctx_free(&ctx);
		return -1;
	}
	fputs("qstar build v2\n", out);
	fprintf(out, "root %s\n", ctx.root_label);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	rc = qstar_graph_visit_closure(graph, label, build_target, &ctx);
	if (rc == 0)
		rc = state_write(graph, &ctx);
	if (rc == 0)
		rc = compile_db_write(graph, &ctx);
	if (rc == 0)
		fputs("status ok\n", out);
	build_ctx_free(&ctx);
	return rc;
}

/** QStar local executor v1로 제한된 build action을 실행한다. */
int
qstar_graph_build(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_build_options options;

	memset(&options, 0, sizeof(options));
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
