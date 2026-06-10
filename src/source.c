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

static int
list_has_duplicate(const struct qstar_string_list *list, const char **dup)
{
	size_t i, j;

	for (i = 0; i < list->len; i++) {
		for (j = 0; j < i; j++) {
			if (strcmp(list->items[i], list->items[j]) == 0) {
				if (dup)
					*dup = list->items[i];
				return 1;
			}
		}
	}
	return 0;
}

static int
list_pair_has_duplicate(const struct qstar_string_list *a,
    const struct qstar_string_list *b, const char **dup)
{
	size_t i, j;

	for (i = 0; i < a->len; i++) {
		for (j = 0; j < b->len; j++) {
			if (strcmp(a->items[i], b->items[j]) == 0) {
				if (dup)
					*dup = a->items[i];
				return 1;
			}
		}
	}
	return 0;
}

/** defsym entry를 NAME=VALUE 형태로 제한한다. */
static int
valid_defsym(const char *value)
{
	const char *eq;

	if (!value || !*value)
		return 0;
	eq = strchr(value, '=');
	return eq && eq != value && eq[1] != '\0';
}

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

/** generated artifact location metadata가 충돌 검사에 의미 있는지 본다. */
static int
artifact_has_location_metadata(const struct qstar_genrule *genrule, size_t index)
{
	return strcmp(qstar_genrule_output_address(genrule, index), "<none>") != 0 ||
	    strcmp(qstar_genrule_output_layout(genrule, index), "<none>") != 0;
}

/** 두 generated output이 같은 binary/image artifact identity 위치를 가리키는지 본다. */
static int
artifact_location_collides(const struct qstar_genrule *a, size_t ai,
    const struct qstar_genrule *b, size_t bi)
{
	if (!artifact_has_location_metadata(a, ai) || !artifact_has_location_metadata(b, bi))
		return 0;
	return strcmp(qstar_genrule_output_group(a, ai),
	    qstar_genrule_output_group(b, bi)) == 0 &&
	    strcmp(qstar_genrule_output_format(a, ai),
	    qstar_genrule_output_format(b, bi)) == 0 &&
	    strcmp(qstar_genrule_output_address(a, ai),
	    qstar_genrule_output_address(b, bi)) == 0 &&
	    strcmp(qstar_genrule_output_layout(a, ai),
	    qstar_genrule_output_layout(b, bi)) == 0;
}

/** include directory list 하나를 package-relative path로 제한한다. */
static int
validate_include_dir_list(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_string_list *list, const char *field)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (!qstar_path_is_package_relative(list->items[i]))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, field, target->label,
			    "qstar: include directory '%s' in '%s' must be package-relative",
			    list->items[i], target->label);
	}
	return 0;
}

/** target의 package path와 선언 fragment ownership이 일치하는지 확인한다. */
static int
validate_target_ownership(struct qstar_graph *graph, const struct qstar_target *target)
{
	char package[QSTAR_PATH_MAX];

	if (qstar_label_package_path(target->label, package, sizeof(package)) < 0)
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "label", target->label,
		    "qstar: external target label '%s' cannot be declared in this package",
		    target->label);
	if (strcmp(package, target->fragment_dir) != 0)
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "label", target->label,
		    "qstar: target '%s' is owned by package '%s' but declared in package '%s'",
		    target->label, package[0] ? package : "<root>",
		    target->fragment_dir[0] ? target->fragment_dir : "<root>");
	return 0;
}

/** generated action label도 선언 fragment package에만 소유되도록 제한한다. */
static int
validate_genrule_ownership(struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	char package[QSTAR_PATH_MAX];

	if (qstar_label_package_path(genrule->label, package, sizeof(package)) < 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "label", genrule->label,
		    "qstar: external generated action label '%s' cannot be declared in this package",
		    genrule->label);
	if (strcmp(package, genrule->fragment_dir) != 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "label", genrule->label,
		    "qstar: generated action '%s' is owned by package '%s' but declared in package '%s'",
		    genrule->label, package[0] ? package : "<root>",
		    genrule->fragment_dir[0] ? genrule->fragment_dir : "<root>");
	return 0;
}

static int
visibility_pattern_matches(const char *pattern, const char *consumer_label,
    const char *consumer_package)
{
	char package[QSTAR_PATH_MAX];
	size_t n;

	if (strcmp(pattern, "//...") == 0)
		return 1;
	if (strcmp(pattern, consumer_label) == 0)
		return 1;
	if (strncmp(pattern, "//", 2) != 0)
		return 0;
	n = strlen(pattern);
	if (n >= 4 && strcmp(pattern + n - 4, ":...") == 0) {
		if (n - 4 - 2 >= sizeof(package))
			return 0;
		memcpy(package, pattern + 2, n - 4 - 2);
		package[n - 4 - 2] = '\0';
		return strcmp(package, consumer_package) == 0;
	}
	return 0;
}

/** dependency target이 consumer에게 보이는지 visibility skeleton 기준으로 확인한다. */
static int
target_visible_to(const struct qstar_target *dep, const struct qstar_target *consumer)
{
	char dep_package[QSTAR_PATH_MAX], consumer_package[QSTAR_PATH_MAX];
	size_t i;

	if (qstar_label_package_path(dep->label, dep_package, sizeof(dep_package)) < 0 ||
	    qstar_label_package_path(consumer->label, consumer_package,
	    sizeof(consumer_package)) < 0)
		return 0;
	if (strcmp(dep_package, consumer_package) == 0)
		return 1;
	if (dep->visibility.len == 0)
		return 1;
	for (i = 0; i < dep->visibility.len; i++) {
		if (visibility_pattern_matches(dep->visibility.items[i], consumer->label,
		    consumer_package))
			return 1;
	}
	return 0;
}

/** visibility entry가 v1 skeleton에서 지원하는 형태인지 확인한다. */
static int
validate_visibility_list(struct qstar_graph *graph, const struct qstar_target *target)
{
	size_t i, n;
	const char *v;

	for (i = 0; i < target->visibility.len; i++) {
		v = target->visibility.items[i];
		n = strlen(v);
		if (strcmp(v, "//...") == 0)
			continue;
		if (strncmp(v, "//", 2) == 0 &&
		    ((n >= 4 && strcmp(v + n - 4, ":...") == 0) ||
		    qstar_label_canonicalize(v, target->fragment_dir,
		    (char[QSTAR_PATH_MAX]){0}, QSTAR_PATH_MAX) == 0))
			continue;
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "visibility", target->label,
		    "qstar: invalid visibility pattern '%s' in '%s'",
		    v, target->label);
	}
	return 0;
}

/** dependency가 visible인지, public/private include 경계가 새지 않는지 확인한다. */
static int
validate_dependency_boundary(struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_string_list *deps, const char *field)
{
	const struct qstar_target *dep;
	size_t i, j;

	for (i = 0; i < deps->len; i++) {
		if (deps->items[i][0] == '@' || strcmp(deps->items[i], "<select>") == 0)
			continue;
		dep = find_target(graph, deps->items[i]);
		if (!dep)
			continue;
		if (!target_visible_to(dep, target))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, field, target->label,
			    "qstar: target '%s' is not visible to '%s'",
			    dep->label, target->label);
		for (j = 0; j < dep->private_include_dirs.len; j++) {
			if (list_contains(&target->include_dirs, dep->private_include_dirs.items[j]) ||
			    list_contains(&target->public_include_dirs,
			    dep->private_include_dirs.items[j]))
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "include_dirs", target->label,
				    "qstar: target '%s' leaks private include directory '%s' from '%s'",
				    target->label, dep->private_include_dirs.items[j], dep->label);
		}
		for (j = 0; j < dep->private_headers.len; j++) {
			if (list_contains(&target->public_headers, dep->private_headers.items[j]))
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "public_headers", target->label,
				    "qstar: target '%s' exposes private header '%s' from '%s'",
				    target->label, dep->private_headers.items[j], dep->label);
		}
	}
	return 0;
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

static int
validate_link_lists(struct qstar_graph *graph, const struct qstar_target *target)
{
	const char *dup;
	size_t i;

	if (list_has_duplicate(&target->deps, &dup) ||
	    list_has_duplicate(&target->private_deps, &dup) ||
	    list_pair_has_duplicate(&target->deps, &target->private_deps, &dup))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "deps", target->label,
		    "qstar: duplicate dependency '%s' in '%s'", dup, target->label);
	if (list_has_duplicate(&target->libs, &dup))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "libs", target->label,
		    "qstar: duplicate system library '%s' in '%s'", dup, target->label);
	if (list_has_duplicate(&target->frameworks, &dup))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "frameworks", target->label,
		    "qstar: duplicate framework '%s' in '%s'", dup, target->label);
	if (list_has_duplicate(&target->defsyms, &dup))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "defsyms", target->label,
		    "qstar: duplicate defsym '%s' in '%s'", dup, target->label);
	if (target->linker_script && *target->linker_script &&
	    !qstar_path_is_package_relative(target->linker_script))
		return qstar_set_error_origin(graph, target->origin_file, target->origin_line,
		    "linker_script", target->label,
		    "qstar: linker_script '%s' in '%s' must be package-relative",
		    target->linker_script, target->label);
	for (i = 0; i < target->defsyms.len; i++) {
		if (!valid_defsym(target->defsyms.items[i]))
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "defsyms", target->label,
			    "qstar: defsym '%s' in '%s' must be NAME=VALUE",
			    target->defsyms.items[i], target->label);
	}
	if (validate_include_dir_list(graph, target, &target->include_dirs,
	    "include_dirs") < 0 ||
	    validate_include_dir_list(graph, target, &target->public_include_dirs,
	    "public_include_dirs") < 0 ||
	    validate_include_dir_list(graph, target, &target->private_include_dirs,
	    "private_include_dirs") < 0 ||
	    validate_include_dir_list(graph, target, &target->interface_include_dirs,
	    "interface_include_dirs") < 0 ||
	    validate_include_dir_list(graph, target, &target->asm_include_dirs,
	    "lang.asm.include_dirs") < 0)
		return -1;
	return 0;
}

/** generated action skeleton 하나의 input/output edge를 검증한다. */
static int
validate_genrule(struct qstar_graph *graph, const struct qstar_genrule *genrule)
{
	const char *path;
	char resolved_tool[QSTAR_PATH_MAX], tool_mode[64], tool_error[QSTAR_PATH_MAX];
	size_t i, j, k;

	if (!genrule->tool || !*genrule->tool)
		return qstar_set_error(graph, "qstar: generated action '%s' has empty tool",
		    genrule->label);
	if (!genrule->config_header &&
	    qstar_profile_resolve_command_tool(graph, genrule->tool, resolved_tool,
	    sizeof(resolved_tool), tool_mode, sizeof(tool_mode), tool_error,
	    sizeof(tool_error)) < 0)
		return qstar_set_error_origin(graph, genrule->origin_file,
		    genrule->origin_line, "command", genrule->label, "%s", tool_error);
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
				if (artifact_location_collides(genrule, i,
				    &graph->genrules[j], k))
					return qstar_set_error_origin(graph,
					    genrule->origin_file, genrule->origin_line,
					    "outputs", genrule->label,
					    "qstar: generated artifact identity group=%s format=%s address=%s layout=%s has multiple outputs",
					    qstar_genrule_output_group(genrule, i),
					    qstar_genrule_output_format(genrule, i),
					    qstar_genrule_output_address(genrule, i),
					    qstar_genrule_output_layout(genrule, i));
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
		if (validate_source_list(graph, &graph->targets[i]) < 0 ||
		    validate_link_lists(graph, &graph->targets[i]) < 0)
			return -1;
	}
	return 0;
}

/** QStar workspace/package ownership과 visibility boundary를 검증한다. */
int
qstar_graph_validate_packages(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (validate_genrule_ownership(graph, &graph->genrules[i]) < 0)
			return -1;
	}
	for (i = 0; i < graph->len; i++) {
		if (validate_target_ownership(graph, &graph->targets[i]) < 0 ||
		    validate_visibility_list(graph, &graph->targets[i]) < 0 ||
		    validate_dependency_boundary(graph, &graph->targets[i],
		    &graph->targets[i].deps, "deps") < 0 ||
		    validate_dependency_boundary(graph, &graph->targets[i],
		    &graph->targets[i].private_deps, "private_deps") < 0)
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
	if (target->linker_script && *target->linker_script &&
	    !file_exists_under_root(graph, target->linker_script))
		return qstar_set_error_origin(graph, target->origin_file,
		    target->origin_line, "linker_script", target->label,
		    "qstar: linker script '%s' in '%s' does not exist under package root",
		    target->linker_script, target->label);
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
	fprintf(out,
	    "  source_file path=%s language=%s tool=%s provider=%s output_group=%s role=compile\n",
	    path, info.language, info.tool_role, info.provider, info.output_group);
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
