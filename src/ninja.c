#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define QSTAR_NINJA_MAX_ARGV 256

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
	char compdb_rel[QSTAR_PATH_MAX];
	char compdb_full[QSTAR_PATH_MAX];
	char default_alias[QSTAR_PATH_MAX];
	const char *root_label;
	int compdb_first;
	int edge_count;
};

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
		if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
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

/** freestanding/cpu/abi profile에서 자동 compile option을 argv에 추가한다. */
static int
append_profile_compile_options(struct qstar_graph *graph, struct ninja_argv *argv)
{
	const char *arch;

	if (profile_is_freestanding(graph)) {
		if (ninja_argv_push(graph, argv, "-ffreestanding") < 0 ||
		    ninja_argv_push(graph, argv, "-fno-builtin") < 0 ||
		    ninja_argv_push(graph, argv, "-fno-stack-protector") < 0)
			return -1;
		arch = profile_arch_hint(graph);
		if ((strstr(arch, "x86_64") || strstr(arch, "amd64")) &&
		    ninja_argv_push(graph, argv, "-mno-red-zone") < 0)
			return -1;
		if ((strstr(arch, "aarch64") || strstr(arch, "arm64")) &&
		    ninja_argv_push(graph, argv, "-mgeneral-regs-only") < 0)
			return -1;
	}
	if (graph->profile.cpu && *graph->profile.cpu &&
	    ninja_argv_pushf(graph, argv, "-mcpu=%s", graph->profile.cpu) < 0)
		return -1;
	if (graph->profile.abi && *graph->profile.abi &&
	    ninja_argv_pushf(graph, argv, "-mabi=%s", graph->profile.abi) < 0)
		return -1;
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

/** source registry 기준으로 compile action이 필요한 입력인지 확인한다. */
static int
source_requires_compile(const struct qstar_source_info *source)
{
	return source->compile_input;
}

/** 이미 만들어진 object 파일을 final archive 입력으로 직접 소비하는지 확인한다. */
static int
source_is_link_object(const struct qstar_source_info *source)
{
	return strcmp(source->language, "object") == 0;
}

/** Ninja backend MVP가 처리할 수 있는 compile source인지 검증한다. */
static int
validate_ninja_compile_source(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_source_info *source, size_t index)
{
	if (source->header_input)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: header source '%s' must be listed as lang.*.public_headers/private_headers",
		    target->sources.items[index]);
	if (strcmp(source->language, "cxx-module") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: ninja backend MVP does not support C++ module source '%s'",
		    target->sources.items[index]);
	if (strcmp(source->language, "cale") == 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: ninja backend MVP supports C, C++, ASM, staticlib, and group only; Cale source '%s' needs -G qstar_graph",
		    target->sources.items[index]);
	if (strcmp(source->language, "c") != 0 && strcmp(source->language, "cxx") != 0 &&
	    !source_is_asm(source))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "sources", target->label,
		    "qstar: ninja backend MVP does not support source '%s' with language '%s'",
		    target->sources.items[index], source->language);
	return 0;
}

/** target source가 final archive action에 제공하는 object path를 계산한다. */
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
write_ninja_header(FILE *f)
{
	fputs("# generated by qstar; do not edit\n", f);
	fputs("ninja_required_version = 1.3\n\n", f);
	fputs("rule qstar_compile\n", f);
	fputs("  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n", f);
	fputs("  depfile = $depfile\n", f);
	fputs("  deps = gcc\n\n", f);
	fputs("rule qstar_compile_nodep\n", f);
	fputs("  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n\n", f);
	fputs("rule qstar_archive\n", f);
	fputs("  command = mkdir -p $out_dir && $command\n", f);
	fputs("  description = [qstar] $description\n\n", f);
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

/** compile action 하나를 Ninja edge와 compile_commands record로 lower한다. */
static int
emit_compile_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    size_t index)
{
	struct qstar_source_info source;
	struct qstar_string_list includes;
	struct ninja_argv argv;
	char object[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], out_dir[QSTAR_PATH_MAX];
	char action_id[QSTAR_PATH_MAX], target_arg[QSTAR_PATH_MAX], sysroot_arg[QSTAR_PATH_MAX];
	char std_arg[128];
	const char *compiler;
	int is_asm, is_cxx, wants_depfile, cross;
	size_t i;

	memset(&argv, 0, sizeof(argv));
	qstar_source_classify(target->sources.items[index], &source);
	if (!source_requires_compile(&source))
		return 0;
	if (validate_ninja_compile_source(graph, target, &source, index) < 0)
		return -1;
	if (qstar_graph_object_output_path(graph, target, index, object, sizeof(object)) < 0 ||
	    qstar_graph_depfile_output_path(graph, target, index, depfile, sizeof(depfile)) < 0 ||
	    path_dirname(object, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja object output path too long");
	memset(&includes, 0, sizeof(includes));
	if (collect_compile_include_dirs(graph, target, &includes) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	is_asm = source_is_asm(&source);
	is_cxx = strcmp(source.language, "cxx") == 0;
	wants_depfile = strcmp(source.language, "c") == 0 || is_cxx ||
	    source_uses_asm_preprocessor(target, &source);
	snprintf(action_id, sizeof(action_id), "%s:compile:%zu", target->label, index);
	snprintf(target_arg, sizeof(target_arg), "--target=%s", toolchain->target);
	snprintf(sysroot_arg, sizeof(sysroot_arg), "--sysroot=%s", toolchain->sysroot);
	snprintf(std_arg, sizeof(std_arg), "-std=%s",
	    target->cxx_standard ? target->cxx_standard : "");
	compiler = is_cxx ? toolchain->cxx : toolchain->cc;
	cross = strcmp(toolchain->name, "clang") == 0 && strcmp(toolchain->target, "host") != 0;
	if (ninja_argv_push(graph, &argv, compiler) < 0)
		goto fail;
	if (cross && ninja_argv_push(graph, &argv, target_arg) < 0)
		goto fail;
	if (toolchain->sysroot[0] && ninja_argv_push(graph, &argv, sysroot_arg) < 0)
		goto fail;
	if (toolchain->resource_dir[0] &&
	    (ninja_argv_push(graph, &argv, "-resource-dir") < 0 ||
	    ninja_argv_push(graph, &argv, toolchain->resource_dir) < 0))
		goto fail;
	if (is_asm &&
	    (ninja_argv_push(graph, &argv, "-x") < 0 ||
	    ninja_argv_push(graph, &argv,
	    source_uses_asm_preprocessor(target, &source) ?
	    "assembler-with-cpp" : "assembler") < 0))
		goto fail;
	if (ninja_argv_push(graph, &argv, "-c") < 0 ||
	    ninja_argv_push(graph, &argv, target->sources.items[index]) < 0 ||
	    ninja_argv_push(graph, &argv, "-o") < 0 ||
	    ninja_argv_push(graph, &argv, object) < 0)
		goto fail;
	if (wants_depfile &&
	    (ninja_argv_push(graph, &argv, "-MMD") < 0 ||
	    ninja_argv_push(graph, &argv, "-MF") < 0 ||
	    ninja_argv_push(graph, &argv, depfile) < 0))
		goto fail;
	if (is_cxx && target->cxx_standard && target->cxx_standard[0] &&
	    ninja_argv_push(graph, &argv, std_arg) < 0)
		goto fail;
	for (i = 0; !is_asm && !is_cxx && i < target->cflags.len; i++) {
		if (ninja_argv_push(graph, &argv, target->cflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_cxx && i < target->cxxflags.len; i++) {
		if (ninja_argv_push(graph, &argv, target->cxxflags.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_asm && i < target->asm_compile_options.len; i++) {
		if (ninja_argv_push(graph, &argv, target->asm_compile_options.items[i]) < 0)
			goto fail;
	}
	if (append_profile_compile_options(graph, &argv) < 0)
		goto fail;
	for (i = 0; i < graph->profile.compile_options.len; i++) {
		if (ninja_argv_push(graph, &argv, graph->profile.compile_options.items[i]) < 0)
			goto fail;
	}
	for (i = 0; i < graph->profile.include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, graph->profile.include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !is_asm && i < includes.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, includes.items[i]) < 0)
			goto fail;
	}
	for (i = 0; is_asm && i < target->asm_include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-I") < 0 ||
		    ninja_argv_push(graph, &argv, target->asm_include_dirs.items[i]) < 0)
			goto fail;
	}
	for (i = 0; !is_asm && i < target->system_include_dirs.len; i++) {
		if (ninja_argv_push(graph, &argv, "-isystem") < 0 ||
		    ninja_argv_push(graph, &argv, target->system_include_dirs.items[i]) < 0)
			goto fail;
	}
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, object);
	fprintf(ctx->ninja, ": %s ", wants_depfile ? "qstar_compile" :
	    "qstar_compile_nodep");
	ninja_path(ctx->ninja, target->sources.items[index]);
	write_header_inputs(ctx->ninja, target);
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	shell_argv(ctx->ninja, &argv);
	fputc('\n', ctx->ninja);
	if (wants_depfile) {
		fputs("  depfile = ", ctx->ninja);
		ninja_path(ctx->ninja, depfile);
		fputc('\n', ctx->ninja);
	}
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fprintf(ctx->ninja, "\n  description = compile %s\n", action_id);
	fprintf(ctx->ninja, "  qstar_action_id = %s\n\n", action_id);
	write_compile_command(ctx, graph->package_root ? graph->package_root : ".",
	    target->sources.items[index], object, &argv);
	ctx->edge_count++;
	qstar_string_list_free(&includes);
	ninja_argv_free(&argv);
	return 0;
fail:
	qstar_string_list_free(&includes);
	ninja_argv_free(&argv);
	return -1;
}

/** target dependency labels를 Ninja order-only dependency alias로 출력한다. */
static int
write_dep_aliases(struct qstar_graph *graph, FILE *f, const struct qstar_target *target,
    const struct qstar_string_list *deps)
{
	char alias[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < deps->len; i++) {
		if (!find_target(graph, deps->items[i]))
			continue;
		if (ninja_alias_path(graph, deps->items[i], alias, sizeof(alias)) < 0)
			return qstar_set_error(graph, "qstar: ninja alias path too long");
		fputc(' ', f);
		ninja_path(f, alias);
	}
	(void)target;
	return 0;
}

/** staticlib final archive action을 Ninja edge로 lower한다. */
static int
emit_staticlib_edge(struct qstar_graph *graph, struct ninja_ctx *ctx,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	struct ninja_argv argv;
	char artifact[QSTAR_PATH_MAX], alias[QSTAR_PATH_MAX], object[QSTAR_PATH_MAX];
	char out_dir[QSTAR_PATH_MAX], action_id[QSTAR_PATH_MAX];
	size_t i;

	memset(&argv, 0, sizeof(argv));
	if (qstar_graph_artifact_output_path(graph, target, artifact, sizeof(artifact)) < 0 ||
	    ninja_alias_path(graph, target->label, alias, sizeof(alias)) < 0 ||
	    path_dirname(artifact, out_dir, sizeof(out_dir)) < 0)
		return qstar_set_error(graph, "qstar: ninja artifact path too long");
	snprintf(action_id, sizeof(action_id), "%s:archive:0", target->label);
	if (ninja_argv_push(graph, &argv, toolchain->ar) < 0 ||
	    ninja_argv_push(graph, &argv, "rcs") < 0 ||
	    ninja_argv_push(graph, &argv, artifact) < 0)
		goto fail;
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_source_classify(target->sources.items[i], &source) < 0)
			goto fail;
		if (!source_requires_compile(&source) && !source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		if (ninja_argv_push(graph, &argv, object) < 0)
			goto fail;
	}
	fprintf(ctx->ninja, "# qstar-action-id: %s\n", action_id);
	fputs("build ", ctx->ninja);
	ninja_path(ctx->ninja, artifact);
	fputs(": qstar_archive", ctx->ninja);
	for (i = 0; i < target->sources.len; i++) {
		struct qstar_source_info source;

		if (qstar_source_classify(target->sources.items[i], &source) < 0)
			goto fail;
		if (!source_requires_compile(&source) && !source_is_link_object(&source))
			continue;
		if (target_source_object_input(graph, target, i, object, sizeof(object)) < 0)
			goto fail;
		fputc(' ', ctx->ninja);
		ninja_path(ctx->ninja, object);
	}
	if (target->deps.len || target->private_deps.len) {
		fputs(" ||", ctx->ninja);
		if (write_dep_aliases(graph, ctx->ninja, target, &target->deps) < 0 ||
		    write_dep_aliases(graph, ctx->ninja, target, &target->private_deps) < 0)
			goto fail;
	}
	fputc('\n', ctx->ninja);
	fputs("  command = ", ctx->ninja);
	shell_argv(ctx->ninja, &argv);
	fputc('\n', ctx->ninja);
	fputs("  out_dir = ", ctx->ninja);
	shell_arg(ctx->ninja, out_dir);
	fprintf(ctx->ninja, "\n  description = archive %s\n", action_id);
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

/** closure target 하나를 Ninja MVP graph로 lower한다. */
static int
emit_target(struct qstar_graph *graph, const struct qstar_target *target, size_t order,
    void *user)
{
	struct ninja_ctx *ctx = user;
	struct qstar_resolved_toolchain toolchain;
	size_t i;

	(void)order;
	if (strcmp(target->kind, "group") == 0)
		return emit_group_edge(graph, ctx, target);
	if (strcmp(target->kind, "staticlib") != 0)
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "kind", target->label,
		    "qstar: ninja backend MVP supports staticlib and group targets only; target '%s' is kind '%s'",
		    target->label, target->kind);
	if (qstar_resolve_toolchain(graph, target, &toolchain) < 0)
		return -1;
	for (i = 0; i < target->sources.len; i++) {
		if (emit_compile_edge(graph, ctx, target, &toolchain, i) < 0)
			return -1;
	}
	return emit_staticlib_edge(graph, ctx, target, &toolchain);
}

/** Ninja output file path와 default alias를 초기화한다. */
static int
init_ninja_ctx(struct qstar_graph *graph, const char *label, struct ninja_ctx *ctx, FILE *out)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->out = out;
	ctx->root_label = label;
	if (qstar_graph_build_path(graph, "ninja/build.ninja", ctx->ninja_rel,
	    sizeof(ctx->ninja_rel)) < 0 ||
	    full_path_under_root(graph, ctx->ninja_rel, ctx->ninja_full,
	    sizeof(ctx->ninja_full)) < 0)
		return qstar_set_error(graph, "qstar: ninja file path too long");
	if (label && ninja_alias_path(graph, label, ctx->default_alias,
	    sizeof(ctx->default_alias)) < 0)
		return qstar_set_error(graph, "qstar: ninja default alias path too long");
	return 0;
}

/** graph closure를 build.ninja와 compile_commands.json으로 출력한다. */
int
qstar_graph_emit_ninja(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct ninja_ctx ctx;

	if (init_ninja_ctx(graph, label, &ctx, out) < 0)
		return -1;
	if (mkdir_parent_under_root(graph, ctx.ninja_rel) < 0)
		return qstar_set_error(graph, "qstar: could not create ninja output dir");
	ctx.ninja = fopen(ctx.ninja_full, "w");
	if (!ctx.ninja)
		return qstar_set_error(graph, "qstar: could not write %s", ctx.ninja_rel);
	if (open_compile_commands(graph, &ctx) < 0) {
		fclose(ctx.ninja);
		ctx.ninja = NULL;
		return -1;
	}
	write_ninja_header(ctx.ninja);
	if (qstar_graph_visit_closure(graph, label, emit_target, &ctx) < 0)
		goto fail;
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
		return -1;
	fprintf(out, "ninja_file %s\n", ctx.ninja_rel);
	if (ctx.compdb_rel[0])
		fprintf(out, "compile_commands %s\n", ctx.compdb_rel);
	else
		fprintf(out, "compile_commands <off>\n");
	if (label)
		fprintf(out, "ninja_default %s\n", ctx.default_alias);
	fprintf(out, "ninja_edges %d\n", ctx.edge_count);
	fprintf(out, "status ok\n");
	return 0;
fail:
	if (ctx.ninja)
		fclose(ctx.ninja);
	if (ctx.compdb)
		fclose(ctx.compdb);
	return -1;
}

/** Ninja executable을 실행해 emitted graph를 빌드한다. */
static int
run_ninja(struct qstar_graph *graph, const char *ninja_rel, const char *alias,
    const struct qstar_build_options *options)
{
	pid_t pid;
	int status, exit_code;
	char jobs_arg[32];

	pid = fork();
	if (pid < 0)
		return qstar_set_error(graph, "qstar: fork failed for ninja");
	if (pid == 0) {
		char *argv[10];
		size_t argc;

		if (chdir(graph->package_root ? graph->package_root : ".") < 0)
			_exit(127);
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
		execvp("ninja", argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return qstar_set_error(graph, "qstar: waitpid failed for ninja");
	exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	if (exit_code == 127)
		return qstar_set_error(graph,
		    "qstar: ninja executable was not found or could not be started");
	if (exit_code != 0)
		return qstar_set_error(graph, "qstar: ninja build failed with exit code %d",
		    exit_code);
	return 0;
}

/** Ninja backend로 build.ninja를 emit한 뒤 requested target을 빌드한다. */
int
qstar_graph_build_ninja(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out)
{
	struct ninja_ctx ctx;

	(void)options;
	if (init_ninja_ctx(graph, label, &ctx, out) < 0)
		return -1;
	if (qstar_graph_emit_ninja(graph, label, out) < 0)
		return -1;
	if (fflush(out) != 0)
		return qstar_set_error(graph, "qstar: could not flush ninja emit output");
	if (run_ninja(graph, ctx.ninja_rel, label ? ctx.default_alias : NULL, options) < 0)
		return -1;
	fprintf(out, "backend ninja\n");
	fprintf(out, "status ok\n");
	return 0;
}
