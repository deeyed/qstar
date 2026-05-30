#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct qstar_plan {
	struct qstar_graph *graph;
	const struct qstar_target **order;
	unsigned char *state;
	size_t len;
	size_t cap;
};

/** profile 문자열이 없을 때 explain dump에 쓸 기본값을 반환한다. */
static const char *
profile_or_default(const char *s, const char *fallback)
{
	return s && *s ? s : fallback;
}

/** 문자열 list를 command-plan dump 형식으로 출력한다. */
static void
dump_list(FILE *out, const struct qstar_string_list *list)
{
	size_t i;

	fputc('[', out);
	for (i = 0; i < list->len; i++) {
		if (i)
			fputs(", ", out);
		fputs(list->items[i], out);
	}
	fputc(']', out);
}

/** package alias map을 action key material 형식으로 출력한다. */
static void
dump_package_aliases(FILE *out, const struct qstar_graph *graph)
{
	size_t i;

	fputc('[', out);
	for (i = 0; i < graph->package_len; i++) {
		if (i)
			fputs(", ", out);
		fprintf(out, "%s=%s", graph->packages[i].alias, graph->packages[i].root);
	}
	fputc(']', out);
}

/** string list를 action key field 하나로 formatting한다. */
static void
format_list_field(char *dst, size_t dstlen, const struct qstar_string_list *list)
{
	size_t i, used, n;

	if (dstlen == 0)
		return;
	used = 0;
	dst[used++] = '[';
	dst[used] = '\0';
	for (i = 0; i < list->len && used < dstlen; i++) {
		n = snprintf(dst + used, dstlen - used, "%s%s", i ? "," : "", list->items[i]);
		if (n >= dstlen - used) {
			dst[dstlen - 1] = '\0';
			return;
		}
		used += n;
	}
	if (used + 2 <= dstlen) {
		dst[used++] = ']';
		dst[used] = '\0';
	}
}

/** canonical label에 대응하는 target index를 찾는다. */
static int
target_index(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return (int)i;
	}
	return -1;
}

/** external dependency가 alias map에서 resolve되는지 확인한다. */
static int
external_dep_resolved(const struct qstar_graph *graph, const char *dep,
    const struct qstar_package_alias **pkg_out)
{
	char alias[QSTAR_PATH_MAX];

	if (qstar_label_package_alias(dep, alias, sizeof(alias)) < 0)
		return 0;
	*pkg_out = qstar_graph_find_package_alias(graph, alias);
	return *pkg_out != NULL;
}

/** closure order list에 target을 추가한다. */
static int
plan_push(struct qstar_graph *graph, struct qstar_plan *plan, const struct qstar_target *target)
{
	const struct qstar_target **order;
	size_t cap;

	if (plan->len == plan->cap) {
		cap = plan->cap ? plan->cap * 2 : 8;
		order = realloc(plan->order, cap * sizeof(plan->order[0]));
		if (!order)
			return qstar_set_error(graph, "qstar: out of memory");
		plan->order = order;
		plan->cap = cap;
	}
	plan->order[plan->len++] = target;
	return 0;
}

/** target dependency를 깊이 우선으로 방문해 dependency-first order를 만든다. */
static int
visit_target(struct qstar_graph *graph, struct qstar_plan *plan, size_t index)
{
	const struct qstar_target *target;
	const struct qstar_package_alias *pkg;
	const char *dep;
	size_t i;
	int dep_index;

	if (plan->state[index] == 2)
		return 0;
	if (plan->state[index] == 1)
		return qstar_set_error(graph, "qstar: dependency cycle includes '%s'",
		    graph->targets[index].label);
	plan->state[index] = 1;
	target = &graph->targets[index];
	for (i = 0; i < target->deps.len; i++) {
		dep = target->deps.items[i];
		if (strcmp(dep, "<select>") == 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "deps", target->label,
			    "qstar: unresolved select dependency in '%s'",
			    target->label);
		dep_index = target_index(graph, dep);
		if (dep_index < 0) {
			if (dep[0] == '@') {
				if (external_dep_resolved(graph, dep, &pkg))
					continue;
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "deps", target->label,
				    "qstar: unresolved package dependency '%s' referenced by '%s'",
				    dep, target->label);
			}
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "deps", target->label,
			    "qstar: unknown dependency label '%s' referenced by '%s'",
			    dep, target->label);
		}
		if (visit_target(graph, plan, (size_t)dep_index) < 0)
			return -1;
	}
	for (i = 0; i < target->private_deps.len; i++) {
		dep = target->private_deps.items[i];
		if (strcmp(dep, "<select>") == 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "private_deps", target->label,
			    "qstar: unresolved select dependency in '%s'",
			    target->label);
		dep_index = target_index(graph, dep);
		if (dep_index < 0) {
			if (dep[0] == '@') {
				if (external_dep_resolved(graph, dep, &pkg))
					continue;
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "private_deps", target->label,
				    "qstar: unresolved package dependency '%s' referenced by '%s'",
				    dep, target->label);
			}
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "private_deps", target->label,
			    "qstar: unknown dependency label '%s' referenced by '%s'",
			    dep, target->label);
		}
		if (visit_target(graph, plan, (size_t)dep_index) < 0)
			return -1;
	}
	plan->state[index] = 2;
	return plan_push(graph, plan, target);
}

/** 전체 graph explain을 위해 target index를 canonical label 순서로 정렬한다. */
static void
sort_indices(const struct qstar_graph *graph, size_t *indices, size_t n)
{
	size_t i, j, v;

	for (i = 1; i < n; i++) {
		v = indices[i];
		j = i;
		while (j > 0 && strcmp(graph->targets[indices[j - 1]].label,
		    graph->targets[v].label) > 0) {
			indices[j] = indices[j - 1];
			j--;
		}
		indices[j] = v;
	}
}

/** 선택 label 또는 전체 graph의 closure를 계산한다. */
static int
build_closure(struct qstar_graph *graph, const char *label, struct qstar_plan *plan)
{
	size_t *indices;
	size_t i;
	int index;

	memset(plan, 0, sizeof(*plan));
	plan->graph = graph;
	plan->state = calloc(graph->len ? graph->len : 1, sizeof(plan->state[0]));
	if (!plan->state)
		return qstar_set_error(graph, "qstar: out of memory");
	if (label && *label) {
		index = target_index(graph, label);
		if (index < 0)
			return qstar_set_error(graph, "qstar: unknown target label '%s'", label);
		return visit_target(graph, plan, (size_t)index);
	}
	indices = malloc((graph->len ? graph->len : 1) * sizeof(indices[0]));
	if (!indices)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 0; i < graph->len; i++)
		indices[i] = i;
	sort_indices(graph, indices, graph->len);
	for (i = 0; i < graph->len; i++) {
		if (visit_target(graph, plan, indices[i]) < 0) {
			free(indices);
			return -1;
		}
	}
	free(indices);
	return 0;
}

/** closure 계산 중 할당한 임시 plan 저장소를 해제한다. */
static void
free_plan(struct qstar_plan *plan)
{
	free(plan->order);
	free(plan->state);
	memset(plan, 0, sizeof(*plan));
}

/** dependency-first closure를 계산해 각 target callback을 순서대로 호출한다. */
int
qstar_graph_visit_closure(struct qstar_graph *graph, const char *label,
    qstar_target_visit_fn visit, void *user)
{
	struct qstar_plan plan;
	size_t i;
	int rc;

	rc = build_closure(graph, label, &plan);
	if (rc < 0) {
		free_plan(&plan);
		return -1;
	}
	for (i = 0; i < plan.len; i++) {
		rc = visit(graph, plan.order[i], i, user);
		if (rc < 0) {
			free_plan(&plan);
			return -1;
		}
	}
	free_plan(&plan);
	return 0;
}

/** target kind에 대응하는 최종 action 이름을 반환한다. */
static const char *
final_action(const struct qstar_target *target)
{
	if (strcmp(target->kind, "exe") == 0)
		return "link";
	if (strcmp(target->kind, "test") == 0)
		return "link";
	if (strcmp(target->kind, "staticlib") == 0)
		return "archive";
	if (strcmp(target->kind, "sharedlib") == 0)
		return "link-shared";
	if (strcmp(target->kind, "objectlib") == 0)
		return "compile-objects";
	return "materialize";
}

/** closure order를 한 줄의 deterministic list로 출력한다. */
static void
dump_closure_order(FILE *out, const struct qstar_plan *plan)
{
	size_t i;

	fputs("closure-order ", out);
	fputc('[', out);
	for (i = 0; i < plan->len; i++) {
		if (i)
			fputs(", ", out);
		fputs(plan->order[i]->label, out);
	}
	fputs("]\n", out);
}

/** profile input과 package alias map을 command-plan header에 출력한다. */
static void
dump_plan_inputs(FILE *out, const struct qstar_graph *graph)
{
	fprintf(out, "profile name=%s target=%s toolchain=%s stdlib=%s\n",
	    profile_or_default(graph->profile.name, "default"),
	    profile_or_default(graph->profile.target, "host"),
	    profile_or_default(graph->profile.toolchain, "default"),
	    profile_or_default(graph->profile.stdlib_policy, "default"));
	fputs("package_aliases ", out);
	dump_package_aliases(out, graph);
	fputc('\n', out);
}

/** target dependency list 중 resolved external package dependency를 출력한다. */
static void
dump_external_deps(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target)
{
	const struct qstar_package_alias *pkg;
	size_t i;

	for (i = 0; i < target->deps.len; i++) {
		if (target->deps.items[i][0] != '@')
			continue;
		if (external_dep_resolved(plan->graph, target->deps.items[i], &pkg))
			fprintf(out, "  external_dep %s alias=%s root=%s\n",
			    target->deps.items[i], pkg->alias, pkg->root);
	}
	for (i = 0; i < target->private_deps.len; i++) {
		if (target->private_deps.items[i][0] != '@')
			continue;
		if (external_dep_resolved(plan->graph, target->private_deps.items[i], &pkg))
			fprintf(out, "  external_private_dep %s alias=%s root=%s\n",
			    target->private_deps.items[i], pkg->alias, pkg->root);
	}
}

/** action key skeleton을 deterministic material line으로 출력한다. */
static void
dump_action_key(FILE *out, const struct qstar_graph *graph, const struct qstar_target *target,
    const char *kind, const char *input, const char *output, const char *language, size_t index)
{
	fprintf(out,
	    "  action_key id=%s:%s:%zu kind=%s owner=%s input=%s output=%s "
	    "language=%s profile=%s target=%s toolchain=%s stdlib=%s deps=",
	    target->label, kind, index, kind, target->label, input, output,
	    language,
	    profile_or_default(graph->profile.name, "default"),
	    profile_or_default(graph->profile.target, "host"),
	    target->toolchain, target->stdlib_policy);
	dump_list(out, &target->deps);
	fputs(" packages=", out);
	dump_package_aliases(out, graph);
	fputc('\n', out);
}

/** non-executing command skeleton을 deterministic material line으로 출력한다. */
static void
dump_command_skeleton(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const char *kind, const char *input,
    const char *output, const char *language, const char *tool, size_t index)
{
	fprintf(out,
	    "  command_skeleton id=%s:%s:%zu phase=%s language=%s tool=%s "
	    "toolchain=%s target=%s stdlib=%s input=%s output=%s execute=no\n",
	    target->label, kind, index, kind, language, tool, target->toolchain,
	    profile_or_default(graph->profile.target, "host"), target->stdlib_policy,
	    input, output);
}

/** 실제 argv plan의 list header를 출력한다. */
static void
begin_argv(FILE *out, const char *id, size_t argc)
{
	fprintf(out, "  command_argv id=%s argc=%zu argv=[", id, argc);
}

/** argv item 하나를 deterministic dump에 추가한다. */
static void
argv_item(FILE *out, size_t *seen, const char *value)
{
	if (*seen)
		fputs(", ", out);
	fputs(value, out);
	(*seen)++;
}

/** command_argv line을 닫는다. */
static void
end_argv(FILE *out)
{
	fputs("]\n", out);
}

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

static const struct qstar_target *
plan_find_target(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return &graph->targets[i];
	}
	return NULL;
}

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
		dep = plan_find_target(graph, target->deps.items[i]);
		if (!dep)
			continue;
		if (collect_public_includes_rec(graph, dep, out, 1) < 0)
			return -1;
	}
	return 0;
}

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

/** compile action의 실제 argv plan을 출력한다. */
static void
dump_compile_argv(FILE *out, const struct qstar_target *target,
    const struct qstar_graph *graph,
    const struct qstar_resolved_toolchain *toolchain, const struct qstar_source_info *source,
    const char *input, const char *output, size_t index)
{
	char id[QSTAR_PATH_MAX], target_arg[QSTAR_PATH_MAX];
	const char *tool;
	struct qstar_string_list includes;
	size_t argc, seen, i;
	int cross;

	memset(&includes, 0, sizeof(includes));
	collect_compile_include_dirs(graph, target, &includes);
	snprintf(id, sizeof(id), "%s:compile:%zu", target->label, index);
	tool = strcmp(source->language, "cale") == 0 ? toolchain->cale : toolchain->cc;
	cross = (strcmp(toolchain->name, "clang") == 0 ||
	    strcmp(toolchain->name, "cale") == 0 ||
	    strcmp(toolchain->name, "cale-sol") == 0) &&
	    strcmp(toolchain->target, "host") != 0;
	argc = 5 + includes.len * 2 + target->system_include_dirs.len * 2 +
	    (cross ? 1 : 0);
	snprintf(target_arg, sizeof(target_arg), "--target=%s", toolchain->target);
	begin_argv(out, id, argc);
	seen = 0;
	argv_item(out, &seen, tool);
	if (cross)
		argv_item(out, &seen, target_arg);
	argv_item(out, &seen, "-c");
	argv_item(out, &seen, input);
	argv_item(out, &seen, "-o");
	argv_item(out, &seen, output);
	for (i = 0; i < includes.len; i++) {
		argv_item(out, &seen, "-I");
		argv_item(out, &seen, includes.items[i]);
	}
	for (i = 0; i < target->system_include_dirs.len; i++) {
		argv_item(out, &seen, "-isystem");
		argv_item(out, &seen, target->system_include_dirs.items[i]);
	}
	end_argv(out);
	qstar_string_list_free(&includes);
}

/** generated action의 실제 argv plan을 출력한다. */
static void
dump_genrule_argv(FILE *out, const struct qstar_genrule *genrule)
{
	size_t i, seen;
	char id[QSTAR_PATH_MAX];

	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	begin_argv(out, id, 1 + genrule->args.len);
	seen = 0;
	argv_item(out, &seen, genrule->tool);
	for (i = 0; i < genrule->args.len; i++)
		argv_item(out, &seen, genrule->args.items[i]);
	end_argv(out);
}

/** target final action의 실제 argv plan을 출력한다. */
static void
dump_final_argv(FILE *out, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *action,
    const char *output)
{
	char id[QSTAR_PATH_MAX];
	char buf[QSTAR_PATH_MAX];
	size_t seen, argc, i;
	int windows;

	snprintf(id, sizeof(id), "%s:%s:0", target->label, action);
	windows = strstr(toolchain->target, "windows") || strstr(toolchain->target, "msvc") ||
	    strstr(toolchain->target, "mingw");
	argc = strcmp(action, "archive") == 0 ? 4 :
	    strcmp(action, "link-shared") == 0 ? 5 : 4;
	if (strcmp(action, "archive") != 0)
		argc += target->lib_dirs.len + target->libs.len +
		    (windows ? 0 : target->frameworks.len * 2);
	begin_argv(out, id, argc);
	seen = 0;
	if (strcmp(action, "archive") == 0) {
		argv_item(out, &seen, toolchain->ar);
		argv_item(out, &seen, "rcs");
		argv_item(out, &seen, output);
		argv_item(out, &seen, "<target-objects>");
	} else {
		argv_item(out, &seen, toolchain->linker);
		if (strcmp(action, "link-shared") == 0)
			argv_item(out, &seen, "-shared");
		argv_item(out, &seen, "-o");
		argv_item(out, &seen, output);
		argv_item(out, &seen, "<target-objects>");
		for (i = 0; i < target->lib_dirs.len; i++) {
			if (windows) {
				snprintf(buf, sizeof(buf), "/LIBPATH:%s", target->lib_dirs.items[i]);
				argv_item(out, &seen, buf);
			} else {
				snprintf(buf, sizeof(buf), "-L%s", target->lib_dirs.items[i]);
				argv_item(out, &seen, buf);
			}
		}
		for (i = 0; i < target->libs.len; i++) {
			if (windows) {
				snprintf(buf, sizeof(buf), "%s.lib", target->libs.items[i]);
				argv_item(out, &seen, buf);
			} else {
				snprintf(buf, sizeof(buf), "-l%s", target->libs.items[i]);
				argv_item(out, &seen, buf);
			}
		}
		if (!windows) {
			for (i = 0; i < target->frameworks.len; i++) {
				argv_item(out, &seen, "-framework");
				argv_item(out, &seen, target->frameworks.items[i]);
			}
		}
	}
	end_argv(out);
}

/** generated output list가 target의 파일 입력 list에 소비되는지 확인한다. */
static int
genrule_output_in_list(const struct qstar_genrule *genrule,
    const struct qstar_string_list *list)
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

/** generated action output 중 target file input으로 소비되는 것이 있는지 확인한다. */
static int
genrule_consumed_by_target(const struct qstar_genrule *genrule,
    const struct qstar_target *target)
{
	return genrule_output_in_list(genrule, &target->sources) ||
	    genrule_output_in_list(genrule, &target->public_headers) ||
	    genrule_output_in_list(genrule, &target->private_headers);
}

/** target이 소비하는 generated output edge를 deterministic하게 출력한다. */
static void
dump_generated_edges(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_genrule *owner;
	size_t i;

	for (i = 0; i < target->sources.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->sources.items[i]);
		if (owner)
			fprintf(out, "  generated_edge source=%s generator=%s output=%s\n",
			    target->sources.items[i], owner->label, target->sources.items[i]);
	}
	for (i = 0; i < target->public_headers.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->public_headers.items[i]);
		if (owner)
			fprintf(out, "  generated_edge header=%s generator=%s output=%s\n",
			    target->public_headers.items[i], owner->label,
			    target->public_headers.items[i]);
	}
	for (i = 0; i < target->private_headers.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->private_headers.items[i]);
		if (owner)
			fprintf(out, "  generated_edge header=%s generator=%s output=%s\n",
			    target->private_headers.items[i], owner->label,
			    target->private_headers.items[i]);
	}
}

/** target plan 안에서 소비되는 generated action skeleton을 출력한다. */
static void
dump_consumed_genrules(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	char inputs[QSTAR_PATH_MAX], outputs[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < plan->graph->genrule_len; i++) {
		genrule = &plan->graph->genrules[i];
		if (!genrule_consumed_by_target(genrule, target))
			continue;
		format_list_field(inputs, sizeof(inputs), &genrule->inputs);
		format_list_field(outputs, sizeof(outputs), &genrule->outputs);
		fprintf(out, "  generated_action id=%s tool=%s inputs=%s outputs=%s args=",
		    genrule->label, genrule->tool, inputs, outputs);
		dump_list(out, &genrule->args);
		fputs(" execute=no\n", out);
		fprintf(out,
		    "  action_key id=%s:generate:0 kind=generate owner=%s consumer=%s "
		    "input=%s output=%s language=generated profile=%s target=%s "
		    "toolchain=%s stdlib=%s deps=[] packages=",
		    genrule->label, genrule->label, target->label, inputs, outputs,
		    profile_or_default(plan->graph->profile.name, "default"),
		    profile_or_default(plan->graph->profile.target, "host"),
		    target->toolchain, target->stdlib_policy);
		dump_package_aliases(out, plan->graph);
		fputc('\n', out);
		fprintf(out,
		    "  command_skeleton id=%s:generate:0 phase=generate language=generated "
		    "tool=%s toolchain=%s target=%s stdlib=%s input=%s output=%s "
		    "consumer=%s execute=no\n",
		    genrule->label, genrule->tool, target->toolchain,
		    profile_or_default(plan->graph->profile.target, "host"),
		    target->stdlib_policy, inputs, outputs, target->label);
		dump_genrule_argv(out, genrule);
	}
}

/** target-local toolchain/profile resolver skeleton을 출력한다. */
static int
dump_resolved_toolchain(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target, struct qstar_resolved_toolchain *resolved)
{
	if (qstar_resolve_toolchain(plan->graph, target, resolved) < 0)
		return -1;
	fprintf(out,
	    "  resolved_toolchain owner=%s toolchain=%s profile=%s target=%s "
	    "stdlib=%s resolver=%s cc=%s cale=%s ar=%s linker=%s\n",
	    target->label, resolved->name,
	    profile_or_default(plan->graph->profile.name, "default"),
	    resolved->target, resolved->stdlib_policy, resolved->resolver,
	    resolved->cc, resolved->cale, resolved->ar, resolved->linker);
	return 0;
}

/** target 하나의 non-executing command-plan record를 출력한다. */
static int
dump_target_plan(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target,
    size_t order)
{
	char output[QSTAR_PATH_MAX];
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	const char *action;
	const char *final_tool;
	size_t i;

	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  order %zu\n", order);
	fprintf(out, "  kind %s\n", target->kind);
	fputs("  deps ", out);
	dump_list(out, &target->deps);
	fputc('\n', out);
	fputs("  private_deps ", out);
	dump_list(out, &target->private_deps);
	fputc('\n', out);
	dump_external_deps(out, plan, target);
	dump_generated_edges(out, plan, target);
	dump_consumed_genrules(out, plan, target);
	qstar_target_dump_source_discovery(target, out);
	fputs("  sources ", out);
	dump_list(out, &target->sources);
	fputc('\n', out);
	fputs("  public_headers ", out);
	dump_list(out, &target->public_headers);
	fputc('\n', out);
	qstar_target_dump_header_files(target, out);
	fputs("  include_dirs ", out);
	dump_list(out, &target->include_dirs);
	fputc('\n', out);
	fputs("  public_include_dirs ", out);
	dump_list(out, &target->public_include_dirs);
	fputc('\n', out);
	fputs("  interface_include_dirs ", out);
	dump_list(out, &target->interface_include_dirs);
	fputc('\n', out);
	fputs("  system_include_dirs ", out);
	dump_list(out, &target->system_include_dirs);
	fputc('\n', out);
	fputs("  libs ", out);
	dump_list(out, &target->libs);
	fputc('\n', out);
	fputs("  lib_dirs ", out);
	dump_list(out, &target->lib_dirs);
	fputc('\n', out);
	fputs("  frameworks ", out);
	dump_list(out, &target->frameworks);
	fputc('\n', out);
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
	if (dump_resolved_toolchain(out, plan, target, &toolchain) < 0)
		return -1;
	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		if (qstar_object_output_path(target, i, output, sizeof(output)) < 0)
			return qstar_set_error(plan->graph, "qstar: object output path too long");
		fprintf(out, "  action compile source=%s output=%s\n",
		    target->sources.items[i], output);
		dump_action_key(out, plan->graph, target, "compile", target->sources.items[i],
		    output, source.language, i);
		dump_command_skeleton(out, plan->graph, target, "compile",
		    target->sources.items[i], output, source.language, source.tool_role, i);
		dump_compile_argv(out, target, plan->graph, &toolchain, &source,
		    target->sources.items[i], output, i);
	}
	action = final_action(target);
	final_tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	if (qstar_artifact_output_path(target, output, sizeof(output)) < 0)
		return qstar_set_error(plan->graph, "qstar: artifact output path too long");
	fprintf(out, "  action %s output=%s\n", action, output);
	dump_action_key(out, plan->graph, target, action, "<target-objects>", output,
	    "artifact", 0);
	dump_command_skeleton(out, plan->graph, target, action, "<target-objects>",
	    output, "artifact", final_tool, 0);
	dump_final_argv(out, target, &toolchain, action, output);
	return 0;
}

/** QStar target closure와 non-executing command plan을 deterministic text로 출력한다. */
int
qstar_graph_explain_plan(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_plan plan;
	size_t i;
	int rc;

	rc = build_closure(graph, label, &plan);
	if (rc < 0) {
		free_plan(&plan);
		return -1;
	}
	fputs("qstar command plan v1\n", out);
	fputs("build-plan-ir v1\n", out);
	fprintf(out, "root %s\n", label && *label ? label : "<all>");
	dump_plan_inputs(out, graph);
	dump_closure_order(out, &plan);
	for (i = 0; i < plan.len; i++) {
		if (dump_target_plan(out, &plan, plan.order[i], i) < 0) {
			free_plan(&plan);
			return -1;
		}
	}
	free_plan(&plan);
	return 0;
}

/** target이 소비하는 generated action dry-run step을 출력한다. */
static void
dump_dry_run_genrules(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	char inputs[QSTAR_PATH_MAX], outputs[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < plan->graph->genrule_len; i++) {
		genrule = &plan->graph->genrules[i];
		if (!genrule_consumed_by_target(genrule, target))
			continue;
		format_list_field(inputs, sizeof(inputs), &genrule->inputs);
		format_list_field(outputs, sizeof(outputs), &genrule->outputs);
		fprintf(out,
		    "dry_run_step id=%s:generate:0 owner=%s consumer=%s kind=generate "
		    "tool=%s inputs=%s outputs=%s args=",
		    genrule->label, genrule->label, target->label, genrule->tool, inputs,
		    outputs);
		dump_list(out, &genrule->args);
		fputs(" execute=no\n", out);
		dump_genrule_argv(out, genrule);
	}
}

/** target의 compile dry-run step을 source 순서대로 출력한다. */
static int
dump_dry_run_compiles(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	char output[QSTAR_PATH_MAX];
	size_t i;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		if (qstar_object_output_path(target, i, output, sizeof(output)) < 0)
			return qstar_set_error(plan->graph, "qstar: object output path too long");
		fprintf(out,
		    "dry_run_step id=%s:compile:%zu owner=%s kind=compile language=%s "
		    "tool=%s toolchain=%s input=%s output=%s execute=no\n",
		    target->label, i, target->label, source.language, source.tool_role,
		    toolchain.name, target->sources.items[i], output);
		dump_compile_argv(out, target, plan->graph, &toolchain, &source,
		    target->sources.items[i], output, i);
	}
	return 0;
}

/** target의 최종 artifact dry-run step을 출력한다. */
static int
dump_dry_run_final(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target)
{
	char output[QSTAR_PATH_MAX];
	struct qstar_resolved_toolchain toolchain;
	const char *action, *tool;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	action = final_action(target);
	tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	if (qstar_artifact_output_path(target, output, sizeof(output)) < 0)
		return qstar_set_error(plan->graph, "qstar: artifact output path too long");
	fprintf(out,
	    "dry_run_step id=%s:%s:0 owner=%s kind=%s tool=%s toolchain=%s "
	    "input=<target-objects> output=%s execute=no\n",
	    target->label, action, target->label, action, tool, toolchain.name, output);
	dump_final_argv(out, target, &toolchain, action, output);
	return 0;
}

/** QStar target closure를 실제 실행 없이 dry-run command stream으로 출력한다. */
int
qstar_graph_dry_run(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_plan plan;
	size_t i;
	int rc;

	rc = build_closure(graph, label, &plan);
	if (rc < 0) {
		free_plan(&plan);
		return -1;
	}
	fputs("qstar dry-run v1\n", out);
	fprintf(out, "root %s\n", label && *label ? label : "<all>");
	dump_plan_inputs(out, graph);
	dump_closure_order(out, &plan);
	for (i = 0; i < plan.len; i++) {
		fprintf(out, "dry_run_target %s order=%zu kind=%s\n",
		    plan.order[i]->label, i, plan.order[i]->kind);
		{
		struct qstar_resolved_toolchain toolchain;
		if (dump_resolved_toolchain(out, &plan, plan.order[i], &toolchain) < 0) {
			free_plan(&plan);
			return -1;
		}
		}
		dump_dry_run_genrules(out, &plan, plan.order[i]);
		if (dump_dry_run_compiles(out, &plan, plan.order[i]) < 0 ||
		    dump_dry_run_final(out, &plan, plan.order[i]) < 0) {
			free_plan(&plan);
			return -1;
		}
	}
	free_plan(&plan);
	return 0;
}

/** QStar authoring check 결과를 deterministic text로 출력한다. */
int
qstar_graph_check(struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_plan plan;
	int rc;

	rc = build_closure(graph, label, &plan);
	if (rc < 0) {
		free_plan(&plan);
		return -1;
	}
	fputs("qstar check v1\n", out);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	fprintf(out, "root %s\n", label && *label ? label : "<all>");
	dump_plan_inputs(out, graph);
	dump_closure_order(out, &plan);
	fprintf(out, "target-count %zu\n", plan.len);
	fprintf(out, "generated-action-count %zu\n", graph->genrule_len);
	fputs("file-inputs ok\n", out);
	fputs("status ok\n", out);
	free_plan(&plan);
	return 0;
}

/** QStar 전체 package doctor 결과를 deterministic text로 출력한다. */
int
qstar_graph_doctor(struct qstar_graph *graph, FILE *out)
{
	struct qstar_plan plan;
	struct qstar_resolved_toolchain toolchain;
	char state_dir[QSTAR_PATH_MAX];
	int rc;

	rc = build_closure(graph, NULL, &plan);
	if (rc < 0) {
		free_plan(&plan);
		return -1;
	}
	fputs("qstar doctor v1\n", out);
	fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
	dump_plan_inputs(out, graph);
	dump_closure_order(out, &plan);
	fprintf(out, "target-count %zu\n", graph->len);
	fprintf(out, "closure-target-count %zu\n", plan.len);
	fprintf(out, "generated-action-count %zu\n", graph->genrule_len);
	if (plan.len > 0 && qstar_resolve_toolchain(graph, plan.order[0], &toolchain) == 0)
		fprintf(out, "toolchain-sanity name=%s cc=%s linker=%s status=resolved\n",
		    toolchain.name, toolchain.cc, toolchain.linker);
	if (qstar_path_join(graph->package_root ? graph->package_root : ".", ".qstar",
	    state_dir, sizeof(state_dir)) == 0) {
		if (mkdir(state_dir, 0777) == 0 || access(state_dir, W_OK) == 0)
			fputs("writable-state-dir yes\n", out);
		else
			fputs("writable-state-dir no\n", out);
	}
	fprintf(out, "profile-file-input name=%s target=%s toolchain=%s stdlib=%s\n",
	    graph->profile.name ? graph->profile.name : "default",
	    graph->profile.target ? graph->profile.target : "host",
	    graph->profile.toolchain ? graph->profile.toolchain : "host",
	    graph->profile.stdlib_policy ? graph->profile.stdlib_policy : "system");
	fputs("diagnostics ok\n", out);
	fputs("file-inputs ok\n", out);
	fputs("status ok\n", out);
	free_plan(&plan);
	return 0;
}
