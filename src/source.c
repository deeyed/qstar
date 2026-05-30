#include "internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** generated output path가 QStar managed output root 아래 있는지 검사한다. */
static int
valid_generated_output_root(const char *path)
{
	return strncmp(path, "generated/", 10) == 0 && path[10] != '\0';
}

/** source list 안에서 같은 path가 두 번 들어왔는지 검사한다. */
static int
source_is_duplicate(const struct qstar_target *target, const char *path, size_t index)
{
	size_t i;

	for (i = 0; i < index; i++) {
		if (strcmp(target->sources.items[i], path) == 0)
			return 1;
	}
	return 0;
}

/** suffix 일치 여부를 확장자 판별용으로 검사한다. */
static int
has_suffix(const char *s, const char *suffix)
{
	size_t ns, nfix;

	ns = strlen(s);
	nfix = strlen(suffix);
	return ns >= nfix && strcmp(s + ns - nfix, suffix) == 0;
}

/** QStar source path를 language/tool role로 분류한다. */
int
qstar_source_classify(const char *path, struct qstar_source_info *info)
{
	if (info) {
		info->path = path;
		info->language = "unknown";
		info->tool_role = "unsupported";
	}
	if (has_suffix(path, ".c")) {
		if (info) {
			info->language = "c";
			info->tool_role = "c-compiler";
		}
		return 0;
	}
	if (has_suffix(path, ".h")) {
		if (info) {
			info->language = "header";
			info->tool_role = "header-input";
		}
		return 0;
	}
	if (has_suffix(path, ".cale")) {
		if (info) {
			info->language = "cale";
			info->tool_role = "cale-compiler";
		}
		return 0;
	}
	if (has_suffix(path, ".s")) {
		if (info) {
			info->language = "asm";
			info->tool_role = "assembler";
		}
		return 0;
	}
	if (has_suffix(path, ".S")) {
		if (info) {
			info->language = "asm-cpp";
			info->tool_role = "preprocessed-assembler";
		}
		return 0;
	}
	return -1;
}

/** source list 하나를 package-relative path와 지원 language 기준으로 검증한다. */
static int
validate_source_list(struct qstar_graph *graph, const struct qstar_target *target)
{
	const char *path;
	size_t i;

	for (i = 0; i < target->sources.len; i++) {
		path = target->sources.items[i];
		if (!qstar_path_is_package_relative(path))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "sources", target->label,
			    "qstar: source path '%s' in '%s' must be package-relative",
			    path, target->label);
		if (source_is_duplicate(target, path, i))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "sources", target->label,
			    "qstar: duplicate source '%s' in '%s'", path, target->label);
		if (qstar_source_classify(path, NULL) < 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "sources", target->label,
			    "qstar: unsupported source extension '%s' in '%s'",
			    path, target->label);
		if (valid_generated_output_root(path) &&
		    !qstar_graph_find_output_owner(graph, path))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "sources", target->label,
			    "qstar: generated source '%s' in '%s' has no generating action",
			    path, target->label);
	}
	return 0;
}

/** generated action skeleton 하나의 input/output edge를 검증한다. */
static int
validate_genrule(struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	const char *path;
	size_t i, j, k;

	if (!genrule->tool || !*genrule->tool)
		return qstar_set_error(graph, "qstar: generated action '%s' has empty tool",
		    genrule->label);
	if (!genrule->config_header && !qstar_path_is_package_relative(genrule->tool))
		return qstar_set_error_origin(graph, genrule->origin_file, genrule->origin_line,
		    "tool", genrule->label,
		    "qstar: generated action tool '%s' in '%s' must be package-relative",
		    genrule->tool, genrule->label);
	if (genrule->outputs.len == 0)
		return qstar_set_error(graph, "qstar: generated action '%s' has no outputs",
		    genrule->label);
	for (i = 0; i < genrule->inputs.len; i++) {
		path = genrule->inputs.items[i];
		if (!qstar_path_is_package_relative(path))
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "inputs", genrule->label,
			    "qstar: generated input '%s' in '%s' must be package-relative",
			    path, genrule->label);
	}
	for (i = 0; i < genrule->outputs.len; i++) {
		path = genrule->outputs.items[i];
		if (!qstar_path_is_package_relative(path))
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "outputs", genrule->label,
			    "qstar: generated output '%s' in '%s' must be package-relative",
			    path, genrule->label);
		if (!valid_generated_output_root(path))
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "outputs", genrule->label,
			    "qstar: generated output '%s' in '%s' must be under generated/",
			    path, genrule->label);
		for (j = 0; j < graph->genrule_len; j++) {
			for (k = 0; k < graph->genrules[j].outputs.len; k++) {
				if (&graph->genrules[j] == genrule && k == i)
					continue;
				if (strcmp(path, graph->genrules[j].outputs.items[k]) == 0)
					return qstar_set_error_origin(graph,
					    genrule->origin_file, genrule->origin_line,
					    "outputs", genrule->label,
					    "qstar: generated output '%s' has multiple producers",
					    path);
			}
		}
	}
	return 0;
}

/** QStar generated output edge skeleton을 검증한다. */
int
qstar_graph_validate_generated_outputs(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (validate_genrule(graph, &graph->genrules[i]) < 0)
			return -1;
	}
	return 0;
}

/** QStar source path와 language classification을 검증한다. */
int
qstar_graph_validate_sources(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (validate_source_list(graph, &graph->targets[i]) < 0)
			return -1;
	}
	return 0;
}

/** package root 기준 path가 실제 파일로 존재하는지 확인한다. */
static int
file_exists_under_root(const struct qstar_graph *graph, const char *path)
{
	char full[QSTAR_PATH_MAX];
	FILE *f;

	if (qstar_path_join(graph->package_root ? graph->package_root : ".", path, full,
	    sizeof(full)) < 0)
		return 0;
	f = fopen(full, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** target의 authoring input 파일이 package root 아래 존재하는지 검증한다. */
static int
validate_target_file_inputs(struct qstar_graph *graph, const struct qstar_target *target)
{
	const char *path;
	size_t i;

	for (i = 0; i < target->sources.len; i++) {
		path = target->sources.items[i];
		if (valid_generated_output_root(path) && qstar_graph_find_output_owner(graph, path))
			continue;
		if (!file_exists_under_root(graph, path))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "sources", target->label,
			    "qstar: source file '%s' in '%s' does not exist under package root",
			    path, target->label);
	}
	for (i = 0; i < target->public_headers.len; i++) {
		path = target->public_headers.items[i];
		if (valid_generated_output_root(path) && qstar_graph_find_output_owner(graph, path))
			continue;
		if (!file_exists_under_root(graph, path))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "public_headers", target->label,
			    "qstar: header file '%s' in '%s' does not exist under package root",
			    path, target->label);
	}
	for (i = 0; i < target->private_headers.len; i++) {
		path = target->private_headers.items[i];
		if (valid_generated_output_root(path) && qstar_graph_find_output_owner(graph, path))
			continue;
		if (!file_exists_under_root(graph, path))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "private_headers", target->label,
			    "qstar: header file '%s' in '%s' does not exist under package root",
			    path, target->label);
	}
	return 0;
}

/** generated action input 파일이 package root 아래 존재하는지 검증한다. */
static int
validate_genrule_file_inputs(struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	const char *path;
	size_t i;

	for (i = 0; i < genrule->inputs.len; i++) {
		path = genrule->inputs.items[i];
		if (!file_exists_under_root(graph, path))
			return qstar_set_error_origin(graph, genrule->origin_file,
			    genrule->origin_line, "inputs", genrule->label,
			    "qstar: generated input '%s' in '%s' does not exist under package root",
			    path, genrule->label);
	}
	return 0;
}

/** QStar authoring input file이 package root 아래 실제로 존재하는지 검증한다. */
int
qstar_graph_validate_file_inputs(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (validate_genrule_file_inputs(graph, &graph->genrules[i]) < 0)
			return -1;
	}
	for (i = 0; i < graph->len; i++) {
		if (validate_target_file_inputs(graph, &graph->targets[i]) < 0)
			return -1;
	}
	return 0;
}

/** source discovery 결과 하나를 deterministic metadata line으로 출력한다. */
static void
dump_source(FILE *out, const char *path)
{
	struct qstar_source_info info;

	if (qstar_source_classify(path, &info) < 0)
		return;
	fprintf(out, "  source_file path=%s language=%s tool=%s role=compile\n",
	    path, info.language, info.tool_role);
}

/** QStar target의 source discovery skeleton을 출력한다. */
void
qstar_target_dump_source_discovery(const struct qstar_target *target, FILE *out)
{
	size_t i;

	fprintf(out, "  source_discovery explicit=%zu modules=%s status=explicit-only\n",
	    target->sources.len, target->modules.present ? "present" : "absent");
	for (i = 0; i < target->sources.len; i++)
		dump_source(out, target->sources.items[i]);
}
