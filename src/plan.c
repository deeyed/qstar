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
			if (dep[0] == '@')
				return qstar_set_error(graph,
				    "qstar: unresolved package dependency '%s' referenced by '%s'",
				    dep, target->label);
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

/** target 하나의 non-executing command-plan record를 출력한다. */
static void
dump_target_plan(FILE *out, const struct qstar_target *target, size_t order)
{
	size_t i;

	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  order %zu\n", order);
	fprintf(out, "  kind %s\n", target->kind);
	fputs("  deps ", out);
	dump_list(out, &target->deps);
	fputc('\n', out);
	fputs("  sources ", out);
	dump_list(out, &target->sources);
	fputc('\n', out);
	fputs("  public_headers ", out);
	dump_list(out, &target->public_headers);
	fputc('\n', out);
	fputs("  include_dirs ", out);
	dump_list(out, &target->include_dirs);
	fputc('\n', out);
	fputs("  system_include_dirs ", out);
	dump_list(out, &target->system_include_dirs);
	fputc('\n', out);
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
	for (i = 0; i < target->sources.len; i++)
		fprintf(out, "  action compile source=%s output=<object:%s:%zu>\n",
		    target->sources.items[i], target->label, i);
	fprintf(out, "  action %s output=<artifact:%s>\n", final_action(target), target->label);
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
	fprintf(out, "root %s\n", label && *label ? label : "<all>");
	dump_closure_order(out, &plan);
	for (i = 0; i < plan.len; i++)
		dump_target_plan(out, plan.order[i], i);
	free_plan(&plan);
	return 0;
}
