#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qstar_plan {
	const struct qstar_graph *graph;
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
			return qstar_set_error(graph, "qstar: unresolved select dependency in '%s'",
			    target->label);
		dep_index = target_index(graph, dep);
		if (dep_index < 0) {
			if (dep[0] == '@') {
				if (external_dep_resolved(graph, dep, &pkg))
					continue;
				return qstar_set_error(graph,
				    "qstar: unresolved package dependency '%s' referenced by '%s'",
				    dep, target->label);
			}
			return qstar_set_error(graph,
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

/** target kind에 대응하는 최종 action 이름을 반환한다. */
static const char *
final_action(const struct qstar_target *target)
{
	if (strcmp(target->kind, "exe") == 0)
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

/** generated action output 중 target source로 소비되는 것이 있는지 확인한다. */
static int
genrule_consumed_by_target(const struct qstar_genrule *genrule,
    const struct qstar_target *target)
{
	size_t i, j;

	for (i = 0; i < genrule->outputs.len; i++) {
		for (j = 0; j < target->sources.len; j++) {
			if (strcmp(genrule->outputs.items[i], target->sources.items[j]) == 0)
				return 1;
		}
	}
	return 0;
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
	}
}

/** target-local toolchain/profile resolver skeleton을 출력한다. */
static void
dump_resolved_toolchain(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	fprintf(out,
	    "  resolved_toolchain owner=%s toolchain=%s profile=%s target=%s "
	    "stdlib=%s resolver=skeleton\n",
	    target->label, target->toolchain,
	    profile_or_default(plan->graph->profile.name, "default"),
	    profile_or_default(plan->graph->profile.target, "host"),
	    target->stdlib_policy);
}

/** target 하나의 non-executing command-plan record를 출력한다. */
static void
dump_target_plan(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target,
    size_t order)
{
	char output[QSTAR_PATH_MAX];
	struct qstar_source_info source;
	const char *action;
	const char *final_tool;
	size_t i;

	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  order %zu\n", order);
	fprintf(out, "  kind %s\n", target->kind);
	fputs("  deps ", out);
	dump_list(out, &target->deps);
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
	fputs("  system_include_dirs ", out);
	dump_list(out, &target->system_include_dirs);
	fputc('\n', out);
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
	dump_resolved_toolchain(out, plan, target);
	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		snprintf(output, sizeof(output), "<object:%s:%zu>", target->label, i);
		fprintf(out, "  action compile source=%s output=<object:%s:%zu>\n",
		    target->sources.items[i], target->label, i);
		dump_action_key(out, plan->graph, target, "compile", target->sources.items[i],
		    output, source.language, i);
		dump_command_skeleton(out, plan->graph, target, "compile",
		    target->sources.items[i], output, source.language, source.tool_role, i);
	}
	action = final_action(target);
	final_tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	snprintf(output, sizeof(output), "<artifact:%s>", target->label);
	fprintf(out, "  action %s output=<artifact:%s>\n", action, target->label);
	dump_action_key(out, plan->graph, target, action, "<target-objects>", output,
	    "artifact", 0);
	dump_command_skeleton(out, plan->graph, target, action, "<target-objects>",
	    output, "artifact", final_tool, 0);
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
	for (i = 0; i < plan.len; i++)
		dump_target_plan(out, &plan, plan.order[i], i);
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
	}
}

/** target의 compile dry-run step을 source 순서대로 출력한다. */
static void
dump_dry_run_compiles(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	struct qstar_source_info source;
	char output[QSTAR_PATH_MAX];
	size_t i;

	(void)plan;
	for (i = 0; i < target->sources.len; i++) {
		qstar_source_classify(target->sources.items[i], &source);
		snprintf(output, sizeof(output), "<object:%s:%zu>", target->label, i);
		fprintf(out,
		    "dry_run_step id=%s:compile:%zu owner=%s kind=compile language=%s "
		    "tool=%s toolchain=%s input=%s output=%s execute=no\n",
		    target->label, i, target->label, source.language, source.tool_role,
		    target->toolchain, target->sources.items[i], output);
	}
}

/** target의 최종 artifact dry-run step을 출력한다. */
static void
dump_dry_run_final(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target)
{
	char output[QSTAR_PATH_MAX];
	const char *action, *tool;

	(void)plan;
	action = final_action(target);
	tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	snprintf(output, sizeof(output), "<artifact:%s>", target->label);
	fprintf(out,
	    "dry_run_step id=%s:%s:0 owner=%s kind=%s tool=%s toolchain=%s "
	    "input=<target-objects> output=%s execute=no\n",
	    target->label, action, target->label, action, tool, target->toolchain, output);
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
		dump_resolved_toolchain(out, &plan, plan.order[i]);
		dump_dry_run_genrules(out, &plan, plan.order[i]);
		dump_dry_run_compiles(out, &plan, plan.order[i]);
		dump_dry_run_final(out, &plan, plan.order[i]);
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
