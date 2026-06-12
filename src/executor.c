#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
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
	int verbose;
	int quiet;
	int progress_mode;
	int color_mode;
	int progress_enabled;
	int color_enabled;
	int progress_next_tick;
	int progress_stage;
	int action_timeout_sec;
	int cancelled;
	int action_scheduler;
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
		char *output_key;
		char *external_tool_key;
	} *prev, *next;
	size_t prev_len, prev_cap, next_len, next_cap;
	struct qstar_file_hash_entry {
		char *rel;
		char digest[32];
	} *hash_cache;
	size_t hash_cache_len, hash_cache_cap;
	struct qstar_action_log_entry {
		char *path;
		char *exit_text;
		char **argv;
		size_t argc;
		int owns_argv;
	} *action_logs;
	size_t action_log_len, action_log_cap;
	struct qstar_pending_depfile_refresh {
		struct qstar_prepared_action *action;
	} *depfile_refresh;
	size_t depfile_refresh_len, depfile_refresh_cap;
	char logdir_full[QSTAR_PATH_MAX];
	int logdir_ready;
	size_t run_count, skip_count, fail_count;
	size_t scheduled_count;
	size_t progress_total;
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
	char output_key[32];
	char external_tool_key[32];
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
	size_t node_index;
	char name[QSTAR_PATH_MAX];
	char action_log[QSTAR_PATH_MAX];
};

enum qstar_sched_node_kind {
	QSTAR_SCHED_COMPILE,
	QSTAR_SCHED_FINAL,
	QSTAR_SCHED_GENERATE,
	QSTAR_SCHED_RUN,
	QSTAR_SCHED_GROUP
};

enum qstar_sched_node_state {
	QSTAR_SCHED_PENDING,
	QSTAR_SCHED_READY,
	QSTAR_SCHED_RUNNING,
	QSTAR_SCHED_DONE
};

struct qstar_sched_node {
	enum qstar_sched_node_kind kind;
	enum qstar_sched_node_state state;
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	struct qstar_resolved_toolchain *toolchain;
	struct qstar_prepared_action action;
	size_t source_index;
	size_t target_queue_index;
	size_t remaining;
	size_t *deps;
	size_t dep_len;
	size_t dep_cap;
	size_t *outs;
	size_t out_len;
	size_t out_cap;
};

struct qstar_scheduler {
	struct qstar_graph *graph;
	struct qstar_build_ctx *ctx;
	struct qstar_sched_node *nodes;
	size_t node_len;
	size_t node_cap;
	const struct qstar_target **targets;
	size_t target_len;
	size_t target_cap;
	struct qstar_resolved_toolchain *toolchains;
	int *target_seen;
	int *genrule_needed;
	int *target_final_node;
	int *genrule_node;
	size_t *ready;
	size_t ready_head;
	size_t ready_len;
	size_t ready_cap;
};

/** FILE stream이 terminal인지 안전하게 확인한다. */
static int
stream_is_tty(FILE *out)
{
	return out && isatty(fileno(out));
}

/** build color 정책을 terminal capability와 합쳐 최종 bool로 만든다. */
static int
build_color_enabled(FILE *out, int color_mode)
{
	if (color_mode == QSTAR_COLOR_ALWAYS)
		return 1;
	if (color_mode == QSTAR_COLOR_NEVER)
		return 0;
	return stream_is_tty(out);
}

/** build progress 정책을 terminal capability와 합쳐 최종 bool로 만든다. */
static int
build_progress_enabled(FILE *out, int progress_mode)
{
	(void)out;
	return progress_mode != QSTAR_PROGRESS_OFF;
}

/** ANSI color prefix를 build color policy에 맞게 반환한다. */
static const char *
build_color(const struct qstar_build_ctx *ctx, const char *role)
{
	if (!ctx->color_enabled)
		return "";
	if (strcmp(role, "error") == 0)
		return "\033[1;31m";
	if (strcmp(role, "warning") == 0)
		return "\033[1;33m";
	if (strcmp(role, "success") == 0)
		return "\033[32m";
	if (strcmp(role, "stage") == 0)
		return "\033[1m";
	return "";
}

/** ANSI reset sequence를 build color policy에 맞게 반환한다. */
static const char *
build_color_reset(const struct qstar_build_ctx *ctx)
{
	return ctx->color_enabled ? "\033[0m" : "";
}

/** 상세 build event stream을 출력할지 결정한다. */
static int
build_trace_enabled(const struct qstar_build_ctx *ctx)
{
	return ctx->verbose || ctx->schedule_trace;
}

/** --verbose/--schedule-trace에서만 상세 build event를 출력한다. */
static void
build_tracef(struct qstar_build_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	if (!build_trace_enabled(ctx))
		return;
	va_start(ap, fmt);
	vfprintf(ctx->out, fmt, ap);
	va_end(ap);
}

/** target kind에 맞는 final action stage 이름을 고른다. */
static const char *
final_stage_name(const struct qstar_target *target)
{
	if (!target)
		return "finalizing";
	if (strcmp(target->kind, "staticlib") == 0)
		return "archiving";
	if (strcmp(target->kind, "executable") == 0 || strcmp(target->kind, "test") == 0)
		return "linking";
	return "finalizing";
}

/** scheduler node에 대응하는 사용자-facing progress stage 이름을 반환한다. */
static const char *
progress_stage_name(const struct qstar_sched_node *node)
{
	switch (node->kind) {
	case QSTAR_SCHED_COMPILE:
		return "compiling";
	case QSTAR_SCHED_FINAL:
		return final_stage_name(node->target);
	case QSTAR_SCHED_GENERATE:
		return "generating";
	case QSTAR_SCHED_RUN:
		return "running";
	case QSTAR_SCHED_GROUP:
		return "grouping";
	}
	return "working";
}

/** scheduler node의 progress label을 반환한다. */
static const char *
progress_node_label(const struct qstar_sched_node *node)
{
	if (node->target)
		return node->target->label;
	if (node->genrule)
		return node->genrule->label;
	return "<action>";
}

/** progress tick 한 줄을 출력한다. */
static void
progress_tick(struct qstar_build_ctx *ctx, int pct, const char *stage,
    const char *label)
{
	if (!ctx->progress_enabled || ctx->quiet)
		return;
	ctx->progress_stage++;
	fprintf(ctx->out, "%s[%d%%]%s Stage %d: %s %s\n",
	    build_color(ctx, "stage"), pct, build_color_reset(ctx),
	    ctx->progress_stage, stage, label && *label ? label : ctx->root_label);
}

/** verbose build status 한 줄을 출력한다. */
static void
progress_status(struct qstar_build_ctx *ctx, const char *stage, const char *label)
{
	if (!ctx->progress_enabled || ctx->quiet || !ctx->verbose)
		return;
	fprintf(ctx->out, "Status: %s %s\n", stage,
	    label && *label ? label : ctx->root_label);
}

/** action DAG progress를 초기화한다. */
static void
progress_begin(struct qstar_build_ctx *ctx, size_t total)
{
	ctx->progress_total = total ? total : 1;
	ctx->progress_next_tick = 5;
	ctx->progress_stage = 0;
	progress_tick(ctx, 5, "prepare", ctx->root_label);
	ctx->progress_next_tick = 10;
}

/** node 완료 수를 5% progress tick으로 변환한다. */
static void
progress_complete(struct qstar_build_ctx *ctx, const struct qstar_sched_node *node,
    size_t completed)
{
	int pct;
	const char *stage, *label;

	if (!ctx->progress_enabled || ctx->quiet)
		return;
	if (ctx->progress_total == 0)
		ctx->progress_total = 1;
	pct = (int)((completed * 100) / ctx->progress_total);
	if (pct > 100)
		pct = 100;
	pct = (pct / 5) * 5;
	stage = progress_stage_name(node);
	label = progress_node_label(node);
	if (pct >= ctx->progress_next_tick && pct <= 100) {
		progress_tick(ctx, pct, stage, label);
		ctx->progress_next_tick = pct + 5;
	}
}

/** build 종료 시 100% tick을 보장한다. */
static void
progress_finish(struct qstar_build_ctx *ctx, int success)
{
	if (!ctx->progress_enabled || ctx->quiet)
		return;
	if (success) {
		if (ctx->progress_next_tick <= 100)
			progress_tick(ctx, 100, "complete", ctx->root_label);
	} else {
		progress_status(ctx, "failed", ctx->root_label);
	}
}

static int run_one_generated_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule);
static int build_target(struct qstar_graph *graph, const struct qstar_target *target,
    size_t order, void *user);

/** child process polling 사이의 지연을 짧게 유지해 작은 action 지연을 줄인다. */
static void
wait_poll_pause(void)
{
	struct timespec delay;

	delay.tv_sec = 0;
	delay.tv_nsec = 1000000L;
	while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
		;
}

/** 명시적 --jobs가 없을 때 host CPU 수를 기준으로 기본 병렬도를 정한다. */
static int
default_job_count(void)
{
#ifdef _SC_NPROCESSORS_ONLN
	long n;

	n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n > 1) {
		if (n > 256)
			return 256;
		return (int)n;
	}
#endif
	return 1;
}

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

/** build directory 아래 상대 path를 package root 기준 절대 실행 path로 바꾼다. */
static int
full_path_under_build(const struct qstar_graph *graph, const char *subpath, char *dst,
    size_t dstlen)
{
	char rel[QSTAR_PATH_MAX];

	if (qstar_graph_build_path(graph, subpath, rel, sizeof(rel)) < 0)
		return -1;
	return full_path_under_root(graph, rel, dst, dstlen);
}

/** build/logs directory를 build context에 cache하고 한 번만 생성한다. */
static int
ensure_action_log_dir(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    char *dst, size_t dstlen)
{
	if (!ctx->logdir_ready) {
		if (full_path_under_build(graph, "logs", ctx->logdir_full,
		    sizeof(ctx->logdir_full)) < 0 ||
		    mkdir_p(ctx->logdir_full) < 0)
			return qstar_set_error(graph, "qstar: could not create action log dir");
		ctx->logdir_ready = 1;
	}
	if (snprintf(dst, dstlen, "%s", ctx->logdir_full) >= (int)dstlen)
		return qstar_set_error(graph, "qstar: action log dir path too long");
	return 0;
}

/** build directory 아래 파일 path의 parent directory를 만든다. */
static int
mkdir_parent_under_build(const struct qstar_graph *graph, const char *subpath)
{
	char rel[QSTAR_PATH_MAX];

	if (qstar_graph_build_path(graph, subpath, rel, sizeof(rel)) < 0)
		return -1;
	return mkdir_parent_under_root(graph, rel);
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

/** package-relative compiler path 또는 PATH command가 실행 가능한지 확인한다. */
static int
command_exists_in_graph(const struct qstar_graph *graph, const char *cmd)
{
	char full[QSTAR_PATH_MAX];

	if (!cmd || !*cmd)
		return 0;
	if (strchr(cmd, '/') && cmd[0] != '/' && qstar_path_is_package_relative(cmd)) {
		if (full_path_under_root(graph, cmd, full, sizeof(full)) < 0)
			return 0;
		return access(full, X_OK) == 0;
	}
	return command_exists(cmd);
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

static void
format_key(unsigned long long h, char *dst, size_t dstlen);

/** 이번 build invocation에서 이미 계산한 file digest를 찾는다. */
static const char *
hash_cache_find(const struct qstar_build_ctx *ctx, const char *rel)
{
	size_t i;

	if (!ctx || !rel)
		return NULL;
	for (i = 0; i < ctx->hash_cache_len; i++) {
		if (strcmp(ctx->hash_cache[i].rel, rel) == 0)
			return ctx->hash_cache[i].digest;
	}
	return NULL;
}

/** file digest cache에 새 항목을 추가한다. 메모리 부족이면 cache만 포기한다. */
static void
hash_cache_add(struct qstar_build_ctx *ctx, const char *rel, const char *digest)
{
	struct qstar_file_hash_entry *p;
	size_t ncap;

	if (!ctx || !rel || !digest)
		return;
	if (ctx->hash_cache_len == ctx->hash_cache_cap) {
		ncap = ctx->hash_cache_cap ? ctx->hash_cache_cap * 2 : 64;
		p = realloc(ctx->hash_cache, ncap * sizeof(ctx->hash_cache[0]));
		if (!p)
			return;
		ctx->hash_cache = p;
		ctx->hash_cache_cap = ncap;
	}
	p = &ctx->hash_cache[ctx->hash_cache_len];
	p->rel = qstar_strdup(rel);
	if (!p->rel)
		return;
	snprintf(p->digest, sizeof(p->digest), "%s", digest);
	ctx->hash_cache_len++;
}

/** action key에 file metadata와 content digest를 memoized 형태로 섞는다. */
static void
hash_file(struct qstar_build_ctx *ctx, unsigned long long *h,
    const struct qstar_graph *graph, const char *rel)
{
	char path[QSTAR_PATH_MAX];
	unsigned char buf[4096];
	struct stat st;
	FILE *f;
	size_t n;
	char meta[128], digest[32];
	unsigned long long file_h;
	const char *cached;

	cached = hash_cache_find(ctx, rel);
	if (cached) {
		hash_str(h, cached);
		return;
	}
	file_h = QSTAR_HASH_INIT;
	hash_str(&file_h, rel);
	if (full_path_under_root(graph, rel, path, sizeof(path)) < 0 || stat(path, &st) < 0) {
		hash_str(&file_h, "<missing>");
		format_key(file_h, digest, sizeof(digest));
		hash_cache_add(ctx, rel, digest);
		hash_str(h, digest);
		return;
	}
	snprintf(meta, sizeof(meta), "size=%lld mtime=%lld", (long long)st.st_size,
	    (long long)st.st_mtime);
	hash_str(&file_h, meta);
	f = fopen(path, "rb");
	if (!f) {
		hash_str(&file_h, "<unreadable>");
		format_key(file_h, digest, sizeof(digest));
		hash_cache_add(ctx, rel, digest);
		hash_str(h, digest);
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		size_t i;
		for (i = 0; i < n; i++) {
			file_h ^= (unsigned long long)buf[i];
			file_h *= QSTAR_HASH_PRIME;
		}
	}
	fclose(f);
	format_key(file_h, digest, sizeof(digest));
	hash_cache_add(ctx, rel, digest);
	hash_str(h, digest);
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
compute_input_key(struct qstar_build_ctx *ctx, struct qstar_graph *graph,
    const struct qstar_string_list *inputs, char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	if (inputs) {
		for (i = 0; i < inputs->len; i++)
			hash_file(ctx, &h, graph, inputs->items[i]);
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
	for (i = 0; i < graph->profile.compile_options.len; i++)
		hash_str(&h, graph->profile.compile_options.items[i]);
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

/** output identity/path만 별도 digest로 만든다. */
static void
compute_output_key(const char *output, char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;

	hash_str(&h, output ? output : "");
	format_key(h, dst, dstlen);
}

/** external tool 선택만 별도 digest로 만든다. */
static void
compute_external_tool_key(struct qstar_graph *graph,
    const struct qstar_resolved_toolchain *toolchain, char *const argv[],
    char *dst, size_t dstlen)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	hash_str(&h, argv && argv[0] ? argv[0] : "");
	for (i = 0; i < graph->profile.path_tools.len; i++)
		hash_str(&h, graph->profile.path_tools.items[i]);
	for (i = 0; i < graph->profile.tool_overrides.len; i++)
		hash_str(&h, graph->profile.tool_overrides.items[i]);
	if (toolchain) {
		hash_str(&h, toolchain->cc);
		hash_str(&h, toolchain->cxx);
		hash_str(&h, toolchain->cale);
		hash_str(&h, toolchain->ar);
		hash_str(&h, toolchain->linker);
	}
	format_key(h, dst, dstlen);
}

/** action key 공통 material을 계산한다. */
static void
compute_action_key(struct qstar_build_ctx *ctx, struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const char *id, const char *kind, char *const argv[], const struct qstar_string_list *inputs,
    const struct qstar_string_list *depfile_inputs, const char *output, char *dst,
    size_t dstlen, struct qstar_action_material *material)
{
	unsigned long long h = QSTAR_HASH_INIT;
	size_t i;

	if (material) {
		compute_argv_key(argv, material->argv_key, sizeof(material->argv_key));
		compute_env_key(material->env_key, sizeof(material->env_key));
		compute_input_key(ctx, graph, inputs, material->input_key,
		    sizeof(material->input_key));
		compute_input_key(ctx, graph, depfile_inputs, material->depfile_key,
		    sizeof(material->depfile_key));
		compute_profile_key(graph, toolchain, material->profile_key,
		    sizeof(material->profile_key));
		compute_output_key(output, material->output_key,
		    sizeof(material->output_key));
		compute_external_tool_key(graph, toolchain, argv,
		    material->external_tool_key,
		    sizeof(material->external_tool_key));
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
	for (i = 0; i < graph->profile.compile_options.len; i++)
		hash_str(&h, graph->profile.compile_options.items[i]);
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
			hash_file(ctx, &h, graph, inputs->items[i]);
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
	char dir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];
	char rel[QSTAR_PATH_MAX];
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
	if (full_path_under_build(graph, "rsp", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0)
		return qstar_set_error(graph, "qstar: could not create response file dir");
	action_log_name(id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "rsp/%s.rsp", name) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0 ||
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
write_action_log_text(const char *path, char *const argv[], const char *exit_text)
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "qstar-action-log v2\nexit=%s\n", exit_text);
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

/** action log argv snapshot을 batch flush용으로 복사한다. */
static char **
action_log_copy_argv(char *const argv[], size_t *argc_out)
{
	char **copy;
	size_t argc, i;

	for (argc = 0; argv[argc]; argc++)
		;
	copy = calloc(argc + 1, sizeof(copy[0]));
	if (!copy)
		return NULL;
	for (i = 0; i < argc; i++) {
		copy[i] = qstar_strdup(argv[i]);
		if (!copy[i]) {
			while (i > 0)
				free(copy[--i]);
			free(copy);
			return NULL;
		}
	}
	copy[argc] = NULL;
	*argc_out = argc;
	return copy;
}

/** action log 하나를 build 종료 시점의 batch flush 목록에 추가한다. */
static void
action_log_queue(struct qstar_build_ctx *ctx, const char *path, char *const argv[],
    const char *exit_text)
{
	struct qstar_action_log_entry *p;
	size_t ncap, argc = 0;
	char **argv_copy;
	char *path_copy, *exit_copy;

	if (!ctx || !path || !argv || !exit_text)
		return;
	path_copy = qstar_strdup(path);
	exit_copy = qstar_strdup(exit_text);
	argv_copy = action_log_copy_argv(argv, &argc);
	if (!path_copy || !exit_copy || !argv_copy) {
		free(path_copy);
		free(exit_copy);
		if (argv_copy) {
			size_t i;

			for (i = 0; i < argc; i++)
				free(argv_copy[i]);
			free(argv_copy);
		}
		return;
	}
	if (ctx->action_log_len == ctx->action_log_cap) {
		ncap = ctx->action_log_cap ? ctx->action_log_cap * 2 : 64;
		p = realloc(ctx->action_logs, ncap * sizeof(ctx->action_logs[0]));
		if (!p) {
			size_t i;

			for (i = 0; i < argc; i++)
				free(argv_copy[i]);
			free(argv_copy);
			free(path_copy);
			free(exit_copy);
			return;
		}
		ctx->action_logs = p;
		ctx->action_log_cap = ncap;
	}
	p = &ctx->action_logs[ctx->action_log_len++];
	p->path = path_copy;
	p->exit_text = exit_copy;
	p->argv = argv_copy;
	p->argc = argc;
	p->owns_argv = 1;
}

/** 수명이 build flush까지 보장되는 argv를 복사 없이 action log queue에 추가한다. */
static void
action_log_queue_ref(struct qstar_build_ctx *ctx, const char *path, char *const argv[],
    const char *exit_text)
{
	struct qstar_action_log_entry *p;
	size_t ncap, argc;
	char *path_copy, *exit_copy;

	if (!ctx || !path || !argv || !exit_text)
		return;
	for (argc = 0; argv[argc]; argc++)
		;
	path_copy = qstar_strdup(path);
	exit_copy = qstar_strdup(exit_text);
	if (!path_copy || !exit_copy) {
		free(path_copy);
		free(exit_copy);
		return;
	}
	if (ctx->action_log_len == ctx->action_log_cap) {
		ncap = ctx->action_log_cap ? ctx->action_log_cap * 2 : 64;
		p = realloc(ctx->action_logs, ncap * sizeof(ctx->action_logs[0]));
		if (!p) {
			free(path_copy);
			free(exit_copy);
			return;
		}
		ctx->action_logs = p;
		ctx->action_log_cap = ncap;
	}
	p = &ctx->action_logs[ctx->action_log_len++];
	p->path = path_copy;
	p->exit_text = exit_copy;
	p->argv = (char **)argv;
	p->argc = argc;
	p->owns_argv = 0;
}

/** integer exit code action log를 batch flush 목록에 추가한다. */
static void
action_log_queue_exit(struct qstar_build_ctx *ctx, const char *path, char *const argv[],
    int exit_code)
{
	char exit_text[32];

	snprintf(exit_text, sizeof(exit_text), "%d", exit_code);
	action_log_queue(ctx, path, argv, exit_text);
}

/** skip action log를 batch flush 목록에 추가한다. */
static void
action_log_queue_skip(struct qstar_build_ctx *ctx, const char *path, char *const argv[])
{
	action_log_queue(ctx, path, argv, "skip");
}

/** compile action처럼 argv 수명이 보장되는 action log를 복사 없이 추가한다. */
static void
action_log_queue_exit_ref(struct qstar_build_ctx *ctx, const char *path,
    char *const argv[], int exit_code)
{
	char exit_text[32];

	snprintf(exit_text, sizeof(exit_text), "%d", exit_code);
	action_log_queue_ref(ctx, path, argv, exit_text);
}

/** compile cache-hit skip log를 복사 없이 batch flush 목록에 추가한다. */
static void
action_log_queue_skip_ref(struct qstar_build_ctx *ctx, const char *path,
    char *const argv[])
{
	action_log_queue_ref(ctx, path, argv, "skip");
}

static void
action_log_queue_free(struct qstar_action_log_entry *entries, size_t len);

/** batch로 모은 action log 파일을 한 번에 flush한다. */
static void
action_log_flush(struct qstar_build_ctx *ctx)
{
	size_t i;

	for (i = 0; i < ctx->action_log_len; i++)
		write_action_log_text(ctx->action_logs[i].path,
		    ctx->action_logs[i].argv, ctx->action_logs[i].exit_text);
	action_log_queue_free(ctx->action_logs, ctx->action_log_len);
	ctx->action_logs = NULL;
	ctx->action_log_len = 0;
	ctx->action_log_cap = 0;
}

/** 실패한 action을 직접 재현할 수 있는 argv 파일을 상세 metadata와 함께 갱신한다. */
static void
write_failure_replay_detail(const struct qstar_graph *graph, const char *id,
    const struct qstar_resolved_toolchain *toolchain, char *const argv[],
    const char *failure_kind, const char *label, const char *stdout_rel,
    const char *stderr_rel, const char *marker, const char *marker_log_rel)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;
	unsigned long long digest;

	if (full_path_under_build(graph, "logs/last-failure.replay", path,
	    sizeof(path)) < 0)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# qstar failure replay v2\ncd %s\n",
	    graph->package_root ? graph->package_root : ".");
	fprintf(f, "failure_kind=%s\n", failure_kind && *failure_kind ? failure_kind :
	    "action-failure");
	fprintf(f, "label=%s\n", label && *label ? label : id);
	fprintf(f, "stdout=%s\n", stdout_rel && *stdout_rel ? stdout_rel : "<none>");
	fprintf(f, "stderr=%s\n", stderr_rel && *stderr_rel ? stderr_rel : "<none>");
	fprintf(f, "marker=%s\n", marker && *marker ? marker : "<none>");
	fprintf(f, "marker_log=%s\n", marker_log_rel && *marker_log_rel ? marker_log_rel :
	    "<none>");
	digest = QSTAR_HASH_INIT;
	for (i = 0; argv[i]; i++)
		hash_str(&digest, argv[i]);
	fprintf(f, "argv_digest=%016llx\n", digest);
	if (toolchain && toolchain->response_files && argv_needs_response_file(argv)) {
		char name[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
		const char *style = toolchain->response_style[0] ?
		    toolchain->response_style : "posix";
		action_log_name(id, name, sizeof(name));
		char sub[QSTAR_PATH_MAX];

		snprintf(sub, sizeof(sub), "rsp/%s.rsp", name);
		if (qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0)
			snprintf(rel, sizeof(rel), "%s/rsp/%s.rsp",
			    qstar_graph_build_dir(graph), name);
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

/** 기존 action failure 경로가 쓰는 replay writer wrapper다. */
static void
write_failure_replay(const struct qstar_graph *graph, const char *id,
    const struct qstar_resolved_toolchain *toolchain, char *const argv[])
{
	write_failure_replay_detail(graph, id, toolchain, argv, "action-failure", id,
	    NULL, NULL, NULL, NULL);
}

/** stage/package failure를 last-failure replay 파일에 기록한다. */
static void
write_stage_failure_replay(const struct qstar_graph *graph, const struct qstar_stage *stage)
{
	char id[QSTAR_PATH_MAX];
	char *argv[6];
	size_t argc;

	snprintf(id, sizeof(id), "%s:stage:0", stage->label);
	argc = 0;
	argv[argc++] = "qstar";
	if (strcmp(qstar_graph_generator(graph), "ninja") == 0) {
		argv[argc++] = "-G";
		argv[argc++] = "ninja";
	}
	argv[argc++] = "stage";
	argv[argc++] = (char *)stage->label;
	argv[argc] = NULL;
	write_failure_replay_detail(graph, id, NULL, argv, "package-failure",
	    stage->label, "<none>", "<none>", NULL, NULL);
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
	p->output_key = qstar_strdup(material ? material->output_key : "");
	p->external_tool_key = qstar_strdup(material ? material->external_tool_key : "");
	return p->id && p->key && p->output && p->status && p->kind &&
	    p->argv_key && p->env_key && p->input_key && p->depfile_key &&
	    p->profile_key && p->output_key && p->external_tool_key ? 0 : -1;
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
		free(entries[i].output_key);
		free(entries[i].external_tool_key);
	}
	free(entries);
}

/** build-local file digest cache를 해제한다. */
static void
hash_cache_free(struct qstar_file_hash_entry *entries, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		free(entries[i].rel);
	free(entries);
}

/** batch action log queue를 해제한다. */
static void
action_log_queue_free(struct qstar_action_log_entry *entries, size_t len)
{
	size_t i, j;

	for (i = 0; i < len; i++) {
		free(entries[i].path);
		free(entries[i].exit_text);
		if (entries[i].argv && entries[i].owns_argv) {
			for (j = 0; j < entries[i].argc; j++)
				free(entries[i].argv[j]);
			free(entries[i].argv);
		}
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

/** action state 문자열 필드를 NULL-safe 방식으로 비교한다. */
static int
state_field_equal(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") == 0;
}

/** status 외 material이 이전 state와 같으면 no-op build state write를 생략할 수 있다. */
static int
state_unchanged_ignoring_status(const struct qstar_build_ctx *ctx)
{
	const struct qstar_state_entry *prev;
	const struct qstar_state_entry *next;
	size_t i;

	if (ctx->prev_len != ctx->next_len)
		return 0;
	for (i = 0; i < ctx->next_len; i++) {
		next = &ctx->next[i];
		prev = state_find(ctx, next->id);
		if (!prev ||
		    !state_field_equal(prev->key, next->key) ||
		    !state_field_equal(prev->output, next->output) ||
		    !state_field_equal(prev->kind, next->kind) ||
		    !state_field_equal(prev->argv_key, next->argv_key) ||
		    !state_field_equal(prev->env_key, next->env_key) ||
		    !state_field_equal(prev->input_key, next->input_key) ||
		    !state_field_equal(prev->depfile_key, next->depfile_key) ||
		    !state_field_equal(prev->profile_key, next->profile_key) ||
		    !state_field_equal(prev->output_key, next->output_key) ||
		    !state_field_equal(prev->external_tool_key, next->external_tool_key))
			return 0;
	}
	return 1;
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
		REPLACE_FIELD(output_key, material->output_key);
		REPLACE_FIELD(external_tool_key, material->external_tool_key);
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

	if (full_path_under_build(graph, "state/actions.json", path, sizeof(path)) < 0)
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
		json_field_copy(line, "output_key", material.output_key,
		    sizeof(material.output_key));
		json_field_copy(line, "external_tool_key", material.external_tool_key,
		    sizeof(material.external_tool_key));
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

	if (full_path_under_build(graph, "state/actions.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: could not create state dir");
	if (state_unchanged_ignoring_status(ctx) && path_exists(path))
		return 0;
	if (full_path_under_build(graph, "state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0)
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
		fputs(",\"output_key\":", f);
		json_string(f, ctx->next[i].output_key);
		fputs(",\"external_tool_key\":", f);
		json_string(f, ctx->next[i].external_tool_key);
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

	if (full_path_under_build(graph, "state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_build(graph, "state/graph.json", path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: could not create graph snapshot dir");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write graph snapshot");
	fputs("{\"schema\":\"qstar-graph-snapshot-v1\",\"package_root\":", f);
	json_string(f, graph->package_root ? graph->package_root : ".");
	fputs(",\"project\":{\"name\":", f);
	json_string(f, graph->project.name ? graph->project.name : "");
	fputs(",\"version\":", f);
	json_string(f, graph->project.version ? graph->project.version : "");
	fputs(",\"root\":", f);
	json_string(f, graph->project.root ? graph->project.root : ".");
	fputs(",\"build_dir\":", f);
	json_string(f, qstar_graph_build_dir(graph));
	fputs(",\"generated_dir\":", f);
	json_string(f, qstar_graph_generated_dir(graph));
	fputs(",\"compile_commands\":", f);
	json_string(f, qstar_graph_compile_commands_policy(graph));
	fputs(",\"generator\":", f);
	json_string(f, qstar_graph_generator(graph));
	fputs(",\"requested_generator\":", f);
	json_string(f, qstar_graph_requested_generator(graph));
	fputc('}', f);
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
		fputs(",\"configs\":", f);
		json_string_list(f, &graph->targets[i].configs);
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
	fputs("\n],\"configs\":[\n", f);
	for (i = 0; i < graph->config_len; i++) {
		if (i)
			fputs(",\n", f);
		fputs("  {\"label\":", f);
		json_string(f, graph->configs[i].label);
		fputs(",\"origin\":", f);
		json_string(f, graph->configs[i].origin_file);
		fprintf(f, ",\"line\":%d}", graph->configs[i].origin_line);
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

	if (full_path_under_build(graph, "state", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_build(graph, "state/last-summary.json", path,
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

/** compile_commands policy에 따라 output path를 계산한다. */
static int
compile_db_path(struct qstar_graph *graph, char *path, size_t pathlen)
{
	const char *policy;

	policy = qstar_graph_compile_commands_policy(graph);
	if (strcmp(policy, "off") == 0) {
		path[0] = '\0';
		return 0;
	}
	if (strcmp(policy, "root") == 0)
		return full_path_under_root(graph, "compile_commands.json", path, pathlen);
	return full_path_under_build(graph, "compile_commands.json", path, pathlen);
}

/** build에서 수집한 compile_commands.json을 project policy에 따라 쓴다. */
static int
compile_db_write(struct qstar_graph *graph, const struct qstar_build_ctx *ctx)
{
	char path[QSTAR_PATH_MAX], tmp[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (compile_db_path(graph, path, sizeof(path)) < 0)
		return qstar_set_error(graph, "qstar: compile_commands path too long");
	if (!path[0])
		return 0;
	if (strcmp(qstar_graph_compile_commands_policy(graph), "root") != 0) {
		char dir[QSTAR_PATH_MAX];

		if (full_path_under_build(graph, "", dir, sizeof(dir)) < 0 ||
		    mkdir_p(dir) < 0)
			return qstar_set_error(graph,
			    "qstar: could not create compile_commands dir");
	}
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
		if (prev->output_key && *prev->output_key &&
		    strcmp(prev->output_key, material->output_key) != 0)
			return "output-changed";
		if (prev->external_tool_key && *prev->external_tool_key &&
		    strcmp(prev->external_tool_key, material->external_tool_key) != 0)
			return "external-tool-changed";
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

/** argv에 특정 문자열이 포함되어 있는지 확인한다. */
static int
argv_contains(char *const argv[], const char *needle)
{
	size_t i;

	if (!needle || !*needle)
		return 0;
	for (i = 0; argv && argv[i]; i++) {
		if (strstr(argv[i], needle))
			return 1;
	}
	return 0;
}

/** run_target이 QEMU wrapper 성격인지 label/argv 기준으로 판별한다. */
static int
action_is_qemu_run(const struct qstar_target *target, char *const argv[])
{
	return (target && strstr(target->label, "qemu")) || argv_contains(argv, "qemu");
}

/** action id에서 사용자가 작성한 owner label을 복원한다. */
static const char *
action_owner_label(const struct qstar_target *target, const char *id, char *dst,
    size_t dstlen)
{
	const char *suffix;
	size_t n;

	if (target)
		return target->label;
	suffix = strstr(id, ":generate:");
	if (!suffix)
		suffix = strstr(id, ":stage:");
	if (!suffix)
		return id;
	n = (size_t)(suffix - id);
	if (n >= dstlen)
		n = dstlen - 1;
	memcpy(dst, id, n);
	dst[n] = '\0';
	return dst;
}

/** 실패한 executor action을 debug UX용 stable kind로 분류한다. */
static const char *
classify_failure_kind(const char *kind, const struct qstar_target *target,
    char *const argv[], const char *base)
{
	if (strcmp(kind, "link") == 0)
		return "link-failure";
	if (strcmp(kind, "archive") == 0)
		return "archive-failure";
	if (strcmp(kind, "generate") == 0 && argv_contains(argv, "objcopy"))
		return "objcopy-failure";
	if (strcmp(kind, "generate") == 0)
		return "generate-failure";
	if (strcmp(kind, "run") == 0 && base && strcmp(base, "timeout") == 0 &&
	    action_is_qemu_run(target, argv))
		return "qemu-timeout";
	if (base && *base)
		return base;
	return "action-failure";
}

/** 실패 action을 JSONL 친화적인 stdout diagnostic으로 남긴다. */
static void
emit_action_diagnostic(FILE *out, const char *id, const char *kind, const char *label,
    const char *failure_kind, const char *status, int exit_code,
    const char *stdout_rel, const char *stderr_rel, const char *replay_rel)
{
	fputs("action_diagnostic_json ", out);
	fputs("{\"schema\":\"qstar-action-diagnostic-v1\",\"id\":", out);
	json_string(out, id);
	fputs(",\"kind\":", out);
	json_string(out, kind);
	fputs(",\"label\":", out);
	json_string(out, label && *label ? label : id);
	fputs(",\"failure_kind\":", out);
	json_string(out, failure_kind);
	fputs(",\"status\":", out);
	json_string(out, status);
	fprintf(out, ",\"exit\":%d,\"stdout\":", exit_code);
	json_string(out, stdout_rel && *stdout_rel ? stdout_rel : "<none>");
	fputs(",\"stderr\":", out);
	json_string(out, stderr_rel && *stderr_rel ? stderr_rel : "<none>");
	fputs(",\"replay\":", out);
	json_string(out, replay_rel && *replay_rel ? replay_rel :
	    "build/qstar/logs/last-failure.replay");
	fputs("}\n", out);
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

/** build log 상대 path를 계산한다. */
static int
build_log_rel(const struct qstar_graph *graph, const char *name, const char *suffix,
    char *dst, size_t dstlen)
{
	char sub[QSTAR_PATH_MAX];
	int n;

	n = snprintf(sub, sizeof(sub), "logs/%s%s", name, suffix);
	return n >= 0 && (size_t)n < sizeof(sub) ?
	    qstar_graph_build_path(graph, sub, dst, dstlen) : -1;
}

/** last-failure replay 상대 path를 계산한다. */
static int
build_replay_rel(const struct qstar_graph *graph, char *dst, size_t dstlen)
{
	return qstar_graph_build_path(graph, "logs/last-failure.replay", dst, dstlen);
}

/** stdout/stderr를 log 파일에 연결해 package root 안에서 argv를 실행하거나 cache skip한다. */
static int
run_action(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target, const char *id, const char *kind, const char *key,
    const struct qstar_string_list *outputs, char *const argv[],
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_action_material *material)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	char replay_path[QSTAR_PATH_MAX];
	char rsp_arg[QSTAR_PATH_MAX];
	char *exec_argv[3];
	char *const *child_argv;
	const struct qstar_state_entry *prev;
	pid_t pid;
	int status, fdout, fderr, exit_code, use_rsp;

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
		if (build_trace_enabled(ctx)) {
			if (ensure_action_log_dir(graph, ctx, logdir, sizeof(logdir)) < 0)
				return -1;
			action_log_name(id, name, sizeof(name));
			snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir,
			    name);
			if (build_log_rel(graph, name, ".stdout", child_stdout_path,
			    sizeof(child_stdout_path)) < 0 ||
			    build_log_rel(graph, name, ".stderr", child_stderr_path,
			    sizeof(child_stderr_path)) < 0)
				return qstar_set_error(graph,
				    "qstar: action log path too long");
			build_tracef(ctx,
			    "build_action id=%s status=skip reason=cache-hit stdout=%s stderr=%s\n",
			    id, child_stdout_path, child_stderr_path);
			action_log_queue_skip(ctx, action_log, argv);
		}
		ctx->skip_count++;
		return state_push(ctx, 1, id, key, outputs->items[0], "skip", kind,
		    material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	if (ensure_action_log_dir(graph, ctx, logdir, sizeof(logdir)) < 0)
		return -1;
	action_log_name(id, name, sizeof(name));
	snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
	if (build_log_rel(graph, name, ".stdout", child_stdout_path,
	    sizeof(child_stdout_path)) < 0 ||
	    build_log_rel(graph, name, ".stderr", child_stderr_path,
	    sizeof(child_stderr_path)) < 0 ||
	    build_replay_rel(graph, replay_path, sizeof(replay_path)) < 0)
		return qstar_set_error(graph, "qstar: action log path too long");
	build_tracef(ctx,
	    "build_action id=%s status=run timeout_sec=%d stdout=%s stderr=%s\n",
	    id, ctx->action_timeout_sec, child_stdout_path, child_stderr_path);
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

		if (strcmp(kind, "archive") == 0 || strcmp(kind, "link") == 0) {
			if (waitpid(pid, &status, 0) < 0)
				return qstar_set_error(graph, "qstar: waitpid failed");
		} else {
			start = time(NULL);
			for (;;) {
				waited = waitpid(pid, &status, WNOHANG);
				if (waited == pid)
					break;
				if (waited < 0)
					return qstar_set_error(graph, "qstar: waitpid failed");
				if (time(NULL) - start >= ctx->action_timeout_sec) {
					char label_buf[QSTAR_PATH_MAX];
					const char *diag_label;
					const char *failure_kind;

					failure_kind = classify_failure_kind(kind, target, argv,
					    "timeout");
					diag_label = action_owner_label(target, id, label_buf,
					    sizeof(label_buf));
					kill(pid, SIGKILL);
					waitpid(pid, &status, 0);
					action_log_queue_exit(ctx, action_log, argv, 124);
					write_failure_replay_detail(graph, id, toolchain, argv,
					    failure_kind, diag_label,
					    child_stdout_path, child_stderr_path,
					    target ? target->run_marker : NULL,
					    target ? target->run_marker_log : NULL);
					ctx->fail_count++;
					ctx->cancelled = 1;
					build_tracef(ctx,
					    "build_action id=%s status=timeout timeout_sec=%d\n",
					    id, ctx->action_timeout_sec);
					emit_action_diagnostic(ctx->out, id, kind,
					    diag_label, failure_kind, "timeout",
					    124, child_stdout_path, child_stderr_path,
					    replay_path);
					if (strcmp(kind, "run") == 0) {
						fprintf(ctx->out,
						    "run_target_result label=%s status=timeout timeout_sec=%d replay=%s stdout=%s stderr=%s\n",
						    target ? target->label : id,
						    ctx->action_timeout_sec,
						    replay_path, child_stdout_path,
						    child_stderr_path);
						return qstar_set_error_origin(graph,
						    target ? target->origin_file : "",
						    target ? target->origin_line : 0, failure_kind,
						    target ? target->label : id,
						    "qstar: run_target '%s' timed out after %d seconds; replay=%s",
						    target ? target->label : id,
						    ctx->action_timeout_sec, replay_path);
					}
					return qstar_set_error_origin(graph,
					    target ? target->origin_file : "",
					    target ? target->origin_line : 0, failure_kind,
					    target ? target->label : id,
					    "qstar: action '%s' timed out after %d seconds; replay=%s",
					    id, ctx->action_timeout_sec, replay_path);
				}
				wait_poll_pause();
			}
		}
	}
	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	action_log_queue_exit(ctx, action_log, argv, exit_code);
	if (exit_code != 0) {
		char label_buf[QSTAR_PATH_MAX];
		const char *diag_label;
		const char *failure_kind;

		failure_kind = classify_failure_kind(kind, target, argv, "exit-code");
		diag_label = action_owner_label(target, id, label_buf, sizeof(label_buf));
		build_tracef(ctx, "build_action id=%s status=fail exit=%d\n", id, exit_code);
		write_failure_replay_detail(graph, id, toolchain, argv,
		    failure_kind, diag_label, child_stdout_path,
		    child_stderr_path, target ? target->run_marker : NULL,
		    target ? target->run_marker_log : NULL);
		emit_action_diagnostic(ctx->out, id, kind, diag_label,
		    failure_kind, "fail", exit_code, child_stdout_path, child_stderr_path,
		    replay_path);
		ctx->fail_count++;
		ctx->cancelled = 1;
		if (strcmp(kind, "run") == 0) {
			fprintf(ctx->out,
			    "run_target_result label=%s status=exit-code exit=%d replay=%s stdout=%s stderr=%s\n",
			    target ? target->label : id, exit_code, replay_path,
			    child_stdout_path, child_stderr_path);
			return qstar_set_error_origin(graph, target ? target->origin_file : "",
			    target ? target->origin_line : 0, failure_kind,
			    target ? target->label : id,
			    "qstar: run_target '%s' failed with exit code %d; replay=%s",
			    target ? target->label : id, exit_code, replay_path);
		}
		return qstar_set_error_origin(graph, target ? target->origin_file : "",
		    target ? target->origin_line : 0, failure_kind, diag_label,
		    "qstar: action '%s' failed with status %d; replay=%s",
		    id, exit_code, replay_path);
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

/** 임시 string list에 문자열이 이미 있는지 확인한다. */
static int
tmp_list_contains(const struct qstar_string_list *list, const char *s)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], s) == 0)
			return 1;
	}
	return 0;
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

static int validate_compile_source(struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_source_info *source, size_t index);

/** source registry 기준으로 compile action이 필요한 입력인지 확인한다. */
static int
source_requires_compile(const struct qstar_source_info *source)
{
	return source->compile_input;
}

/** 이미 만들어진 object 파일을 final archive/link 입력으로 직접 소비하는지 확인한다. */
static int
source_is_link_object(const struct qstar_source_info *source)
{
	return strcmp(source->language, "object") == 0;
}

/** target source가 final action에 제공하는 object path를 계산한다. */
static int
target_source_object_input(struct qstar_graph *graph, const struct qstar_target *target,
    size_t index, char *dst, size_t dstlen)
{
	struct qstar_source_info source;

	if (qstar_source_classify(target->sources.items[index], &source) < 0)
		return qstar_set_error(graph, "qstar: unsupported source '%s'",
		    target->sources.items[index]);
	if (source_is_link_object(&source)) {
		return snprintf(dst, dstlen, "%s", target->sources.items[index]) <
		    (int)dstlen ? 0 : -1;
	}
	return qstar_graph_object_output_path(graph, target, index, dst, dstlen);
}

/** target 안에서 실제 compiler process가 필요한 source 개수를 센다. */
static size_t
target_compile_input_count(const struct qstar_target *target)
{
	struct qstar_source_info source;
	size_t i, count;

	count = 0;
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_source_classify(target->sources.items[i], &source) == 0 &&
		    source_requires_compile(&source))
			count++;
	}
	return count;
}

/** compile action을 만들지 않는 source가 link-only object인지 검증한다. */
static int
validate_noncompile_sources(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	struct qstar_source_info source;
	size_t i;

	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		if (source_requires_compile(&source) || source_is_link_object(&source))
			continue;
		if (validate_compile_source(graph, target, toolchain, &source, i) < 0)
			return -1;
	}
	return 0;
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
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	const struct qstar_state_entry *prev;

	if (ensure_action_log_dir(graph, ctx, logdir, sizeof(logdir)) < 0)
		return -1;
	action_log_name(id, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, name);
	snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
	if (build_log_rel(graph, name, ".stdout", child_stdout_path,
	    sizeof(child_stdout_path)) < 0 ||
	    build_log_rel(graph, name, ".stderr", child_stderr_path,
	    sizeof(child_stderr_path)) < 0)
		return qstar_set_error(graph, "qstar: action log path too long");
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
		build_tracef(ctx,
		    "build_action id=%s status=skip reason=cache-hit stdout=%s stderr=%s\n",
		    id, child_stdout_path, child_stderr_path);
		action_log_queue_skip(ctx, action_log, argv);
		ctx->skip_count++;
		return state_push(ctx, 1, id, key, genrule->outputs.items[0], "skip",
		    "generate", material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	build_tracef(ctx,
	    "build_action id=%s status=run timeout_sec=internal stdout=%s stderr=%s\n",
	    id, child_stdout_path, child_stderr_path);
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
	action_log_queue_exit(ctx, action_log, argv, 0);
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
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(arg, label, sizeof(label));
	if (rc == 0) {
		if (snprintf(dst, dstlen, "%s", arg) >= (int)dstlen)
			return qstar_set_error(graph, "qstar: command argv item is too long");
		return 0;
	}
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
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
	if (strcmp(target->kind, "group") == 0)
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "target_file", target->label,
		    "qstar: qstar.target_file cannot reference group target '%s' because group targets have no artifact; depend on the group directly or reference one of its artifact-producing deps",
		    label);
	if (qstar_graph_artifact_output_path(graph, target, dst, dstlen) < 0)
		return qstar_set_error(graph, "qstar: target_file artifact path is too long");
	return 0;
}

/** generated action input token을 실제 package-relative cache input으로 추가한다. */
static int
push_generated_action_input(struct qstar_graph *graph, struct qstar_string_list *inputs,
    const char *input)
{
	char resolved[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(input, (char[QSTAR_PATH_MAX]){0},
	    QSTAR_PATH_MAX);
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (resolve_target_file_token(graph, input, resolved, sizeof(resolved)) < 0)
			return -1;
		return push_unique_tmp(inputs, resolved) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	return push_unique_tmp(inputs, input) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

/** generated action argv 안의 target_file artifact를 cache input으로 추가한다. */
static int
push_target_file_argv_inputs(struct qstar_graph *graph, const struct qstar_genrule *genrule,
    struct qstar_string_list *inputs)
{
	char label[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX];
	size_t i;
	int rc;

	for (i = 0; i < genrule->args.len; i++) {
		rc = qstar_target_file_token_label(genrule->args.items[i], label, sizeof(label));
		if (rc < 0)
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "command", genrule->label,
			    "qstar: malformed target_file placeholder");
		if (rc == 0)
			continue;
		if (resolve_target_file_token(graph, genrule->args.items[i],
		    resolved, sizeof(resolved)) < 0)
			return -1;
		if (qstar_string_list_push(inputs, resolved) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

/** generated action이 참조하는 target/custom artifact producer를 먼저 빌드한다. */
static int
build_generated_artifact_dependency(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_genrule *genrule, const char *input,
    struct qstar_string_list *visited)
{
	const struct qstar_genrule *owner;
	const struct qstar_target *target;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(input, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "inputs", genrule->label,
		    "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (tmp_list_contains(visited, input))
			return 0;
		if (qstar_string_list_push(visited, input) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
		if (strcmp(label, genrule->label) == 0)
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "inputs", genrule->label,
			    "qstar: generated action '%s' cannot depend on itself",
			    genrule->label);
		target = find_target(graph, label);
		if (target)
			return qstar_graph_visit_closure(graph, label, build_target, ctx);
		owner = qstar_graph_find_genrule(graph, label);
		if (owner)
			return run_one_generated_action(graph, ctx, NULL, owner);
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "inputs", genrule->label,
		    "qstar: generated input target '%s' in '%s' is unknown",
		    label, genrule->label);
	}
	owner = qstar_graph_find_output_owner(graph, input);
	if (owner && owner != genrule) {
		if (tmp_list_contains(visited, owner->label))
			return 0;
		if (qstar_string_list_push(visited, owner->label) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
		return run_one_generated_action(graph, ctx, NULL, owner);
	}
	return 0;
}

/** generated action의 inputs와 command target_file edge를 선행 action으로 실행한다. */
static int
build_generated_artifact_dependencies(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_genrule *genrule)
{
	struct qstar_string_list visited;
	size_t i;
	int rc;

	memset(&visited, 0, sizeof(visited));
	for (i = 0; i < genrule->inputs.len; i++) {
		if (build_generated_artifact_dependency(graph, ctx, genrule,
		    genrule->inputs.items[i], &visited) < 0) {
			qstar_string_list_free(&visited);
			return -1;
		}
	}
	for (i = 0; i < genrule->args.len; i++) {
		rc = build_generated_artifact_dependency(graph, ctx, genrule,
		    genrule->args.items[i], &visited);
		if (rc < 0) {
			qstar_string_list_free(&visited);
			return -1;
		}
	}
	qstar_string_list_free(&visited);
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
	if (!ctx->action_scheduler &&
	    build_generated_artifact_dependencies(graph, ctx, genrule) < 0)
		return -1;
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
		if (push_generated_action_input(graph, &inputs,
		    genrule->inputs.items[inputi]) < 0) {
			qstar_string_list_free(&inputs);
			free_owned_argv(argv, argc);
			return -1;
		}
	}
	if (push_target_file_argv_inputs(graph, genrule, &inputs) < 0) {
		qstar_string_list_free(&inputs);
		free_owned_argv(argv, argc);
		return -1;
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
	compute_action_key(ctx, graph, target, &toolchain, id, "generate", argv,
	    &inputs, NULL, output_identity, key, sizeof(key), &material);
	qstar_string_list_free(&inputs);
	build_tracef(ctx,
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

/** full path 파일 안에 marker 문자열이 들어 있는지 확인한다. */
static int
file_contains_marker(const char *path, const char *marker)
{
	FILE *f;
	char buf[4096];

	if (!marker || !*marker)
		return 1;
	f = fopen(path, "rb");
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

/** run_target marker를 stdout/stderr와 선택적 serial log에서 찾는다. */
static int
run_marker_match(struct qstar_graph *graph, const char *id, const char *marker,
    const char *marker_log, char *source, size_t sourcelen, char *rel, size_t rellen)
{
	char name[QSTAR_PATH_MAX], stdout_rel[QSTAR_PATH_MAX], stderr_rel[QSTAR_PATH_MAX];
	char full[QSTAR_PATH_MAX];
	const char *sources[] = { "stdout", "stderr", "marker_log" };
	const char *paths[3];
	size_t i;

	if (!marker || !*marker)
		return 1;
	action_log_name(id, name, sizeof(name));
	if (build_log_rel(graph, name, ".stdout", stdout_rel, sizeof(stdout_rel)) < 0 ||
	    build_log_rel(graph, name, ".stderr", stderr_rel, sizeof(stderr_rel)) < 0)
		return 0;
	paths[0] = stdout_rel;
	paths[1] = stderr_rel;
	paths[2] = marker_log && *marker_log ? marker_log : NULL;
	for (i = 0; i < 3; i++) {
		if (!paths[i])
			continue;
		if (full_path_under_root(graph, paths[i], full, sizeof(full)) == 0 &&
		    file_contains_marker(full, marker)) {
			snprintf(source, sourcelen, "%s", sources[i]);
			snprintf(rel, rellen, "%s", paths[i]);
			return 1;
		}
	}
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

/** marker가 없는 `true` run_target은 과거 aggregate 우회로로 보고 no-op 처리한다. */
static int
run_target_is_noop_true(const struct qstar_target *target)
{
	return target && target->run_command.len == 1 &&
	    strcmp(target->run_command.items[0], "true") == 0 &&
	    (!target->run_marker || !*target->run_marker) &&
	    (!target->run_marker_log || !*target->run_marker_log);
}

/** qstar.run_target command를 dependency build 이후 실행한다. */
static int
run_target_command(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_target *target)
{
	struct qstar_string_list inputs, outputs;
	struct qstar_action_material material;
	char *argv[QSTAR_EXEC_MAX_ARGV];
	char id[QSTAR_PATH_MAX], stamp[QSTAR_PATH_MAX], stamp_sub[QSTAR_PATH_MAX];
	char key[32], artifact[QSTAR_PATH_MAX];
	char marker_source[32], marker_path[QSTAR_PATH_MAX];
	char action_name[QSTAR_PATH_MAX], stdout_rel[QSTAR_PATH_MAX], stderr_rel[QSTAR_PATH_MAX];
	char owner[QSTAR_PATH_MAX];
	size_t argc, i;
	int old_timeout, rc;
	const struct qstar_target *dep;

	if (run_target_is_noop_true(target)) {
		build_tracef(ctx,
		    "run_target label=%s command=noop reason=true-aggregate action=none artifact=<none>\n",
		    target->label);
		return 0;
	}
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
		if (dep && strcmp(dep->kind, "group") == 0)
			continue;
		if (!dep || qstar_graph_artifact_output_path(graph, dep, artifact, sizeof(artifact)) < 0)
			continue;
		if (qstar_string_list_push(&inputs, artifact) < 0) {
			qstar_string_list_free(&inputs);
			free_owned_argv(argv, argc);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	qstar_mangle_label(target->label, owner, sizeof(owner));
	if (snprintf(stamp_sub, sizeof(stamp_sub), "out/%s/run.stamp", owner) >=
	    (int)sizeof(stamp_sub) ||
	    qstar_graph_build_path(graph, stamp_sub, stamp, sizeof(stamp)) < 0) {
		qstar_string_list_free(&inputs);
		free_owned_argv(argv, argc);
		return qstar_set_error(graph, "qstar: run stamp path too long");
	}
	if (qstar_string_list_push(&outputs, stamp) < 0) {
		qstar_string_list_free(&inputs);
		free_owned_argv(argv, argc);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	snprintf(id, sizeof(id), "%s:run:0", target->label);
	compute_action_key(ctx, graph, target, NULL, id, "run", argv, &inputs, NULL,
	    stamp, key, sizeof(key), &material);
	fprintf(ctx->out,
	    "run_target label=%s command=argv timeout_sec=%d marker=%s marker_log=%s\n",
	    target->label, target->run_timeout_sec ? target->run_timeout_sec :
	    ctx->action_timeout_sec,
	    target->run_marker && *target->run_marker ? target->run_marker : "<none>",
	    target->run_marker_log && *target->run_marker_log ? target->run_marker_log :
	    "<none>");
	old_timeout = ctx->action_timeout_sec;
	if (target->run_timeout_sec > 0)
		ctx->action_timeout_sec = target->run_timeout_sec;
	rc = run_action(graph, ctx, target, id, "run", key, &outputs, argv, NULL, &material);
	ctx->action_timeout_sec = old_timeout;
	if (rc == 0 && target->run_marker && *target->run_marker &&
	    !run_marker_match(graph, id, target->run_marker, target->run_marker_log,
	    marker_source, sizeof(marker_source), marker_path, sizeof(marker_path))) {
		action_log_name(id, action_name, sizeof(action_name));
		if (build_log_rel(graph, action_name, ".stdout", stdout_rel,
		    sizeof(stdout_rel)) < 0 ||
		    build_log_rel(graph, action_name, ".stderr", stderr_rel,
		    sizeof(stderr_rel)) < 0) {
			qstar_string_list_free(&inputs);
			qstar_string_list_free(&outputs);
			free_owned_argv(argv, argc);
			return qstar_set_error(graph, "qstar: action log path too long");
		}
		write_failure_replay_detail(graph, id, NULL, argv, "marker-missing",
		    target->label, stdout_rel, stderr_rel, target->run_marker,
		    target->run_marker_log);
		ctx->fail_count++;
		ctx->cancelled = 1;
		fprintf(ctx->out,
		    "run_target_result label=%s status=marker-missing marker=%s stdout=%s stderr=%s marker_log=%s replay=%s/logs/last-failure.replay\n",
		    target->label, target->run_marker, stdout_rel, stderr_rel,
		    target->run_marker_log && *target->run_marker_log ?
		    target->run_marker_log : "<none>", qstar_graph_build_dir(graph));
		rc = qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "marker", target->label,
		    "qstar: run_target '%s' marker '%s' was not found; replay=%s/logs/last-failure.replay",
		    target->label, target->run_marker, qstar_graph_build_dir(graph));
	}
	if (rc == 0 && write_run_stamp(graph, stamp) < 0)
		rc = -1;
	if (rc == 0 && target->run_marker && *target->run_marker)
		fprintf(ctx->out, "run_marker label=%s status=matched marker=%s source=%s path=%s\n",
		    target->label, target->run_marker, marker_source, marker_path);
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
		    "qstar: header source '%s' must be listed as lang.*.public_headers/private_headers",
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
	    strcmp(toolchain->name, "cale-sol") == 0) &&
	    !command_exists_in_graph(graph, toolchain->cale))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: Cale compiler '%s' not found for source '%s'",
		    toolchain->cale, target->sources.items[index]);
	if (strcmp(source->language, "cxx") == 0 &&
	    !command_exists_in_graph(graph, toolchain->cxx))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: C++ compiler '%s' not found for source '%s'",
		    toolchain->cxx, target->sources.items[index]);
	if (source_is_asm(source) && !command_exists_in_graph(graph, toolchain->cc))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: assembler compiler driver '%s' not found for source '%s'",
		    toolchain->cc, target->sources.items[index]);
	return 0;
}

/** compile action 실행 후 depfile이 필요한 경우 생성 여부를 확인한다. */
static int
check_compile_depfile(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_prepared_action *action)
{
	char full_depfile[QSTAR_PATH_MAX];

	if (!action->wants_depfile)
		return 0;
	if (full_path_under_root(graph, action->depfile, full_depfile,
	    sizeof(full_depfile)) == 0 && path_exists(full_depfile))
		return 0;
	if (outputs_exist(graph, &action->outputs)) {
		build_tracef(ctx,
		    "depfile_fallback id=%s depfile=%s status=missing cache=explicit-inputs-only\n",
		    action->id, action->depfile);
		return 0;
	}
	return qstar_set_error(graph, "qstar: compiler did not produce depfile '%s'",
	    action->depfile);
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
	compute_action_key(ctx, graph, target, toolchain, action->id, "compile",
	    action->argv, &inputs, &dep_inputs, action->outputs.items[0], action->key,
	    sizeof(action->key), &action->material);
	qstar_string_list_free(&inputs);
	qstar_string_list_free(&dep_inputs);
	if (state_update_material(ctx, action->id, action->key, &action->material) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** compile depfile refresh를 scheduler hot path 밖에서 처리하도록 queue에 넣는다. */
static int
depfile_refresh_queue_push(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    struct qstar_prepared_action *action)
{
	struct qstar_pending_depfile_refresh *p;
	size_t ncap;

	if (ctx->depfile_refresh_len == ctx->depfile_refresh_cap) {
		ncap = ctx->depfile_refresh_cap ? ctx->depfile_refresh_cap * 2 : 64;
		p = realloc(ctx->depfile_refresh,
		    ncap * sizeof(ctx->depfile_refresh[0]));
		if (!p)
			return qstar_set_error(graph, "qstar: out of memory");
		ctx->depfile_refresh = p;
		ctx->depfile_refresh_cap = ncap;
	}
	ctx->depfile_refresh[ctx->depfile_refresh_len++].action = action;
	return 0;
}

/** queue에 모인 compile depfile refresh를 build graph 실행 이후 일괄 처리한다. */
static int
depfile_refresh_queue_flush(struct qstar_graph *graph, struct qstar_build_ctx *ctx)
{
	struct qstar_prepared_action *action;
	size_t i;

	for (i = 0; i < ctx->depfile_refresh_len; i++) {
		action = ctx->depfile_refresh[i].action;
		if (!action)
			continue;
		if (refresh_compile_state_after_depfile(graph, ctx, action->target,
		    action->toolchain, action) < 0)
			return -1;
	}
	ctx->depfile_refresh_len = 0;
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
	if (qstar_graph_object_output_path(graph, target, index, object,
	    sizeof(object)) < 0 ||
	    qstar_graph_depfile_output_path(graph, target, index, action->depfile,
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
	if (graph->profile.compile_options.len +
	    graph->profile.include_dirs.len * 2 +
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
	for (i = 0; !is_cale && i < graph->profile.compile_options.len; i++) {
		if (prepared_action_push_argv(graph, action,
		    graph->profile.compile_options.items[i]) < 0)
			goto fail;
	}
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
	compute_action_key(ctx, graph, target, toolchain, action->id, "compile",
	    action->argv, &inputs, &dep_inputs, object, action->key, sizeof(action->key),
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
	struct qstar_source_info source;
	int rc;

	qstar_source_classify(target->sources.items[index], &source);
	if (!source_requires_compile(&source)) {
		if (!source_is_link_object(&source))
			return validate_compile_source(graph, target, toolchain, &source, index);
		return 0;
	}
	if (prepare_compile_action(graph, ctx, target, toolchain, index, &action) < 0)
		return -1;
	rc = run_action(graph, ctx, target, action.id, action.kind, action.key,
	    &action.outputs, action.argv, toolchain, &action.material);
	if (rc == 0)
		rc = check_compile_depfile(graph, ctx, &action);
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

	build_tracef(ctx,
	    "parallel_event target=%s event=queue id=%s order=%zu slot=%zu state=ready retry=no\n",
	    action->target->label, action->id, queue_index, slot);
	if (ensure_action_log_dir(graph, ctx, logdir, sizeof(logdir)) < 0)
		return -1;
	action_log_name(action->id, running->name, sizeof(running->name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout", logdir, running->name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.stderr", logdir, running->name);
	snprintf(running->action_log, sizeof(running->action_log), "%s/%s.log", logdir,
	    running->name);
	if (build_log_rel(graph, running->name, ".stdout", child_stdout_path,
	    sizeof(child_stdout_path)) < 0 ||
	    build_log_rel(graph, running->name, ".stderr", child_stderr_path,
	    sizeof(child_stderr_path)) < 0)
		return qstar_set_error(graph, "qstar: action log path too long");
	prev = state_find(ctx, action->id);
	ctx->scheduled_count++;
	build_tracef(ctx, "schedule_action id=%s kind=%s slot=%zu jobs=%d state=ready\n",
	    action->id, action->kind, slot, ctx->jobs);
	if (prev && strcmp(prev->key, action->key) == 0 &&
	    outputs_exist(graph, &action->outputs)) {
		build_tracef(ctx,
		    "build_action id=%s status=skip reason=cache-hit stdout=%s stderr=%s\n",
		    action->id, child_stdout_path, child_stderr_path);
		action_log_queue_skip_ref(ctx, running->action_log, action->argv);
		ctx->skip_count++;
		build_tracef(ctx,
		    "parallel_event target=%s event=skip id=%s slot=%zu state=cache-hit retry=no\n",
		    action->target->label, action->id, slot);
		return state_push(ctx, 1, action->id, action->key, action->outputs.items[0],
		    "skip", action->kind, &action->material) < 0 ?
		    qstar_set_error(graph, "qstar: out of memory") : 0;
	}
	build_tracef(ctx,
	    "build_action id=%s status=run timeout_sec=%d stdout=%s stderr=%s\n",
	    action->id, ctx->action_timeout_sec, child_stdout_path, child_stderr_path);
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
	build_tracef(ctx, "schedule_action id=%s slot=%zu pid=%ld state=started\n",
	    action->id, slot, (long)pid);
	build_tracef(ctx,
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
	char stdout_rel[QSTAR_PATH_MAX], stderr_rel[QSTAR_PATH_MAX], replay_rel[QSTAR_PATH_MAX];
	const char *failure_kind;
	int exit_code;

	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	action_log_queue_exit_ref(ctx, running->action_log, action->argv, exit_code);
	if (exit_code != 0) {
		failure_kind = classify_failure_kind(action->kind, action->target,
		    action->argv, "exit-code");
		if (build_log_rel(graph, running->name, ".stdout", stdout_rel,
		    sizeof(stdout_rel)) < 0)
			snprintf(stdout_rel, sizeof(stdout_rel), "<none>");
		if (build_log_rel(graph, running->name, ".stderr", stderr_rel,
		    sizeof(stderr_rel)) < 0)
			snprintf(stderr_rel, sizeof(stderr_rel), "<none>");
		if (build_replay_rel(graph, replay_rel, sizeof(replay_rel)) < 0)
			snprintf(replay_rel, sizeof(replay_rel),
			    "build/qstar/logs/last-failure.replay");
		build_tracef(ctx, "build_action id=%s status=fail exit=%d\n",
		    action->id, exit_code);
		build_tracef(ctx,
		    "parallel_event target=%s event=fail id=%s slot=%zu exit=%d state=failed retry=next-build cancel=active\n",
		    action->target->label, action->id, running->slot, exit_code);
		write_failure_replay_detail(graph, action->id, action->toolchain,
		    action->argv, failure_kind, action->target->label, stdout_rel,
		    stderr_rel, NULL, NULL);
		emit_action_diagnostic(ctx->out, action->id, action->kind,
		    action->target->label, failure_kind, "fail", exit_code,
		    stdout_rel, stderr_rel, replay_rel);
		ctx->fail_count++;
		ctx->cancelled = 1;
		return qstar_set_error_origin(graph, action->target->origin_file,
		    action->target->origin_line, failure_kind, action->target->label,
		    "qstar: action '%s' failed with status %d; replay=%s/logs/last-failure.replay",
		    action->id, exit_code, qstar_graph_build_dir(graph));
	}
	ctx->run_count++;
	if (state_push(ctx, 1, action->id, action->key, action->outputs.items[0],
	    "run", action->kind, &action->material) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	build_tracef(ctx, "build_action id=%s status=done exit=0\n", action->id);
	build_tracef(ctx, "schedule_action id=%s slot=%zu state=finished\n", action->id,
	    running->slot);
	build_tracef(ctx,
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
		build_tracef(ctx,
		    "build_action id=%s status=cancelled reason=parallel-failure retry=next-build\n",
		    running[i].action->id);
		build_tracef(ctx, "schedule_action id=%s slot=%zu state=cancelled\n",
		    running[i].action->id, running[i].slot);
		build_tracef(ctx,
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
	size_t *indices;
	size_t i, j, next, active, jobs, slot, compile_count;
	int rc, status, progressed;
	pid_t waited;
	struct qstar_source_info source;

	if (target->sources.len == 0)
		return 0;
	compile_count = target_compile_input_count(target);
	if (compile_count == 0)
		return 0;
	jobs = ctx->jobs > 1 ? (size_t)ctx->jobs : 1;
	indices = calloc(compile_count, sizeof(indices[0]));
	actions = calloc(compile_count, sizeof(actions[0]));
	running = calloc(jobs, sizeof(running[0]));
	if (!indices || !actions || !running) {
		free(indices);
		free(actions);
		free(running);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	for (i = 0, j = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		if (source_requires_compile(&source))
			indices[j++] = i;
	}
	for (i = 0; i < compile_count; i++) {
		if (prepare_compile_action(graph, ctx, target, toolchain, indices[i],
		    &actions[i]) < 0) {
			while (i > 0)
				prepared_action_free(&actions[--i]);
			free(indices);
			free(actions);
			free(running);
			return -1;
		}
	}
	next = 0;
	active = 0;
	rc = 0;
	build_tracef(ctx,
	    "parallel_batch target=%s jobs=%zu total=%zu policy=fifo fairness=source-order cancel=kill-active retry=next-build\n",
	    target->label, jobs, compile_count);
	while (next < compile_count || active > 0) {
		while (next < compile_count && active < jobs) {
			slot = find_free_slot(running, jobs);
			if (slot >= jobs) {
				rc = qstar_set_error(graph, "qstar: no free parallel slot");
				goto fail;
			}
			build_tracef(ctx,
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
					action_log_queue_exit_ref(ctx, running[i].action_log,
					    running[i].action->argv, 124);
					write_failure_replay(graph, running[i].action->id,
					    running[i].action->toolchain,
					    running[i].action->argv);
					ctx->fail_count++;
					ctx->cancelled = 1;
					build_tracef(ctx,
					    "build_action id=%s status=timeout timeout_sec=%d\n",
					    running[i].action->id, ctx->action_timeout_sec);
					build_tracef(ctx,
					    "parallel_event target=%s event=timeout id=%s slot=%zu state=timeout retry=next-build cancel=active\n",
					    running[i].action->target->label,
					    running[i].action->id, running[i].slot);
					rc = qstar_set_error_origin(graph,
					    running[i].action->target->origin_file,
						    running[i].action->target->origin_line, "action",
						    running[i].action->target->label,
						    "qstar: action '%s' timed out after %d seconds; replay=%s/logs/last-failure.replay",
						    running[i].action->id, ctx->action_timeout_sec,
						    qstar_graph_build_dir(graph));
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
			wait_poll_pause();
	}
	for (i = 0; i < compile_count; i++) {
		if (check_compile_depfile(graph, ctx, &actions[i]) < 0) {
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
	for (i = 0; i < compile_count; i++)
		prepared_action_free(&actions[i]);
	free(indices);
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
	if (strcmp(dep->kind, "group") == 0)
		return 0;
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
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
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
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
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
	compute_action_key(ctx, graph, target, toolchain, id,
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
	size_t i, compile_count;

	fprintf(ctx->out, "build_target %s order=%zu kind=%s\n", target->label, order,
	    target->kind);
	fprintf(ctx->out,
	    "action_dag target=%s order=%zu parallel=%s reason=deterministic-v3 failure=stop-on-first-failure timeout_sec=%d\n",
	    target->label, order,
	    ctx->jobs > 1 && target->sources.len > 1 ? "compile-process-v3" : "no",
	    ctx->action_timeout_sec);
	if (strcmp(target->kind, "sharedlib") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: sharedlib targets are plan-only in local executor v1");
	if (strcmp(target->kind, "run_target") == 0)
		return run_target_command(graph, ctx, target);
	if (strcmp(target->kind, "group") == 0) {
		fprintf(ctx->out,
		    "group_target label=%s deps=%zu private_deps=%zu action=none artifact=<none>\n",
		    target->label, target->deps.len, target->private_deps.len);
		return 0;
	}
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	fprintf(ctx->out,
	    "resolved_toolchain owner=%s toolchain=%s target=%s cc=%s cxx=%s ar=%s linker=%s\n",
	    target->label, toolchain.name, toolchain.target, toolchain.cc, toolchain.cxx,
	    toolchain.ar, toolchain.linker);
	if (validate_noncompile_sources(graph, target, &toolchain) < 0)
		return -1;
	if (run_generated_actions(graph, ctx, target) < 0)
		return -1;
	compile_count = target_compile_input_count(target);
	if (ctx->jobs > 1 && compile_count > 1) {
		fprintf(ctx->out,
		    "parallel_compile target=%s jobs=%d sources=%zu mode=process-v3 failure=cancel-active fairness=fifo\n",
		    target->label, ctx->jobs, compile_count);
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

/** scheduler node kind를 log-friendly 문자열로 변환한다. */
static const char *
sched_kind_name(enum qstar_sched_node_kind kind)
{
	switch (kind) {
	case QSTAR_SCHED_COMPILE:
		return "compile";
	case QSTAR_SCHED_FINAL:
		return "final";
	case QSTAR_SCHED_GENERATE:
		return "generate";
	case QSTAR_SCHED_RUN:
		return "run";
	case QSTAR_SCHED_GROUP:
		return "group";
	}
	return "unknown";
}

/** target pointer를 graph target index로 변환한다. */
static int
sched_target_index(const struct qstar_graph *graph, const struct qstar_target *target)
{
	if (!target || target < graph->targets || target >= graph->targets + graph->len)
		return -1;
	return (int)(target - graph->targets);
}

/** label을 graph target index로 변환한다. */
static int
sched_target_index_label(const struct qstar_graph *graph, const char *label)
{
	const struct qstar_target *target;

	target = find_target(graph, label);
	return target ? sched_target_index(graph, target) : -1;
}

/** genrule pointer를 graph genrule index로 변환한다. */
static int
sched_genrule_index(const struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	if (!genrule || genrule < graph->genrules ||
	    genrule >= graph->genrules + graph->genrule_len)
		return -1;
	return (int)(genrule - graph->genrules);
}

/** scheduler가 소유한 동적 저장소를 해제한다. */
static void
scheduler_free(struct qstar_scheduler *sched)
{
	size_t i;

	if (sched->nodes) {
		for (i = 0; i < sched->node_len; i++) {
			free(sched->nodes[i].deps);
			free(sched->nodes[i].outs);
			prepared_action_free(&sched->nodes[i].action);
		}
	}
	free(sched->nodes);
	free(sched->targets);
	free(sched->toolchains);
	free(sched->target_seen);
	free(sched->genrule_needed);
	free(sched->target_final_node);
	free(sched->genrule_node);
	free(sched->ready);
	memset(sched, 0, sizeof(*sched));
}

/** scheduler 배열과 index map을 초기화한다. */
static int
scheduler_init(struct qstar_scheduler *sched, struct qstar_graph *graph,
    struct qstar_build_ctx *ctx)
{
	size_t i;

	memset(sched, 0, sizeof(*sched));
	sched->graph = graph;
	sched->ctx = ctx;
	sched->toolchains = calloc(graph->len ? graph->len : 1,
	    sizeof(sched->toolchains[0]));
	sched->target_seen = calloc(graph->len ? graph->len : 1,
	    sizeof(sched->target_seen[0]));
	sched->target_final_node = malloc((graph->len ? graph->len : 1) *
	    sizeof(sched->target_final_node[0]));
	sched->genrule_needed = calloc(graph->genrule_len ? graph->genrule_len : 1,
	    sizeof(sched->genrule_needed[0]));
	sched->genrule_node = malloc((graph->genrule_len ? graph->genrule_len : 1) *
	    sizeof(sched->genrule_node[0]));
	if (!sched->toolchains || !sched->target_seen || !sched->target_final_node ||
	    !sched->genrule_needed || !sched->genrule_node) {
		scheduler_free(sched);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	for (i = 0; i < graph->len; i++)
		sched->target_final_node[i] = -1;
	for (i = 0; i < graph->genrule_len; i++)
		sched->genrule_node[i] = -1;
	return 0;
}

/** scheduler target list에 closure target을 한 번만 추가한다. */
static int
scheduler_add_target(struct qstar_scheduler *sched, const struct qstar_target *target)
{
	int index;
	const struct qstar_target **next;
	size_t cap;

	index = sched_target_index(sched->graph, target);
	if (index < 0)
		return qstar_set_error(sched->graph, "qstar: invalid scheduler target");
	if (sched->target_seen[index])
		return 0;
	if (sched->target_len == sched->target_cap) {
		cap = sched->target_cap ? sched->target_cap * 2 : 16;
		next = realloc(sched->targets, cap * sizeof(sched->targets[0]));
		if (!next)
			return qstar_set_error(sched->graph, "qstar: out of memory");
		sched->targets = next;
		sched->target_cap = cap;
	}
	sched->target_seen[index] = 1;
	sched->targets[sched->target_len++] = target;
	return 0;
}

/** closure visitor에서 target을 scheduler discovery set에 넣는다. */
static int
scheduler_collect_target(struct qstar_graph *graph, const struct qstar_target *target,
    size_t order, void *user)
{
	(void)graph;
	(void)order;
	return scheduler_add_target(user, target);
}

/** target label의 dependency closure를 scheduler discovery set에 추가한다. */
static int
scheduler_add_target_closure(struct qstar_scheduler *sched, const char *label)
{
	return qstar_graph_visit_closure(sched->graph, label, scheduler_collect_target,
	    sched);
}

/** genrule을 scheduler discovery set에 추가한다. */
static int
scheduler_mark_genrule(struct qstar_scheduler *sched, const struct qstar_genrule *genrule)
{
	int index;

	index = sched_genrule_index(sched->graph, genrule);
	if (index < 0)
		return qstar_set_error(sched->graph, "qstar: invalid scheduler genrule");
	sched->genrule_needed[index] = 1;
	return 0;
}

/** genrule input/argv dependency를 discovery set에 반영한다. */
static int
scheduler_discover_genrule_deps(struct qstar_scheduler *sched,
    const struct qstar_genrule *genrule, int *changed)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	size_t i;
	int rc, target_index, gen_index, before_targets;

	for (i = 0; i < genrule->inputs.len + genrule->args.len; i++) {
		const char *item = i < genrule->inputs.len ? genrule->inputs.items[i] :
		    genrule->args.items[i - genrule->inputs.len];

		rc = qstar_target_file_token_label(item, label, sizeof(label));
		if (rc < 0)
			return qstar_set_error_origin(sched->graph, genrule->origin_file,
			    genrule->origin_line, "inputs", genrule->label,
			    "qstar: malformed target_file placeholder");
		if (rc == 1) {
			target_index = sched_target_index_label(sched->graph, label);
			if (target_index >= 0) {
				before_targets = (int)sched->target_len;
				if (scheduler_add_target_closure(sched, label) < 0)
					return -1;
				if ((size_t)before_targets != sched->target_len)
					*changed = 1;
				continue;
			}
			owner = qstar_graph_find_genrule(sched->graph, label);
			if (owner) {
				gen_index = sched_genrule_index(sched->graph, owner);
				if (gen_index >= 0 && !sched->genrule_needed[gen_index]) {
					if (scheduler_mark_genrule(sched, owner) < 0)
						return -1;
					*changed = 1;
				}
			}
			continue;
		}
		owner = qstar_graph_find_output_owner(sched->graph, item);
		if (owner && owner != genrule) {
			gen_index = sched_genrule_index(sched->graph, owner);
			if (gen_index >= 0 && !sched->genrule_needed[gen_index]) {
				if (scheduler_mark_genrule(sched, owner) < 0)
					return -1;
				*changed = 1;
			}
		}
	}
	return 0;
}

/** target이 소비하는 generated action을 discovery set에 반영한다. */
static int
scheduler_discover_target_genrules(struct qstar_scheduler *sched,
    const struct qstar_target *target, int *changed)
{
	size_t i;

	for (i = 0; i < sched->graph->genrule_len; i++) {
		if (!target_consumes_genrule(target, &sched->graph->genrules[i]))
			continue;
		if (!sched->genrule_needed[i]) {
			if (scheduler_mark_genrule(sched, &sched->graph->genrules[i]) < 0)
				return -1;
			*changed = 1;
		}
	}
	return 0;
}

/** root label에서 필요한 target/genrule 닫힌 집합을 수집한다. */
static int
scheduler_discover(struct qstar_scheduler *sched, const char *label)
{
	const struct qstar_genrule *root_genrule;
	size_t i;
	int changed;

	root_genrule = label && *label ? qstar_graph_find_genrule(sched->graph, label) :
	    NULL;
	if (root_genrule) {
		if (scheduler_mark_genrule(sched, root_genrule) < 0)
			return -1;
	} else if (scheduler_add_target_closure(sched, label) < 0) {
		return -1;
	}
	do {
		changed = 0;
		for (i = 0; i < sched->target_len; i++) {
			if (scheduler_discover_target_genrules(sched, sched->targets[i],
			    &changed) < 0)
				return -1;
		}
		for (i = 0; i < sched->graph->genrule_len; i++) {
			if (!sched->genrule_needed[i])
				continue;
			if (scheduler_discover_genrule_deps(sched,
			    &sched->graph->genrules[i], &changed) < 0)
				return -1;
		}
	} while (changed);
	return 0;
}

/** scheduler node를 추가한다. */
static int
scheduler_add_node(struct qstar_scheduler *sched, enum qstar_sched_node_kind kind,
    const struct qstar_target *target, const struct qstar_genrule *genrule,
    size_t source_index, size_t target_queue_index, size_t *out_index)
{
	struct qstar_sched_node *next, *node;
	size_t cap;

	if (sched->node_len == sched->node_cap) {
		cap = sched->node_cap ? sched->node_cap * 2 : 32;
		next = realloc(sched->nodes, cap * sizeof(sched->nodes[0]));
		if (!next)
			return qstar_set_error(sched->graph, "qstar: out of memory");
		sched->nodes = next;
		sched->node_cap = cap;
	}
	node = &sched->nodes[sched->node_len];
	memset(node, 0, sizeof(*node));
	node->kind = kind;
	node->state = QSTAR_SCHED_PENDING;
	node->target = target;
	node->genrule = genrule;
	node->source_index = source_index;
	node->target_queue_index = target_queue_index;
	*out_index = sched->node_len++;
	return 0;
}

/** scheduler index list에 값을 중복 없이 추가한다. */
static int
scheduler_index_list_push(size_t **items, size_t *len, size_t *cap, size_t value)
{
	size_t i, next_cap;
	size_t *next;

	for (i = 0; i < *len; i++) {
		if ((*items)[i] == value)
			return 0;
	}
	if (*len == *cap) {
		next_cap = *cap ? *cap * 2 : 4;
		next = realloc(*items, next_cap * sizeof((*items)[0]));
		if (!next)
			return -1;
		*items = next;
		*cap = next_cap;
	}
	(*items)[(*len)++] = value;
	return 0;
}

/** dep -> user edge를 scheduler graph에 추가한다. */
static int
scheduler_add_edge(struct qstar_scheduler *sched, size_t dep, size_t user)
{
	if (dep == user)
		return qstar_set_error(sched->graph, "qstar: scheduler self dependency");
	if (scheduler_index_list_push(&sched->nodes[user].deps,
	    &sched->nodes[user].dep_len, &sched->nodes[user].dep_cap, dep) < 0 ||
	    scheduler_index_list_push(&sched->nodes[dep].outs,
	    &sched->nodes[dep].out_len, &sched->nodes[dep].out_cap, user) < 0)
		return qstar_set_error(sched->graph, "qstar: out of memory");
	sched->nodes[user].remaining = sched->nodes[user].dep_len;
	return 0;
}

/** ready queue에 scheduler node를 추가한다. */
static int
scheduler_ready_push(struct qstar_scheduler *sched, size_t node_index)
{
	size_t cap;
	size_t *next;

	if (sched->ready_len == sched->ready_cap) {
		cap = sched->ready_cap ? sched->ready_cap * 2 : 32;
		next = realloc(sched->ready, cap * sizeof(sched->ready[0]));
		if (!next)
			return qstar_set_error(sched->graph, "qstar: out of memory");
		sched->ready = next;
		sched->ready_cap = cap;
	}
	sched->nodes[node_index].state = QSTAR_SCHED_READY;
	sched->ready[sched->ready_len++] = node_index;
	return 0;
}

/** ready queue에 남은 항목 수를 반환한다. */
static size_t
scheduler_ready_count(const struct qstar_scheduler *sched)
{
	return sched->ready_len - sched->ready_head;
}

/** completed node의 dependent remaining count를 낮추고 ready queue를 채운다. */
static int
scheduler_complete_node(struct qstar_scheduler *sched, size_t node_index)
{
	struct qstar_sched_node *dep;
	size_t i, out_index;

	sched->nodes[node_index].state = QSTAR_SCHED_DONE;
	for (i = 0; i < sched->nodes[node_index].out_len; i++) {
		out_index = sched->nodes[node_index].outs[i];
		dep = &sched->nodes[out_index];
		if (dep->state != QSTAR_SCHED_PENDING)
			continue;
		if (dep->remaining > 0)
			dep->remaining--;
		if (dep->remaining == 0 && scheduler_ready_push(sched, out_index) < 0)
			return -1;
	}
	return 0;
}

/** target node skeleton을 만들고 final node index map을 채운다. */
static int
scheduler_create_target_nodes(struct qstar_scheduler *sched, const struct qstar_target *target,
    size_t order)
{
	struct qstar_graph *graph = sched->graph;
	struct qstar_build_ctx *ctx = sched->ctx;
	struct qstar_resolved_toolchain *toolchain;
	struct qstar_source_info source;
	size_t i, node_index, compile_ordinal;
	int target_index;

	target_index = sched_target_index(graph, target);
	if (target_index < 0)
		return qstar_set_error(graph, "qstar: invalid scheduler target");
	build_tracef(ctx, "build_target %s order=%zu kind=%s\n", target->label, order,
	    target->kind);
	build_tracef(ctx,
	    "action_dag target=%s order=%zu parallel=%s reason=ready-queue-v1 failure=stop-on-first-failure timeout_sec=%d\n",
	    target->label, order,
	    ctx->jobs > 1 && target_compile_input_count(target) > 0 ?
	    "action-dag-ready-queue" : "no", ctx->action_timeout_sec);
	if (strcmp(target->kind, "sharedlib") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: sharedlib targets are plan-only in local executor v1");
	if (strcmp(target->kind, "group") == 0) {
		if (scheduler_add_node(sched, QSTAR_SCHED_GROUP, target, NULL, 0, 0,
		    &node_index) < 0)
			return -1;
		sched->target_final_node[target_index] = (int)node_index;
		return 0;
	}
	if (strcmp(target->kind, "run_target") == 0) {
		if (scheduler_add_node(sched, QSTAR_SCHED_RUN, target, NULL, 0, 0,
		    &node_index) < 0)
			return -1;
		sched->target_final_node[target_index] = (int)node_index;
		return 0;
	}
	toolchain = &sched->toolchains[target_index];
	if (qstar_resolve_toolchain(graph, target, toolchain) < 0)
		return -1;
	build_tracef(ctx,
	    "resolved_toolchain owner=%s toolchain=%s target=%s cc=%s cxx=%s ar=%s linker=%s\n",
	    target->label, toolchain->name, toolchain->target, toolchain->cc,
	    toolchain->cxx, toolchain->ar, toolchain->linker);
	if (validate_noncompile_sources(graph, target, toolchain) < 0)
		return -1;
	compile_ordinal = 0;
	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		if (!source_requires_compile(&source))
			continue;
		if (scheduler_add_node(sched, QSTAR_SCHED_COMPILE, target, NULL, i,
		    compile_ordinal, &node_index) < 0)
			return -1;
		sched->nodes[node_index].toolchain = toolchain;
		compile_ordinal++;
	}
	if (compile_ordinal > 1)
		build_tracef(ctx,
		    "parallel_batch target=%s jobs=%d total=%zu policy=fifo fairness=ready-queue cancel=kill-active retry=next-build\n",
		    target->label, ctx->jobs, compile_ordinal);
	if (compile_ordinal > 1)
		build_tracef(ctx,
		    "parallel_compile target=%s jobs=%d sources=%zu mode=process-v3 failure=cancel-active fairness=ready-queue\n",
		    target->label, ctx->jobs, compile_ordinal);
	if (scheduler_add_node(sched, QSTAR_SCHED_FINAL, target, NULL, 0, 0,
	    &node_index) < 0)
		return -1;
	sched->nodes[node_index].toolchain = toolchain;
	sched->target_final_node[target_index] = (int)node_index;
	return 0;
}

/** needed genrule node skeleton을 만든다. */
static int
scheduler_create_genrule_nodes(struct qstar_scheduler *sched)
{
	size_t i, node_index;

	for (i = 0; i < sched->graph->genrule_len; i++) {
		if (!sched->genrule_needed[i])
			continue;
		if (scheduler_add_node(sched, QSTAR_SCHED_GENERATE, NULL,
		    &sched->graph->genrules[i], 0, 0, &node_index) < 0)
			return -1;
		sched->genrule_node[i] = (int)node_index;
	}
	return 0;
}

/** target_file/plain generated input을 node edge로 연결한다. */
static int
scheduler_add_genrule_item_edge(struct qstar_scheduler *sched,
    const struct qstar_genrule *genrule, const char *item, size_t user_node)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc, target_index, gen_index;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error_origin(sched->graph, genrule->origin_file,
		    genrule->origin_line, "inputs", genrule->label,
		    "qstar: malformed target_file placeholder");
	if (rc == 1) {
		target_index = sched_target_index_label(sched->graph, label);
		if (target_index >= 0 && sched->target_final_node[target_index] >= 0)
			return scheduler_add_edge(sched,
			    (size_t)sched->target_final_node[target_index], user_node);
		owner = qstar_graph_find_genrule(sched->graph, label);
		if (owner) {
			gen_index = sched_genrule_index(sched->graph, owner);
			if (gen_index >= 0 && sched->genrule_node[gen_index] >= 0)
				return scheduler_add_edge(sched,
				    (size_t)sched->genrule_node[gen_index], user_node);
		}
		return 0;
	}
	owner = qstar_graph_find_output_owner(sched->graph, item);
	if (owner && owner != genrule) {
		gen_index = sched_genrule_index(sched->graph, owner);
		if (gen_index >= 0 && sched->genrule_node[gen_index] >= 0)
			return scheduler_add_edge(sched,
			    (size_t)sched->genrule_node[gen_index], user_node);
	}
	return 0;
}

/** genrule dependency edge를 scheduler graph에 연결한다. */
static int
scheduler_connect_genrule_edges(struct qstar_scheduler *sched)
{
	const struct qstar_genrule *genrule;
	size_t i, j, node_index;

	for (i = 0; i < sched->graph->genrule_len; i++) {
		if (sched->genrule_node[i] < 0)
			continue;
		genrule = &sched->graph->genrules[i];
		node_index = (size_t)sched->genrule_node[i];
		for (j = 0; j < genrule->inputs.len; j++) {
			if (scheduler_add_genrule_item_edge(sched, genrule,
			    genrule->inputs.items[j], node_index) < 0)
				return -1;
		}
		for (j = 0; j < genrule->args.len; j++) {
			if (scheduler_add_genrule_item_edge(sched, genrule,
			    genrule->args.items[j], node_index) < 0)
				return -1;
		}
	}
	return 0;
}

/** target dependency label list를 target node edge로 연결한다. */
static int
scheduler_connect_target_dep_list(struct qstar_scheduler *sched,
    const struct qstar_string_list *deps, size_t user_node)
{
	int dep_index;
	size_t i;

	for (i = 0; i < deps->len; i++) {
		if (deps->items[i][0] == '@')
			continue;
		dep_index = sched_target_index_label(sched->graph, deps->items[i]);
		if (dep_index >= 0 && sched->target_final_node[dep_index] >= 0) {
			if (scheduler_add_edge(sched,
			    (size_t)sched->target_final_node[dep_index], user_node) < 0)
				return -1;
		}
	}
	return 0;
}

/** target이 소비하는 generated action edge를 node에 연결한다. */
static int
scheduler_connect_consumed_genrules(struct qstar_scheduler *sched,
    const struct qstar_target *target, size_t user_node)
{
	size_t i;

	for (i = 0; i < sched->graph->genrule_len; i++) {
		if (sched->genrule_node[i] < 0 ||
		    !target_consumes_genrule(target, &sched->graph->genrules[i]))
			continue;
		if (scheduler_add_edge(sched, (size_t)sched->genrule_node[i],
		    user_node) < 0)
			return -1;
	}
	return 0;
}

/** target 내부 compile/final/run/group edge를 scheduler graph에 연결한다. */
static int
scheduler_connect_target_edges(struct qstar_scheduler *sched)
{
	const struct qstar_target *target;
	struct qstar_sched_node *node;
	size_t i, j, final_node;
	int target_index;

	for (i = 0; i < sched->target_len; i++) {
		target = sched->targets[i];
		target_index = sched_target_index(sched->graph, target);
		if (target_index < 0 || sched->target_final_node[target_index] < 0)
			return qstar_set_error(sched->graph, "qstar: missing target final node");
		final_node = (size_t)sched->target_final_node[target_index];
		if (scheduler_connect_target_dep_list(sched, &target->deps, final_node) < 0 ||
		    scheduler_connect_target_dep_list(sched, &target->private_deps,
		    final_node) < 0)
			return -1;
		if (strcmp(target->kind, "group") == 0 ||
		    strcmp(target->kind, "run_target") == 0)
			continue;
		if (scheduler_connect_consumed_genrules(sched, target, final_node) < 0)
			return -1;
		for (j = 0; j < sched->node_len; j++) {
			node = &sched->nodes[j];
			if (node->kind != QSTAR_SCHED_COMPILE || node->target != target)
				continue;
			if (scheduler_add_edge(sched, j, final_node) < 0 ||
			    scheduler_connect_target_dep_list(sched, &target->deps, j) < 0 ||
			    scheduler_connect_target_dep_list(sched, &target->private_deps, j) < 0 ||
			    scheduler_connect_consumed_genrules(sched, target, j) < 0)
				return -1;
		}
	}
	return 0;
}

/** discovery set에서 scheduler action graph를 만든다. */
static int
scheduler_build_nodes(struct qstar_scheduler *sched)
{
	size_t i;

	for (i = 0; i < sched->target_len; i++) {
		if (scheduler_create_target_nodes(sched, sched->targets[i], i) < 0)
			return -1;
	}
	if (scheduler_create_genrule_nodes(sched) < 0)
		return -1;
	if (scheduler_connect_genrule_edges(sched) < 0 ||
	    scheduler_connect_target_edges(sched) < 0)
		return -1;
	return 0;
}

/** compile cache hit을 ready queue 진입 전에 처리할 수 있는지 확인한다. */
static int
prepared_action_cache_hit(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const struct qstar_prepared_action *action)
{
	const struct qstar_state_entry *prev;

	prev = state_find(ctx, action->id);
	return prev && strcmp(prev->key, action->key) == 0 &&
	    outputs_exist(graph, &action->outputs);
}

/** compile cache hit action을 process slot 배정 전에 skip 처리한다. */
static int
skip_prepared_action_prequeue(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    struct qstar_prepared_action *action)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], action_log[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];

	ctx->scheduled_count++;
	build_tracef(ctx,
	    "schedule_action id=%s kind=%s slot=prequeue jobs=%d state=skipped-prequeue\n",
	    action->id, action->kind, ctx->jobs);
	if (build_trace_enabled(ctx)) {
		if (ensure_action_log_dir(graph, ctx, logdir, sizeof(logdir)) < 0)
			return -1;
		action_log_name(action->id, name, sizeof(name));
		snprintf(action_log, sizeof(action_log), "%s/%s.log", logdir, name);
		if (build_log_rel(graph, name, ".stdout", child_stdout_path,
		    sizeof(child_stdout_path)) < 0 ||
		    build_log_rel(graph, name, ".stderr", child_stderr_path,
		    sizeof(child_stderr_path)) < 0)
			return qstar_set_error(graph, "qstar: action log path too long");
		build_tracef(ctx,
		    "build_action id=%s status=skip reason=cache-hit stdout=%s stderr=%s\n",
		    action->id, child_stdout_path, child_stderr_path);
		build_tracef(ctx,
		    "scheduler_event event=pre_skip id=%s target=%s state=cache-hit retry=no\n",
		    action->id, action->target->label);
		action_log_queue_skip_ref(ctx, action_log, action->argv);
	}
	ctx->skip_count++;
	return state_push(ctx, 1, action->id, action->key, action->outputs.items[0],
	    "skip", action->kind, &action->material) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

/** ready queue의 첫 항목을 꺼낸다. */
static int
scheduler_ready_pop(struct qstar_scheduler *sched, size_t *node_index)
{
	if (scheduler_ready_count(sched) == 0)
		return 0;
	*node_index = sched->ready[sched->ready_head++];
	if (sched->ready_head == sched->ready_len) {
		sched->ready_head = 0;
		sched->ready_len = 0;
	}
	return 1;
}

/** sync node를 실행한다. */
static int
scheduler_run_sync_node(struct qstar_scheduler *sched, size_t node_index)
{
	struct qstar_sched_node *node = &sched->nodes[node_index];
	int rc;

	build_tracef(sched->ctx,
	    "scheduler_event event=run_sync id=%zu kind=%s state=ready\n",
	    node_index, sched_kind_name(node->kind));
	if (node->kind == QSTAR_SCHED_GROUP) {
		fprintf(sched->ctx->out,
		    "group_target label=%s deps=%zu private_deps=%zu action=none artifact=<none>\n",
		    node->target->label, node->target->deps.len,
		    node->target->private_deps.len);
		return 0;
	}
	if (node->kind == QSTAR_SCHED_GENERATE)
		return run_one_generated_action(sched->graph, sched->ctx, NULL,
		    node->genrule);
	if (node->kind == QSTAR_SCHED_RUN)
		return run_target_command(sched->graph, sched->ctx, node->target);
	if (node->kind == QSTAR_SCHED_FINAL) {
		rc = run_final(sched->graph, sched->ctx, node->target, node->toolchain);
		return rc;
	}
	return qstar_set_error(sched->graph, "qstar: invalid sync scheduler node");
}

/** scheduler를 중지하면서 active compile process를 정리한다. */
static void
scheduler_cancel_active(struct qstar_build_ctx *ctx, struct qstar_running_action *running,
    size_t jobs)
{
	cancel_running_actions(ctx, running, jobs);
}

/** ready-queue action DAG를 실행한다. */
static int
scheduler_execute(struct qstar_scheduler *sched)
{
	struct qstar_running_action *running;
	size_t i, node_index, completed, active, jobs, slot;
	int rc, status, popped, progressed;
	pid_t waited;

	jobs = sched->ctx->jobs > 1 ? (size_t)sched->ctx->jobs : 1;
	running = calloc(jobs, sizeof(running[0]));
	if (!running)
		return qstar_set_error(sched->graph, "qstar: out of memory");
	for (i = 0; i < sched->node_len; i++) {
		if (sched->nodes[i].remaining == 0 &&
		    scheduler_ready_push(sched, i) < 0) {
			free(running);
			return -1;
		}
	}
	build_tracef(sched->ctx,
	    "action_scheduler version=v1 nodes=%zu ready=%zu jobs=%zu policy=ready-queue failure=stop-on-first-failure\n",
	    sched->node_len, scheduler_ready_count(sched), jobs);
	progress_begin(sched->ctx, sched->node_len);
	completed = 0;
	active = 0;
	rc = 0;
	while (completed < sched->node_len) {
		progressed = 0;
		while (scheduler_ready_count(sched) > 0) {
			popped = scheduler_ready_pop(sched, &node_index);
			if (!popped)
				break;
			if (sched->nodes[node_index].state != QSTAR_SCHED_READY)
				continue;
			if (sched->nodes[node_index].kind != QSTAR_SCHED_COMPILE) {
				progress_status(sched->ctx,
				    progress_stage_name(&sched->nodes[node_index]),
				    progress_node_label(&sched->nodes[node_index]));
				rc = scheduler_run_sync_node(sched, node_index);
				if (rc < 0)
					goto fail;
				if (scheduler_complete_node(sched, node_index) < 0) {
					rc = -1;
					goto fail;
				}
				completed++;
				progress_complete(sched->ctx, &sched->nodes[node_index],
				    completed);
				progressed = 1;
				continue;
			}
			if (sched->nodes[node_index].action.id[0] == '\0' &&
			    prepare_compile_action(sched->graph, sched->ctx,
			    sched->nodes[node_index].target,
			    sched->nodes[node_index].toolchain,
			    sched->nodes[node_index].source_index,
			    &sched->nodes[node_index].action) < 0) {
				rc = -1;
				goto fail;
			}
			progress_status(sched->ctx,
			    progress_stage_name(&sched->nodes[node_index]),
			    progress_node_label(&sched->nodes[node_index]));
			if (prepared_action_cache_hit(sched->graph, sched->ctx,
			    &sched->nodes[node_index].action)) {
				rc = skip_prepared_action_prequeue(sched->graph,
				    sched->ctx, &sched->nodes[node_index].action);
				if (rc < 0)
					goto fail;
				if (scheduler_complete_node(sched, node_index) < 0) {
					rc = -1;
					goto fail;
				}
				completed++;
				progress_complete(sched->ctx, &sched->nodes[node_index],
				    completed);
				progressed = 1;
				continue;
			}
			if (active >= jobs) {
				if (scheduler_ready_push(sched, node_index) < 0) {
					rc = -1;
					goto fail;
				}
				break;
			}
			slot = find_free_slot(running, jobs);
			if (slot >= jobs) {
				rc = qstar_set_error(sched->graph, "qstar: no free scheduler slot");
				goto fail;
			}
			build_tracef(sched->ctx,
			    "parallel_slot target=%s slot=%zu state=assign action=%s queue=%zu scheduler=global\n",
			    sched->nodes[node_index].target->label, slot,
			    sched->nodes[node_index].action.id,
			    sched->nodes[node_index].target_queue_index);
			rc = start_prepared_action(sched->graph, sched->ctx,
			    &sched->nodes[node_index].action, &running[slot], slot,
			    sched->nodes[node_index].target_queue_index);
			if (rc < 0)
				goto fail;
			if (rc == 1)
				running[slot].node_index = node_index;
			sched->nodes[node_index].state = rc == 1 ? QSTAR_SCHED_RUNNING :
			    QSTAR_SCHED_DONE;
			if (rc == 1)
				active++;
			else {
				if (scheduler_complete_node(sched, node_index) < 0) {
					rc = -1;
					goto fail;
				}
				completed++;
				progress_complete(sched->ctx, &sched->nodes[node_index],
				    completed);
			}
			progressed = 1;
		}
		for (i = 0; i < jobs; i++) {
			if (running[i].pid <= 0)
				continue;
			waited = waitpid(running[i].pid, &status, WNOHANG);
			if (waited == 0) {
				if (time(NULL) - running[i].start >=
				    sched->ctx->action_timeout_sec) {
					kill(running[i].pid, SIGKILL);
					waitpid(running[i].pid, &status, 0);
					running[i].pid = 0;
					action_log_queue_exit_ref(sched->ctx,
					    running[i].action_log,
					    running[i].action->argv, 124);
					write_failure_replay(sched->graph,
					    running[i].action->id,
					    running[i].action->toolchain,
					    running[i].action->argv);
					sched->ctx->fail_count++;
					sched->ctx->cancelled = 1;
					build_tracef(sched->ctx,
					    "build_action id=%s status=timeout timeout_sec=%d\n",
					    running[i].action->id,
					    sched->ctx->action_timeout_sec);
					build_tracef(sched->ctx,
					    "parallel_event target=%s event=timeout id=%s slot=%zu state=timeout retry=next-build cancel=active\n",
					    running[i].action->target->label,
					    running[i].action->id, running[i].slot);
					rc = qstar_set_error_origin(sched->graph,
					    running[i].action->target->origin_file,
					    running[i].action->target->origin_line, "action",
					    running[i].action->target->label,
					    "qstar: action '%s' timed out after %d seconds; replay=%s/logs/last-failure.replay",
					    running[i].action->id,
					    sched->ctx->action_timeout_sec,
					    qstar_graph_build_dir(sched->graph));
					goto fail;
				}
				continue;
			}
			if (waited < 0) {
				rc = qstar_set_error(sched->graph, "qstar: waitpid failed");
				goto fail;
			}
			running[i].pid = 0;
			node_index = running[i].node_index;
			rc = finish_running_action(sched->graph, sched->ctx, &running[i],
			    status);
			if (rc == 0)
				rc = check_compile_depfile(sched->graph, sched->ctx,
				    running[i].action);
			if (rc == 0)
				rc = depfile_refresh_queue_push(sched->graph,
				    sched->ctx, running[i].action);
			running[i].action = NULL;
			active--;
			if (rc < 0)
				goto fail;
			if (scheduler_complete_node(sched, node_index) < 0) {
				rc = -1;
				goto fail;
			}
			completed++;
			progress_complete(sched->ctx, &sched->nodes[node_index],
			    completed);
			progressed = 1;
			break;
		}
		if (!progressed) {
			if (active == 0 && scheduler_ready_count(sched) == 0 &&
			    completed < sched->node_len) {
				rc = qstar_set_error(sched->graph,
				    "qstar: scheduler stalled with pending actions");
				goto fail;
			}
			wait_poll_pause();
		}
	}
	if (rc == 0)
		rc = depfile_refresh_queue_flush(sched->graph, sched->ctx);
	free(running);
	return rc;
fail:
	if (active > 0)
		scheduler_cancel_active(sched->ctx, running, jobs);
	free(running);
	return rc < 0 ? rc : -1;
}

/** QStar ready-queue action scheduler entry point다. */
static int
build_with_action_scheduler(struct qstar_graph *graph, struct qstar_build_ctx *ctx,
    const char *label)
{
	struct qstar_scheduler sched;
	const struct qstar_genrule *genrule;
	int rc;

	if (scheduler_init(&sched, graph, ctx) < 0)
		return -1;
	ctx->action_scheduler = 1;
	genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	if (genrule) {
		build_tracef(ctx, "build_generated_action %s order=0 kind=custom_target\n",
		    genrule->label);
		build_tracef(ctx,
		    "action_dag target=%s order=0 parallel=%s reason=ready-queue-v1 failure=stop-on-first-failure timeout_sec=%d\n",
		    genrule->label, ctx->jobs > 1 ? "action-dag-ready-queue" : "no",
		    ctx->action_timeout_sec);
	}
	rc = scheduler_discover(&sched, label);
	if (rc == 0)
		rc = scheduler_build_nodes(&sched);
	if (rc == 0)
		rc = scheduler_execute(&sched);
	action_log_flush(ctx);
	ctx->action_scheduler = 0;
	scheduler_free(&sched);
	return rc;
}

/** build context가 소유한 동적 저장소를 해제한다. */
static void
build_ctx_free(struct qstar_build_ctx *ctx)
{
	state_free(ctx->prev, ctx->prev_len);
	state_free(ctx->next, ctx->next_len);
	hash_cache_free(ctx->hash_cache, ctx->hash_cache_len);
	action_log_queue_free(ctx->action_logs, ctx->action_log_len);
	free(ctx->depfile_refresh);
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
	ctx.jobs = options && options->jobs > 0 ? options->jobs : default_job_count();
	ctx.schedule_trace = options ? options->schedule_trace : 0;
	ctx.verbose = options ? options->verbose : 0;
	ctx.quiet = options ? options->quiet : 0;
	ctx.progress_mode = options ? options->progress_mode : QSTAR_PROGRESS_AUTO;
	ctx.color_mode = options ? options->color_mode : QSTAR_COLOR_AUTO;
	ctx.progress_enabled = build_progress_enabled(out, ctx.progress_mode);
	ctx.color_enabled = build_color_enabled(out, ctx.color_mode);
	ctx.action_timeout_sec = action_timeout_sec_from_env();
	if (state_load(graph, &ctx) < 0) {
		build_ctx_free(&ctx);
		return -1;
	}
	build_tracef(&ctx, "qstar build v2\n");
	build_tracef(&ctx, "root %s\n", ctx.root_label);
	build_tracef(&ctx, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	build_tracef(&ctx,
	    "executor-policy version=v4 parallel=%s jobs=%d active=%s failure=stop-on-first-failure action_timeout_sec=%d cancel=kill-process-and-stop-queue\n",
	    ctx.jobs > 1 ? "optional" : "no", ctx.jobs,
	    ctx.jobs > 1 ? "action-dag-ready-queue" : "serial-ready-queue",
	    ctx.action_timeout_sec);
	rc = graph_snapshot_write(graph);
	if (rc == 0)
		rc = build_with_action_scheduler(graph, &ctx, label);
	action_log_flush(&ctx);
	if (rc == 0)
		rc = state_write(graph, &ctx);
	if (rc == 0)
		rc = compile_db_write(graph, &ctx);
	if (build_summary_write(graph, &ctx, rc == 0 ? "success" : "failure") < 0 &&
	    rc == 0)
		rc = -1;
	progress_finish(&ctx, rc == 0);
	if (rc == 0) {
		fprintf(out, "%sstatus ok%s run=%zu skip=%zu fail=%zu\n",
		    build_color(&ctx, "success"), build_color_reset(&ctx),
		    ctx.run_count, ctx.skip_count, ctx.fail_count);
	} else {
		if (ctx.cancelled)
			fprintf(out,
			    "cancel_propagation policy=stop-on-first-failure pending=not-started scheduled=%zu\n",
			    ctx.scheduled_count);
		fprintf(out, "%sstatus fail%s run=%zu skip=%zu fail=%zu\n",
		    build_color(&ctx, "error"), build_color_reset(&ctx),
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

/** target 하나의 build output directory를 지운다. */
static int
clean_target_output(struct qstar_graph *graph, const struct qstar_target *target)
{
	char owner[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	char full[QSTAR_PATH_MAX];

	qstar_mangle_label(target->label, owner, sizeof(owner));
	if (snprintf(sub, sizeof(sub), "out/%s", owner) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0 ||
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
		if (full_path_under_root(graph, qstar_graph_build_dir(graph), full,
		    sizeof(full)) < 0 ||
		    remove_tree(full) < 0)
			return qstar_set_error(graph, "qstar: clean failed for '%s'",
			    qstar_graph_build_dir(graph));
		if (strcmp(qstar_graph_compile_commands_policy(graph), "root") == 0 &&
		    full_path_under_root(graph, "compile_commands.json", full, sizeof(full)) == 0 &&
		    remove_tree(full) < 0)
			return qstar_set_error(graph,
			    "qstar: clean failed for root compile_commands.json");
		fprintf(out, "clean_all %s compile_commands=%s\n",
		    qstar_graph_build_dir(graph),
		    qstar_graph_compile_commands_policy(graph));
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
	if (full_path_under_build(graph, "logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create test log dir");
	action_log_name(target->label, name, sizeof(name));
	snprintf(stdout_path, sizeof(stdout_path), "%s/%s.test.stdout", logdir, name);
	snprintf(stderr_path, sizeof(stderr_path), "%s/%s.test.stderr", logdir, name);
	if (build_log_rel(graph, name, ".test.stdout", child_stdout_path,
	    sizeof(child_stdout_path)) < 0 ||
	    build_log_rel(graph, name, ".test.stderr", child_stderr_path,
	    sizeof(child_stderr_path)) < 0)
		return qstar_set_error(graph, "qstar: test log path too long");
	fprintf(out,
	    "test_run label=%s artifact=%s stdout=%s stderr=%s\n",
	    target->label, artifact, child_stdout_path, child_stderr_path);
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
		wait_poll_pause();
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
	if (full_path_under_build(graph, "install", dir, sizeof(dir)) < 0 ||
	    mkdir_p(dir) < 0 ||
	    full_path_under_build(graph, "install/manifest.json",
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
	fprintf(out, "install_manifest %s/install/manifest.json\n",
	    qstar_graph_build_dir(graph));
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
	struct qstar_build_options build_options;
	char artifact[QSTAR_PATH_MAX], dst[QSTAR_PATH_MAX];
	const char *role;
	size_t i;

	if (!qstar_target_is_installable(target))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: target '%s' is not installable", target->label);
	if (!ctx->options->dry_run && strcmp(qstar_graph_generator(graph), "ninja") == 0) {
		memset(&build_options, 0, sizeof(build_options));
		if (qstar_graph_build_ninja(graph, target->label, &build_options,
		    ctx->out) < 0)
			return -1;
	}
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

/** stage manifest v2를 쓰기 시작한다. */
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
	if (snprintf(dir, sizeof(dir), "stage/%s", owner) >= (int)sizeof(dir) ||
	    mkdir_parent_under_build(graph, "stage/.keep") < 0 ||
	    full_path_under_build(graph, dir, ctx->manifest_path, sizeof(ctx->manifest_path)) < 0 ||
	    mkdir_p(ctx->manifest_path) < 0)
		return qstar_set_error(graph, "qstar: could not create stage manifest dir");
	if (snprintf(dir, sizeof(dir), "stage/%s/manifest.json", owner) >=
	    (int)sizeof(dir) ||
	    full_path_under_build(graph, dir, ctx->manifest_path,
	    sizeof(ctx->manifest_path)) < 0)
		return qstar_set_error(graph, "qstar: stage manifest path too long");
	snprintf(ctx->manifest_tmp, sizeof(ctx->manifest_tmp), "%s.tmp",
	    ctx->manifest_path);
	ctx->manifest = fopen(ctx->manifest_tmp, "w");
	if (!ctx->manifest)
		return qstar_set_error(graph, "qstar: could not write stage manifest");
	fputs("{\n  \"schema\":\"qstar-stage-manifest-v2\",\n  \"label\":", ctx->manifest);
	json_string(ctx->manifest, stage->label);
	fputs(",\n  \"root\":", ctx->manifest);
	json_string(ctx->manifest, ctx->root_rel);
	fputs(",\n  \"mode\":", ctx->manifest);
	json_string(ctx->manifest, options && options->dry_run ? "dry-run" : "copy");
	fprintf(ctx->manifest,
	    ",\n  \"layout\":{\"status\":\"ok\",\"file_count\":%zu}",
	    stage->srcs.len);
	fputs(",\n  \"entries\":[\n", ctx->manifest);
	fprintf(out, "stage_manifest %s/stage/%s/manifest.json\n",
	    qstar_graph_build_dir(graph), owner);
	fprintf(out, "stage_layout label=%s root=%s files=%zu status=ok\n",
	    stage->label, ctx->root_rel, stage->srcs.len);
	return 0;
}

/** stage manifest에 copy/diff record 하나를 추가한다. */
static void
stage_manifest_record(struct qstar_stage_ctx *ctx, const char *src_rel,
    const char *dst_rel, const char *action, const char *kind, const char *producer)
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
	fputs(",\"kind\":", ctx->manifest);
	json_string(ctx->manifest, kind);
	fputs(",\"producer\":", ctx->manifest);
	json_string(ctx->manifest, producer);
	fputs("}", ctx->manifest);
	ctx->manifest_len++;
}

/** stage manifest v2를 완료한다. */
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

	rc = qstar_target_file_token_label(src, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed stage target_file source");
	if (dry_run)
		return 0;
	if (rc == 1) {
		if (strcmp(qstar_graph_generator(graph), "ninja") == 0) {
			struct qstar_build_options options;

			memset(&options, 0, sizeof(options));
			return qstar_graph_build_ninja(graph, label, &options, out);
		}
		return qstar_graph_build(graph, label, out);
	}
	owner = qstar_graph_find_output_owner(graph, src);
	if (owner) {
		if (strcmp(qstar_graph_generator(graph), "ninja") == 0) {
			struct qstar_build_options options;

			memset(&options, 0, sizeof(options));
			return qstar_graph_build_ninja(graph, owner->label, &options, out);
		}
		return qstar_graph_build(graph, owner->label, out);
	}
	return 0;
}

/** stage source token을 package-relative source path로 해석한다. */
static int
stage_resolve_src(struct qstar_graph *graph, const char *src, char *dst, size_t dstlen)
{
	return resolve_target_file_token(graph, src, dst, dstlen);
}

/** stage source token을 manifest용 source kind/producer로 분류한다. */
static void
stage_source_kind(struct qstar_graph *graph, const char *src_token,
    const char *src_rel, const char **kind, const char **producer,
    char *producer_buf, size_t producer_buf_len)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc;

	*kind = "file";
	*producer = "<none>";
	rc = qstar_target_file_token_label(src_token, label, sizeof(label));
	if (rc == 1) {
		if (find_target(graph, label)) {
			*kind = "target";
			snprintf(producer_buf, producer_buf_len, "%s", label);
			*producer = producer_buf;
			return;
		}
		if (qstar_graph_find_genrule(graph, label)) {
			*kind = "custom_target";
			snprintf(producer_buf, producer_buf_len, "%s", label);
			*producer = producer_buf;
			return;
		}
	}
	owner = qstar_graph_find_output_owner(graph, src_rel);
	if (owner) {
		*kind = "custom_output";
		snprintf(producer_buf, producer_buf_len, "%s", owner->label);
		*producer = producer_buf;
	}
}

/** single stage copy 또는 dry-run diff line을 수행한다. */
static int
stage_file(struct qstar_graph *graph, struct qstar_stage_ctx *ctx, const char *src_token,
    const char *dst_rel)
{
	char src_rel[QSTAR_PATH_MAX], src_full[QSTAR_PATH_MAX], dst_stage_rel[QSTAR_PATH_MAX];
	char dst_full[QSTAR_PATH_MAX], producer_buf[QSTAR_PATH_MAX];
	const char *action, *kind, *producer;
	int exists;

	if (stage_build_source_dependency(graph, src_token,
	    ctx->options && ctx->options->dry_run, ctx->out) < 0)
		return -1;
	if (stage_resolve_src(graph, src_token, src_rel, sizeof(src_rel)) < 0)
		return -1;
	if (qstar_path_join(ctx->root_rel, dst_rel, dst_stage_rel,
	    sizeof(dst_stage_rel)) < 0)
		return qstar_set_error_origin(graph, ctx->stage->origin_file,
		    ctx->stage->origin_line, "package-failure", ctx->stage->label,
		    "qstar: stage destination path too long");
	if (full_path_under_root(graph, src_rel, src_full, sizeof(src_full)) < 0 ||
	    full_path_under_root(graph, dst_stage_rel, dst_full, sizeof(dst_full)) < 0)
		return qstar_set_error_origin(graph, ctx->stage->origin_file,
		    ctx->stage->origin_line, "package-failure", ctx->stage->label,
		    "qstar: stage file path too long");
	exists = path_exists(dst_full);
	if (exists && file_content_equal(src_full, dst_full))
		action = "unchanged";
	else if (exists)
		action = "would-update";
	else
		action = "would-create";
	stage_source_kind(graph, src_token, src_rel, &kind, &producer, producer_buf,
	    sizeof(producer_buf));
	fprintf(ctx->out, "stage_file src=%s dst=%s mode=%s kind=%s producer=%s\n",
	    src_rel, dst_stage_rel,
	    ctx->options && ctx->options->dry_run ? "dry-run" : "copy",
	    kind, producer);
	fprintf(ctx->out, "stage_diff dst=%s action=%s exists=%s\n", dst_stage_rel,
	    action, exists ? "yes" : "no");
	stage_manifest_record(ctx, src_rel, dst_stage_rel, action, kind, producer);
	if (ctx->options && ctx->options->dry_run)
		return 0;
	if (!path_exists(src_full))
		return qstar_set_error_origin(graph, ctx->stage->origin_file,
		    ctx->stage->origin_line, "package-failure", ctx->stage->label,
		    "qstar: stage source '%s' is missing", src_rel);
	if (mkdir_parent_under_root(graph, dst_stage_rel) < 0 ||
	    copy_file_to_path(src_full, dst_full) < 0)
		return qstar_set_error_origin(graph, ctx->stage->origin_file,
		    ctx->stage->origin_line, "package-failure", ctx->stage->label,
		    "qstar: failed to stage '%s' to '%s'",
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
	fputs("qstar stage v2\n", out);
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
		else {
			const char *failure_kind = graph->error_field[0] ?
			    graph->error_field : "package-failure";

			fprintf(out,
			    "stage_result label=%s status=fail failure_kind=%s replay=%s/logs/last-failure.replay\n",
			    stage->label, failure_kind, qstar_graph_build_dir(graph));
		if (strcmp(failure_kind, "package-failure") == 0)
			write_stage_failure_replay(graph, stage);
	}
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
	if (full_path_under_build(graph, "logs", dirpath, sizeof(dirpath)) < 0)
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
		fprintf(out, "log_file %s/logs/%s\n", qstar_graph_build_dir(graph),
		    names[i]);
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

	if (full_path_under_build(graph, "logs/last-failure.replay", path,
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
	if (build_log_rel(graph, name, ".log", rel, rel_len) < 0 ||
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
