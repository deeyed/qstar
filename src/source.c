#include "internal.h"

#include <stdio.h>
#include <string.h>

/** source path가 package-relative normalized path인지 검사한다. */
static int
valid_relative_path(const char *path)
{
	if (!path || !*path || path[0] == '/')
		return 0;
	if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0)
		return 0;
	if (strncmp(path, "../", 3) == 0 || strstr(path, "/../") ||
	    strstr(path, "/./") || strstr(path, "//"))
		return 0;
	return 1;
}

/** generated output path가 QStar managed output root 아래 있는지 검사한다. */
static int
valid_generated_output_root(const char *path)
{
	return strncmp(path, "generated/", 10) == 0 && path[10] != '\0';
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
		if (!valid_relative_path(path))
			return qstar_set_error(graph,
			    "qstar: source path '%s' in '%s' must be package-relative",
			    path, target->label);
		if (qstar_source_classify(path, NULL) < 0)
			return qstar_set_error(graph,
			    "qstar: unsupported source extension '%s' in '%s'",
			    path, target->label);
		if (valid_generated_output_root(path) &&
		    !qstar_graph_find_output_owner(graph, path))
			return qstar_set_error(graph,
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
	if (genrule->outputs.len == 0)
		return qstar_set_error(graph, "qstar: generated action '%s' has no outputs",
		    genrule->label);
	for (i = 0; i < genrule->inputs.len; i++) {
		path = genrule->inputs.items[i];
		if (!valid_relative_path(path))
			return qstar_set_error(graph,
			    "qstar: generated input '%s' in '%s' must be package-relative",
			    path, genrule->label);
	}
	for (i = 0; i < genrule->outputs.len; i++) {
		path = genrule->outputs.items[i];
		if (!valid_relative_path(path))
			return qstar_set_error(graph,
			    "qstar: generated output '%s' in '%s' must be package-relative",
			    path, genrule->label);
		if (!valid_generated_output_root(path))
			return qstar_set_error(graph,
			    "qstar: generated output '%s' in '%s' must be under generated/",
			    path, genrule->label);
		for (j = 0; j < graph->genrule_len; j++) {
			for (k = 0; k < graph->genrules[j].outputs.len; k++) {
				if (&graph->genrules[j] == genrule && k == i)
					continue;
				if (strcmp(path, graph->genrules[j].outputs.items[k]) == 0)
					return qstar_set_error(graph,
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
