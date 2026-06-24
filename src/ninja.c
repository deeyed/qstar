#include "internal.h"

#include <ctype.h>
#include <errno.h>
#if defined(_WIN32)
#define QSTAR_PLATFORM_WINDOWS 1
#else
#define QSTAR_PLATFORM_WINDOWS 0
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define QSTAR_NINJA_MAX_ARGV 256
#define QSTAR_NINJA_RESPONSE_ARGV_BYTES 512
#define QSTAR_NINJA_RUN_TIMEOUT_SEC 30
#define QSTAR_NINJA_TEST_TIMEOUT_SEC 5

struct ninja_argv {
	char *items[QSTAR_NINJA_MAX_ARGV];
	size_t len;
};

struct ninja_ctx {
	FILE *ninja;
	FILE *compdb;
	FILE *out;
	char ninja_rel[QSTAR_PATH_MAX];
	char ninja_full[QSTAR_PATH_MAX];
	char ninja_dir_rel[QSTAR_PATH_MAX];
	char compdb_rel[QSTAR_PATH_MAX];
	char compdb_full[QSTAR_PATH_MAX];
	char default_alias[QSTAR_PATH_MAX];
	const char *root_label;
	int *target_emitted;
	int *genrule_state;
	int *stage_state;
	int compdb_first;
	int edge_count;
};

static int emit_target(struct qstar_graph *graph, const struct qstar_target *target,
    size_t order, void *user);
static int emit_genrule_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule);
static int emit_stage_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_stage *stage);
static int target_compile_needs_pic(const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, int is_asm);
static int ninja_argv_push_tool_role(struct qstar_graph *graph, struct ninja_argv *argv,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const char *role, const char *fallback);
static int ninja_argv_push_provider_template(struct qstar_graph *graph,
    struct ninja_argv *argv, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_string_list *template);
static void ninja_path(FILE *f, const char *s);

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

/** graph target pointer의 index를 찾는다. */
static int
target_index(const struct qstar_graph *graph, const struct qstar_target *target)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (&graph->targets[i] == target)
			return (int)i;
	}
	return -1;
}

/** graph generated action pointer의 index를 찾는다. */
static int
genrule_index(const struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (&graph->genrules[i] == genrule)
			return (int)i;
	}
	return -1;
}

/** graph stage pointer의 index를 찾는다. */
static int
stage_index(const struct qstar_graph *graph, const struct qstar_stage *stage)
{
	size_t i;

	for (i = 0; i < graph->stage_len; i++) {
		if (&graph->stages[i] == stage)
			return (int)i;
	}
	return -1;
}

/** package root 아래 상대 path를 절대 path로 계산한다. */
static int
full_path_under_root(const struct qstar_graph *graph, const char *rel, char *dst,
    size_t dstlen)
{
	const char *root;
	int n;

	root = graph->package_root ? graph->package_root : ".";
	if (rel[0] == '\0')
		n = snprintf(dst, dstlen, "%s", root);
	else
		n = snprintf(dst, dstlen, "%s/%s", root, rel);
	return n >= 0 && n < (int)dstlen ? 0 : -1;
}

/** mkdir -p 동작으로 directory chain을 만든다. */
static int
mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	size_t i, len;

	if (!path || !*path)
		return 0;
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	len = strlen(tmp);
	while (len > 1 && tmp[len - 1] == '/')
		tmp[--len] = '\0';
	for (i = 1; tmp[i]; i++) {
		if (tmp[i] != '/')
			continue;
		tmp[i] = '\0';
		if (qstar_platform_mkdir(tmp, 0777) < 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}
	if (qstar_platform_mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/** 파일 존재 여부를 검사한다. */
static int
path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/** package-relative file path의 parent directory를 만든다. */
static int
mkdir_parent_under_root(const struct qstar_graph *graph, const char *rel)
{
	char full[QSTAR_PATH_MAX];
	char *slash;

	if (full_path_under_root(graph, rel, full, sizeof(full)) < 0)
		return -1;
	slash = strrchr(full, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(full);
}

/** path parent 부분을 dst에 복사한다. */
static int
path_dirname(const char *path, char *dst, size_t dstlen)
{
	const char *slash;
	size_t n;

	slash = strrchr(path, '/');
	if (!slash) {
		if (dstlen < 2)
			return -1;
		snprintf(dst, dstlen, ".");
		return 0;
	}
	n = (size_t)(slash - path);
	if (n == 0)
		n = 1;
	if (n + 1 > dstlen)
		return -1;
	memcpy(dst, path, n);
	dst[n] = '\0';
	return 0;
}

/** action id를 log filename으로 쓸 수 있게 정규화한다. */
static void
action_log_name(const char *id, char *dst, size_t dstlen)
{
	qstar_mangle_label(id, dst, dstlen);
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

/** action log 상대 path를 계산한다. */
static int
build_log_rel(const struct qstar_graph *graph, const char *name, const char *suffix,
    char *dst, size_t dstlen)
{
	char sub[QSTAR_PATH_MAX];

	if (snprintf(sub, sizeof(sub), "logs/%s%s", name, suffix) >= (int)sizeof(sub))
		return -1;
	return qstar_graph_build_path(graph, sub, dst, dstlen);
}

/** 임시 argv storage를 해제한다. */
static void
ninja_argv_free(struct ninja_argv *argv)
{
	size_t i;

	for (i = 0; i < argv->len; i++)
		free(argv->items[i]);
	memset(argv, 0, sizeof(*argv));
}

/** Ninja edge command용 argv item을 소유 문자열로 추가한다. */
static int
ninja_argv_push(struct qstar_graph *graph, struct ninja_argv *argv, const char *s)
{
	if (argv->len + 1 >= QSTAR_NINJA_MAX_ARGV)
		return qstar_set_error(graph, "qstar: ninja command argv too long");
	argv->items[argv->len] = qstar_strdup(s);
	if (!argv->items[argv->len])
		return qstar_set_error(graph, "qstar: out of memory");
	argv->len++;
	argv->items[argv->len] = NULL;
	return 0;
}

/** format string으로 argv item을 만들어 추가한다. */
static int
ninja_argv_pushf(struct qstar_graph *graph, struct ninja_argv *argv, const char *fmt, ...)
{
	char buf[QSTAR_PATH_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(buf))
		return qstar_set_error(graph, "qstar: ninja command argv item is too long");
	return ninja_argv_push(graph, argv, buf);
}

/** argv 전체를 새 storage로 복사한다. */
static int
ninja_argv_clone(struct qstar_graph *graph, struct ninja_argv *dst,
    const struct ninja_argv *src)
{
	size_t i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < src->len; i++) {
		if (ninja_argv_push(graph, dst, src->items[i]) < 0) {
			ninja_argv_free(dst);
			return -1;
		}
	}
	return 0;
}

/** 임시 string list에 문자열이 이미 있는지 확인한다. */
static int
list_contains(const struct qstar_string_list *list, const char *s)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], s) == 0)
			return 1;
	}
	return 0;
}

/** 임시 string list에 중복 없이 문자열을 추가한다. */
static int
push_unique_tmp(struct qstar_string_list *list, const char *s)
{
	if (list_contains(list, s))
		return 0;
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
	for (i = 0; i < target->private_include_dirs.len; i++) {
		if (push_unique_tmp(out, target->private_include_dirs.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < target->public_include_dirs.len; i++) {
		if (push_unique_tmp(out, target->public_include_dirs.items[i]) < 0)
			return -1;
	}
	if (collect_public_includes_rec(graph, target, out, 0) < 0)
		return -1;
	return 0;
}

/** Ninja backend가 처리할 수 있는 compile source인지 검증한다. */
static int
validate_ninja_compile_source(struct qstar_graph *graph,
    const struct qstar_target *source_owner,
    const struct qstar_source_info *source, size_t index)
{
	if (source->header_input)
		return qstar_set_error_origin(graph, source_owner->origin_file,
		    source_owner->origin_line,
		    "sources", source_owner->label,
		    "qstar: header source '%s' must be listed as lang.*.public_headers/private_headers",
		    source_owner->sources.items[index]);
	if (qstar_source_is_cxx_module(source))
		return qstar_set_error_origin(graph, source_owner->origin_file,
		    source_owner->origin_line,
		    "sources", source_owner->label,
		    "qstar: ninja backend MVP does not support C++ module source '%s'",
		    source_owner->sources.items[index]);
	if (!qstar_graph_language_provider_is_available(graph, source->provider) ||
	    !qstar_source_toolset_role(source)[0])
		return qstar_set_error_origin(graph, source_owner->origin_file,
		    source_owner->origin_line,
		    "sources", source_owner->label,
		    "qstar: ninja backend MVP does not support source '%s' with language '%s'",
		    source_owner->sources.items[index], source->language);
	return 0;
}

/** target source가 final archive action에 제공하는 object path를 계산한다. */
static int
target_source_object_input(struct qstar_graph *graph, const struct qstar_target *target,
    size_t index, char *dst, size_t dstlen)
{
	struct qstar_source_info source;

	if (qstar_target_source_classify(target, index, &source) < 0)
		return qstar_set_error(graph, "qstar: unsupported source '%s'",
		    target->sources.items[index]);
	if (qstar_source_is_link_object(&source)) {
		return snprintf(dst, dstlen, "%s", target->sources.items[index]) <
		    (int)dstlen ? 0 : -1;
	}
	return qstar_graph_object_output_path(graph, target, index, dst, dstlen);
}

/** objectlib가 consumer context에서 compile되는지 확인한다. */
static int
objectlib_uses_consumer_context(const struct qstar_target *objectlib)
{
	return objectlib && objectlib->compile_context &&
	    strcmp(objectlib->compile_context, "consumer") == 0;
}

/** consumer target 안에서 objectlib source가 제공하는 object path를 계산한다. */
static int
objectlib_source_object_input(struct qstar_graph *graph,
    const struct qstar_target *consumer, const struct qstar_target *objectlib,
    size_t index, char *dst, size_t dstlen)
{
	struct qstar_source_info source;

	if (qstar_target_source_classify(objectlib, index, &source) < 0)
		return qstar_set_error(graph, "qstar: unsupported source '%s'",
		    objectlib->sources.items[index]);
	if (qstar_source_is_link_object(&source))
		return snprintf(dst, dstlen, "%s", objectlib->sources.items[index]) <
		    (int)dstlen ? 0 : -1;
	if (objectlib_uses_consumer_context(objectlib))
		return qstar_graph_consumer_object_output_path(graph, consumer, objectlib,
		    index, dst, dstlen);
	return qstar_graph_object_output_path(graph, objectlib, index, dst, dstlen);
}

/** consumer target 안에서 objectlib source의 depfile path를 계산한다. */
static int
objectlib_source_depfile_output(struct qstar_graph *graph,
    const struct qstar_target *consumer, const struct qstar_target *objectlib,
    size_t index, char *dst, size_t dstlen)
{
	if (objectlib_uses_consumer_context(objectlib))
		return qstar_graph_consumer_depfile_output_path(graph, consumer, objectlib,
		    index, dst, dstlen);
	return qstar_graph_depfile_output_path(graph, objectlib, index, dst, dstlen);
}

/** consumer target 안에서 objectlib source compile action id index를 계산한다. */
static size_t
objectlib_consumer_compile_index(struct qstar_graph *graph,
    const struct qstar_target *consumer, size_t objectlib_list_index,
    size_t source_index)
{
	const struct qstar_target *objectlib;
	size_t i, index;

	index = consumer->sources.len;
	for (i = 0; i < objectlib_list_index; i++) {
		objectlib = find_target(graph, consumer->objects.items[i]);
		if (objectlib)
			index += objectlib->sources.len;
	}
	return index + source_index;
}

/** objectlib의 object 입력을 Ninja argv에 추가한다. */
static int
ninja_argv_push_objectlib_objects(struct qstar_graph *graph, struct ninja_argv *argv,
    const struct qstar_target *target)
{
	const struct qstar_target *objectlib;
	char object[QSTAR_PATH_MAX];
	size_t i, j;

	for (i = 0; i < target->objects.len; i++) {
		objectlib = find_target(graph, target->objects.items[i]);
		if (!objectlib)
			return qstar_set_error(graph,
			    "qstar: unknown objectlib '%s' referenced by '%s'",
			    target->objects.items[i], target->label);
		for (j = 0; j < objectlib->sources.len; j++) {
			if (objectlib_source_object_input(graph, target, objectlib, j, object,
			    sizeof(object)) < 0)
				return qstar_set_error(graph,
				    "qstar: objectlib object path too long");
			if (ninja_argv_push(graph, argv, object) < 0)
				return -1;
		}
	}
	return 0;
}

/** objectlib의 object 입력을 Ninja edge input으로 출력한다. */
static int
write_objectlib_object_deps(struct qstar_graph *graph, FILE *f,
    const struct qstar_target *target)
{
	const struct qstar_target *objectlib;
	char object[QSTAR_PATH_MAX];
	size_t i, j;

	for (i = 0; i < target->objects.len; i++) {
		objectlib = find_target(graph, target->objects.items[i]);
		if (!objectlib)
			return qstar_set_error(graph,
			    "qstar: unknown objectlib '%s' referenced by '%s'",
			    target->objects.items[i], target->label);
		for (j = 0; j < objectlib->sources.len; j++) {
			if (objectlib_source_object_input(graph, target, objectlib, j, object,
			    sizeof(object)) < 0)
				return qstar_set_error(graph,
				    "qstar: objectlib object path too long");
			fputc(' ', f);
			ninja_path(f, object);
		}
	}
	return 0;
}

/** JSON string 문자 하나를 escape하여 출력한다. */
static void
json_char(FILE *f, unsigned char c)
{
	if (c == '"' || c == '\\')
		fprintf(f, "\\%c", c);
	else if (c == '\n')
		fputs("\\n", f);
	else if (c == '\r')
		fputs("\\r", f);
	else if (c == '\t')
		fputs("\\t", f);
	else
		fputc(c, f);
}

/** JSON string 값을 출력한다. */
static void
json_string(FILE *f, const char *s)
{
	const unsigned char *p;

	fputc('"', f);
	for (p = (const unsigned char *)(s ? s : ""); *p; p++)
		json_char(f, *p);
	fputc('"', f);
}

/** shell-safe single command argument를 출력한다. */
static void
shell_arg(FILE *f, const char *s)
{
	const unsigned char *p;
	int simple;

	simple = s && *s;
	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (!(('a' <= *p && *p <= 'z') || ('A' <= *p && *p <= 'Z') ||
		    ('0' <= *p && *p <= '9') || *p == '_' || *p == '-' ||
		    *p == '.' || *p == '/' || *p == ':' || *p == '=' ||
		    *p == '+' || *p == ',' || *p == '@' || *p == '%')) {
			simple = 0;
			break;
		}
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

/** JSON string literal 자체를 shell-safe single argument로 출력한다. */
static void
shell_json_string_arg(FILE *f, const char *s)
{
	const unsigned char *p;

	fputc('\'', f);
	fputc('"', f);
	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '\'') {
			fputs("'\\''", f);
			continue;
		}
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
	}
	fputc('"', f);
	fputc('\'', f);
}

/** action/replay log의 사용자-facing description metadata를 쓴다. */
static void
write_log_description(FILE *f, const char *description)
{
	fputs("description=", f);
	shell_arg(f, description && *description ? description : "<none>");
	fputc('\n', f);
}

/** argv 전체를 shell command string으로 출력한다. */
static void
shell_argv(FILE *f, const struct ninja_argv *argv)
{
	size_t i;

	for (i = 0; i < argv->len; i++) {
		if (i)
			fputc(' ', f);
		shell_arg(f, argv->items[i]);
	}
}

static void
shell_env_assignment(FILE *f, const char *item)
{
	const char *eq;

	eq = item ? strchr(item, '=') : NULL;
	if (!eq || eq == item) {
		shell_arg(f, item ? item : "");
		return;
	}
	fwrite(item, 1, (size_t)(eq - item) + 1, f);
	shell_arg(f, eq + 1);
}

static void
cmd_env_assignment(FILE *f, const char *item)
{
	const unsigned char *p;

	fputs("set \"", f);
	for (p = (const unsigned char *)(item ? item : ""); *p; p++) {
		if (*p == '"')
			fputs("\\\"", f);
		else
			fputc(*p, f);
	}
	fputs("\"", f);
}

static void
shell_env_prefix(FILE *f, const struct qstar_string_list *env, int windows)
{
	size_t i;

	for (i = 0; env && i < env->len; i++) {
		if (windows) {
			cmd_env_assignment(f, env->items[i]);
			fputs(" && ", f);
		} else {
			shell_env_assignment(f, env->items[i]);
			fputc(' ', f);
		}
	}
}

static int
ninja_arg_is_sh_script(const char *s)
{
	size_t n;

	if (!s)
		return 0;
	n = strlen(s);
	return n > 3 && strcmp(s + n - 3, ".sh") == 0;
}

/** Windows Ninja cannot CreateProcess package-local shell scripts directly. */
static void
shell_argv_ninja_command(FILE *f, const struct ninja_argv *argv, int windows)
{
	if (windows && argv && argv->len > 0 && ninja_arg_is_sh_script(argv->items[0])) {
		shell_arg(f, "sh");
		fputc(' ', f);
	}
	shell_argv(f, argv);
}

/** Ninja variable value에서 special token을 escape해 한 줄로 출력한다. */
static void
ninja_var_text(FILE *f, const char *s)
{
	const unsigned char *p;

	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '$')
			fputs("$$", f);
		else
			fputc(*p, f);
	}
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
		fputs(s ? s : "", f);
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
			while (backslashes > 0) {
				fputs("\\\\", f);
				backslashes--;
			}
			fputs("\\\"", f);
			continue;
		}
		while (backslashes > 0) {
			fputc('\\', f);
			backslashes--;
		}
		fputc(*p, f);
	}
	while (backslashes > 0) {
		fputs("\\\\", f);
		backslashes--;
	}
	fputc('"', f);
}

/** response file style에 맞춰 argv item을 쓴다. */
static void
write_response_arg(FILE *f, const char *s, const char *style)
{
	if (style && (strcmp(style, "windows") == 0 || strcmp(style, "msvc") == 0))
		write_windows_response_arg(f, s);
	else
		shell_arg(f, s);
}

/** argv가 response file로 내릴 만큼 긴지 계산한다. */
static int
ninja_argv_needs_response_file(const struct ninja_argv *argv)
{
	size_t i, bytes;

	bytes = 0;
	for (i = 0; i < argv->len; i++)
		bytes += strlen(argv->items[i]) + 1;
	return bytes >= QSTAR_NINJA_RESPONSE_ARGV_BYTES || i > 48;
}

/** response file을 만들어 실제 Ninja command argv를 축약한다. */
static int
prepare_ninja_response_file(struct qstar_graph *graph, const char *id,
    const struct qstar_resolved_toolchain *toolchain, const struct ninja_argv *full,
    struct ninja_argv *run, char *rsp_rel, size_t rsp_rel_len)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], full_path[QSTAR_PATH_MAX];
	char rsp_arg[QSTAR_PATH_MAX];
	const char *style;
	FILE *f;
	size_t i;

	rsp_rel[0] = '\0';
	if (!toolchain || !toolchain->response_files ||
	    !ninja_argv_needs_response_file(full))
		return ninja_argv_clone(graph, run, full);
	style = toolchain->response_style[0] ? toolchain->response_style : "posix";
	action_log_name(id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "rsp/%s.rsp", name) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, rsp_rel, rsp_rel_len) < 0 ||
	    full_path_under_root(graph, rsp_rel, full_path, sizeof(full_path)) < 0)
		return qstar_set_error(graph, "qstar: response file path too long");
	if (mkdir_parent_under_root(graph, rsp_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create response file dir");
	f = fopen(full_path, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write response file");
	for (i = 1; i < full->len; i++) {
		write_response_arg(f, full->items[i], style);
		fputc('\n', f);
	}
	if (fclose(f) != 0)
		return qstar_set_error(graph, "qstar: could not close response file");
	if (snprintf(rsp_arg, sizeof(rsp_arg), "@%s", rsp_rel) >= (int)sizeof(rsp_arg))
		return qstar_set_error(graph, "qstar: response file arg too long");
	memset(run, 0, sizeof(*run));
	if (ninja_argv_push(graph, run, full->items[0]) < 0 ||
	    ninja_argv_push(graph, run, rsp_arg) < 0) {
		ninja_argv_free(run);
		return -1;
	}
	return 0;
}

/** Ninja action log stream에 output list를 deterministic하게 기록한다. */
static void
write_ninja_action_outputs(FILE *f, const struct qstar_string_list *outputs)
{
	size_t i;

	fprintf(f, "output_count=%zu\n", outputs ? outputs->len : 0);
	for (i = 0; outputs && i < outputs->len; i++) {
		fprintf(f, "output[%zu]=", i);
		shell_arg(f, outputs->items[i]);
		fputc('\n', f);
	}
}

static void
write_ninja_env_redacted(FILE *f, const struct qstar_string_list *env)
{
	const char *item, *eq;
	size_t i, n;

	fprintf(f, "envc=%zu\n", env ? env->len : 0);
	for (i = 0; env && i < env->len; i++) {
		item = env->items[i] ? env->items[i] : "";
		eq = strchr(item, '=');
		n = eq ? (size_t)(eq - item) : strlen(item);
		fprintf(f, "env[%zu]=", i);
		fwrite(item, 1, n, f);
		fputs("=<redacted>\n", f);
	}
}

/** Ninja action도 qstar action-log/replay가 읽을 수 있는 v2 log를 남긴다. */
static int
write_ninja_action_log(struct qstar_graph *graph, const char *id,
    const struct ninja_argv *argv, const char *rsp_rel, const char *description,
    const struct qstar_string_list *env, const struct qstar_string_list *outputs)
{
	char logdir[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], log_path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (full_path_under_build(graph, "logs", logdir, sizeof(logdir)) < 0 ||
	    mkdir_p(logdir) < 0)
		return qstar_set_error(graph, "qstar: could not create ninja action log dir");
	action_log_name(id, name, sizeof(name));
	snprintf(log_path, sizeof(log_path), "%s/%s.log", logdir, name);
	f = fopen(log_path, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write ninja action log");
	fprintf(f, "qstar-action-log v2\nexit=ninja\nbackend=ninja\n");
	write_log_description(f, description);
	write_ninja_env_redacted(f, env);
	write_ninja_action_outputs(f, outputs);
	fprintf(f, "response_file=%s\n", rsp_rel && *rsp_rel ? rsp_rel : "<none>");
	fprintf(f, "argc=%zu\n", argv->len);
	for (i = 0; i < argv->len; i++) {
		fprintf(f, "argv[%zu]=", i);
		shell_arg(f, argv->items[i]);
		fputc('\n', f);
	}
	fputs("argv=", f);
	for (i = 0; i < argv->len; i++)
		fprintf(f, "%s%s", i ? " " : "", argv->items[i]);
	fputs("\nargv_shell=", f);
	shell_argv(f, argv);
	fputc('\n', f);
	fclose(f);
	return 0;
}

/** edge command를 필요하면 response file로 축약해 Ninja variable로 쓴다. */
static int
write_edge_command(struct qstar_graph *graph, struct ninja_ctx *ctx, const char *id,
    const struct qstar_resolved_toolchain *toolchain, const struct ninja_argv *argv,
    const char *description, const struct qstar_string_list *env,
    const struct qstar_string_list *outputs)
{
	struct ninja_argv run;
	char rsp_rel[QSTAR_PATH_MAX];
	int windows;

	memset(&run, 0, sizeof(run));
	if (prepare_ninja_response_file(graph, id, toolchain, argv, &run, rsp_rel,
	    sizeof(rsp_rel)) < 0)
		return -1;
	windows = qstar_platform_is_windows(qstar_graph_platform(graph)) ||
	    (toolchain && qstar_platform_is_windows(toolchain->platform));
	shell_env_prefix(ctx->ninja, env, windows);
	shell_argv_ninja_command(ctx->ninja, &run, windows);
	ninja_argv_free(&run);
	return write_ninja_action_log(graph, id, argv, rsp_rel, description, env,
	    outputs);
}

/** shell command string을 JSON value로 출력한다. */
static void
json_shell_argv(FILE *f, const struct ninja_argv *argv)
{
	size_t i;
	const unsigned char *p;

	fputc('"', f);
	for (i = 0; i < argv->len; i++) {
		if (i)
			json_char(f, ' ');
		if (argv->items[i][0]) {
			int simple = 1;
			for (p = (const unsigned char *)argv->items[i]; *p; p++) {
				if (!(('a' <= *p && *p <= 'z') ||
				    ('A' <= *p && *p <= 'Z') ||
				    ('0' <= *p && *p <= '9') || *p == '_' ||
				    *p == '-' || *p == '.' || *p == '/' ||
				    *p == ':' || *p == '=' || *p == '+' ||
				    *p == ',' || *p == '@' || *p == '%')) {
					simple = 0;
					break;
				}
			}
			if (simple) {
				for (p = (const unsigned char *)argv->items[i]; *p; p++)
					json_char(f, *p);
				continue;
			}
		}
		json_char(f, '\'');
		for (p = (const unsigned char *)argv->items[i]; *p; p++) {
			if (*p == '\'') {
				json_char(f, '\'');
				json_char(f, '\\');
				json_char(f, '\'');
				json_char(f, '\'');
			} else {
				json_char(f, *p);
			}
		}
		json_char(f, '\'');
	}
	fputc('"', f);
}

/** Ninja path token을 escape하여 출력한다. */
static void
ninja_path(FILE *f, const char *s)
{
	const unsigned char *p;

	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == ' ')
			fputs("$ ", f);
		else if (*p == ':')
			fputs("$:", f);
		else if (*p == '$')
			fputs("$$", f);
		else
			fputc(*p, f);
	}
}

/** QStar label에 대응하는 Ninja phony alias output path를 만든다. */
static int
ninja_alias_path(const struct qstar_graph *graph, const char *label, char *dst,
    size_t dstlen)
{
	char mangle[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];

	qstar_mangle_label(label, mangle, sizeof(mangle));
	if (snprintf(sub, sizeof(sub), "ninja/targets/%s", mangle) >= (int)sizeof(sub))
		return -1;
	return qstar_graph_build_path(graph, sub, dst, dstlen);
}

/** stage manifest path를 package-relative build path로 만든다. */
static int
stage_manifest_path(const struct qstar_graph *graph, const char *label, char *dst,
    size_t dstlen)
{
	char mangle[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];

	qstar_mangle_label(label, mangle, sizeof(mangle));
	if (snprintf(sub, sizeof(sub), "stage/%s/manifest.json", mangle) >=
	    (int)sizeof(sub))
		return -1;
	return qstar_graph_build_path(graph, sub, dst, dstlen);
}

/** stage label에 대응하는 Ninja phony alias output path를 만든다. */
static int
stage_alias_path(const struct qstar_graph *graph, const char *label, char *dst,
    size_t dstlen)
{
	char mangle[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX];

	qstar_mangle_label(label, mangle, sizeof(mangle));
	if (snprintf(sub, sizeof(sub), "ninja/stages/%s", mangle) >= (int)sizeof(sub))
		return -1;
	return qstar_graph_build_path(graph, sub, dst, dstlen);
}

/** compile_commands policy에 맞는 output path를 계산한다. */
static int
compile_commands_path(struct qstar_graph *graph, char *rel, size_t rellen,
    char *full, size_t fulllen)
{
	const char *policy;

	policy = qstar_graph_compile_commands_policy(graph);
	if (strcmp(policy, "off") == 0) {
		rel[0] = '\0';
		full[0] = '\0';
		return 0;
	}
	if (strcmp(policy, "root") == 0) {
		if (snprintf(rel, rellen, "compile_commands.json") >= (int)rellen)
			return -1;
	} else if (qstar_graph_build_path(graph, "compile_commands.json", rel,
	    rellen) < 0) {
		return -1;
	}
	return full_path_under_root(graph, rel, full, fulllen);
}

/** compile_commands.json writer를 project policy에 따라 연다. */
static int
open_compile_commands(struct qstar_graph *graph, struct ninja_ctx *ctx)
{
	if (compile_commands_path(graph, ctx->compdb_rel, sizeof(ctx->compdb_rel),
	    ctx->compdb_full, sizeof(ctx->compdb_full)) < 0)
		return qstar_set_error(graph, "qstar: compile_commands path too long");
	if (!ctx->compdb_rel[0])
		return 0;
	if (mkdir_parent_under_root(graph, ctx->compdb_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create compile_commands dir");
	ctx->compdb = fopen(ctx->compdb_full, "w");
	if (!ctx->compdb)
		return qstar_set_error(graph, "qstar: could not write compile_commands.json");
	fputs("[\n", ctx->compdb);
	ctx->compdb_first = 1;
	return 0;
}

/** compile_commands.json record 하나를 추가한다. */
static void
write_compile_command(struct ninja_ctx *ctx, const char *directory, const char *source,
    const char *object, const struct ninja_argv *argv)
{
	if (!ctx->compdb)
		return;
	if (!ctx->compdb_first)
		fputs(",\n", ctx->compdb);
	ctx->compdb_first = 0;
	fputs("  {\"directory\":", ctx->compdb);
	json_string(ctx->compdb, directory);
	fputs(",\"file\":", ctx->compdb);
	json_string(ctx->compdb, source);
	fputs(",\"output\":", ctx->compdb);
	json_string(ctx->compdb, object);
	fputs(",\"command\":", ctx->compdb);
	json_shell_argv(ctx->compdb, argv);
	fputs("}", ctx->compdb);
}

/** compile_commands.json writer를 닫는다. */
static int
close_compile_commands(struct qstar_graph *graph, struct ninja_ctx *ctx)
{
	if (!ctx->compdb)
		return 0;
	fputs("\n]\n", ctx->compdb);
	if (fclose(ctx->compdb) != 0) {
		ctx->compdb = NULL;
		return qstar_set_error(graph, "qstar: could not close compile_commands.json");
	}
	ctx->compdb = NULL;
	return 0;
}

/** Ninja file header와 공통 rule을 출력한다. */
static void
write_ninja_header(FILE *f, const char *builddir, int windows)
{
	fputs("# generated by qstar; do not edit\n", f);
	fputs("ninja_required_version = 1.3\n", f);
	fputs("builddir = ", f);
	ninja_path(f, builddir);
	fputs("\n\n", f);
	fputs("rule qstar_compile\n", f);
	fputs(windows ? "  command = $command\n" :
	    "  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n", f);
	fputs("  depfile = $depfile\n", f);
	fputs("  deps = gcc\n\n", f);
	fputs("rule qstar_compile_nodep\n", f);
	fputs(windows ? "  command = $command\n" :
	    "  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n\n", f);
	fputs("rule qstar_archive\n", f);
	fputs(windows ? "  command = $command\n" :
	    "  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n\n", f);
	fputs("rule qstar_link\n", f);
	fputs(windows ? "  command = $command\n" :
	    "  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n\n", f);
	fputs("rule qstar_generate\n", f);
	fputs("  command = $command\n", f);
	fputs("  description = [qstar] $description\n", f);
	fputs("  restat = 1\n\n", f);
	fputs("rule qstar_stage\n", f);
	fputs("  command = $command\n", f);
	fputs("  description = [qstar] $description\n", f);
	fputs("  restat = 1\n\n", f);
	fputs("rule qstar_run\n", f);
	fputs("  command = $command\n", f);
	fputs("  description = [qstar] $description\n", f);
	fputs("  restat = 1\n\n", f);
}

/** target header inputs를 Ninja implicit dependency로 출력한다. */
static void
write_header_inputs(FILE *f, const struct qstar_target *target)
{
	size_t i;

	if (target->public_headers.len == 0 && target->private_headers.len == 0)
		return;
	fputs(" |", f);
	for (i = 0; i < target->public_headers.len; i++) {
		fputc(' ', f);
		ninja_path(f, target->public_headers.items[i]);
	}
	for (i = 0; i < target->private_headers.len; i++) {
		fputc(' ', f);
		ninja_path(f, target->private_headers.items[i]);
	}
}

/** genrule output list가 target 파일 입력 list에 소비되는지 검사한다. */
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

/** target이 generated action output을 source/header/link input으로 소비하는지 검사한다. */
static int
target_consumes_genrule(const struct qstar_target *target, const struct qstar_genrule *genrule)
{
	return generated_output_in_list(genrule, &target->sources) ||
	    generated_output_in_list(genrule, &target->public_headers) ||
	    generated_output_in_list(genrule, &target->private_headers) ||
	    generated_output_in_list(genrule, &target->link_inputs);
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
		dst[i++] = (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
		    ('0' <= c && c <= '9')) ? (char)c : '_';
	}
	dst[i] = '\0';
}

/** qstar.configure_file용 backend script를 생성한다. */
static int
write_config_script(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_genrule *genrule, char *script_rel, size_t script_rel_len)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	char guard[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	if (genrule->outputs.len != 1)
		return qstar_set_error_origin(graph, genrule->origin_file, genrule->origin_line,
		    "outputs", genrule->label,
		    "qstar: config header '%s' must have exactly one output",
		    genrule->label);
	action_log_name(genrule->label, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "ninja/scripts/%s.sh", name) >=
	    (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, script_rel, script_rel_len) < 0 ||
	    full_path_under_root(graph, script_rel, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: configure_file script path too long");
	if (mkdir_parent_under_root(graph, script_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create configure_file script dir");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write configure_file script");
	config_guard_name(genrule->outputs.items[0], guard, sizeof(guard));
	fputs("set -eu\n", f);
	fputs("mkdir -p ", f);
	{
		char dir[QSTAR_PATH_MAX];
		if (path_dirname(genrule->outputs.items[0], dir, sizeof(dir)) < 0) {
			fclose(f);
			return qstar_set_error(graph, "qstar: configure_file output path too long");
		}
		shell_arg(f, dir);
	}
	fputc('\n', f);
	fputs("cat > ", f);
	shell_arg(f, genrule->outputs.items[0]);
	fputs(" <<'QSTAR_CONFIG_EOF'\n", f);
	fprintf(f, "/* generated by qstar.configure_file: %s */\n", genrule->label);
	fprintf(f, "#ifndef %s\n#define %s\n", guard, guard);
	for (i = 0; i < genrule->args.len; i++)
		write_config_define(f, genrule->args.items[i]);
	fprintf(f, "#endif /* %s */\n", guard);
	fputs("QSTAR_CONFIG_EOF\n", f);
	fclose(f);
	(void)ctx;
	return 0;
}

/** run_target용 Ninja wrapper script를 생성한다. */
static int
write_run_script(struct qstar_graph *graph, const struct qstar_target *target,
    const char *action_id, const struct ninja_argv *argv, const char *stamp,
    const char *description, char *script_rel, size_t script_rel_len)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	char stdout_rel[QSTAR_PATH_MAX], stderr_rel[QSTAR_PATH_MAX];
	char logdir[QSTAR_PATH_MAX], stamp_dir[QSTAR_PATH_MAX], replay_rel[QSTAR_PATH_MAX];
	int timeout;
	FILE *f;

	action_log_name(action_id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "ninja/scripts/%s.sh", name) >=
	    (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, script_rel, script_rel_len) < 0 ||
	    full_path_under_root(graph, script_rel, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: run_target script path too long");
	if (build_log_rel(graph, name, ".stdout", stdout_rel, sizeof(stdout_rel)) < 0 ||
	    build_log_rel(graph, name, ".stderr", stderr_rel, sizeof(stderr_rel)) < 0 ||
	    qstar_graph_build_path(graph, "logs", logdir, sizeof(logdir)) < 0 ||
	    qstar_graph_build_path(graph, "logs/last-failure.replay", replay_rel,
	    sizeof(replay_rel)) < 0 ||
	    path_dirname(stamp, stamp_dir, sizeof(stamp_dir)) < 0)
		return qstar_set_error(graph, "qstar: run_target log path too long");
	if (mkdir_parent_under_root(graph, script_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create run_target script dir");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write run_target script");
	timeout = target->run_timeout_sec > 0 ? target->run_timeout_sec :
	    QSTAR_NINJA_RUN_TIMEOUT_SEC;
	fputs("set -u\n", f);
	fputs("label=", f);
	shell_arg(f, target->label);
	fputs("\nexpect_contains=", f);
	shell_arg(f, target->run_expect_contains && *target->run_expect_contains ? target->run_expect_contains : "");
	fputs("\nexpect_file_path=", f);
	shell_arg(f, target->run_expect_file && *target->run_expect_file ?
	    target->run_expect_file : "");
	fputs("\nexpect_file_display=", f);
	shell_arg(f, target->run_expect_file && *target->run_expect_file ?
	    target->run_expect_file : "<none>");
	fputs("\nstdout=", f);
	shell_arg(f, stdout_rel);
	fputs("\nstderr=", f);
	shell_arg(f, stderr_rel);
	fputs("\nstamp=", f);
	shell_arg(f, stamp);
	fputs("\nreplay=", f);
	shell_arg(f, replay_rel);
	fprintf(f, "\ntimeout=%d\n", timeout);
	fputs("mkdir -p ", f);
	shell_arg(f, logdir);
	fputc(' ', f);
	shell_arg(f, stamp_dir);
	fputc('\n', f);
	fputs("write_replay() {\n", f);
	fputs("  kind=$1\n", f);
	fputs("  {\n", f);
	fputs("    printf '%s\\n' '# qstar failure replay v2'\n", f);
	fputs("    printf 'cd %s\\n' ", f);
	shell_arg(f, graph->package_root ? graph->package_root : ".");
	fputc('\n', f);
	fputs("    printf 'failure_kind=%s\\n' \"$kind\"\n", f);
	fputs("    cat <<'QSTAR_REPLAY_DESCRIPTION'\n", f);
	write_log_description(f, description);
	fputs("QSTAR_REPLAY_DESCRIPTION\n", f);
	fputs("    printf 'label=%s\\n' \"$label\"\n", f);
	fputs("    printf 'stdout=%s\\n' \"$stdout\"\n", f);
	fputs("    printf 'stderr=%s\\n' \"$stderr\"\n", f);
	fputs("    if [ -n \"$expect_contains\" ]; then printf 'expect_contains=%s\\n' \"$expect_contains\"; else printf '%s\\n' 'expect_contains=<none>'; fi\n", f);
	fputs("    printf 'expect_file=%s\\n' \"$expect_file_display\"\n", f);
	fputs("    printf '%s\\n' 'response_file path=<none> style=none digest=<none>'\n", f);
	fputs("    cat <<'QSTAR_REPLAY_CMD'\n", f);
	shell_argv(f, argv);
	fputs("\nQSTAR_REPLAY_CMD\n", f);
	fputs("  } > \"$replay\"\n", f);
	fputs("}\n", f);
	fputs("printf 'run_target label=%s command=argv timeout_sec=%d expect_contains=%s expect_file=%s backend=ninja\\n' \"$label\" \"$timeout\" \"${expect_contains:-<none>}\" \"$expect_file_display\"\n", f);
	fputs("rm -f \"$stdout\" \"$stderr\"\n", f);
	fputs("timeout_flag=\"$stamp.timeout\"\n", f);
	fputs("rm -f \"$timeout_flag\"\n", f);
	fputs("(\n  ", f);
	shell_argv(f, argv);
	fputs("\n) >\"$stdout\" 2>\"$stderr\" &\n", f);
	fputs("pid=$!\n", f);
	fputs("watchdog=\n", f);
	fputs("if [ \"$timeout\" -gt 0 ]; then\n", f);
	fputs("  (\n", f);
	fputs("    sleep_pid=\n", f);
	fputs("    trap 'if [ -n \"$sleep_pid\" ]; then kill \"$sleep_pid\" 2>/dev/null || true; fi; exit 0' TERM INT\n", f);
	fputs("    sleep \"$timeout\" &\n", f);
	fputs("    sleep_pid=$!\n", f);
	fputs("    wait \"$sleep_pid\" 2>/dev/null || exit 0\n", f);
	fputs("    if kill -0 \"$pid\" 2>/dev/null; then printf 'timeout\\n' >\"$timeout_flag\"; kill \"$pid\" 2>/dev/null || true; fi\n", f);
	fputs("  ) &\n", f);
	fputs("  watchdog=$!\n", f);
	fputs("fi\n", f);
	fputs("wait \"$pid\"\n", f);
	fputs("rc=$?\n", f);
	fputs("if [ -n \"$watchdog\" ]; then kill \"$watchdog\" 2>/dev/null || true; wait \"$watchdog\" 2>/dev/null || true; fi\n", f);
	fputs("if [ -f \"$timeout_flag\" ]; then\n", f);
	fputs("  write_replay timeout\n", f);
	fputs("  printf 'run_target_result label=%s status=timeout timeout_sec=%d replay=%s stdout=%s stderr=%s backend=ninja\\n' \"$label\" \"$timeout\" \"$replay\" \"$stdout\" \"$stderr\"\n", f);
	fputs("  exit 124\n", f);
	fputs("fi\n", f);
	fputs("if [ \"$rc\" -ne 0 ]; then\n", f);
	fputs("  write_replay exit-code\n", f);
	fputs("  printf 'run_target_result label=%s status=exit-code exit=%d replay=%s stdout=%s stderr=%s backend=ninja\\n' \"$label\" \"$rc\" \"$replay\" \"$stdout\" \"$stderr\"\n", f);
	fputs("  exit \"$rc\"\n", f);
	fputs("fi\n", f);
	fputs("if [ -n \"$expect_contains\" ]; then\n", f);
	fputs("  found=0\n", f);
	fputs("  source=stdout\n", f);
	fputs("  path=$stdout\n", f);
	fputs("  if grep -F -- \"$expect_contains\" \"$stdout\" >/dev/null 2>&1; then found=1; fi\n", f);
	fputs("  if [ \"$found\" -eq 0 ] && grep -F -- \"$expect_contains\" \"$stderr\" >/dev/null 2>&1; then found=1; source=stderr; path=$stderr; fi\n", f);
	fputs("  if [ \"$found\" -eq 0 ] && [ -n \"$expect_file_path\" ] && [ -f \"$expect_file_path\" ] && grep -F -- \"$expect_contains\" \"$expect_file_path\" >/dev/null 2>&1; then found=1; source=file; path=$expect_file_path; fi\n", f);
	fputs("  if [ \"$found\" -eq 0 ]; then\n", f);
	fputs("    write_replay expect-missing\n", f);
	fputs("    printf 'run_target_result label=%s status=expect-missing expect_contains=%s stdout=%s stderr=%s expect_file=%s replay=%s backend=ninja\\n' \"$label\" \"$expect_contains\" \"$stdout\" \"$stderr\" \"$expect_file_display\" \"$replay\"\n", f);
	fputs("    exit 125\n", f);
	fputs("  fi\n", f);
	fputs("  printf 'run_expect label=%s status=matched contains=%s source=%s path=%s backend=ninja\\n' \"$label\" \"$expect_contains\" \"$source\" \"$path\"\n", f);
	fputs("fi\n", f);
	fputs("printf 'qstar run_target stamp\\n' >\"$stamp\"\n", f);
	fputs("printf 'run_target_result label=%s status=pass exit=0 stdout=%s stderr=%s backend=ninja\\n' \"$label\" \"$stdout\" \"$stderr\"\n", f);
	fclose(f);
	return 0;
}

/** generated action argv가 package root 밖 path를 직접 참조하는지 검사한다. */
static int
generated_arg_escapes_package(const char *arg)
{
	return arg && (arg[0] == '/' || strcmp(arg, "..") == 0 ||
	    strncmp(arg, "../", 3) == 0 || strstr(arg, "/../") ||
	    strstr(arg, "=../") || strstr(arg, ":../"));
}

/** qstar.target_file placeholder token에서 artifact path를 해석한다. */
static int
resolve_target_file_token(struct qstar_graph *graph, const char *arg, char *dst,
    size_t dstlen)
{
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX], artifact[64];
	int rc;

	rc = qstar_target_file_token_parse(arg, label, sizeof(label), artifact,
	    sizeof(artifact));
	if (rc == 0) {
		if (snprintf(dst, dstlen, "%s", arg) >= (int)dstlen)
			return qstar_set_error(graph, "qstar: command argv item is too long");
		return 0;
	}
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	target = find_target(graph, label);
	if (target) {
		if (strcmp(target->kind, "group") == 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "target_file", target->label,
			    "qstar: qstar.target_file cannot reference group target '%s' because it has no artifact",
			    label);
		if (strcmp(target->kind, "objectlib") == 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "target_file", target->label,
			    "qstar: qstar.target_file cannot reference objectlib target '%s' because object libraries have no artifact; consume it with objects = { ... }",
			    label);
		return qstar_graph_target_artifact_path(graph, target, artifact, dst, dstlen);
	}
	genrule = qstar_graph_find_genrule(graph, label);
	if (genrule && genrule->outputs.len > 0) {
		if (artifact[0])
			return qstar_set_error(graph,
			    "qstar: target_file artifact selector '%s' cannot be used with generated action '%s'",
			    artifact, label);
		if (snprintf(dst, dstlen, "%s", genrule->outputs.items[0]) >=
		    (int)dstlen)
			return qstar_set_error(graph,
			    "qstar: target_file generated output path is too long");
		return 0;
	}
	return qstar_set_error(graph, "qstar: target_file references unknown target '%s'",
	    label);
}

/** stage source token을 manifest용 source kind/producer로 분류한다. */
static void
stage_source_kind(struct qstar_graph *graph, const char *src_token,
    const char *src_rel, const char **kind, const char **producer,
    const char **artifact, char *producer_buf, size_t producer_buf_len,
    char *artifact_buf, size_t artifact_buf_len)
{
	const struct qstar_genrule *owner;
	const struct qstar_target *target;
	struct qstar_target_artifact_map map;
	char label[QSTAR_PATH_MAX], selector[64];
	int rc;

	*kind = "file";
	*producer = "<none>";
	*artifact = "";
	if (artifact_buf_len)
		artifact_buf[0] = '\0';
	rc = qstar_target_file_token_parse(src_token, label, sizeof(label), selector,
	    sizeof(selector));
	if (rc == 1) {
		target = find_target(graph, label);
		if (target) {
			*kind = "target";
			snprintf(producer_buf, producer_buf_len, "%s", label);
			*producer = producer_buf;
			if (selector[0]) {
				snprintf(artifact_buf, artifact_buf_len, "%s", selector);
			} else if (qstar_graph_target_artifact_map(graph, target, &map) == 0) {
				size_t i;

				artifact_buf[0] = '\0';
				for (i = 0; i < map.len; i++) {
					if (map.items[i].primary) {
						snprintf(artifact_buf, artifact_buf_len, "%s",
						    map.items[i].id);
						break;
					}
				}
			}
			*artifact = artifact_buf;
			return;
		}
		if (qstar_graph_find_genrule(graph, label)) {
			*kind = "custom_target";
			snprintf(producer_buf, producer_buf_len, "%s", label);
			*producer = producer_buf;
			*artifact = "primary";
			return;
		}
	}
	owner = qstar_graph_find_output_owner(graph, src_rel);
	if (owner) {
		*kind = "custom_output";
		snprintf(producer_buf, producer_buf_len, "%s", owner->label);
		*producer = producer_buf;
		*artifact = "output";
	}
}

/** stage를 Ninja에서 실행할 package-local wrapper script로 쓴다. */
static int
write_stage_script(struct qstar_graph *graph, const struct qstar_stage *stage,
    const char *action_id, const char *manifest, const char *description,
    char *script_rel, size_t script_rel_len)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	char manifest_dir[QSTAR_PATH_MAX], root_rel[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	action_log_name(action_id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "ninja/scripts/%s.sh", name) >=
	    (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, script_rel, script_rel_len) < 0 ||
	    full_path_under_root(graph, script_rel, full, sizeof(full)) < 0 ||
	    path_dirname(manifest, manifest_dir, sizeof(manifest_dir)) < 0)
		return qstar_set_error(graph, "qstar: stage script path too long");
	if (mkdir_parent_under_root(graph, script_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create stage script dir");
	snprintf(root_rel, sizeof(root_rel), "%s",
	    stage->root && *stage->root ? stage->root : ".");
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write stage script");
	fputs("set -eu\n", f);
	fputs("label=", f);
	shell_arg(f, stage->label);
	fputs("\nroot=", f);
	shell_arg(f, root_rel);
	fputs("\nmanifest=", f);
	shell_arg(f, manifest);
	fputs("\nmanifest_tmp=", f);
	shell_arg(f, manifest);
	fputs(".tmp\nmanifest_dir=", f);
	shell_arg(f, manifest_dir);
	fputs("\ndescription=", f);
	shell_arg(f, description && *description ? description : "<none>");
	fputs("\nmkdir -p \"$manifest_dir\" \"$root\"\n", f);
	fputs("printf 'qstar stage v2\\n'\n", f);
	fputs("printf 'root %s\\n' \"$label\"\n", f);
	fputs("printf 'stage-root %s\\n' \"$root\"\n", f);
	fputs("printf 'mode copy\\n'\n", f);
	fputs("printf 'stage_manifest %s\\n' \"$manifest\"\n", f);
	fprintf(f,
	    "printf 'stage_layout label=%%s root=%%s files=%zu status=ok\\n' \"$label\" \"$root\"\n",
	    stage->srcs.len);
	fputs("cat > \"$manifest_tmp\" <<'QSTAR_STAGE_MANIFEST_HEAD'\n", f);
	fputs("{\n  \"schema\":\"qstar-stage-manifest-v2\",\n  \"label\":", f);
	json_string(f, stage->label);
	fputs(",\n  \"root\":", f);
	json_string(f, root_rel);
	fputs(",\n  \"mode\":\"copy\",\n  \"layout\":{\"status\":\"ok\",\"file_count\":", f);
	fprintf(f, "%zu", stage->srcs.len);
	fputs("},\n  \"entries\":[\n", f);
	fputs("QSTAR_STAGE_MANIFEST_HEAD\n", f);
	fputs("entry_index=0\n", f);
	for (i = 0; i < stage->srcs.len; i++) {
		char src_rel[QSTAR_PATH_MAX], dst_stage_rel[QSTAR_PATH_MAX];
		char producer_buf[QSTAR_PATH_MAX], artifact_buf[64];
		const char *kind, *producer, *artifact, *artifact_display;

		if (resolve_target_file_token(graph, stage->srcs.items[i], src_rel,
		    sizeof(src_rel)) < 0) {
			fclose(f);
			return -1;
		}
		if (qstar_path_join(root_rel, stage->dsts.items[i], dst_stage_rel,
		    sizeof(dst_stage_rel)) < 0) {
			fclose(f);
			return qstar_set_error_origin(graph, stage->origin_file,
			    stage->origin_line, "package-failure", stage->label,
			    "qstar: stage destination path too long");
		}
		stage_source_kind(graph, stage->srcs.items[i], src_rel, &kind,
		    &producer, &artifact, producer_buf, sizeof(producer_buf),
		    artifact_buf, sizeof(artifact_buf));
		artifact_display = artifact && *artifact ? artifact : "<none>";
		fputs("src=", f);
		shell_arg(f, src_rel);
		fputs("\ndst=", f);
		shell_arg(f, dst_stage_rel);
		fputs("\nkind=", f);
		shell_arg(f, kind);
		fputs("\nproducer=", f);
		shell_arg(f, producer);
		fputs("\nartifact=", f);
		shell_arg(f, artifact_display);
		fputs("\nmanifest_artifact=", f);
		shell_arg(f, artifact && *artifact ? artifact : "");
		fputs("\nif [ -f \"$dst\" ] && cmp -s \"$src\" \"$dst\"; then action=unchanged; exists=yes\n", f);
		fputs("elif [ -e \"$dst\" ]; then action=would-update; exists=yes\n", f);
		fputs("else action=would-create; exists=no\nfi\n", f);
		fputs("if [ ! -f \"$src\" ]; then printf 'stage_result label=%s status=fail failure_kind=package-failure replay=<none>\\n' \"$label\"; exit 1; fi\n", f);
		fputs("printf 'stage_file src=%s dst=%s mode=copy kind=%s producer=%s artifact=%s description=\"%s\"\\n' \"$src\" \"$dst\" \"$kind\" \"$producer\" \"$artifact\" \"$description\"\n", f);
		fputs("printf 'stage_diff dst=%s action=%s exists=%s\\n' \"$dst\" \"$action\" \"$exists\"\n", f);
		fputs("mkdir -p \"$(dirname \"$dst\")\"\n", f);
		fputs("cp \"$src\" \"$dst\"\n", f);
		fputs("if [ \"$entry_index\" -gt 0 ]; then printf ',\\n' >> \"$manifest_tmp\"; fi\n", f);
		fputs("printf '    {\"src\":%s,\"dst\":%s,\"action\":\"%s\",\"kind\":%s,\"producer\":%s,\"artifact\":%s}' ", f);
		shell_json_string_arg(f, src_rel);
		fputc(' ', f);
		shell_json_string_arg(f, dst_stage_rel);
		fputs(" \"$action\" ", f);
		shell_json_string_arg(f, kind);
		fputc(' ', f);
		shell_json_string_arg(f, producer);
		fputc(' ', f);
		shell_json_string_arg(f, artifact && *artifact ? artifact : "");
		fputs(" >> \"$manifest_tmp\"\n", f);
		fputs("entry_index=$((entry_index + 1))\n", f);
	}
	fputs("printf '\\n  ],\\n  \"status\":\"ok\"\\n}\\n' >> \"$manifest_tmp\"\n", f);
	fputs("mv \"$manifest_tmp\" \"$manifest\"\n", f);
	fputs("printf 'status ok\\n'\n", f);
	fclose(f);
	return 0;
}

/** qstar.stage_dir placeholder를 Ninja run argv용 path로 해석한다. */
static int
resolve_stage_dir_token(struct qstar_graph *graph, const char *arg, char *dst,
    size_t dstlen)
{
	const struct qstar_stage *stage;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_stage_dir_token_label(arg, label, sizeof(label));
	if (rc == 0)
		return 0;
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed stage_dir placeholder");
	stage = qstar_graph_find_stage(graph, label);
	if (!stage)
		return qstar_set_error(graph, "qstar: stage_dir references unknown stage '%s'",
		    label);
	if (snprintf(dst, dstlen, "%s", stage->root && *stage->root ? stage->root : ".") >=
	    (int)dstlen)
		return qstar_set_error(graph, "qstar: stage_dir root path is too long");
	return 1;
}

/** run_target command token을 Ninja argv path로 해석한다. */
static int
resolve_run_command_token(struct qstar_graph *graph, const char *arg, char *dst,
    size_t dstlen)
{
	int rc;

	rc = resolve_stage_dir_token(graph, arg, dst, dstlen);
	if (rc < 0)
		return -1;
	if (rc == 1)
		return 0;
	return resolve_target_file_token(graph, arg, dst, dstlen);
}

/** target closure를 Ninja graph에 중복 없이 emit한다. */
static int
emit_target_closure(struct qstar_graph *graph, struct ninja_ctx *ctx, const char *label)
{
	return qstar_graph_visit_closure(graph, label, emit_target, ctx);
}

/** genrule input/argv item이 가리키는 producer edge를 먼저 emit한다. */
static int
emit_genrule_item_producer(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_genrule *genrule, const char *item)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "inputs", genrule->label,
		    "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (find_target(graph, label))
			return emit_target_closure(graph, ctx, label);
		owner = qstar_graph_find_genrule(graph, label);
		if (owner)
			return emit_genrule_edge(graph, ctx, NULL, owner);
		return 0;
	}
	owner = qstar_graph_find_output_owner(graph, item);
	if (owner && owner != genrule)
		return emit_genrule_edge(graph, ctx, NULL, owner);
	return 0;
}

/** target link_inputs 항목이 가리키는 producer edge를 먼저 emit한다. */
static int
emit_target_link_input_producer(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const char *item)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "link_inputs", target->label,
		    "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (find_target(graph, label))
			return emit_target_closure(graph, ctx, label);
		owner = qstar_graph_find_genrule(graph, label);
		if (owner)
			return emit_genrule_edge(graph, ctx, NULL, owner);
		return 0;
	}
	owner = qstar_graph_find_output_owner(graph, item);
	if (owner)
		return emit_genrule_edge(graph, ctx, NULL, owner);
	return 0;
}

/** target link_inputs의 producer edge를 emit한다. */
static int
emit_target_link_input_producers(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	size_t i;

	for (i = 0; i < target->link_inputs.len; i++) {
		if (emit_target_link_input_producer(graph, ctx, target,
		    target->link_inputs.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** target deps/private_deps에 직접 적힌 generated action producer를 먼저 emit한다. */
static int
emit_target_dep_genrule_producers(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	const struct qstar_string_list *lists[2];
	size_t li, i;

	lists[0] = &target->deps;
	lists[1] = &target->private_deps;
	for (li = 0; li < 2; li++) {
		for (i = 0; i < lists[li]->len; i++) {
			if (lists[li]->items[i][0] == '@')
				continue;
			genrule = qstar_graph_find_genrule(graph, lists[li]->items[i]);
			if (genrule && emit_genrule_edge(graph, ctx, NULL, genrule) < 0)
				return -1;
		}
	}
	return 0;
}

/** genrule dependency item을 Ninja edge input으로 출력한다. */
static int
write_genrule_dep_item(struct qstar_graph *graph, FILE *f, const char *item)
{
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 1) {
		target = find_target(graph, label);
		if (target) {
			if (ninja_alias_path(graph, label, alias, sizeof(alias)) < 0)
				return qstar_set_error(graph, "qstar: ninja alias path too long");
			fputc(' ', f);
			ninja_path(f, alias);
			return 0;
		}
		genrule = qstar_graph_find_genrule(graph, label);
		if (genrule && genrule->outputs.len > 0) {
			fputc(' ', f);
			ninja_path(f, genrule->outputs.items[0]);
		}
		return 0;
	}
	if (resolve_target_file_token(graph, item, resolved, sizeof(resolved)) < 0)
		return -1;
	fputc(' ', f);
	ninja_path(f, resolved);
	return 0;
}

/** genrule edge의 입력 dependency를 Ninja syntax로 출력한다. */
static int
write_genrule_deps(struct qstar_graph *graph, FILE *f, const struct qstar_genrule *genrule)
{
	size_t i;

	if (genrule->inputs.len == 0 && genrule->args.len == 0)
		return 0;
	fputs(" |", f);
	for (i = 0; i < genrule->inputs.len; i++) {
		if (write_genrule_dep_item(graph, f, genrule->inputs.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < genrule->args.len; i++) {
		char label[QSTAR_PATH_MAX];
		int rc;

		rc = qstar_target_file_token_label(genrule->args.items[i], label,
		    sizeof(label));
		if (rc < 0)
			return qstar_set_error(graph, "qstar: malformed target_file placeholder");
		if (rc == 1 && write_genrule_dep_item(graph, f, genrule->args.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** custom_target 또는 configure_file을 Ninja generate edge로 lower한다. */
static int
emit_genrule_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_genrule *genrule)
{
	struct ninja_argv argv;
	char action_id[QSTAR_PATH_MAX], resolved_tool[QSTAR_PATH_MAX];
	char tool_mode[64], tool_error[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX];
	char script_rel[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	size_t i;
	int index;

	index = genrule_index(graph, genrule);
	if (index < 0)
		return qstar_set_error(graph, "qstar: invalid ninja generated action");
	if (ctx->genrule_state[index] == 2)
		return 0;
	if (ctx->genrule_state[index] == 1)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "inputs", genrule->label,
		    "qstar: circular generated action dependency at '%s'",
		    genrule->label);
	ctx->genrule_state[index] = 1;
	for (i = 0; i < genrule->inputs.len; i++) {
		if (emit_genrule_item_producer(graph, ctx, genrule,
		    genrule->inputs.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < genrule->args.len; i++) {
		if (emit_genrule_item_producer(graph, ctx, genrule,
		    genrule->args.items[i]) < 0)
			return -1;
	}
	memset(&argv, 0, sizeof(argv));
	snprintf(action_id, sizeof(action_id), "%s:generate:0", genrule->label);
	if (genrule->config_header) {
		if (write_config_script(graph, ctx, genrule, script_rel,
		    sizeof(script_rel)) < 0)
			return -1;
		if (ninja_argv_push(graph, &argv, "sh") < 0 ||
		    ninja_argv_push(graph, &argv, script_rel) < 0)
			goto fail;
	} else {
		if (qstar_resolve_command_tool_for_genrule(graph, target, genrule,
		    genrule->tool, resolved_tool, sizeof(resolved_tool), tool_mode,
		    sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0)
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "command", genrule->label, "%s",
			    tool_error);
		if (ninja_argv_push(graph, &argv, resolved_tool) < 0)
			goto fail;
		for (i = 0; i < genrule->args.len; i++) {
			char resolved[QSTAR_PATH_MAX];

			if (generated_arg_escapes_package(genrule->args.items[i]))
				return qstar_set_error_origin(graph, genrule->origin_file,
				    genrule->origin_line, "args", genrule->label,
				    "qstar: generated action arg '%s' escapes package root",
				    genrule->args.items[i]);
			if (resolve_target_file_token(graph, genrule->args.items[i],
			    resolved, sizeof(resolved)) < 0)
				goto fail;
			if (ninja_argv_push(graph, &argv, resolved) < 0)
				goto fail;
		}
	}
	if (qstar_action_description_generate(genrule, description, sizeof(description)) < 0)
		goto fail;
	for (i = 0; i < genrule->outputs.len; i++) {
		if (mkdir_parent_under_root(graph, genrule->outputs.items[i]) < 0)
			goto fail;
	}
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build", ctx->ninja);
	for (i = 0; i < genrule->outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, genrule->outputs.items[i]);
	}
	fputs(": qstar_generate", ctx->ninja);
	if (genrule->config_header) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, script_rel);
	}
	if (write_genrule_deps(graph, ctx->ninja, genrule) < 0)
		goto fail;
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	if (!genrule->config_header &&
	    !qstar_platform_is_windows(qstar_graph_platform(graph))) {
		fputs("mkdir -p", ctx->ninja);
		for (i = 0; i < genrule->outputs.len; i++) {
			char dir[QSTAR_PATH_MAX];

			if (path_dirname(genrule->outputs.items[i], dir, sizeof(dir)) < 0)
				goto fail;
			fputc(' ', ctx->ninja);
			shell_arg(ctx->ninja, dir);
		}
		fputs(" && ", ctx->ninja);
	}
	if (write_edge_command(graph, ctx, action_id, NULL, &argv, description, NULL,
	    &genrule->outputs) < 0)
		goto fail;
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	if (ninja_alias_path(graph, genrule->label, alias, sizeof(alias)) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", genrule->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony", ctx->ninja);
	for (i = 0; i < genrule->outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, genrule->outputs.items[i]);
	}
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", genrule->label);
	ctx->edge_count += 2;
	ctx->genrule_state[index] = 2;
	ninja_argv_free(&argv);
	return 0;
fail:
	ninja_argv_free(&argv);
	return -1;
}

/** target이 소비하는 generated action edge를 먼저 emit한다. */
static int
emit_consumed_genrules(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (!target_consumes_genrule(target, &graph->genrules[i]))
			continue;
		if (emit_genrule_edge(graph, ctx, target, &graph->genrules[i]) < 0)
			return -1;
	}
	return 0;
}

/** compile action 하나를 Ninja edge와 compile_commands record로 lower한다. */
static int
emit_compile_edge_from_source(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_target *source_owner,
    const struct qstar_resolved_toolchain *toolchain, size_t index,
    size_t action_index, const char *object_override, const char *depfile_override)
{
	struct qstar_source_info source;
	const struct qstar_provider_source_unit *provider_unit;
	struct qstar_string_list includes;
	struct ninja_argv argv;
	char object[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], out_dir[QSTAR_PATH_MAX];
	char action_id[QSTAR_PATH_MAX];
	char description[QSTAR_PATH_MAX], std_arg[128];
	const char *compiler;
	int is_asm, is_cxx, provider_source, wants_depfile;
	size_t i;

	memset(&argv, 0, sizeof(argv));
	qstar_target_source_classify(source_owner, index, &source);
	if (!qstar_source_requires_compile(&source))
		return 0;
	if (validate_ninja_compile_source(graph, source_owner, &source, index) < 0)
		return -1;
	if (object_override)
		snprintf(object, sizeof(object), "%s", object_override);
	else if (qstar_graph_object_output_path(graph, target, index, object,
	    sizeof(object)) < 0)
		return qstar_set_error(graph, "qstar: ninja object output path too long");
	if (depfile_override)
		snprintf(depfile, sizeof(depfile), "%s", depfile_override);
	else if (qstar_graph_depfile_output_path(graph, target, index, depfile,
	    sizeof(depfile)) < 0)
		return qstar_set_error(graph, "qstar: ninja depfile output path too long");
	if (path_dirname(object, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja object output path too long");
	if (mkdir_parent_under_root(graph, object) < 0 ||
	    mkdir_parent_under_root(graph, depfile) < 0)
		return qstar_set_error(graph, "qstar: could not create ninja object dir");
	memset(&includes, 0, sizeof(includes));
	if (collect_compile_include_dirs(graph, target, &includes) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	is_asm = qstar_source_is_asm(&source);
	is_cxx = strcmp(source.provider, "cxx") == 0;
	provider_unit = qstar_target_provider_source_unit(source_owner, index);
	if (provider_unit && source_owner != target) {
		qstar_string_list_free(&includes);
		return qstar_set_error_origin(graph, source_owner->origin_file,
		    source_owner->origin_line, "sources", source_owner->label,
		    "qstar: provider source token '%s' cannot be used from compile_context = \"consumer\" objectlib yet; use raw provider source strings or compile_context = \"own\"",
		    source_owner->sources.items[index]);
	}
	provider_source = provider_unit != NULL;
	wants_depfile = !provider_source &&
	    (strcmp(source.provider, "c") == 0 || is_cxx ||
	    qstar_source_uses_asm_preprocessor(target, &source));
	snprintf(action_id, sizeof(action_id), "%s:compile:%zu", target->label,
	    action_index);
	snprintf(std_arg, sizeof(std_arg), "-std=%s",
	    target->cxx_standard ? target->cxx_standard : "");
	if (provider_unit) {
		const char *provider_depfile;

		wants_depfile = provider_unit->action.wants_depfile;
		provider_depfile = provider_unit->action.depfile &&
		    *provider_unit->action.depfile ? provider_unit->action.depfile :
		    depfile;
		if (ninja_argv_push_provider_template(graph, &argv, target,
		    toolchain, &provider_unit->action.argv) < 0)
			goto fail;
		if (qstar_action_description_compile(source_owner, &source, object,
		    description, sizeof(description)) < 0)
			goto fail;
		for (i = 0; i < provider_unit->action.outputs.len; i++) {
			if (mkdir_parent_under_root(graph,
			    provider_unit->action.outputs.items[i]) < 0)
				goto fail;
		}
		fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
		fputs("build", ctx->ninja);
		for (i = 0; i < provider_unit->action.outputs.len; i++) {
			fputc(' ', ctx->ninja);
			ninja_path(ctx->ninja, provider_unit->action.outputs.items[i]);
		}
		fprintf(ctx->ninja, ": %s", wants_depfile ? "qstar_compile" :
		    "qstar_compile_nodep");
		for (i = 0; i < provider_unit->action.inputs.len; i++) {
			fputc(' ', ctx->ninja);
			ninja_path(ctx->ninja, provider_unit->action.inputs.items[i]);
		}
		fputc('\n', ctx->ninja);
		fputs("  command = ", ctx->ninja);
		if (write_edge_command(graph, ctx, action_id, toolchain, &argv,
		    description, &provider_unit->action.env,
		    &provider_unit->action.outputs) < 0)
			goto fail;
		fputc('\n', ctx->ninja);
		if (wants_depfile) {
			fputs("  depfile = ", ctx->ninja);
			ninja_path(ctx->ninja, provider_depfile);
			fputc('\n', ctx->ninja);
		}
		fputs("  out_dir = ", ctx->ninja);
		shell_arg(ctx->ninja, out_dir);
		fputs("\n  description = ", ctx->ninja);
		ninja_var_text(ctx->ninja, description);
		fputc('\n', ctx->ninja);
		fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
		write_compile_command(ctx, graph->package_root ? graph->package_root : ".",
		    source_owner->sources.items[index], object, &argv);
		ctx->edge_count++;
		qstar_string_list_free(&includes);
		ninja_argv_free(&argv);
		return 0;
	}
	compiler = qstar_resolved_toolchain_provider_tool(toolchain, source.provider,
	    source.provider_role);
	if (!compiler && !provider_source)
		compiler = toolchain->cc;
	if (ninja_argv_push_tool_role(graph, &argv, target, toolchain,
	    qstar_source_toolset_role(&source),
	    compiler) < 0)
		goto fail;
	if (!provider_source && target_compile_needs_pic(target, toolchain, is_asm) &&
	    ninja_argv_push(graph, &argv, "-fPIC") < 0)
		goto fail;
	if (!provider_source && is_asm &&
	    (ninja_argv_push(graph, &argv, "-x") < 0 ||
	    ninja_argv_push(graph, &argv,
	    qstar_source_uses_asm_preprocessor(target, &source) ?
	    "assembler-with-cpp" : "assembler") < 0))
		goto fail;
	if (ninja_argv_push(graph, &argv, "-c") < 0 ||
	    ninja_argv_push(graph, &argv, source_owner->sources.items[index]) < 0 ||
	    ninja_argv_push(graph, &argv, "-o") < 0 ||
	    ninja_argv_push(graph, &argv, object) < 0)
		goto fail;
	if (wants_depfile &&
	    (ninja_argv_push(graph, &argv, "-MMD") < 0 ||
	    ninja_argv_push(graph, &argv, "-MF") < 0 ||
	    ninja_argv_push(graph, &argv, depfile) < 0))
		goto fail;
	if (!provider_source && is_cxx && target->cxx_standard && target->cxx_standard[0] &&
	    ninja_argv_push(graph, &argv, std_arg) < 0)
		goto fail;
	for (i = 0; !provider_source && !is_asm && !is_cxx && i < target->cflags.len; i++) {
		if (ninja_argv_push(graph, &argv, target->cflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && is_cxx && i < target->cxxflags.len; i++) {
		if (ninja_argv_push(graph, &argv, target->cxxflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && is_asm && i < target->asm_compile_options.len; i++) {
		if (ninja_argv_push(graph, &argv, target->asm_compile_options.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && i < graph->build_context.compile_options.len; i++) {
		if (ninja_argv_push(graph, &argv, graph->build_context.compile_options.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && i < graph->build_context.include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, graph->build_context.include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && !is_asm && i < includes.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, includes.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && is_asm && i < target->asm_include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, target->asm_include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !provider_source && !is_asm && i < target->system_include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-isystem") < 0 ||
		    ninja_argv_push(graph, &argv, target->system_include_dirs.items[i]) < 0)
			goto fail;
	}
	if (qstar_action_description_compile(source_owner, &source, object, description,
	    sizeof(description)) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, object);
	fprintf(ctx->ninja, ": %s ", wants_depfile ? "qstar_compile" :
	    "qstar_compile_nodep");
	ninja_path(ctx->ninja, source_owner->sources.items[index]);
	write_header_inputs(ctx->ninja, target);
	if (source_owner != target)
		write_header_inputs(ctx->ninja, source_owner);
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	if (write_edge_command(graph, ctx, action_id, toolchain, &argv, description,
	    NULL, NULL) < 0)
		goto fail;
	fputc('\n', ctx->ninja);
	if (wants_depfile) {
		fputs("  depfile = ", ctx->ninja);
		ninja_path(ctx->ninja, depfile);
		fputc('\n', ctx->ninja);
	}
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	write_compile_command(ctx, graph->package_root ? graph->package_root : ".",
	    source_owner->sources.items[index], object, &argv);
	ctx->edge_count++;
	qstar_string_list_free(&includes);
	ninja_argv_free(&argv);
	return 0;
fail:
	qstar_string_list_free(&includes);
	ninja_argv_free(&argv);
	return -1;
}

static int
emit_compile_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    size_t index)
{
	return emit_compile_edge_from_source(graph, ctx, target, target, toolchain,
	    index, index, NULL, NULL);
}

/** target dependency labels를 Ninja order-only dependency alias로 출력한다. */
static int
write_dep_aliases(struct qstar_graph *graph, FILE *f, const struct qstar_target *target,
    const struct qstar_string_list *deps)
{
	const struct qstar_genrule *genrule;
	char alias[QSTAR_PATH_MAX];
	size_t i, j;

	for (i = 0; i < deps->len; i++) {
		if (find_target(graph, deps->items[i])) {
			if (ninja_alias_path(graph, deps->items[i], alias,
			    sizeof(alias)) < 0)
				return qstar_set_error(graph,
				    "qstar: ninja alias path too long");
			fputc(' ', f);
			ninja_path(f, alias);
			continue;
		}
		genrule = qstar_graph_find_genrule(graph, deps->items[i]);
		if (!genrule)
			continue;
		for (j = 0; j < genrule->outputs.len; j++) {
			fputc(' ', f);
			ninja_path(f, genrule->outputs.items[j]);
		}
	}
	(void)target;
	return 0;
}

/** artifact path에서 basename component를 반환한다. */
static const char *
artifact_basename(const char *path)
{
	const char *slash;

	slash = strrchr(path ? path : "", '/');
	return slash ? slash + 1 : path;
}

/** target toolset role argv-vector 또는 fallback tool 하나를 Ninja argv에 추가한다. */
static int
ninja_argv_push_tool_role(struct qstar_graph *graph, struct ninja_argv *argv,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const char *role, const char *fallback)
{
	const struct qstar_toolset *toolset;
	const struct qstar_string_list *role_argv;
	size_t i;

	if (toolchain && toolchain->toolset[0]) {
		toolset = qstar_graph_find_toolset(graph, toolchain->toolset);
		role_argv = qstar_toolset_role_argv(toolset, role);
	} else {
		role_argv = qstar_target_tool_role_argv(graph, target, role);
	}
	if (!role_argv && (!fallback || !*fallback))
		return qstar_set_error(graph, "qstar: tool role '%s' is not configured",
		    role ? role : "<unknown>");
	if (!role_argv)
		return ninja_argv_push(graph, argv, fallback);
	for (i = 0; i < role_argv->len; i++) {
		if (ninja_argv_push(graph, argv, role_argv->items[i]) < 0)
			return -1;
	}
	return 0;
}

static int
ninja_argv_push_provider_template(struct qstar_graph *graph,
    struct ninja_argv *argv, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_string_list *template)
{
	char role[128];
	size_t i;
	int rc;

	for (i = 0; template && i < template->len; i++) {
		rc = qstar_provider_tool_token_role(template->items[i], role,
		    sizeof(role));
		if (rc < 0)
			return qstar_set_error(graph,
			    "qstar: malformed provider tool placeholder");
		if (rc == 1) {
			if (ninja_argv_push_tool_role(graph, argv, target, toolchain,
			    role, NULL) < 0)
				return -1;
			continue;
		}
		if (ninja_argv_push(graph, argv, template->items[i]) < 0)
			return -1;
	}
	return 0;
}

/** sharedlib target이 현재 toolchain target에서 Ninja lowering 가능한지 검증한다. */
static int
validate_sharedlib_platform(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	if (strcmp(target->kind, "sharedlib") != 0)
		return 0;
	if (qstar_platform_supports_sharedlib(toolchain->platform))
		return 0;
	return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
	    "kind", target->label,
	    "qstar: sharedlib target '%s' is not supported for platform '%s'",
	    target->label, toolchain->platform);
}

/** sharedlib link action에 platform별 dynamic-library flag를 추가한다. */
static int
append_sharedlib_link_flags(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *artifact,
    struct ninja_argv *argv)
{
	char import_lib[QSTAR_PATH_MAX], install_name[QSTAR_PATH_MAX];
	char soname[QSTAR_PATH_MAX], flag[QSTAR_PATH_MAX];

	if (strcmp(target->kind, "sharedlib") != 0)
		return 0;
	if (validate_sharedlib_platform(graph, target, toolchain) < 0)
		return -1;
	if (qstar_platform_is_darwin(toolchain->platform)) {
		snprintf(install_name, sizeof(install_name), "@rpath/%s",
		    artifact_basename(artifact));
		return ninja_argv_push(graph, argv, "-dynamiclib") < 0 ||
			    ninja_argv_push(graph, argv, "-install_name") < 0 ||
			    ninja_argv_push(graph, argv, install_name) < 0 ? -1 : 0;
	}
	if (qstar_platform_is_windows(toolchain->platform)) {
		if (qstar_graph_target_artifact_path(graph, target, "import_lib",
		    import_lib, sizeof(import_lib)) < 0)
			return -1;
		if (strcmp(toolchain->link_style, "msvc") == 0) {
			if (snprintf(flag, sizeof(flag), "/IMPLIB:%s", import_lib) >=
			    (int)sizeof(flag))
				return qstar_set_error(graph,
				    "qstar: import library path is too long");
			return ninja_argv_push(graph, argv, "/DLL") < 0 ||
			    ninja_argv_push(graph, argv, flag) < 0 ? -1 : 0;
		}
		if (snprintf(flag, sizeof(flag), "-Wl,--out-implib,%s", import_lib) >=
		    (int)sizeof(flag))
			return qstar_set_error(graph,
			    "qstar: import library path is too long");
		return ninja_argv_push(graph, argv, "-shared") < 0 ||
		    ninja_argv_push(graph, argv, flag) < 0 ? -1 : 0;
	}
	snprintf(soname, sizeof(soname), "-Wl,-soname,%s", artifact_basename(artifact));
	return ninja_argv_push(graph, argv, "-shared") < 0 ||
	    ninja_argv_push(graph, argv, soname) < 0 ? -1 : 0;
}

/** shared library source compile에는 PIC flag가 필요한지 확인한다. */
static int
target_compile_needs_pic(const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, int is_asm)
{
	return strcmp(target->kind, "sharedlib") == 0 &&
	    qstar_platform_supports_sharedlib(toolchain->platform) &&
	    !qstar_platform_is_windows(toolchain->platform) && !is_asm;
}

/** MSVC target에서 clang-cl style driver가 link flag boundary를 필요로 하는지 본다. */
static int
toolchain_needs_msvc_link_boundary(const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_target *target)
{
	const char *tool;

	if (!toolchain || !target || strcmp(toolchain->link_style, "msvc") != 0)
		return 0;
	tool = qstar_target_has_compile_provider(target, "cxx") ? toolchain->cxx :
	    toolchain->linker;
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
	tool = qstar_target_has_compile_provider(target, "cxx") ? toolchain->cxx :
	    toolchain->linker;
	return strstr(tool, "lld-link") != NULL || strstr(tool, "link.exe") != NULL;
}

/** target/build context link_options를 argv에 추가한다. */
static int
append_link_policy_flags(struct qstar_graph *graph, const struct qstar_target *target,
    struct ninja_argv *argv)
{
	size_t i;

	for (i = 0; i < graph->build_context.link_options.len; i++) {
		if (ninja_argv_push(graph, argv, graph->build_context.link_options.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < target->link_options.len; i++) {
		if (ninja_argv_push(graph, argv, target->link_options.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** system lib/lib_dir/framework link flags를 target context별 spelling으로 추가한다. */
static int
append_system_link_flags(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, struct ninja_argv *argv)
{
	char flag[QSTAR_PATH_MAX];
	size_t i;
	int msvc;

	msvc = strcmp(toolchain->link_style, "msvc") == 0;
	for (i = 0; i < graph->build_context.lib_dirs.len; i++) {
		if (msvc)
			snprintf(flag, sizeof(flag), "/LIBPATH:%s", graph->build_context.lib_dirs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-L%s", graph->build_context.lib_dirs.items[i]);
		if (ninja_argv_push(graph, argv, flag) < 0)
			return -1;
	}
	for (i = 0; i < target->lib_dirs.len; i++) {
		if (msvc)
			snprintf(flag, sizeof(flag), "/LIBPATH:%s", target->lib_dirs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-L%s", target->lib_dirs.items[i]);
		if (ninja_argv_push(graph, argv, flag) < 0)
			return -1;
	}
	for (i = 0; i < target->libs.len; i++) {
		if (msvc)
			snprintf(flag, sizeof(flag), "%s.lib", target->libs.items[i]);
		else
			snprintf(flag, sizeof(flag), "-l%s", target->libs.items[i]);
		if (ninja_argv_push(graph, argv, flag) < 0)
			return -1;
	}
	if (target->frameworks.len && !qstar_platform_is_darwin(toolchain->platform))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "link.frameworks", target->label,
		    "qstar: link.frameworks is supported only for Darwin-like targets");
	for (i = 0; i < target->frameworks.len; i++) {
		if (ninja_argv_push(graph, argv, "-framework") < 0 ||
		    ninja_argv_push(graph, argv, target->frameworks.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** dependency artifact list에 이미 추가한 target label인지 검사한다. */
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
collect_dep_artifact_rec(struct qstar_graph *graph, const struct qstar_target *dep,
    const struct qstar_resolved_toolchain *toolchain, struct qstar_string_list *out,
    struct qstar_string_list *seen)
{
	const struct qstar_target *next;
	char artifact[QSTAR_PATH_MAX];
	size_t i;

	if (seen_label(seen, dep->label))
		return 0;
	if (qstar_string_list_push(seen, dep->label) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (strcmp(dep->kind, "group") == 0 ||
	    strcmp(dep->kind, "objectlib") == 0)
		return 0;
	if (qstar_graph_target_link_artifact_path(graph, dep, toolchain->platform, artifact,
	    sizeof(artifact)) < 0)
		return qstar_set_error(graph, "qstar: dependency artifact path too long");
	if (qstar_string_list_push(out, artifact) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 0; i < dep->deps.len; i++) {
		if (dep->deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->deps.items[i]);
		if (next && collect_dep_artifact_rec(graph, next, toolchain, out,
		    seen) < 0)
			return -1;
	}
	for (i = 0; i < dep->private_deps.len; i++) {
		if (dep->private_deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->private_deps.items[i]);
		if (next && collect_dep_artifact_rec(graph, next, toolchain, out,
		    seen) < 0)
			return -1;
	}
	return 0;
}

/** target의 transitive dependency artifact list를 수집한다. */
static int
collect_dep_artifacts(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, struct qstar_string_list *out)
{
	const struct qstar_target *dep;
	struct qstar_string_list seen;
	size_t i;
	int rc;

	memset(out, 0, sizeof(*out));
	memset(&seen, 0, sizeof(seen));
	for (i = 0; i < target->deps.len; i++) {
		if (target->deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		rc = collect_dep_artifact_rec(graph, dep, toolchain, out, &seen);
		if (rc < 0) {
			qstar_string_list_free(out);
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
		rc = collect_dep_artifact_rec(graph, dep, toolchain, out, &seen);
		if (rc < 0) {
			qstar_string_list_free(out);
			qstar_string_list_free(&seen);
			return rc;
		}
	}
	qstar_string_list_free(&seen);
	return 0;
}

/** sharedlib dependency의 build-tree runtime rpath를 재귀 수집한다. */
static int
collect_sharedlib_rpath_rec(struct qstar_graph *graph, const struct qstar_target *dep,
    const char *consumer_dir, const struct qstar_resolved_toolchain *toolchain,
    struct qstar_string_list *rpaths, struct qstar_string_list *seen)
{
	const struct qstar_target *next;
	char artifact[QSTAR_PATH_MAX], dep_dir[QSTAR_PATH_MAX];
	char rel[QSTAR_PATH_MAX], rpath[QSTAR_PATH_MAX], flag[QSTAR_PATH_MAX];
	const char *base;
	size_t i;

	if (seen_label(seen, dep->label))
		return 0;
	if (qstar_string_list_push(seen, dep->label) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (strcmp(dep->kind, "sharedlib") == 0) {
		if (qstar_graph_artifact_output_path(graph, dep, artifact,
		    sizeof(artifact)) < 0 ||
		    path_dirname(artifact, dep_dir, sizeof(dep_dir)) < 0 ||
		    qstar_path_relative_between_dirs(consumer_dir, dep_dir, rel,
		    sizeof(rel)) < 0)
			return qstar_set_error(graph,
			    "qstar: sharedlib runtime path is too long");
		base = qstar_platform_is_darwin(toolchain->platform) ?
		    "@loader_path" : "$$ORIGIN";
		if (strcmp(rel, ".") == 0)
			snprintf(rpath, sizeof(rpath), "%s", base);
		else
			snprintf(rpath, sizeof(rpath), "%s/%s", base, rel);
		if (snprintf(flag, sizeof(flag), "-Wl,-rpath,%s", rpath) >=
		    (int)sizeof(flag))
			return qstar_set_error(graph,
			    "qstar: sharedlib runtime path is too long");
		if (push_unique_tmp(rpaths, flag) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	for (i = 0; i < dep->deps.len; i++) {
		if (dep->deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->deps.items[i]);
		if (next && collect_sharedlib_rpath_rec(graph, next, consumer_dir,
		    toolchain, rpaths, seen) < 0)
			return -1;
	}
	for (i = 0; i < dep->private_deps.len; i++) {
		if (dep->private_deps.items[i][0] == '@')
			continue;
		next = find_target(graph, dep->private_deps.items[i]);
		if (next && collect_sharedlib_rpath_rec(graph, next, consumer_dir,
		    toolchain, rpaths, seen) < 0)
			return -1;
	}
	return 0;
}

/** sharedlib dependency를 실행할 수 있도록 build-tree rpath link flag를 추가한다. */
static int
append_sharedlib_runtime_rpaths(struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const char *artifact, struct ninja_argv *argv)
{
	const struct qstar_target *dep;
	struct qstar_string_list rpaths, seen;
	char consumer_dir[QSTAR_PATH_MAX];
	size_t i;
	int rc;

	if (strcmp(target->kind, "staticlib") == 0 ||
	    !qstar_platform_supports_sharedlib(toolchain->platform) ||
	    qstar_platform_is_windows(toolchain->platform))
		return 0;
	if (path_dirname(artifact, consumer_dir, sizeof(consumer_dir)) < 0)
		return qstar_set_error(graph, "qstar: artifact directory path too long");
	memset(&rpaths, 0, sizeof(rpaths));
	memset(&seen, 0, sizeof(seen));
	rc = 0;
	for (i = 0; i < target->deps.len && rc == 0; i++) {
		if (target->deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->deps.items[i]);
		if (dep)
			rc = collect_sharedlib_rpath_rec(graph, dep, consumer_dir,
			    toolchain, &rpaths, &seen);
	}
	for (i = 0; i < target->private_deps.len && rc == 0; i++) {
		if (target->private_deps.items[i][0] == '@')
			continue;
		dep = find_target(graph, target->private_deps.items[i]);
		if (dep)
			rc = collect_sharedlib_rpath_rec(graph, dep, consumer_dir,
			    toolchain, &rpaths, &seen);
	}
	if (rc == 0) {
		for (i = 0; i < rpaths.len; i++) {
			if (ninja_argv_push(graph, argv, rpaths.items[i]) < 0) {
				rc = -1;
				break;
			}
		}
	}
	qstar_string_list_free(&rpaths);
	qstar_string_list_free(&seen);
	return rc;
}

/** staticlib final archive action을 Ninja edge로 lower한다. */
static int
emit_provider_final_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	const struct qstar_provider_action_template *tmpl;
	struct ninja_argv argv;
	char artifact[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX], out_dir[QSTAR_PATH_MAX];
	char action_id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	const char *final_action, *rule, *depfile;
	size_t i;

	if (!qstar_target_has_provider_final_action(target))
		return 0;
	memset(&argv, 0, sizeof(argv));
	tmpl = &target->provider_final.action;
	final_action = qstar_target_final_action(target);
	if (qstar_graph_artifact_output_path(graph, target, artifact,
	    sizeof(artifact)) < 0 ||
	    ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0 ||
	    path_dirname(artifact, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja provider final path too long");
	if (qstar_action_description_final(target, final_action, artifact, description,
	    sizeof(description)) < 0)
		return qstar_set_error(graph, "qstar: provider final description too long");
	if (ninja_argv_push_provider_template(graph, &argv, target, toolchain,
	    &tmpl->argv) < 0)
		goto fail;
	for (i = 0; i < tmpl->outputs.len; i++) {
		if (mkdir_parent_under_root(graph, tmpl->outputs.items[i]) < 0)
			goto fail;
	}
	if (tmpl->outputs.len == 0) {
		qstar_set_error(graph, "qstar: provider final action has no outputs");
		goto fail;
	}
	snprintf(action_id, sizeof(action_id), "%s:%s:0", target->label,
	    final_action);
	rule = tmpl->wants_depfile ? "qstar_compile" :
	    strcmp(final_action, "archive") == 0 ? "qstar_archive" : "qstar_link";
	depfile = tmpl->depfile && *tmpl->depfile ? tmpl->depfile : "";
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build", ctx->ninja);
	for (i = 0; i < tmpl->outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, tmpl->outputs.items[i]);
	}
	fprintf(ctx->ninja, ": %s", rule);
	for (i = 0; i < tmpl->inputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, tmpl->inputs.items[i]);
	}
	if (target->link_inputs.len) {
		fputs(" |", ctx->ninja);
		for (i = 0; i < target->link_inputs.len; i++) {
			if (write_genrule_dep_item(graph, ctx->ninja,
			    target->link_inputs.items[i]) < 0)
				goto fail;
		}
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	if (write_edge_command(graph, ctx, action_id, toolchain, &argv,
	    description, &tmpl->env, &tmpl->outputs) < 0)
		goto fail;
	fputc('\n', ctx->ninja);
	if (tmpl->wants_depfile) {
		fputs("  depfile = ", ctx->ninja);
		ninja_path(ctx->ninja, depfile);
		fputc('\n', ctx->ninja);
	}
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", target->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony", ctx->ninja);
	for (i = 0; i < tmpl->outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, tmpl->outputs.items[i]);
	}
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", target->label);
	ctx->edge_count += 2;
	ninja_argv_free(&argv);
	return 1;
fail:
	ninja_argv_free(&argv);
	return -1;
}

/** consumer-context objectlib source를 소비 target의 compile context로 lower한다. */
static int
emit_consumer_objectlib_compile_edges(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	const struct qstar_target *objectlib;
	struct qstar_source_info source;
	char object[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX];
	size_t i, j, action_index;

	for (i = 0; i < target->objects.len; i++) {
		objectlib = find_target(graph, target->objects.items[i]);
		if (!objectlib || !objectlib_uses_consumer_context(objectlib))
			continue;
		for (j = 0; j < objectlib->sources.len; j++) {
			qstar_target_source_classify(objectlib, j, &source);
			if (!qstar_source_requires_compile(&source))
				continue;
			if (objectlib_source_object_input(graph, target, objectlib, j,
			    object, sizeof(object)) < 0 ||
			    objectlib_source_depfile_output(graph, target, objectlib, j,
			    depfile, sizeof(depfile)) < 0)
				return qstar_set_error(graph,
				    "qstar: consumer objectlib path too long");
			action_index = objectlib_consumer_compile_index(graph, target, i, j);
			if (emit_compile_edge_from_source(graph, ctx, target, objectlib,
			    toolchain, j, action_index, object, depfile) < 0)
				return -1;
		}
	}
	return 0;
}

static int
emit_staticlib_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	struct ninja_argv argv;
	char artifact[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX];
	char out_dir[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	size_t i;

	memset(&argv, 0, sizeof(argv));
	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0 ||
	    path_dirname(artifact, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja artifact path too long");
	if (mkdir_parent_under_root(graph, artifact) < 0)
		return qstar_set_error(graph, "qstar: could not create ninja artifact dir");
	snprintf(action_id, sizeof(action_id), "%s:archive:0", target->label);
	if (ninja_argv_push_tool_role(graph, &argv, target, toolchain, "archive",
	    toolchain->ar) < 0 ||
	    ninja_argv_push(graph, &argv, "rcs") < 0 ||
	    ninja_argv_push(graph, &argv, artifact) < 0)
		goto fail;
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_target_source_classify(target, i, &source) < 0)
			goto fail;
		if (!qstar_source_requires_compile(&source) &&
		    !qstar_source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		if (ninja_argv_push(graph, &argv, object) < 0)
			goto fail;
	}
	if (ninja_argv_push_objectlib_objects(graph, &argv, target) < 0)
		goto fail;
	if (qstar_action_description_final(target, "archive", artifact, description,
	    sizeof(description)) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, artifact);
	fputs(": qstar_archive", ctx->ninja);
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_target_source_classify(target, i, &source) < 0)
			goto fail;
		if (!qstar_source_requires_compile(&source) &&
		    !qstar_source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, object);
	}
	if (write_objectlib_object_deps(graph, ctx->ninja, target) < 0)
		goto fail;
	if (target->link_inputs.len) {
		fputs(" |", ctx->ninja);
		for (i = 0; i < target->link_inputs.len; i++) {
			if (write_genrule_dep_item(graph, ctx->ninja,
			    target->link_inputs.items[i]) < 0)
				goto fail;
		}
	}
	if (target->deps.len || target->private_deps.len) {
		fputs(" ||", ctx->ninja);
		if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
		    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
			goto fail;
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	if (write_edge_command(graph, ctx, action_id, toolchain, &argv, description,
	    NULL, NULL) < 0)
		goto fail;
	fputc('\n', ctx->ninja);
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", target->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony ", ctx->ninja);
	ninja_path(ctx->ninja, artifact);
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", target->label);
	ctx->edge_count += 2;
	ninja_argv_free(&argv);
	return 0;
fail:
	ninja_argv_free(&argv);
	return -1;
}

/** executable/test final link action을 Ninja edge로 lower한다. */
static int
emit_link_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	struct ninja_argv argv;
	struct qstar_string_list dep_artifacts, outputs;
	char artifact[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX];
	char out_dir[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	const char *final_action;
	size_t i;

	memset(&argv, 0, sizeof(argv));
	memset(&dep_artifacts, 0, sizeof(dep_artifacts));
	memset(&outputs, 0, sizeof(outputs));
	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0 ||
	    path_dirname(artifact, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja artifact path too long");
	if (qstar_graph_target_artifact_outputs(graph, target, &outputs) < 0)
		return -1;
	for (i = 0; i < outputs.len; i++) {
		if (mkdir_parent_under_root(graph, outputs.items[i]) < 0)
			goto fail;
	}
	if (outputs.len == 0) {
		qstar_set_error(graph, "qstar: ninja target has no artifact outputs");
		goto fail;
	}
	if (validate_sharedlib_platform(graph, target, toolchain) < 0)
		goto fail;
	final_action = qstar_target_final_action(target);
	snprintf(action_id, sizeof(action_id), "%s:%s:0", target->label, final_action);
	if (ninja_argv_push_tool_role(graph, &argv, target, toolchain, "link",
	    qstar_target_has_compile_provider(target, "cxx") ? toolchain->cxx :
	    toolchain->linker) < 0)
		goto fail;
	if (toolchain_uses_msvc_out_arg(toolchain, target)) {
		if (ninja_argv_pushf(graph, &argv, "/out:%s", artifact) < 0)
			goto fail;
	} else if (ninja_argv_push(graph, &argv, "-o") < 0 ||
	    ninja_argv_push(graph, &argv, artifact) < 0) {
		goto fail;
	}
	if (append_sharedlib_link_flags(graph, target, toolchain, artifact, &argv) < 0)
		goto fail;
	if (append_link_policy_flags(graph, target, &argv) < 0)
		goto fail;
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_target_source_classify(target, i, &source) < 0)
			goto fail;
		if (!qstar_source_requires_compile(&source) &&
		    !qstar_source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		if (ninja_argv_push(graph, &argv, object) < 0)
			goto fail;
	}
	if (ninja_argv_push_objectlib_objects(graph, &argv, target) < 0)
		goto fail;
	if (collect_dep_artifacts(graph, target, toolchain, &dep_artifacts) < 0)
		goto fail;
	for (i = 0; i < dep_artifacts.len; i++) {
		if (ninja_argv_push(graph, &argv, dep_artifacts.items[i]) < 0)
			goto fail;
	}
	if (append_sharedlib_runtime_rpaths(graph, target, toolchain, artifact,
	    &argv) < 0)
		goto fail;
	if (toolchain_needs_msvc_link_boundary(toolchain, target) &&
	    ninja_argv_push(graph, &argv, "/link") < 0)
		goto fail;
	if (append_system_link_flags(graph, target, toolchain, &argv) < 0)
		goto fail;
	if (qstar_action_description_final(target, final_action, artifact, description,
	    sizeof(description)) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build", ctx->ninja);
	for (i = 0; i < outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, outputs.items[i]);
	}
	fputs(": qstar_link", ctx->ninja);
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_target_source_classify(target, i, &source) < 0)
			goto fail;
		if (!qstar_source_requires_compile(&source) &&
		    !qstar_source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, object);
	}
	if (write_objectlib_object_deps(graph, ctx->ninja, target) < 0)
		goto fail;
	for (i = 0; i < dep_artifacts.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, dep_artifacts.items[i]);
	}
	if (target->link_inputs.len) {
		fputs(" |", ctx->ninja);
		for (i = 0; i < target->link_inputs.len; i++) {
			if (write_genrule_dep_item(graph, ctx->ninja,
			    target->link_inputs.items[i]) < 0)
				goto fail;
		}
	}
	if (target->deps.len || target->private_deps.len) {
		fputs(" ||", ctx->ninja);
		if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
		    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
			goto fail;
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	if (write_edge_command(graph, ctx, action_id, toolchain, &argv, description,
	    NULL, &outputs) < 0)
		goto fail;
	fputc('\n', ctx->ninja);
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", target->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony", ctx->ninja);
	for (i = 0; i < outputs.len; i++) {
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, outputs.items[i]);
	}
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", target->label);
	ctx->edge_count += 2;
	qstar_string_list_free(&outputs);
	qstar_string_list_free(&dep_artifacts);
	ninja_argv_free(&argv);
	return 0;
fail:
	qstar_string_list_free(&outputs);
	qstar_string_list_free(&dep_artifacts);
	ninja_argv_free(&argv);
	return -1;
}

/** objectlib target을 object collection alias로 lower한다. */
static int
emit_objectlib_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	char alias[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX];
	size_t i;

	if (ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0)
		return qstar_set_error(graph, "qstar: ninja objectlib alias path too long");
	snprintf(action_id, sizeof(action_id), "%s:compile-objects:0", target->label);
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony", ctx->ninja);
	for (i = 0; !objectlib_uses_consumer_context(target) &&
	    i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_target_source_classify(target, i, &source) < 0)
			return -1;
		if (!qstar_source_requires_compile(&source) &&
		    !qstar_source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			return qstar_set_error(graph, "qstar: objectlib object path too long");
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, object);
	}
	if (target->deps.len || target->private_deps.len) {
		fputs(" ||", ctx->ninja);
		if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
		    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
			return -1;
	}
	fprintf(ctx->ninja, "\n  qstar_action_id = %s\n\n", action_id);
	ctx->edge_count++;
	return 0;
}

/** group target을 Ninja phony edge로 lower한다. */
static int
emit_group_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	char alias[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX];

	if (ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0)
		return qstar_set_error(graph, "qstar: ninja alias path too long");
	snprintf(action_id, sizeof(action_id), "%s:group:0", target->label);
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony", ctx->ninja);
	if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
	    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
		return -1;
	fprintf(ctx->ninja, "\n  qstar_action_id = %s\n\n", action_id);
	ctx->edge_count++;
	return 0;
}

/** stage source item이 가리키는 producer edge를 먼저 emit한다. */
static int
emit_stage_source_producer(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_stage *stage, const char *item)
{
	const struct qstar_genrule *owner;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error_origin(graph, stage->origin_file,
		    stage->origin_line, "files", stage->label,
		    "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (find_target(graph, label))
			return emit_target_closure(graph, ctx, label);
		owner = qstar_graph_find_genrule(graph, label);
		if (owner)
			return emit_genrule_edge(graph, ctx, NULL, owner);
		return 0;
	}
	owner = qstar_graph_find_output_owner(graph, item);
	if (owner)
		return emit_genrule_edge(graph, ctx, NULL, owner);
	return 0;
}

/** stage source item을 Ninja dependency로 출력한다. */
static int
write_stage_source_dep(struct qstar_graph *graph, FILE *f, const char *item)
{
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 1) {
		target = find_target(graph, label);
		if (target) {
			if (ninja_alias_path(graph, label, alias, sizeof(alias)) < 0)
				return qstar_set_error(graph,
				    "qstar: ninja alias path too long");
			fputc(' ', f);
			ninja_path(f, alias);
			return 0;
		}
		genrule = qstar_graph_find_genrule(graph, label);
		if (genrule && genrule->outputs.len > 0) {
			fputc(' ', f);
			ninja_path(f, genrule->outputs.items[0]);
		}
		return 0;
	}
	if (resolve_target_file_token(graph, item, resolved, sizeof(resolved)) < 0)
		return -1;
	fputc(' ', f);
	ninja_path(f, resolved);
	return 0;
}

/** stage layout을 Ninja producer edge로 lower한다. */
static int
emit_stage_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_stage *stage)
{
	struct ninja_argv argv;
	char action_id[QSTAR_PATH_MAX], manifest[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX];
	char script_rel[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	char root_rel[QSTAR_PATH_MAX];
	int index;
	size_t i;

	index = stage_index(graph, stage);
	if (index < 0)
		return qstar_set_error(graph, "qstar: invalid ninja stage");
	if (ctx->stage_state[index] == 2)
		return 0;
	if (ctx->stage_state[index] == 1)
		return qstar_set_error_origin(graph, stage->origin_file, stage->origin_line,
		    "files", stage->label,
		    "qstar: circular stage dependency at '%s'", stage->label);
	ctx->stage_state[index] = 1;
	for (i = 0; i < stage->srcs.len; i++) {
		if (emit_stage_source_producer(graph, ctx, stage,
		    stage->srcs.items[i]) < 0)
			return -1;
	}
	memset(&argv, 0, sizeof(argv));
	snprintf(action_id, sizeof(action_id), "%s:stage:0", stage->label);
	if (stage_manifest_path(graph, stage->label, manifest, sizeof(manifest)) < 0 ||
	    stage_alias_path(graph, stage->label, alias, sizeof(alias)) < 0)
		return qstar_set_error(graph, "qstar: ninja stage path too long");
	if (qstar_action_description_stage(stage, description, sizeof(description)) < 0)
		snprintf(description, sizeof(description), "<too-long>");
	snprintf(root_rel, sizeof(root_rel), "%s",
	    stage->root && *stage->root ? stage->root : ".");
	if (mkdir_parent_under_root(graph, manifest) < 0)
		return qstar_set_error(graph, "qstar: could not create stage manifest dir");
	if (write_stage_script(graph, stage, action_id, manifest, description,
	    script_rel, sizeof(script_rel)) < 0)
		return -1;
	if (ninja_argv_push(graph, &argv, "sh") < 0 ||
	    ninja_argv_push(graph, &argv, script_rel) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, manifest);
	for (i = 0; i < stage->dsts.len; i++) {
		char staged_output[QSTAR_PATH_MAX];

		if (qstar_path_join(root_rel, stage->dsts.items[i], staged_output,
		    sizeof(staged_output)) < 0)
			goto path_fail;
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, staged_output);
	}
	fputs(": qstar_stage ", ctx->ninja);
	ninja_path(ctx->ninja, script_rel);
	if (stage->srcs.len) {
		fputs(" |", ctx->ninja);
		for (i = 0; i < stage->srcs.len; i++) {
			if (write_stage_source_dep(graph, ctx->ninja,
			    stage->srcs.items[i]) < 0)
				goto fail;
		}
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	shell_argv(ctx->ninja, &argv);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", stage->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony ", ctx->ninja);
	ninja_path(ctx->ninja, manifest);
	for (i = 0; i < stage->dsts.len; i++) {
		char staged_output[QSTAR_PATH_MAX];

		if (qstar_path_join(root_rel, stage->dsts.items[i], staged_output,
		    sizeof(staged_output)) < 0)
			goto path_fail;
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, staged_output);
	}
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", stage->label);
	ctx->edge_count += 2;
	ctx->stage_state[index] = 2;
	ninja_argv_free(&argv);
	return 0;
path_fail:
	qstar_set_error_origin(graph, stage->origin_file, stage->origin_line,
	    "package-failure", stage->label,
	    "qstar: stage destination path too long");
fail:
	ninja_argv_free(&argv);
	return -1;
}

/** run_target command item이 가리키는 producer edge를 먼저 emit한다. */
static int
emit_run_item_producer(struct qstar_graph *graph, struct ninja_ctx *ctx, const char *item)
{
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 0)
		return 0;
	if (find_target(graph, label))
		return emit_target_closure(graph, ctx, label);
	genrule = qstar_graph_find_genrule(graph, label);
	if (genrule)
		return emit_genrule_edge(graph, ctx, NULL, genrule);
	return qstar_set_error(graph, "qstar: target_file references unknown target '%s'",
	    label);
}

/** run_target inputs item이 가리키는 producer edge를 먼저 emit한다. */
static int
emit_run_input_producer(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const char *item)
{
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_stage_dir_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed stage_dir placeholder");
	if (rc == 1) {
		const struct qstar_stage *stage;

		stage = qstar_graph_find_stage(graph, label);
		if (!stage)
			return qstar_set_error(graph,
			    "qstar: stage_dir references unknown stage '%s'", label);
		return emit_stage_edge(graph, ctx, stage);
	}
	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (find_target(graph, label))
			return emit_target_closure(graph, ctx, label);
		genrule = qstar_graph_find_genrule(graph, label);
		if (genrule)
			return emit_genrule_edge(graph, ctx, NULL, genrule);
		return qstar_set_error(graph,
		    "qstar: target_file references unknown target '%s'", label);
	}
	genrule = qstar_graph_find_output_owner(graph, item);
	if (genrule)
		return emit_genrule_edge(graph, ctx, NULL, genrule);
	return 0;
}

/** run_target command item이 가리키는 Ninja dependency를 출력한다. */
static int
write_run_item_dep(struct qstar_graph *graph, FILE *f, const char *item)
{
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 0)
		return 0;
	if (find_target(graph, label)) {
		if (ninja_alias_path(graph, label, alias, sizeof(alias)) < 0)
			return qstar_set_error(graph, "qstar: ninja alias path too long");
		fputc(' ', f);
		ninja_path(f, alias);
		return 0;
	}
	genrule = qstar_graph_find_genrule(graph, label);
	if (genrule && genrule->outputs.len > 0) {
		fputc(' ', f);
		ninja_path(f, genrule->outputs.items[0]);
		return 0;
	}
	return qstar_set_error(graph, "qstar: target_file references unknown target '%s'",
	    label);
}

/** run_target inputs item을 Ninja dependency로 출력한다. */
static int
write_run_input_dep(struct qstar_graph *graph, FILE *f, const char *item)
{
	const struct qstar_genrule *genrule;
	char label[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_stage_dir_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed stage_dir placeholder");
	if (rc == 1) {
		if (stage_alias_path(graph, label, alias, sizeof(alias)) < 0)
			return qstar_set_error(graph, "qstar: ninja stage alias path too long");
		fputc(' ', f);
		ninja_path(f, alias);
		return 0;
	}
	rc = qstar_target_file_token_label(item, label, sizeof(label));
	if (rc < 0)
		return qstar_set_error(graph, "qstar: malformed target_file placeholder");
	if (rc == 1) {
		if (find_target(graph, label)) {
			if (ninja_alias_path(graph, label, alias, sizeof(alias)) < 0)
				return qstar_set_error(graph,
				    "qstar: ninja alias path too long");
			fputc(' ', f);
			ninja_path(f, alias);
			return 0;
		}
		genrule = qstar_graph_find_genrule(graph, label);
		if (genrule && genrule->outputs.len > 0) {
			fputc(' ', f);
			ninja_path(f, genrule->outputs.items[0]);
			return 0;
		}
		return qstar_set_error(graph,
		    "qstar: target_file references unknown target '%s'", label);
	}
	if (resolve_target_file_token(graph, item, resolved, sizeof(resolved)) < 0)
		return -1;
	fputc(' ', f);
	ninja_path(f, resolved);
	return 0;
}

/** run_target command argv에 포함된 target_file dependency를 출력한다. */
static int
write_run_command_deps(struct qstar_graph *graph, FILE *f,
    const struct qstar_target *target)
{
	size_t i;
	int wrote;

	wrote = 0;
	for (i = 0; i < target->run_command.len; i++) {
		char label[QSTAR_PATH_MAX];
		int rc;

		rc = qstar_target_file_token_label(target->run_command.items[i], label,
		    sizeof(label));
		if (rc < 0)
			return qstar_set_error(graph, "qstar: malformed target_file placeholder");
		if (rc == 0)
			continue;
		if (!wrote) {
			fputs(" |", f);
			wrote = 1;
		}
		if (write_run_item_dep(graph, f, target->run_command.items[i]) < 0)
			return -1;
	}
	for (i = 0; i < target->run_inputs.len; i++) {
		if (!wrote) {
			fputs(" |", f);
			wrote = 1;
		}
		if (write_run_input_dep(graph, f, target->run_inputs.items[i]) < 0)
			return -1;
	}
	return 0;
}

/** run_target을 Ninja wrapper action으로 lower한다. */
static int
emit_run_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target)
{
	struct ninja_argv argv, script_argv;
	char alias[QSTAR_PATH_MAX], owner[QSTAR_PATH_MAX], stamp_sub[QSTAR_PATH_MAX];
	char stamp[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX], script_rel[QSTAR_PATH_MAX];
	char description[QSTAR_PATH_MAX];
	size_t i;

	memset(&argv, 0, sizeof(argv));
	memset(&script_argv, 0, sizeof(script_argv));
	if (target->run_command.len == 0)
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "command", target->label,
		    "qstar: run_target '%s' requires command = qstar.cli { ... }",
		    target->label);
	for (i = 0; i < target->run_inputs.len; i++) {
		if (emit_run_input_producer(graph, ctx, target->run_inputs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; i < target->run_command.len; i++) {
		char resolved[QSTAR_PATH_MAX];

		if (emit_run_item_producer(graph, ctx, target->run_command.items[i]) < 0)
			goto fail;
		if (resolve_run_command_token(graph, target->run_command.items[i],
		    resolved, sizeof(resolved)) < 0)
			goto fail;
		if (ninja_argv_push(graph, &argv, resolved) < 0)
			goto fail;
	}
	qstar_mangle_label(target->label, owner, sizeof(owner));
	if (snprintf(stamp_sub, sizeof(stamp_sub), "out/%s/run.stamp", owner) >=
	    (int)sizeof(stamp_sub) ||
	    qstar_graph_build_path(graph, stamp_sub, stamp, sizeof(stamp)) < 0 ||
	    ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0)
		goto path_fail;
	snprintf(action_id, sizeof(action_id), "%s:run:0", target->label);
	if (qstar_action_description_run(target, description, sizeof(description)) < 0)
		goto fail;
	if (write_run_script(graph, target, action_id, &argv, stamp, description,
	    script_rel, sizeof(script_rel)) < 0)
		goto fail;
	if (ninja_argv_push(graph, &script_argv, "sh") < 0 ||
	    ninja_argv_push(graph, &script_argv, script_rel) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, stamp);
	fputs(": qstar_run ", ctx->ninja);
	ninja_path(ctx->ninja, script_rel);
	if (write_run_command_deps(graph, ctx->ninja, target) < 0)
		goto fail;
	if (target->deps.len || target->private_deps.len) {
		fputs(" ||", ctx->ninja);
		if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
		    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
			goto fail;
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	shell_argv(ctx->ninja, &script_argv);
	fputs("\n  description = ", ctx->ninja);
	ninja_var_text(ctx->ninja, description);
	fputc('\n', ctx->ninja);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	if (write_ninja_action_log(graph, action_id, &argv, NULL, description, NULL,
	    NULL) < 0)
		goto fail;
	fprintf(ctx->ninja, "# qstar-action-id: %s:alias\n", target->label);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, alias);
	fputs(": phony ", ctx->ninja);
	ninja_path(ctx->ninja, stamp);
	fprintf(ctx->ninja, "\n  qstar_action_id = %s:alias\n\n", target->label);
	ctx->edge_count += 2;
	ninja_argv_free(&script_argv);
	ninja_argv_free(&argv);
	return 0;

path_fail:
	qstar_set_error(graph, "qstar: ninja run_target path too long");
fail:
	ninja_argv_free(&script_argv);
	ninja_argv_free(&argv);
	return -1;
}

/** closure target 하나를 Ninja graph로 lower한다. */
static int
emit_target(struct qstar_graph *graph, const struct qstar_target *target, size_t order,
    void *user)
{
	struct ninja_ctx *ctx = user;
	struct qstar_resolved_toolchain toolchain;
	int index;
	size_t i;

	(void)order;
	index = target_index(graph, target);
	if (index >= 0 && ctx->target_emitted[index])
		return 0;
	if (index >= 0)
		ctx->target_emitted[index] = 1;
	if (emit_target_dep_genrule_producers(graph, ctx, target) < 0)
		return -1;
	if (strcmp(target->kind, "group") == 0)
		return emit_group_edge(graph, ctx, target);
	if (strcmp(target->kind, "run_target") == 0)
		return emit_run_edge(graph, ctx, target);
	if (strcmp(target->kind, "staticlib") != 0 &&
	    strcmp(target->kind, "sharedlib") != 0 &&
	    strcmp(target->kind, "exe") != 0 &&
	    strcmp(target->kind, "test") != 0 &&
	    strcmp(target->kind, "objectlib") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: ninja backend supports custom_target, executable, test, run_target, objectlib, staticlib, sharedlib, and group in this release; target '%s' is kind '%s'",
		    target->label, target->kind);
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	if (emit_consumed_genrules(graph, ctx, target) < 0 ||
	    emit_target_link_input_producers(graph, ctx, target) < 0)
		return -1;
	for (i = 0; !objectlib_uses_consumer_context(target) &&
	    !qstar_target_has_provider_final_action(target) &&
	    i < target->sources.len; i++) {
		if (emit_compile_edge(graph, ctx, target, &toolchain, i) < 0)
			return -1;
	}
	if (emit_consumer_objectlib_compile_edges(graph, ctx, target, &toolchain) < 0)
		return -1;
	if (qstar_target_has_provider_final_action(target))
		return emit_provider_final_edge(graph, ctx, target, &toolchain) < 0 ?
		    -1 : 0;
	if (strcmp(target->kind, "objectlib") == 0)
		return emit_objectlib_edge(graph, ctx, target);
	if (strcmp(target->kind, "staticlib") == 0)
		return emit_staticlib_edge(graph, ctx, target, &toolchain);
	return emit_link_edge(graph, ctx, target, &toolchain);
}

/** Ninja output file path와 default alias를 초기화한다. */
static int
init_ninja_ctx(struct qstar_graph *graph, const char *label, struct ninja_ctx *ctx, FILE *out)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->out = out;
	ctx->root_label = label;
	ctx->target_emitted = calloc(graph->len ? graph->len : 1,
	    sizeof(ctx->target_emitted[0]));
	ctx->genrule_state = calloc(graph->genrule_len ? graph->genrule_len : 1,
	    sizeof(ctx->genrule_state[0]));
	ctx->stage_state = calloc(graph->stage_len ? graph->stage_len : 1,
	    sizeof(ctx->stage_state[0]));
	if (!ctx->target_emitted || !ctx->genrule_state || !ctx->stage_state) {
		free(ctx->target_emitted);
		free(ctx->genrule_state);
		free(ctx->stage_state);
		ctx->target_emitted = NULL;
		ctx->genrule_state = NULL;
		ctx->stage_state = NULL;
		return qstar_set_error(graph, "qstar: out of memory");
	}
	if (qstar_graph_build_path(graph, "ninja", ctx->ninja_dir_rel,
	    sizeof(ctx->ninja_dir_rel)) < 0 ||
	    qstar_graph_build_path(graph, "ninja/build.ninja", ctx->ninja_rel,
	    sizeof(ctx->ninja_rel)) < 0 ||
	    full_path_under_root(graph, ctx->ninja_rel, ctx->ninja_full,
	    sizeof(ctx->ninja_full)) < 0) {
		free(ctx->target_emitted);
		free(ctx->genrule_state);
		free(ctx->stage_state);
		ctx->target_emitted = NULL;
		ctx->genrule_state = NULL;
		ctx->stage_state = NULL;
		return qstar_set_error(graph, "qstar: ninja file path too long");
	}
	if (label && qstar_graph_find_stage(graph, label) && !find_target(graph, label) &&
	    !qstar_graph_find_genrule(graph, label)) {
		if (stage_alias_path(graph, label, ctx->default_alias,
		    sizeof(ctx->default_alias)) >= 0)
			return 0;
		free(ctx->target_emitted);
		free(ctx->genrule_state);
		free(ctx->stage_state);
		ctx->target_emitted = NULL;
		ctx->genrule_state = NULL;
		ctx->stage_state = NULL;
		return qstar_set_error(graph, "qstar: ninja default alias path too long");
	}
	if (label && ninja_alias_path(graph, label, ctx->default_alias,
	    sizeof(ctx->default_alias)) < 0) {
		free(ctx->target_emitted);
		free(ctx->genrule_state);
		free(ctx->stage_state);
		ctx->target_emitted = NULL;
		ctx->genrule_state = NULL;
		ctx->stage_state = NULL;
		return qstar_set_error(graph, "qstar: ninja default alias path too long");
	}
	return 0;
}

/** Ninja emit context가 소유한 임시 상태를 해제한다. */
static void
ninja_ctx_free(struct ninja_ctx *ctx)
{
	free(ctx->target_emitted);
	free(ctx->genrule_state);
	free(ctx->stage_state);
	ctx->target_emitted = NULL;
	ctx->genrule_state = NULL;
	ctx->stage_state = NULL;
}

/** graph closure를 build.ninja와 compile_commands.json으로 출력한다. */
int
qstar_graph_emit_ninja(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct ninja_ctx ctx;
	const struct qstar_genrule *root_genrule;
	const struct qstar_stage *root_stage;

	if (init_ninja_ctx(graph, label, &ctx, out) < 0)
		return -1;
	if (mkdir_parent_under_root(graph, ctx.ninja_rel) < 0) {
		qstar_set_error(graph, "qstar: could not create ninja output dir");
		goto fail;
	}
	ctx.ninja = fopen(ctx.ninja_full, "w");
	if (!ctx.ninja)
		goto fail_open;
	if (open_compile_commands(graph, &ctx) < 0) {
		fclose(ctx.ninja);
		ctx.ninja = NULL;
		return -1;
	}
	write_ninja_header(ctx.ninja, ctx.ninja_dir_rel,
	    qstar_platform_is_windows(qstar_graph_platform(graph)));
	root_genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	root_stage = label && *label ? qstar_graph_find_stage(graph, label) : NULL;
	if (root_genrule) {
		if (emit_genrule_edge(graph, &ctx, NULL, root_genrule) < 0)
			goto fail;
	} else if (root_stage) {
		if (emit_stage_edge(graph, &ctx, root_stage) < 0)
			goto fail;
	} else if (qstar_graph_visit_closure(graph, label, emit_target, &ctx) < 0) {
		goto fail;
	}
	if (label) {
		fputs("default ", ctx.ninja);
		ninja_path(ctx.ninja, ctx.default_alias);
		fputc('\n', ctx.ninja);
	} else if (graph->len > 0) {
		size_t i;

		fputs("default", ctx.ninja);
		for (i = 0; i < graph->len; i++) {
			char alias[QSTAR_PATH_MAX];

			if (ninja_alias_path(graph, graph->targets[i].label, alias,
			    sizeof(alias)) < 0) {
				qstar_set_error(graph, "qstar: ninja default alias path too long");
				goto fail;
			}
			fputc(' ', ctx.ninja);
			ninja_path(ctx.ninja, alias);
		}
		fputc('\n', ctx.ninja);
	}
	if (fclose(ctx.ninja) != 0) {
		ctx.ninja = NULL;
		goto fail;
	}
	ctx.ninja = NULL;
	if (close_compile_commands(graph, &ctx) < 0)
		goto fail;
	fprintf(out, "ninja_file %s\n", ctx.ninja_rel);
	if (ctx.compdb_rel[0])
		fprintf(out, "compile_commands %s\n", ctx.compdb_rel);
	else
		fprintf(out, "compile_commands <off>\n");
	if (label)
		fprintf(out, "ninja_default %s\n", ctx.default_alias);
	fprintf(out, "ninja_edges %d\n", ctx.edge_count);
	fprintf(out, "status ok\n");
	ninja_ctx_free(&ctx);
	return 0;
fail_open:
	qstar_set_error(graph, "qstar: could not write %s", ctx.ninja_rel);
fail:
	if (ctx.ninja)
		fclose(ctx.ninja);
	if (ctx.compdb)
		fclose(ctx.compdb);
	ninja_ctx_free(&ctx);
	return -1;
}

/** Ninja 실패를 last-failure replay 파일로 남긴다. */
static void
write_ninja_failure_replay(struct qstar_graph *graph, const char *ninja_rel,
    const char *alias, int exit_code)
{
	char path[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	FILE *f;

	if (full_path_under_build(graph, "logs/last-failure.replay", path,
	    sizeof(path)) < 0)
		return;
	if (path_exists(path))
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# qstar failure replay v2\ncd %s\n",
	    graph->package_root ? graph->package_root : ".");
	fputs("failure_kind=ninja-failure\n", f);
	snprintf(description, sizeof(description), "Running Ninja build %s",
	    alias && *alias ? alias : "//...");
	write_log_description(f, description);
	fprintf(f, "label=%s\n", alias && *alias ? alias : "//...");
	fputs("stdout=<inherit>\nstderr=<inherit>\n", f);
	fputs("expect_contains=<none>\nexpect_file=<none>\n", f);
	fputs("response_file path=<none> style=none digest=<none>\n", f);
	fprintf(f, "exit=%d\n", exit_code);
	fputs("ninja -f ", f);
	shell_arg(f, ninja_rel);
	if (alias && *alias) {
		fputc(' ', f);
		shell_arg(f, alias);
	}
	fputc('\n', f);
	fclose(f);
}

/** 이전 Ninja failure replay가 이번 실패를 가리지 않도록 지운다. */
static void
clear_ninja_failure_replay(struct qstar_graph *graph)
{
	char path[QSTAR_PATH_MAX];

	if (full_path_under_build(graph, "logs/last-failure.replay", path,
	    sizeof(path)) == 0)
		remove(path);
}

/** Ninja executable을 실행해 emitted graph를 빌드한다. */
static int
run_ninja(struct qstar_graph *graph, const char *ninja_rel, const char *alias,
    const struct qstar_build_options *options)
{
	qstar_process_id pid;
	int status, exit_code;
	char jobs_arg[32];
	char *argv[10];
	size_t argc;
	const char *runner;

	clear_ninja_failure_replay(graph);
	argc = 0;
	argv[argc++] = "ninja";
	argv[argc++] = "-f";
	argv[argc++] = (char *)ninja_rel;
	if (options && options->jobs > 0) {
		snprintf(jobs_arg, sizeof(jobs_arg), "%d", options->jobs);
		argv[argc++] = "-j";
		argv[argc++] = jobs_arg;
	}
	if (options && options->verbose)
		argv[argc++] = "-v";
	if (alias && *alias)
		argv[argc++] = (char *)alias;
	argv[argc] = NULL;
	if (qstar_platform_process_start_inherit(graph,
	    graph->package_root ? graph->package_root : ".", argv, &pid, &runner) < 0)
		return -1;
	if (qstar_platform_process_wait_blocking(pid, &status) < 0)
		return qstar_set_error(graph, "qstar: process wait failed for ninja");
	exit_code = qstar_platform_process_exit_code(status);
	if (exit_code == 127)
		return qstar_set_error(graph,
		    "qstar: ninja executable was not found or could not be started");
	if (exit_code != 0) {
		write_ninja_failure_replay(graph, ninja_rel, alias, exit_code);
		return qstar_set_error(graph, "qstar: ninja build failed with exit code %d",
		    exit_code);
	}
	return 0;
}

/** Ninja backend로 build.ninja를 emit한 뒤 requested target을 빌드한다. */
int
qstar_graph_build_ninja(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out)
{
	struct ninja_ctx ctx;

	if (init_ninja_ctx(graph, label, &ctx, out) < 0)
		return -1;
	if (qstar_graph_emit_ninja(graph, label, out) < 0) {
		ninja_ctx_free(&ctx);
		return -1;
	}
	if (fflush(out) != 0) {
		ninja_ctx_free(&ctx);
		return qstar_set_error(graph, "qstar: could not flush ninja emit output");
	}
	if (run_ninja(graph, ctx.ninja_rel, label ? ctx.default_alias : NULL, options) < 0) {
		ninja_ctx_free(&ctx);
		return -1;
	}
	fprintf(out, "backend ninja\n");
	fprintf(out, "status ok\n");
	ninja_ctx_free(&ctx);
	return 0;
}

/** test target artifact를 제한된 runner로 실행한다. */
static int
run_ninja_test_artifact(struct qstar_graph *graph, const struct qstar_target *target, FILE *out)
{
	char artifact[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], logdir[QSTAR_PATH_MAX];
	char name[QSTAR_PATH_MAX], stdout_path[QSTAR_PATH_MAX], stderr_path[QSTAR_PATH_MAX];
	char child_stdout_path[QSTAR_PATH_MAX], child_stderr_path[QSTAR_PATH_MAX];
	char *argv[2];
	const char *runner;
	qstar_process_id pid;
	time_t start;
	int status, done;

	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    full_path_under_root(graph, artifact, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: test artifact path too long");
	if (!path_exists(full))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "test", target->label,
		    "qstar: test artifact '%s' is missing after ninja build", artifact);
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
	fprintf(out, "test_run label=%s artifact=%s stdout=%s stderr=%s backend=ninja\n",
	    target->label, artifact, child_stdout_path, child_stderr_path);
	argv[0] = artifact;
	argv[1] = NULL;
	if (qstar_platform_process_start_file_output(graph,
	    graph->package_root ? graph->package_root : ".", argv, child_stdout_path,
	    child_stderr_path, &pid, &runner) < 0)
		return -1;
	start = time(NULL);
	for (;;) {
		if (qstar_platform_process_wait_nohang(pid, &status, &done) < 0)
			return qstar_set_error(graph, "qstar: process wait failed");
		if (done)
			break;
		if (time(NULL) - start >= QSTAR_NINJA_TEST_TIMEOUT_SEC) {
			qstar_platform_process_terminate(pid, &status);
			fprintf(out, "test_result label=%s status=timeout timeout_sec=%d\n",
			    target->label, QSTAR_NINJA_TEST_TIMEOUT_SEC);
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "test", target->label,
			    "qstar: test '%s' timed out", target->label);
		}
		if (qstar_platform_process_sleep_ms(10) < 0)
			return qstar_set_error(graph, "qstar: process wait failed");
	}
	if (qstar_platform_process_exited_success(status)) {
		fprintf(out, "test_result label=%s status=pass exit=0 backend=ninja\n",
		    target->label);
		return 0;
	}
	if (qstar_platform_process_has_exit_code(status)) {
		fprintf(out, "test_result label=%s status=fail exit=%d backend=ninja\n",
		    target->label, qstar_platform_process_status_exit_code(status));
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "test", target->label,
		    "qstar: test '%s' failed with status %d", target->label,
		    qstar_platform_process_status_exit_code(status));
	}
	fprintf(out, "test_result label=%s status=signal signal=%d backend=ninja\n",
	    target->label, qstar_platform_process_signal_number(status));
	return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
	    "test", target->label, "qstar: test '%s' terminated by signal %d",
	    target->label, qstar_platform_process_signal_number(status));
}

/** 단일 test target을 Ninja backend로 build 후 실행한다. */
static int
test_one_target_ninja(struct qstar_graph *graph, const struct qstar_target *target, FILE *out)
{
	struct qstar_build_options options;

	if (!qstar_target_has_executable_artifact(target) || strcmp(target->kind, "test") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: target '%s' is not a test target", target->label);
	memset(&options, 0, sizeof(options));
	if (qstar_graph_build_ninja(graph, target->label, &options, out) < 0)
		return -1;
	return run_ninja_test_artifact(graph, target, out);
}

/** Ninja backend로 test target을 build한 뒤 제한된 runner로 실행한다. */
int
qstar_graph_test_ninja(struct qstar_graph *graph, const char *label, FILE *out)
{
	const struct qstar_target *target;
	size_t i, ran;

	fputs("qstar test v1\n", out);
	fprintf(out, "root %s\n", label && *label ? label : "//...");
	fprintf(out, "backend ninja\n");
	if (label && *label && strcmp(label, "//...") != 0) {
		target = find_target(graph, label);
		if (!target)
			return qstar_set_error(graph, "qstar: unknown target label '%s'", label);
		if (test_one_target_ninja(graph, target, out) < 0)
			return -1;
		fputs("status ok\n", out);
		return 0;
	}
	ran = 0;
	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].kind, "test") != 0)
			continue;
		ran++;
		if (test_one_target_ninja(graph, &graph->targets[i], out) < 0)
			return -1;
	}
	if (ran == 0)
		return qstar_set_error(graph, "qstar: no test targets found");
	fputs("status ok\n", out);
	return 0;
}
