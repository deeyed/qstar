#include "internal.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** 메시지 문자열이 부분 문자열을 포함하는지 검사한다. */
static int
contains_text(const char *message, const char *needle)
{
	return message && needle && strstr(message, needle) != NULL;
}

/** 기존 graph error 메시지를 QSTAR lint code로 보수적으로 분류한다. */
static const char *
classify_error_code(const char *message)
{
	if (contains_text(message, "could not find qstar.lua") ||
	    contains_text(message, "root entry must be qstar.lua"))
		return "QSTAR001";
	if (contains_text(message, "leaks private include") ||
	    contains_text(message, "exposes private header"))
		return "QSTAR030";
	if (contains_text(message, "header source"))
		return "QSTAR040";
	if (contains_text(message, "public header") &&
	    contains_text(message, "must be under"))
		return "QSTAR041";
	if (contains_text(message, "invalid visibility pattern"))
		return "QSTAR050";
	if (contains_text(message, "generated output") &&
	    contains_text(message, "multiple producers"))
		return "QSTAR060";
	if (contains_text(message, "must be package-relative") ||
	    contains_text(message, "escapes package root") ||
	    contains_text(message, "outside package root"))
		return "QSTAR020";
	if (contains_text(message, "unknown target label") ||
	    contains_text(message, "invalid label") ||
	    contains_text(message, "invalid target name") ||
	    contains_text(message, "owned by package") ||
	    contains_text(message, "not visible"))
		return "QSTAR010";
	if (contains_text(message, "duplicate target label") ||
	    contains_text(message, "duplicate generated action label") ||
	    contains_text(message, "already used by generated action") ||
	    contains_text(message, "conflicts with target"))
		return "QSTAR011";
	return "QSTAR900";
}

/** QStar lint diagnostic을 graph에 추가한다. */
int
qstar_graph_add_lint(struct qstar_graph *graph, const char *code,
    const char *severity, const char *file, int line, const char *field,
    const char *label, const char *fmt, ...)
{
	struct qstar_lint_diagnostic *items, *diag;
	size_t cap;
	char message[1024];
	va_list ap;

	if (!graph)
		return -1;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	if (graph->lint_len == graph->lint_cap) {
		cap = graph->lint_cap ? graph->lint_cap * 2 : 8;
		items = realloc(graph->lint_diagnostics, cap * sizeof(items[0]));
		if (!items)
			return qstar_set_error(graph, "qstar: out of memory");
		graph->lint_diagnostics = items;
		graph->lint_cap = cap;
	}
	diag = &graph->lint_diagnostics[graph->lint_len++];
	memset(diag, 0, sizeof(*diag));
	diag->code = qstar_strdup(code ? code : "QSTAR900");
	diag->severity = qstar_strdup(severity ? severity : "error");
	diag->file = qstar_strdup(file && *file ? file : "<unknown>");
	diag->field = qstar_strdup(field && *field ? field : "<none>");
	diag->label = qstar_strdup(label && *label ? label : "<none>");
	diag->message = qstar_strdup(message);
	diag->line = line;
	if (!diag->code || !diag->severity || !diag->file || !diag->field ||
	    !diag->label || !diag->message)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** 현재 graph error buffer를 lint diagnostic으로 변환해 추가한다. */
int
qstar_graph_add_lint_from_error(struct qstar_graph *graph)
{
	if (!graph || !graph->error[0])
		return 0;
	return qstar_graph_add_lint(graph, classify_error_code(graph->error), "error",
	    graph->error_file[0] ? graph->error_file : "<unknown>",
	    graph->error_line,
	    graph->error_field[0] ? graph->error_field : "<none>",
	    graph->error_label[0] ? graph->error_label : "<none>",
	    "%s", graph->error);
}

/** label이 graph target으로 존재하는지 확인한다. */
static int
target_exists(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return 1;
	}
	return 0;
}

/** JSON 문자열을 LSP가 읽을 수 있게 escaping한다. */
static void
json_string(FILE *out, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	fputc('"', out);
	while (*p) {
		if (*p == '"' || *p == '\\')
			fprintf(out, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", out);
		else if (*p == '\r')
			fputs("\\r", out);
		else if (*p == '\t')
			fputs("\\t", out);
		else if (*p < 0x20)
			fprintf(out, "\\u%04x", *p);
		else
			fputc(*p, out);
		p++;
	}
	fputc('"', out);
}

/** diagnostic severity별 개수를 계산한다. */
static void
count_diagnostics(const struct qstar_graph *graph, int *errors, int *warnings)
{
	size_t i;

	*errors = 0;
	*warnings = 0;
	for (i = 0; i < graph->lint_len; i++) {
		if (strcmp(graph->lint_diagnostics[i].severity, "error") == 0)
			(*errors)++;
		else if (strcmp(graph->lint_diagnostics[i].severity, "warning") == 0)
			(*warnings)++;
	}
}

/** lint text output에 ANSI color를 쓸지 결정한다. */
static int
lint_color_enabled(FILE *out, int color_mode)
{
	if (color_mode == QSTAR_COLOR_ALWAYS)
		return 1;
	if (color_mode == QSTAR_COLOR_NEVER)
		return 0;
	return isatty(fileno(out));
}

/** severity를 color policy에 맞게 출력한다. */
static void
print_colored_severity(FILE *out, const char *severity, int use_color)
{
	const char *color;

	color = "";
	if (use_color && strcmp(severity, "warning") == 0)
		color = "\033[1;33m";
	else if (use_color && strcmp(severity, "error") == 0)
		color = "\033[1;31m";
	fprintf(out, "%s%s%s", color, severity, use_color && *color ? "\033[0m" : "");
}

/** status를 color policy에 맞게 출력한다. */
static void
print_colored_status(FILE *out, const char *status, int use_color)
{
	const char *color;

	color = "";
	if (use_color && strcmp(status, "ok") == 0)
		color = "\033[32m";
	else if (use_color && strcmp(status, "warning") == 0)
		color = "\033[1;33m";
	else if (use_color && strcmp(status, "error") == 0)
		color = "\033[1;31m";
	fprintf(out, "%s%s%s", color, status, use_color && *color ? "\033[0m" : "");
}

/** path가 suffix로 끝나는지 검사한다. */
static int
has_suffix(const char *path, const char *suffix)
{
	size_t npath, nsuffix;

	npath = strlen(path);
	nsuffix = strlen(suffix);
	return npath >= nsuffix && strcmp(path + npath - nsuffix, suffix) == 0;
}

/** lint label scope에 target이 포함되는지 검사한다. */
static int
target_in_scope(const struct qstar_target *target, const char *label)
{
	return !label || !*label || strcmp(label, "//...") == 0 ||
	    strcmp(label, target->label) == 0;
}

/** canonical public header root인지 검사한다. */
static int
is_public_header_root(const struct qstar_target *target, const char *path)
{
	char package[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX];

	if (qstar_label_package_path(target->label, package, sizeof(package)) < 0)
		return 0;
	if (!package[0])
		return strncmp(path, "include/", 8) == 0 && path[8] != '\0';
	if (snprintf(prefix, sizeof(prefix), "%s/include/", package) >=
	    (int)sizeof(prefix))
		return 0;
	return strncmp(path, prefix, strlen(prefix)) == 0 && path[strlen(prefix)] != '\0';
}

/** graph 안에서 target label을 찾는다. */
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

/** string list에 정확히 같은 항목이 있는지 검사한다. */
static int
string_list_contains(const struct qstar_string_list *list, const char *value)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], value) == 0)
			return 1;
	}
	return 0;
}

/** target label의 colon 뒤 target name 부분을 반환한다. */
static const char *
target_label_name(const struct qstar_target *target)
{
	const char *colon;

	colon = strrchr(target->label, ':');
	return colon ? colon + 1 : target->name;
}

/** target name이 family variant naming convention에 맞는지 검사한다. */
static int
target_matches_family_variant(const struct qstar_target *target,
    const struct qstar_target_family *family, const char *variant)
{
	char pattern[QSTAR_PATH_MAX];
	const char *name;

	name = target->name && *target->name ? target->name : target_label_name(target);
	if (snprintf(pattern, sizeof(pattern), "%s_%s", family->name, variant) <
	    (int)sizeof(pattern) && strcmp(name, pattern) == 0)
		return 1;
	if (snprintf(pattern, sizeof(pattern), "%s-%s", family->name, variant) <
	    (int)sizeof(pattern) && strcmp(name, pattern) == 0)
		return 1;
	return 0;
}

/** target이 target_family에 속하는지 검사한다. */
static int
target_in_family(const struct qstar_target *target,
    const struct qstar_target_family *family)
{
	size_t i;

	if (string_list_contains(&family->targets, target->label))
		return 1;
	for (i = 0; i < family->variants.len; i++) {
		if (target_matches_family_variant(target, family, family->variants.items[i]))
			return 1;
	}
	return 0;
}

/** target pair가 shared source를 허용하는 같은 family에 속하는지 검사한다. */
static int
target_pair_allows_shared_source(const struct qstar_graph *graph,
    const struct qstar_target *a, const struct qstar_target *b)
{
	size_t i;

	for (i = 0; i < graph->family_len; i++) {
		const struct qstar_target_family *family = &graph->families[i];

		if (!family->allow_shared_sources)
			continue;
		if (target_in_family(a, family) && target_in_family(b, family))
			return 1;
	}
	return 0;
}

/** 기록된 evaluated fragment에 path가 포함되는지 검사한다. */
static int
fragment_was_evaluated(const struct qstar_graph *graph, const char *path)
{
	size_t i;

	for (i = 0; i < graph->evaluated_fragments.len; i++) {
		if (strcmp(graph->evaluated_fragments.items[i], path) == 0)
			return 1;
	}
	return 0;
}

/** qstar source path가 canonical <folder>/<folder>.qst 형태인지 검사한다. */
static int
is_canonical_subdir_fragment(const char *path)
{
	const char *slash, *base, *dot;
	size_t folder_len, base_len;

	if (!has_suffix(path, ".qst"))
		return 0;
	slash = strrchr(path, '/');
	if (!slash)
		return 0;
	base = slash + 1;
	dot = strrchr(base, '.');
	if (!dot)
		return 0;
	base_len = (size_t)(dot - base);
	folder_len = (size_t)(slash - path);
	while (folder_len > 0 && path[folder_len - 1] != '/')
		folder_len--;
	if (folder_len > 0)
		folder_len = (size_t)(slash - path - folder_len);
	else
		folder_len = (size_t)(slash - path);
	return base_len == folder_len &&
	    strncmp(base, slash - folder_len, base_len) == 0;
}

/** directory 이름이 lint scan에서 건너뛰어야 하는 cache/vendor 경계인지 검사한다. */
static int
skip_lint_dir(const char *name)
{
	return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
	    strcmp(name, ".git") == 0 || strcmp(name, ".qstar") == 0 ||
	    strcmp(name, "node_modules") == 0 || strcmp(name, "build") == 0 ||
	    strcmp(name, "vendor") == 0;
}

/** package root 아래 .qst fragment를 재귀적으로 찾아 orphan warning을 추가한다. */
static int
scan_orphan_fragments(struct qstar_graph *graph, const char *rel_dir)
{
	char full[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	DIR *dir, *child;
	struct dirent *ent;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    rel_dir && *rel_dir ? rel_dir : ".", full, sizeof(full)) < 0)
		return qstar_graph_add_lint(graph, "QSTAR070", "warning",
		    "<lint>", 0, "subdir", "<none>",
		    "orphan fragment scan path is too long");
	dir = opendir(full);
	if (!dir)
		return 0;
	while ((ent = readdir(dir)) != NULL) {
		if (skip_lint_dir(ent->d_name))
			continue;
		if (rel_dir && *rel_dir)
			snprintf(rel, sizeof(rel), "%s/%s", rel_dir, ent->d_name);
		else
			snprintf(rel, sizeof(rel), "%s", ent->d_name);
		if (qstar_path_join(graph->package_root ? graph->package_root : ".",
		    rel, full, sizeof(full)) < 0)
			continue;
		child = opendir(full);
		if (child) {
			closedir(child);
			if (scan_orphan_fragments(graph, rel) < 0) {
				closedir(dir);
				return -1;
			}
			continue;
		}
		if (has_suffix(rel, ".qs")) {
			if (qstar_graph_add_lint(graph, "QSTAR003", "error", rel,
			    1, "subdir", "<none>",
			    ".qs fragments were removed; rename '%s' to .qst",
			    rel) < 0) {
				closedir(dir);
				return -1;
			}
			continue;
		}
		if (strcmp(ent->d_name, "qstar.workspace") == 0) {
			if (qstar_graph_add_lint(graph, "QSTAR004", "error", rel,
			    1, "file", "<none>",
			    "qstar.workspace was removed; qstar.lua is the package root marker") < 0) {
				closedir(dir);
				return -1;
			}
			continue;
		}
		if (!has_suffix(rel, ".qst") || fragment_was_evaluated(graph, rel))
			continue;
		if (is_canonical_subdir_fragment(rel)) {
			if (qstar_graph_add_lint(graph, "QSTAR071", "warning", rel,
			    1, "subdir", "<none>",
			    "canonical fragment '%s' is not reached by qstar.subdir()",
			    rel) < 0) {
				closedir(dir);
				return -1;
			}
		} else if (qstar_graph_add_lint(graph, "QSTAR070", "warning", rel,
		    1, "subdir", "<none>",
		    "orphan .qst fragment '%s' is not reached by qstar.subdir()",
		    rel) < 0) {
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);
	return 0;
}

/** target-level authoring smell을 warning/info diagnostic으로 수집한다. */
static int
lint_target_shape(struct qstar_graph *graph, const struct qstar_target *target,
    const char *label)
{
	struct qstar_source_info info;
	const struct qstar_target *dep;
	size_t i;
	int saw_cxx_missing_standard;

	if (!target_in_scope(target, label))
		return 0;
	saw_cxx_missing_standard = 0;
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_source_classify(target->sources.items[i], &info) < 0)
			continue;
		if (info.header_input) {
			if (qstar_graph_add_lint(graph, "QSTAR040", "warning",
			    target->origin_file, target->origin_line, "sources",
			    target->label,
			    "header '%s' is listed in sources; use lang.*.public_headers/private_headers",
			    target->sources.items[i]) < 0)
				return -1;
		}
		if (strcmp(info.language, "cxx") == 0 &&
		    (!target->cxx_standard || !*target->cxx_standard) &&
		    !saw_cxx_missing_standard) {
			saw_cxx_missing_standard = 1;
			if (qstar_graph_add_lint(graph, "QSTAR044", "info",
			    target->origin_file, target->origin_line, "lang.cxx.standard",
			    target->label,
			    "C++ source in '%s' has no lang.cxx.standard; default compiler mode will be used",
			    target->label) < 0)
				return -1;
		}
		}
	for (i = 0; i < target->public_headers.len; i++) {
		if (!is_public_header_root(target, target->public_headers.items[i]) &&
		    qstar_graph_find_output_owner(graph, target->public_headers.items[i]) &&
		    qstar_graph_add_lint(graph, "QSTAR041", "warning",
		    target->origin_file, target->origin_line, "public_headers",
		    target->label,
		    "generated public header '%s' is outside include/ install surface",
		    target->public_headers.items[i]) < 0)
			return -1;
	}
	if (target->public_headers.len > 0) {
		for (i = 0; i < target->private_deps.len; i++) {
			dep = find_target(graph, target->private_deps.items[i]);
			if (!dep)
				continue;
			if (dep->public_headers.len > 0 || dep->public_include_dirs.len > 0) {
				if (qstar_graph_add_lint(graph, "QSTAR042", "warning",
				    target->origin_file, target->origin_line,
				    "private_deps", target->label,
				    "private dependency '%s' has public include surface; use deps/public_deps if '%s' public headers expose it",
				    dep->label, target->label) < 0)
					return -1;
			}
		}
	}
	return 0;
}

/** target 간 source 재사용을 warning으로 수집한다. */
static int
lint_duplicate_sources_across_targets(struct qstar_graph *graph, const char *label)
{
	size_t i, j, a, b;

	for (i = 0; i < graph->len; i++) {
		for (j = i + 1; j < graph->len; j++) {
			if (!target_in_scope(&graph->targets[i], label) &&
			    !target_in_scope(&graph->targets[j], label))
				continue;
			for (a = 0; a < graph->targets[i].sources.len; a++) {
				for (b = 0; b < graph->targets[j].sources.len; b++) {
					if (strcmp(graph->targets[i].sources.items[a],
					    graph->targets[j].sources.items[b]) != 0)
						continue;
					if (target_pair_allows_shared_source(graph,
					    &graph->targets[i], &graph->targets[j]))
						continue;
					if (qstar_graph_add_lint(graph, "QSTAR043",
					    "warning", graph->targets[j].origin_file,
					    graph->targets[j].origin_line, "sources",
					    graph->targets[j].label,
					    "source '%s' is used by both '%s' and '%s'",
					    graph->targets[i].sources.items[a],
					    graph->targets[i].label,
					    graph->targets[j].label) < 0)
						return -1;
				}
			}
		}
	}
	return 0;
}

/** 기존 validator를 통과한 graph 위에 authoring lint pass를 추가한다. */
static int
run_deep_lint(struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (lint_target_shape(graph, &graph->targets[i], label) < 0)
			return -1;
	}
	if (lint_duplicate_sources_across_targets(graph, label) < 0)
		return -1;
	if ((!label || !*label || strcmp(label, "//...") == 0) &&
	    scan_orphan_fragments(graph, "") < 0)
		return -1;
	return 0;
}

/** text lint output을 deterministic line format으로 출력한다. */
static void
emit_text(const struct qstar_graph *graph, int color_mode, FILE *out)
{
	int errors, warnings;
	int use_color;
	size_t i;

	use_color = lint_color_enabled(out, color_mode);
	count_diagnostics(graph, &errors, &warnings);
	fputs("qstar lint v1\n", out);
	for (i = 0; i < graph->lint_len; i++) {
		const struct qstar_lint_diagnostic *diag = &graph->lint_diagnostics[i];
		fprintf(out,
		    "diagnostic code=%s severity=", diag->code);
		print_colored_severity(out, diag->severity, use_color);
		fprintf(out,
		    " file=%s line=%d field=%s label=%s message=%s\n",
		    diag->file, diag->line, diag->field, diag->label, diag->message);
	}
	fprintf(out, "summary errors=%d warnings=%d\n", errors, warnings);
	fputs("status ", out);
	print_colored_status(out, errors ? "error" : warnings ? "warning" : "ok",
	    use_color);
	fputc('\n', out);
}

/** JSON lint output을 단일 object로 출력한다. */
static void
emit_json(const struct qstar_graph *graph, FILE *out)
{
	int errors, warnings;
	size_t i;

	count_diagnostics(graph, &errors, &warnings);
	fputs("{\"schema\":\"qstar-lint-v1\",\"diagnostics\":[", out);
	for (i = 0; i < graph->lint_len; i++) {
		const struct qstar_lint_diagnostic *diag = &graph->lint_diagnostics[i];
		if (i)
			fputc(',', out);
		fputs("{\"code\":", out);
		json_string(out, diag->code);
		fputs(",\"severity\":", out);
		json_string(out, diag->severity);
		fputs(",\"file\":", out);
		json_string(out, diag->file);
		fprintf(out, ",\"line\":%d,\"field\":", diag->line);
		json_string(out, diag->field);
		fputs(",\"label\":", out);
		json_string(out, diag->label);
		fputs(",\"message\":", out);
		json_string(out, diag->message);
		fputc('}', out);
	}
	fprintf(out,
	    "],\"summary\":{\"errors\":%d,\"warnings\":%d},\"status\":",
	    errors, warnings);
	json_string(out, errors ? "error" : warnings ? "warning" : "ok");
	fputs("}\n", out);
}

/** QStar lint diagnostic을 text 또는 LSP-ready JSON으로 출력한다. */
int
qstar_graph_lint(struct qstar_graph *graph, const char *label, const char *format, FILE *out)
{
	return qstar_graph_lint_with_color(graph, label, format, QSTAR_COLOR_AUTO, out);
}

/** QStar lint diagnostic을 text 또는 LSP-ready JSON으로 출력한다. */
int
qstar_graph_lint_with_color(struct qstar_graph *graph, const char *label,
    const char *format, int color_mode, FILE *out)
{
	int errors, warnings;

	if (!graph->error[0] && run_deep_lint(graph, label) < 0)
		return -1;
	if (label && *label && strcmp(label, "//...") != 0 && !target_exists(graph, label)) {
		if (qstar_graph_add_lint(graph, "QSTAR010", "error", "<command-line>", 0,
		    "label", label, "unknown target label '%s'", label) < 0)
			return -1;
	}
	if (!format || strcmp(format, "text") == 0)
		emit_text(graph, color_mode, out);
	else if (strcmp(format, "json") == 0)
		emit_json(graph, out);
	else
		return qstar_set_error(graph, "qstar: unsupported lint format '%s'", format);
	count_diagnostics(graph, &errors, &warnings);
	(void)warnings;
	return errors ? -1 : 0;
}
