#include "internal.h"

#include <ctype.h>
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

struct qstar_argv_dump {
	struct qstar_graph *graph;
	const struct qstar_resolved_toolchain *toolchain;
	struct qstar_argv logical;
	const char *id;
	size_t seen;
	int oom;
};

struct doctor_tool_requirements {
	int cc;
	int cxx;
	int ar;
	int linker;
	struct qstar_string_list provider_roles;
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

/** provider env assignment은 plan에서 값 없이 변수 이름만 노출한다. */
static void
dump_provider_env_names(FILE *out, const struct qstar_string_list *env)
{
	const char *eq;
	size_t i;

	fputc('[', out);
	for (i = 0; env && i < env->len; i++) {
		if (i)
			fputs(", ", out);
		eq = strchr(env->items[i], '=');
		if (eq)
			fwrite(env->items[i], 1, (size_t)(eq - env->items[i]), out);
		else
			fputs(env->items[i], out);
	}
	fputc(']', out);
}

static void
dump_provider_action_contract(FILE *out, const char *id, const char *phase,
    const char *api, const char *provider,
    const struct qstar_provider_action_template *action)
{
	fprintf(out,
	    "  provider_action_contract id=%s phase=%s api=%s provider=%s inputs=%zu outputs=%zu env_names=",
	    id, phase, api && *api ? api : "<unknown>",
	    provider && *provider ? provider : "<unknown>",
	    action->inputs.len, action->outputs.len);
	dump_provider_env_names(out, &action->env);
	fprintf(out, " depfile=%s wants_depfile=%s\n",
	    action->depfile && *action->depfile ? action->depfile : "<none>",
	    action->wants_depfile ? "true" : "false");
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

/** generated output artifact metadata line을 deterministic하게 출력한다. */
static void
dump_genrule_artifacts(FILE *out, const struct qstar_genrule *genrule, const char *prefix)
{
	char identity[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < genrule->outputs.len; i++) {
			if (qstar_genrule_output_identity(genrule, i, identity,
			    sizeof(identity)) < 0)
				snprintf(identity, sizeof(identity), "<too-long>");
			fprintf(out,
			    "%sgenerated_artifact output=%s group=%s format=%s identity=%s\n",
			    prefix ? prefix : "", genrule->outputs.items[i],
			    qstar_genrule_output_group(genrule, i),
			    qstar_genrule_output_format(genrule, i), identity);
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
#define VISIT_TARGET_LABEL_LIST(list_field, field_name, allow_external) \
	do { \
		for (i = 0; i < target->list_field.len; i++) { \
			dep = target->list_field.items[i]; \
			dep_index = target_index(graph, dep); \
			if (dep_index < 0) { \
				if ((allow_external) && dep[0] == '@') { \
					if (external_dep_resolved(graph, dep, &pkg)) \
						continue; \
					return qstar_set_error_origin(graph, target->origin_file, \
					    target->origin_line, field_name, target->label, \
					    "qstar: unresolved package dependency '%s' referenced by '%s'", \
					    dep, target->label); \
				} \
				if (qstar_graph_find_genrule(graph, dep)) \
					continue; \
				return qstar_set_error_origin(graph, target->origin_file, \
				    target->origin_line, field_name, target->label, \
				    "qstar: unknown dependency label '%s' referenced by '%s'", \
				    dep, target->label); \
			} \
			if (visit_target(graph, plan, (size_t)dep_index) < 0) \
				return -1; \
		} \
	} while (0)
	VISIT_TARGET_LABEL_LIST(objects, "objects", 0);
	for (i = 0; i < target->deps.len; i++) {
		dep = target->deps.items[i];
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
			if (qstar_graph_find_genrule(graph, dep))
				continue;
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
			if (qstar_graph_find_genrule(graph, dep))
				continue;
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "private_deps", target->label,
			    "qstar: unknown dependency label '%s' referenced by '%s'",
			    dep, target->label);
		}
		if (visit_target(graph, plan, (size_t)dep_index) < 0)
			return -1;
	}
#undef VISIT_TARGET_LABEL_LIST
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

/** platform, project option, package alias map을 command-plan header에 출력한다. */
static void
dump_plan_inputs(FILE *out, const struct qstar_graph *graph)
{
	size_t i;

	fprintf(out, "platform %s\n", qstar_graph_platform(graph));
	fprintf(out, "local_action_cache mode=%s audit=report-only sandbox=off\n",
	    graph->project.action_cache && *graph->project.action_cache ?
	    graph->project.action_cache : "off");
	for (i = 0; i < graph->project_option_len; i++) {
		fprintf(out,
		    "project_option name=%s type=%s value=%s effective=%s overridden=%s choices=",
		    graph->project_options[i].name ? graph->project_options[i].name : "",
		    graph->project_options[i].type ? graph->project_options[i].type : "",
		    graph->project_options[i].value ? graph->project_options[i].value : "",
		    qstar_project_option_effective_value(&graph->project_options[i]),
		    graph->project_options[i].overridden ? "true" : "false");
		dump_list(out, &graph->project_options[i].choices);
		fputc('\n', out);
	}
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
	    "language=%s platform=%s toolset=%s deps=",
	    target->label, kind, index, kind, target->label, input, output,
	    language, qstar_graph_platform(graph),
	    target->toolset && *target->toolset ? target->toolset : "<none>");
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
	    "platform=%s toolset=%s input=%s output=%s execute=no\n",
	    target->label, kind, index, kind, language, tool,
	    qstar_graph_platform(graph),
	    target->toolset && *target->toolset ? target->toolset : "<none>", input,
	    output);
}

/** 실제 argv plan의 list header를 출력한다. */
static void
begin_argv(FILE *out, struct qstar_argv_dump *dump, struct qstar_graph *graph,
    const char *id, size_t argc,
    const struct qstar_resolved_toolchain *toolchain)
{
	memset(dump, 0, sizeof(*dump));
	dump->graph = graph;
	dump->toolchain = toolchain;
	dump->id = id;
	fprintf(out, "  command_argv id=%s argc=%zu argv=[", id, argc);
}

/** argv item 하나를 deterministic dump에 추가한다. */
static void
write_argv_value(FILE *out, const char *value)
{
	const unsigned char *p;
	int simple;

	p = (const unsigned char *)(value ? value : "");
	simple = *p != '\0';
	for (; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '/' ||
		    *p == ':' || *p == '=' || *p == '+' || *p == ','))
			simple = 0;
	}
	if (simple) {
		fputs(value, out);
		return;
	}
	fputc('"', out);
	for (p = (const unsigned char *)(value ? value : ""); *p; p++) {
		if (*p == '"' || *p == '\\')
			fprintf(out, "\\%c", *p);
		else
			fputc(*p, out);
	}
	fputc('"', out);
}

/** argv item 하나를 deterministic dump에 추가한다. */
static void
argv_item(FILE *out, struct qstar_argv_dump *dump, const char *value)
{
	if (dump->seen)
		fputs(", ", out);
	write_argv_value(out, value);
	if (!dump->oom && qstar_argv_push(&dump->logical, value) < 0)
		dump->oom = 1;
	dump->seen++;
}

/** action description을 explain/dry-run에 deterministic하게 출력한다. */
static void
dump_action_description(FILE *out, const char *prefix, const char *id,
    const char *description)
{
	fprintf(out, "%saction_description id=%s text=",
	    prefix ? prefix : "", id);
	write_argv_value(out, description && *description ? description : "<none>");
	fputc('\n', out);
}

/** group target이 progress action에서 제외됨을 explain/dry-run에 출력한다. */
static void
dump_progress_action_exclusion(FILE *out, const char *prefix, const char *label,
    const char *reason)
{
	fprintf(out, "%sprogress_action label=%s include=no reason=%s\n",
	    prefix ? prefix : "", label, reason);
}

/** command_argv line을 닫는다. */
static void
end_argv(FILE *out, struct qstar_argv_dump *dump)
{
	struct qstar_materialized_command command;
	char digest[32];

	memset(&command, 0, sizeof(command));
	qstar_argv_digest(&dump->logical, digest, sizeof(digest));
	if (dump->oom) {
		fprintf(out, "] digest=%s response=invalid materialization=out-of-memory\n",
		    digest);
		qstar_argv_free(&dump->logical);
		return;
	}
	if (!dump->toolchain && qstar_action_needs_response_file(&dump->logical)) {
		fprintf(out,
		    "] digest=%s response=none logical_argc=%zu logical_bytes=%zu "
		    "exec_argc=%zu response_capability=off\n",
		    digest, dump->logical.len, dump->logical.bytes,
		    dump->logical.len);
		qstar_argv_free(&dump->logical);
		return;
	}
	if (qstar_action_materialize(dump->graph, dump->id, dump->toolchain,
	    &dump->logical, NULL, 0, &command) < 0) {
		fprintf(out, "] digest=%s response=invalid\n", digest);
		qstar_argv_free(&dump->logical);
		return;
	}
	fprintf(out, "] digest=%s response=%s",
	    command.logical_argv_digest,
	    command.uses_response_file ? "skeleton" : "none");
	if (command.uses_response_file)
		fprintf(out, " response_file=%s response_style=%s response_digest=%s",
		    command.response_file, command.response_style,
		    command.response_digest);
	else if (!dump->toolchain || !dump->toolchain->response_files)
		fputs(" response_capability=off", out);
	fprintf(out, " logical_argc=%zu logical_bytes=%zu exec_argc=%zu",
	    command.logical_argc, command.logical_bytes, command.exec_argc);
	fputc('\n', out);
	qstar_materialized_command_free(&command);
	qstar_argv_free(&dump->logical);
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
objectlib_consumer_compile_index(const struct qstar_graph *graph,
    const struct qstar_target *consumer, size_t objectlib_list_index,
    size_t source_index)
{
	const struct qstar_target *objectlib;
	size_t i, index;

	index = consumer->sources.len;
	for (i = 0; i < objectlib_list_index; i++) {
		objectlib = plan_find_target(graph, consumer->objects.items[i]);
		if (objectlib)
			index += objectlib->sources.len;
	}
	return index + source_index;
}

/** generated action input artifact edge를 explain/dry-run에 출력한다. */
static void
dump_genrule_input_edges(FILE *out, const struct qstar_graph *graph,
    const struct qstar_genrule *genrule, const char *prefix)
{
	const struct qstar_genrule *producer;
	const struct qstar_target *target;
	char label[QSTAR_PATH_MAX], path[QSTAR_PATH_MAX];
	const char *input;
	size_t i;
	int rc;

	for (i = 0; i < genrule->inputs.len; i++) {
		input = genrule->inputs.items[i];
		rc = qstar_target_file_token_label(input, label, sizeof(label));
		if (rc == 1) {
			target = plan_find_target(graph, label);
			if (target &&
			    qstar_graph_artifact_output_path(graph, target, path,
			    sizeof(path)) == 0) {
				fprintf(out,
				    "%sartifact_input_edge input=%s producer=%s path=%s\n",
				    prefix ? prefix : "", input, label, path);
				continue;
			}
			producer = qstar_graph_find_genrule(graph, label);
			if (producer && producer->outputs.len > 0)
				fprintf(out,
				    "%sartifact_input_edge input=%s producer=%s path=%s\n",
				    prefix ? prefix : "", input, label,
				    producer->outputs.items[0]);
			continue;
		}
		producer = qstar_graph_find_output_owner(graph, input);
		if (producer && producer != genrule)
			fprintf(out,
			    "%sgenerated_input_edge input=%s producer=%s path=%s\n",
			    prefix ? prefix : "", input, producer->label, input);
	}
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

/** target compile option의 explicit config/local merge 결과를 설명한다. */
static void
dump_effective_compile_merge(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	fprintf(out,
	    "  effective_compile_merge owner=%s platform=%s response_files=%s response_style=%s order=config,target ",
	    target->label, qstar_graph_platform(graph),
	    toolchain->response_files ? "on" : "off", toolchain->response_style);
	fputs("target_c_compile_options=", out);
	dump_list(out, &target->cflags);
	fputs(" target_cxx_compile_options=", out);
	dump_list(out, &target->cxxflags);
	fputs(" target_asm_compile_options=", out);
	dump_list(out, &target->asm_compile_options);
	fputs(" target_include_dirs=", out);
	dump_list(out, &target->include_dirs);
	fputs(" target_public_include_dirs=", out);
	dump_list(out, &target->public_include_dirs);
	fputs(" target_interface_include_dirs=", out);
	dump_list(out, &target->interface_include_dirs);
	fputs(" target_system_include_dirs=", out);
	dump_list(out, &target->system_include_dirs);
	fputc('\n', out);
}

static void
dump_dependency_usage(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const char *indent)
{
	struct qstar_string_list compile_options, compile_inputs;
	struct qstar_string_list link_options, link_inputs;
	const char *prefix = indent ? indent : "";

	memset(&compile_options, 0, sizeof(compile_options));
	memset(&compile_inputs, 0, sizeof(compile_inputs));
	memset(&link_options, 0, sizeof(link_options));
	memset(&link_inputs, 0, sizeof(link_inputs));
	fprintf(out, "%scompile_usage.options ", prefix);
	dump_list(out, &target->compile_usage_options);
	fputc('\n', out);
	fprintf(out, "%scompile_usage.inputs ", prefix);
	dump_list(out, &target->compile_usage_inputs);
	fputc('\n', out);
	fprintf(out, "%slink_usage.options ", prefix);
	dump_list(out, &target->link_usage_options);
	fputc('\n', out);
	fprintf(out, "%slink_usage.inputs ", prefix);
	dump_list(out, &target->link_usage_inputs);
	fputc('\n', out);
	if (qstar_graph_collect_compile_usage(graph, target, &compile_options,
	    &compile_inputs) == 0 &&
	    qstar_graph_collect_link_usage(graph, target, &link_options,
	    &link_inputs) == 0) {
		fprintf(out, "%seffective_compile_usage.options ", prefix);
		dump_list(out, &compile_options);
		fputc('\n', out);
		fprintf(out, "%seffective_compile_usage.inputs ", prefix);
		dump_list(out, &compile_inputs);
		fputc('\n', out);
		fprintf(out, "%seffective_link_usage.options ", prefix);
		dump_list(out, &link_options);
		fputc('\n', out);
		fprintf(out, "%seffective_link_usage.inputs ", prefix);
		dump_list(out, &link_inputs);
		fputc('\n', out);
	}
	qstar_string_list_free(&compile_options);
	qstar_string_list_free(&compile_inputs);
	qstar_string_list_free(&link_options);
	qstar_string_list_free(&link_inputs);
}

/** toolset role argv-vector가 있으면 argc 계산에 반영한다. */
static const struct qstar_string_list *
plan_resolved_tool_role_argv(const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain,
    const char *role)
{
	const struct qstar_toolset *toolset;

	if (toolchain && toolchain->toolset[0]) {
		toolset = qstar_graph_find_toolset(graph, toolchain->toolset);
		return qstar_toolset_role_argv(toolset, role);
	}
	return qstar_target_tool_role_argv(graph, target, role);
}

static size_t
plan_tool_role_argc(const struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *role)
{
	const struct qstar_string_list *argv;

	argv = plan_resolved_tool_role_argv(graph, target, toolchain, role);
	return argv ? argv->len : 1;
}

/** toolset role argv-vector 또는 fallback tool 하나를 command plan에 쓴다. */
static void
plan_argv_tool_role(FILE *out, struct qstar_argv_dump *dump,
    const struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain, const char *role,
    const char *fallback)
{
	const struct qstar_string_list *argv;
	size_t i;

	argv = plan_resolved_tool_role_argv(graph, target, toolchain, role);
	if (!argv) {
		argv_item(out, dump, fallback);
		return;
	}
	for (i = 0; i < argv->len; i++)
		argv_item(out, dump, argv->items[i]);
}

static size_t
plan_provider_template_argc(const struct qstar_graph *graph,
    const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_string_list *argv)
{
	char role[128];
	size_t argc, i;
	int rc;

	argc = 0;
	for (i = 0; argv && i < argv->len; i++) {
		rc = qstar_provider_tool_token_role(argv->items[i], role, sizeof(role));
		if (rc == 1)
			argc += plan_tool_role_argc(graph, target, toolchain, role);
		else
			argc++;
	}
	return argc;
}

static void
plan_argv_provider_template(FILE *out, struct qstar_argv_dump *dump,
    const struct qstar_graph *graph, const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_string_list *argv)
{
	const struct qstar_string_list *role_argv;
	char role[128], missing[160];
	size_t i, j;
	int rc;

	for (i = 0; argv && i < argv->len; i++) {
		rc = qstar_provider_tool_token_role(argv->items[i], role, sizeof(role));
		if (rc == 1) {
			role_argv = plan_resolved_tool_role_argv(graph, target,
			    toolchain, role);
			if (!role_argv) {
				snprintf(missing, sizeof(missing), "<missing-tool:%s>",
				    role);
				argv_item(out, dump, missing);
				continue;
			}
			for (j = 0; j < role_argv->len; j++)
				argv_item(out, dump, role_argv->items[j]);
			continue;
		}
		argv_item(out, dump, argv->items[i]);
	}
}

/** compile action의 실제 argv plan을 출력한다. */
static void
dump_compile_argv(FILE *out, const struct qstar_target *target,
    const struct qstar_target *source_owner, const struct qstar_graph *graph,
    const struct qstar_resolved_toolchain *toolchain, const struct qstar_source_info *source,
    const char *input, const char *output, size_t source_index, size_t action_index,
    const char *depfile_override)
{
	const struct qstar_provider_source_unit *provider_unit;
	const struct qstar_language_provider *provider;
	char id[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], std_arg[128];
	char pch[QSTAR_PATH_MAX], pch_include[QSTAR_PATH_MAX];
	char module_dir[QSTAR_PATH_MAX], module_flag[QSTAR_PATH_MAX + 32];
	char bmi[QSTAR_PATH_MAX], bmi_flag[QSTAR_PATH_MAX + 32];
	const char *role;
	const char *tool;
	struct qstar_string_list includes, usage_options, usage_inputs, module_flags;
	struct qstar_argv_dump dump;
	size_t argc, i;
	int is_asm, is_cxx, provider_source, wants_depfile, unity_strategy;
	size_t unity_batch;

	memset(&includes, 0, sizeof(includes));
	memset(&usage_options, 0, sizeof(usage_options));
	memset(&usage_inputs, 0, sizeof(usage_inputs));
	memset(&module_flags, 0, sizeof(module_flags));
	collect_compile_include_dirs(graph, target, &includes);
	qstar_graph_collect_compile_usage(graph, target, &usage_options,
	    &usage_inputs);
	is_asm = qstar_source_is_asm(source);
	is_cxx = strcmp(source->provider, "cxx") == 0;
	provider_unit = qstar_target_provider_source_unit(source_owner, source_index);
	provider_source = provider_unit != NULL;
	if (!provider_source && is_cxx && target->cxx_modules_enabled)
		qstar_cxx_collect_module_flags(graph, target, source_index,
		    &module_flags);
	unity_strategy = 0;
	if (source_owner == target)
		qstar_cxx_unity_source_info(target, source_index, &unity_batch,
		    &unity_strategy);
	if (unity_strategy)
		snprintf(id, sizeof(id), "%s:compile-unity-%zu:0", target->label,
		    unity_batch);
	else if (qstar_cxx_source_is_module_interface(source_owner, source_index))
		snprintf(id, sizeof(id), "%s:compile-module-interface-%zu:0",
		    target->label, action_index);
	else if (is_cxx && target->cxx_modules_enabled)
		snprintf(id, sizeof(id), "%s:compile-module-implementation-%zu:0",
		    target->label, action_index);
	else
		snprintf(id, sizeof(id), "%s:compile:%zu", target->label, action_index);
	if (unity_strategy)
		qstar_cxx_unity_depfile_path(graph, target, unity_batch, depfile,
		    sizeof(depfile));
	else if (depfile_override)
		snprintf(depfile, sizeof(depfile), "%s", depfile_override);
	else
		qstar_graph_depfile_output_path(graph, target, source_index, depfile,
		    sizeof(depfile));
	wants_depfile = !provider_source &&
	    (strcmp(source->provider, "c") == 0 || is_cxx ||
	    qstar_source_uses_asm_preprocessor(target, source));
	if (provider_unit) {
		provider = qstar_graph_find_language_provider(graph,
		    provider_unit->provider);
		argc = plan_provider_template_argc(graph, target, toolchain,
		    &provider_unit->action.argv) + usage_options.len;
		dump_provider_action_contract(out, id, "compile",
		    provider ? provider->api : "", provider_unit->provider,
		    &provider_unit->action);
		begin_argv(out, &dump, (struct qstar_graph *)graph, id, argc,
		    toolchain);
		plan_argv_provider_template(out, &dump, graph, target, toolchain,
		    &provider_unit->action.argv);
		for (i = 0; i < usage_options.len; i++)
			argv_item(out, &dump, usage_options.items[i]);
		end_argv(out, &dump);
		qstar_string_list_free(&includes);
		qstar_string_list_free(&usage_options);
		qstar_string_list_free(&usage_inputs);
		qstar_string_list_free(&module_flags);
		return;
	}
	role = qstar_source_toolset_role(source);
	tool = qstar_resolved_toolchain_provider_tool(toolchain, source->provider,
	    source->provider_role);
	if (!tool)
		tool = toolchain->cc;
	argc = 5 + plan_tool_role_argc(graph, target, toolchain, role) - 1 +
	    (provider_source ? 0 :
	    (is_asm ? target->asm_include_dirs.len * 2 : includes.len * 2)) +
	    (provider_source || is_asm ? 0 : target->system_include_dirs.len * 2) +
	    (wants_depfile ? 3 : 0) +
	    (!provider_source && is_asm ? 2 : 0) +
	    (!provider_source && qstar_cxx_source_is_module_interface(source_owner,
	    source_index) ? 2 : 0) +
	    (strcmp(target->kind, "sharedlib") == 0 &&
	    qstar_platform_supports_sharedlib(toolchain->platform) &&
	    !qstar_platform_is_windows(toolchain->platform) && !provider_source &&
	    !is_asm ? 1 : 0) +
	    (!provider_source && is_cxx && target->cxx_standard[0] ? 1 : 0) +
	    (!provider_source && is_cxx &&
	    !qstar_cxx_source_is_module_interface(source_owner, source_index) &&
	    target->cxx_precompiled_header && *target->cxx_precompiled_header ? 2 : 0) +
	    (!provider_source && is_cxx && target->cxx_modules_enabled ?
	    (qstar_cxx_source_is_module_interface(source_owner, source_index) ? 2 : 1) : 0) +
	    module_flags.len +
	    (!provider_source && is_cxx && unity_strategy ? 2 : 0) +
	    usage_options.len +
	    (provider_source ? 0 : is_asm ? target->asm_compile_options.len :
	    is_cxx ? target->cxxflags.len : target->cflags.len);
	snprintf(std_arg, sizeof(std_arg), "-std=%s", target->cxx_standard);
	begin_argv(out, &dump, (struct qstar_graph *)graph, id, argc, toolchain);
	plan_argv_tool_role(out, &dump, graph, target, toolchain, role, tool);
	if (strcmp(target->kind, "sharedlib") == 0 &&
	    qstar_platform_supports_sharedlib(toolchain->platform) &&
	    !qstar_platform_is_windows(toolchain->platform) && !provider_source &&
	    !is_asm)
		argv_item(out, &dump, "-fPIC");
	if (!provider_source && is_asm) {
		argv_item(out, &dump, "-x");
		argv_item(out, &dump, qstar_source_uses_asm_preprocessor(target, source) ?
		    "assembler-with-cpp" : "assembler");
	}
	if (!provider_source && qstar_cxx_source_is_module_interface(source_owner,
	    source_index)) {
		argv_item(out, &dump, "-x");
		argv_item(out, &dump, "c++-module");
	}
	argv_item(out, &dump, "-c");
	argv_item(out, &dump, input);
	argv_item(out, &dump, "-o");
	argv_item(out, &dump, output);
	if (wants_depfile) {
		argv_item(out, &dump, "-MMD");
		argv_item(out, &dump, "-MF");
		argv_item(out, &dump, depfile);
	}
	if (!provider_source && is_cxx && target->cxx_standard[0])
		argv_item(out, &dump, std_arg);
	if (!provider_source && is_cxx &&
	    !qstar_cxx_source_is_module_interface(source_owner, source_index) &&
	    target->cxx_precompiled_header && *target->cxx_precompiled_header &&
	    qstar_cxx_pch_output_path(graph, target, toolchain, pch, sizeof(pch)) == 0 &&
	    qstar_cxx_pch_include_path(graph, target, toolchain, pch_include,
	    sizeof(pch_include)) == 0) {
		argv_item(out, &dump, strcmp(qstar_cxx_compiler_family(toolchain), "gcc") == 0 ?
		    "-include" : "-include-pch");
		argv_item(out, &dump, pch_include);
	}
	if (!provider_source && is_cxx && target->cxx_modules_enabled &&
	    qstar_cxx_module_dir_path(graph, target, module_dir, sizeof(module_dir)) == 0) {
		snprintf(module_flag, sizeof(module_flag), "-fprebuilt-module-path=%s",
		    module_dir);
		argv_item(out, &dump, module_flag);
		if (qstar_cxx_source_is_module_interface(source_owner, source_index) &&
		    qstar_cxx_module_output_path(graph, target, source_index, bmi,
		    sizeof(bmi)) == 0) {
			snprintf(bmi_flag, sizeof(bmi_flag), "-fmodule-output=%s", bmi);
			argv_item(out, &dump, bmi_flag);
		}
		for (i = 0; i < module_flags.len; i++)
			argv_item(out, &dump, module_flags.items[i]);
	}
	if (!provider_source && is_cxx && unity_strategy) {
		argv_item(out, &dump, "-iquote");
		argv_item(out, &dump, ".");
	}
	for (i = 0; i < usage_options.len; i++)
		argv_item(out, &dump, usage_options.items[i]);
	for (i = 0; !provider_source && !is_asm && !is_cxx && i < target->cflags.len; i++)
		argv_item(out, &dump, target->cflags.items[i]);
	for (i = 0; !provider_source && is_cxx && i < target->cxxflags.len; i++)
		argv_item(out, &dump, target->cxxflags.items[i]);
	for (i = 0; !provider_source && is_asm && i < target->asm_compile_options.len; i++)
		argv_item(out, &dump, target->asm_compile_options.items[i]);
	for (i = 0; !provider_source && !is_asm && i < includes.len; i++) {
		argv_item(out, &dump, "-I");
		argv_item(out, &dump, includes.items[i]);
	}
	for (i = 0; !provider_source && is_asm && i < target->asm_include_dirs.len; i++) {
		argv_item(out, &dump, "-I");
		argv_item(out, &dump, target->asm_include_dirs.items[i]);
	}
	for (i = 0; !provider_source && !is_asm && i < target->system_include_dirs.len; i++) {
		argv_item(out, &dump, "-isystem");
		argv_item(out, &dump, target->system_include_dirs.items[i]);
	}
	end_argv(out, &dump);
	qstar_string_list_free(&includes);
	qstar_string_list_free(&usage_options);
	qstar_string_list_free(&usage_inputs);
	qstar_string_list_free(&module_flags);
}

/** consumer-context objectlib compile action을 command plan에 출력한다. */
static int
dump_consumer_objectlib_compile_plan(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	const struct qstar_target *objectlib;
	struct qstar_source_info source;
	char output[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX];
	char description[QSTAR_PATH_MAX];
	size_t i, j, action_index;

	for (i = 0; i < target->objects.len; i++) {
		objectlib = plan_find_target(plan->graph, target->objects.items[i]);
		if (!objectlib || !objectlib_uses_consumer_context(objectlib))
			continue;
		for (j = 0; j < objectlib->sources.len; j++) {
			qstar_target_source_classify(objectlib, j, &source);
			if (!qstar_source_requires_compile(&source))
				continue;
			if (qstar_target_provider_source_unit(objectlib, j))
				return qstar_set_error_origin(plan->graph,
				    objectlib->origin_file, objectlib->origin_line,
				    "sources", objectlib->label,
				    "qstar: provider source token '%s' cannot be used from compile_context = \"consumer\" objectlib yet; use raw provider source strings or compile_context = \"own\"",
				    objectlib->sources.items[j]);
			if (objectlib_source_object_input(plan->graph, target, objectlib, j,
			    output, sizeof(output)) < 0 ||
			    objectlib_source_depfile_output(plan->graph, target, objectlib, j,
			    depfile, sizeof(depfile)) < 0)
				return qstar_set_error(plan->graph,
				    "qstar: consumer objectlib path too long");
			action_index = objectlib_consumer_compile_index(plan->graph, target,
			    i, j);
			snprintf(id, sizeof(id), "%s:compile:%zu", target->label,
			    action_index);
			if (qstar_action_description_compile(objectlib, &source, output,
			    description, sizeof(description)) < 0)
				snprintf(description, sizeof(description), "<too-long>");
			dump_action_description(out, "  ", id, description);
			fprintf(out,
			    "  action compile source=%s source_owner=%s output=%s compile_context=consumer\n",
			    objectlib->sources.items[j], objectlib->label, output);
			dump_action_key(out, plan->graph, target, "compile",
			    objectlib->sources.items[j], output, source.language,
			    action_index);
			dump_command_skeleton(out, plan->graph, target, "compile",
			    objectlib->sources.items[j], output, source.language,
			    source.tool_role, action_index);
			dump_compile_argv(out, target, objectlib, plan->graph, toolchain,
			    &source, objectlib->sources.items[j], output, j,
			    action_index, depfile);
		}
	}
	return 0;
}

/** generated action의 실제 argv plan을 출력한다. */
static const char *
plan_resolve_target_file_arg(const struct qstar_graph *graph, const char *arg,
    char *buf, size_t buflen)
{
	const struct qstar_genrule *genrule;
	const struct qstar_target *target;
	char label[QSTAR_PATH_MAX], artifact[64];
	int rc;

	rc = qstar_target_file_token_parse(arg, label, sizeof(label), artifact,
	    sizeof(artifact));
	if (rc <= 0)
		return arg;
	target = plan_find_target(graph, label);
	if (target && strcmp(target->kind, "group") == 0) {
		snprintf(buf, buflen, "<group-has-no-artifact>");
		return buf;
	}
	if (target && strcmp(artifact, "@tool") == 0 &&
	    qstar_graph_target_tool_path((struct qstar_graph *)graph, target, buf,
	    buflen) == 0)
		return buf;
	if (target && qstar_graph_target_artifact_path((struct qstar_graph *)graph,
	    target, artifact, buf, buflen) == 0)
		return buf;
	genrule = qstar_graph_find_genrule(graph, label);
	if (genrule && genrule->outputs.len > 0 && !artifact[0]) {
		snprintf(buf, buflen, "%s", genrule->outputs.items[0]);
		return buf;
	}
	return arg;
}

/** run_target argv/input token을 display용 path로 해석한다. */
static const char *
plan_resolve_run_arg(const struct qstar_graph *graph, const char *arg,
    char *buf, size_t buflen)
{
	const struct qstar_stage *stage;
	char label[QSTAR_PATH_MAX];
	int rc;

	rc = qstar_stage_dir_token_label(arg, label, sizeof(label));
	if (rc == 1) {
		stage = qstar_graph_find_stage(graph, label);
		snprintf(buf, buflen, "%s", stage && stage->root && *stage->root ?
		    stage->root : ".");
		return buf;
	}
	if (rc < 0) {
		snprintf(buf, buflen, "<malformed-stage-dir>");
		return buf;
	}
	return plan_resolve_target_file_arg(graph, arg, buf, buflen);
}

/** generated action의 실제 argv plan을 출력한다. */
static void
dump_genrule_argv(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_genrule *genrule)
{
	size_t i;
	struct qstar_argv_dump dump;
	struct qstar_resolved_toolchain materialize_toolchain;
	const struct qstar_resolved_toolchain *materialize_toolchain_ptr;
	char id[QSTAR_PATH_MAX], resolved_tool[QSTAR_PATH_MAX], tool_mode[64];
	char resolved_arg[QSTAR_PATH_MAX];
	char tool_error[QSTAR_PATH_MAX];

	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	materialize_toolchain_ptr = NULL;
	if (genrule->toolset && *genrule->toolset &&
	    qstar_resolve_toolset_context((struct qstar_graph *)graph,
	    genrule->toolset, &materialize_toolchain) == 0)
		materialize_toolchain_ptr = &materialize_toolchain;
	begin_argv(out, &dump, (struct qstar_graph *)graph, id,
	    1 + genrule->args.len, materialize_toolchain_ptr);
	if (genrule->config_header) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "builtin");
		argv_item(out, &dump, resolved_tool);
	} else if (qstar_resolve_command_tool_for_genrule(graph, target, genrule,
	    genrule->tool,
	    resolved_tool, sizeof(resolved_tool), tool_mode, sizeof(tool_mode),
	    tool_error, sizeof(tool_error)) == 0)
		argv_item(out, &dump, resolved_tool);
	else {
		snprintf(tool_mode, sizeof(tool_mode), "invalid");
		argv_item(out, &dump, genrule->tool);
	}
	for (i = 0; i < genrule->args.len; i++)
		argv_item(out, &dump, plan_resolve_target_file_arg(graph,
		    genrule->args.items[i], resolved_arg, sizeof(resolved_arg)));
	end_argv(out, &dump);
}

/** run_target command argv plan을 출력한다. */
static void
dump_run_argv(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	struct qstar_argv_dump dump;
	struct qstar_resolved_toolchain materialize_toolchain;
	const struct qstar_resolved_toolchain *materialize_toolchain_ptr;
	char id[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX];
	size_t i;

	snprintf(id, sizeof(id), "%s:run:0", target->label);
	materialize_toolchain_ptr = NULL;
	if (target->toolset && *target->toolset &&
	    qstar_resolve_toolchain((struct qstar_graph *)graph, target,
	    &materialize_toolchain) == 0)
		materialize_toolchain_ptr = &materialize_toolchain;
	begin_argv(out, &dump, (struct qstar_graph *)graph, id,
	    target->run_command.len, materialize_toolchain_ptr);
	for (i = 0; i < target->run_command.len; i++)
		argv_item(out, &dump, plan_resolve_run_arg(graph,
		    target->run_command.items[i], resolved, sizeof(resolved)));
	end_argv(out, &dump);
}

static int
path_is_object_input(const char *path)
{
	size_t len;

	len = strlen(path ? path : "");
	return (len >= 2 && strcmp(path + len - 2, ".o") == 0) ||
	    (len >= 4 && strcmp(path + len - 4, ".obj") == 0);
}

/** shared final lowering과 materializer가 계산한 실제 logical argv를 출력한다. */
static int
dump_lowered_final_argv(FILE *out, struct qstar_graph *graph,
    const struct qstar_target *target,
    const struct qstar_resolved_toolchain *toolchain)
{
	struct qstar_lowered_action action;
	struct qstar_materialized_command command;
	size_t i, object_count;

	memset(&action, 0, sizeof(action));
	memset(&command, 0, sizeof(command));
	if (qstar_lower_final_action(graph, target, toolchain, &action) < 0)
		return -1;
	if (qstar_target_has_provider_final_action(target))
		dump_provider_action_contract(out, action.id, "final",
		    target->provider_final.api, target->provider_final.provider,
		    &target->provider_final.action);
	if (qstar_action_materialize(graph, action.id, toolchain,
	    &action.logical_argv, &action.env, 0, &command) < 0) {
		qstar_lowered_action_free(&action);
		return -1;
	}
	object_count = 0;
	for (i = 0; i < action.inputs.len; i++) {
		if (path_is_object_input(action.inputs.items[i]))
			object_count++;
	}
	fprintf(out, "  command_argv id=%s argc=%zu argv=[",
	    action.id, action.logical_argv.len);
	for (i = 0; i < action.logical_argv.len; i++) {
		if (i)
			fputs(", ", out);
		write_argv_value(out, action.logical_argv.items[i]);
	}
	fprintf(out, "] digest=%s response=%s",
	    command.logical_argv_digest,
	    command.uses_response_file ? "skeleton" : "none");
	if (command.uses_response_file)
		fprintf(out,
		    " response_file=%s response_style=%s response_digest=%s",
		    command.response_file, command.response_style,
		    command.response_digest);
	else {
		if (!toolchain->response_files &&
		    qstar_action_needs_response_file(&action.logical_argv))
			fputs(" response_capability=off", out);
	}
	fprintf(out,
	    " logical_argc=%zu logical_bytes=%zu object_count=%zu "
	    "input_count=%zu exec_argc=%zu",
	    command.logical_argc, command.logical_bytes, object_count,
	    action.inputs.len, command.exec_argc);
	fputc('\n', out);
	qstar_materialized_command_free(&command);
	qstar_lowered_action_free(&action);
	return 0;
}

static const char *
provider_final_input_summary(const struct qstar_target *target)
{
	return target && target->provider_final.api &&
	    strcmp(target->provider_final.api, "qstar.lang/2") == 0 ?
	    "<provider-owned-inputs>" : "<provider-sources>";
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

/** generated action에서 특정 output path의 index를 찾는다. */
static size_t
genrule_output_index_for_path(const struct qstar_genrule *genrule, const char *path)
{
	size_t i;

	for (i = 0; i < genrule->outputs.len; i++) {
		if (strcmp(genrule->outputs.items[i], path) == 0)
			return i;
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

/** list가 generated action label이나 target_file token으로 genrule을 참조하는지 확인한다. */
static int
genrule_referenced_in_list(const struct qstar_genrule *genrule,
    const struct qstar_string_list *list)
{
	char label[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], genrule->label) == 0)
			return 1;
		if (qstar_target_file_token_label(list->items[i], label,
		    sizeof(label)) == 1 &&
		    strcmp(label, genrule->label) == 0)
			return 1;
	}
	return 0;
}

/** target이 source/header/link/deps 경로 어디에서든 genrule을 참조하는지 확인한다. */
static int
genrule_referenced_by_target(const struct qstar_genrule *genrule,
    const struct qstar_target *target)
{
	return genrule_consumed_by_target(genrule, target) ||
	    genrule_output_in_list(genrule, &target->link_inputs) ||
	    genrule_referenced_in_list(genrule, &target->link_inputs) ||
	    genrule_referenced_in_list(genrule, &target->compile_usage_inputs) ||
	    genrule_referenced_in_list(genrule, &target->link_usage_inputs) ||
	    genrule_referenced_in_list(genrule, &target->run_inputs) ||
	    genrule_referenced_in_list(genrule, &target->run_command) ||
	    genrule_referenced_in_list(genrule, &target->deps) ||
	    genrule_referenced_in_list(genrule, &target->private_deps);
}

/** generated dependency edge 한 줄을 출력한다. */
static void
dump_generated_dep_edge(FILE *out, const char *field,
    const struct qstar_target *target, const struct qstar_genrule *genrule,
    const char *output)
{
	fprintf(out,
	    "  generated_dep_edge field=%s dependent=%s producer=%s output=%s\n",
	    field, target->label, genrule->label, output && *output ? output : "<none>");
}

/** target이 소비하는 generated output edge를 deterministic하게 출력한다. */
static void
dump_generated_edges(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_genrule *owner;
	char identity[QSTAR_PATH_MAX];
	size_t i, oi;

	for (i = 0; i < target->sources.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->sources.items[i]);
		if (owner) {
			oi = genrule_output_index_for_path(owner, target->sources.items[i]);
			if (qstar_genrule_output_identity(owner, oi, identity,
			    sizeof(identity)) < 0)
				snprintf(identity, sizeof(identity), "<too-long>");
			fprintf(out,
			    "  generated_edge source=%s generator=%s output=%s identity=%s\n",
			    target->sources.items[i], owner->label, target->sources.items[i],
			    identity);
		}
	}
	for (i = 0; i < target->public_headers.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->public_headers.items[i]);
		if (owner) {
			oi = genrule_output_index_for_path(owner, target->public_headers.items[i]);
			if (qstar_genrule_output_identity(owner, oi, identity,
			    sizeof(identity)) < 0)
				snprintf(identity, sizeof(identity), "<too-long>");
			fprintf(out,
			    "  generated_edge header=%s generator=%s output=%s identity=%s\n",
			    target->public_headers.items[i], owner->label,
			    target->public_headers.items[i], identity);
		}
	}
	for (i = 0; i < target->private_headers.len; i++) {
		owner = qstar_graph_find_output_owner(plan->graph, target->private_headers.items[i]);
		if (owner) {
			oi = genrule_output_index_for_path(owner, target->private_headers.items[i]);
			if (qstar_genrule_output_identity(owner, oi, identity,
			    sizeof(identity)) < 0)
				snprintf(identity, sizeof(identity), "<too-long>");
			fprintf(out,
			    "  generated_edge header=%s generator=%s output=%s identity=%s\n",
			    target->private_headers.items[i], owner->label,
			    target->private_headers.items[i], identity);
		}
	}
	for (i = 0; i < target->link_inputs.len; i++) {
		char label[QSTAR_PATH_MAX];

		owner = qstar_graph_find_output_owner(plan->graph, target->link_inputs.items[i]);
		if (owner) {
			dump_generated_dep_edge(out, "link_inputs", target, owner,
			    target->link_inputs.items[i]);
			continue;
		}
		if (qstar_target_file_token_label(target->link_inputs.items[i], label,
		    sizeof(label)) == 1) {
			owner = qstar_graph_find_genrule(plan->graph, label);
			if (owner && owner->outputs.len > 0)
				dump_generated_dep_edge(out, "link_inputs", target,
				    owner, owner->outputs.items[0]);
		}
	}
	for (i = 0; i < target->run_command.len; i++) {
		char label[QSTAR_PATH_MAX];

		if (qstar_target_file_token_label(target->run_command.items[i], label,
		    sizeof(label)) != 1)
			continue;
		owner = qstar_graph_find_genrule(plan->graph, label);
		if (owner && owner->outputs.len > 0)
			dump_generated_dep_edge(out, "command", target, owner,
			    owner->outputs.items[0]);
	}
	for (i = 0; i < target->run_inputs.len; i++) {
		char label[QSTAR_PATH_MAX];

		owner = qstar_graph_find_output_owner(plan->graph, target->run_inputs.items[i]);
		if (owner) {
			dump_generated_dep_edge(out, "inputs", target, owner,
			    target->run_inputs.items[i]);
			continue;
		}
		if (qstar_target_file_token_label(target->run_inputs.items[i], label,
		    sizeof(label)) == 1) {
			owner = qstar_graph_find_genrule(plan->graph, label);
			if (owner && owner->outputs.len > 0)
				dump_generated_dep_edge(out, "inputs", target,
				    owner, owner->outputs.items[0]);
		}
	}
	for (i = 0; i < target->deps.len; i++) {
		owner = qstar_graph_find_genrule(plan->graph, target->deps.items[i]);
		if (owner && owner->outputs.len > 0)
			dump_generated_dep_edge(out, "deps", target, owner,
			    owner->outputs.items[0]);
	}
	for (i = 0; i < target->private_deps.len; i++) {
		owner = qstar_graph_find_genrule(plan->graph,
		    target->private_deps.items[i]);
		if (owner && owner->outputs.len > 0)
			dump_generated_dep_edge(out, "private_deps", target, owner,
			    owner->outputs.items[0]);
	}
}

/** target plan 안에서 소비되는 generated action skeleton을 출력한다. */
static void
dump_consumed_genrules(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_genrule *genrule;
	char inputs[QSTAR_PATH_MAX], outputs[QSTAR_PATH_MAX], identities[QSTAR_PATH_MAX];
	char resolved_tool[QSTAR_PATH_MAX], tool_mode[64], tool_error[QSTAR_PATH_MAX];
	char id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < plan->graph->genrule_len; i++) {
		genrule = &plan->graph->genrules[i];
		if (!genrule_referenced_by_target(genrule, target))
			continue;
		format_list_field(inputs, sizeof(inputs), &genrule->inputs);
		format_list_field(outputs, sizeof(outputs), &genrule->outputs);
		if (qstar_genrule_output_identity_list(genrule, identities,
		    sizeof(identities)) < 0)
			snprintf(identities, sizeof(identities), "[<too-long>]");
		if (genrule->config_header) {
			snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
			snprintf(tool_mode, sizeof(tool_mode), "builtin");
		} else if (qstar_resolve_command_tool_for_genrule(plan->graph, target,
		    genrule, genrule->tool, resolved_tool, sizeof(resolved_tool),
		    tool_mode, sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0) {
			snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
			snprintf(tool_mode, sizeof(tool_mode), "invalid");
		}
		snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
		if (qstar_action_description_generate(genrule, description,
		    sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		fprintf(out,
		    "  generated_action id=%s tool=%s toolset=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s args=",
		    genrule->label, genrule->tool,
		    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>",
		    tool_mode, resolved_tool, inputs, outputs, identities);
		dump_list(out, &genrule->args);
		fputs(" execute=no\n", out);
		dump_genrule_input_edges(out, plan->graph, genrule, "  ");
		dump_genrule_artifacts(out, genrule, "  ");
		fprintf(out,
		    "  action_key id=%s:generate:0 kind=generate owner=%s consumer=%s "
		    "input=%s output=%s language=generated platform=%s toolset=%s deps=[] packages=",
		    genrule->label, genrule->label, target->label, inputs, identities,
		    qstar_graph_platform(plan->graph),
		    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>");
		dump_package_aliases(out, plan->graph);
		fputc('\n', out);
		fprintf(out,
		    "  command_skeleton id=%s:generate:0 phase=generate language=generated "
		    "tool=%s toolset=%s resolved_tool=%s tool_mode=%s platform=%s input=%s output=%s "
		    "consumer=%s execute=no\n",
		    genrule->label, genrule->tool,
		    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>",
		    resolved_tool, tool_mode,
		    qstar_graph_platform(plan->graph), inputs, identities, target->label);
		dump_genrule_argv(out, plan->graph, target, genrule);
	}
}

/** 직접 선택된 generated action의 non-executing command-plan record를 출력한다. */
static void
dump_direct_genrule_plan(FILE *out, const struct qstar_graph *graph,
    const struct qstar_genrule *genrule, const char *mode)
{
	char inputs[QSTAR_PATH_MAX], outputs[QSTAR_PATH_MAX], identities[QSTAR_PATH_MAX];
	char resolved_tool[QSTAR_PATH_MAX], tool_mode[64], tool_error[QSTAR_PATH_MAX];
	char id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];

	format_list_field(inputs, sizeof(inputs), &genrule->inputs);
	format_list_field(outputs, sizeof(outputs), &genrule->outputs);
	if (qstar_genrule_output_identity_list(genrule, identities, sizeof(identities)) < 0)
		snprintf(identities, sizeof(identities), "[<too-long>]");
	if (genrule->config_header) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "builtin");
	} else if (qstar_resolve_command_tool_for_genrule(graph, NULL, genrule,
	    genrule->tool, resolved_tool, sizeof(resolved_tool), tool_mode,
	    sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "invalid");
	}
	fprintf(out,
	    "%s_generated_action %s tool=%s toolset=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s cacheable=%s\n",
	    mode, genrule->label, genrule->tool,
	    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>",
	    tool_mode, resolved_tool, inputs, outputs, identities,
	    genrule->cacheable ? "true" : "false");
	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	if (qstar_action_description_generate(genrule, description,
	    sizeof(description)) < 0)
		snprintf(description, sizeof(description), "<too-long>");
	dump_action_description(out, "  ", id, description);
	dump_genrule_input_edges(out, graph, genrule, "  ");
	dump_genrule_artifacts(out, genrule, "  ");
	fprintf(out,
	    "  action_key id=%s:generate:0 kind=generate owner=%s consumer=<direct> "
	    "input=%s output=%s language=generated platform=%s toolset=%s deps=[] packages=",
	    genrule->label, genrule->label, inputs, identities,
	    qstar_graph_platform(graph),
	    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>");
	dump_package_aliases(out, graph);
	fputc('\n', out);
	fprintf(out,
	    "  command_skeleton id=%s:generate:0 phase=generate language=generated "
	    "tool=%s toolset=%s resolved_tool=%s tool_mode=%s platform=%s input=%s output=%s consumer=<direct> execute=no\n",
	    genrule->label, genrule->tool,
	    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>",
	    resolved_tool, tool_mode, qstar_graph_platform(graph), inputs, identities);
	dump_genrule_argv(out, graph, NULL, genrule);
}

/** target-local toolset resolver skeleton을 출력한다. */
static int
dump_resolved_toolchain(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target, struct qstar_resolved_toolchain *resolved)
{
	if (qstar_resolve_toolchain(plan->graph, target, resolved) < 0)
		return -1;
	fprintf(out,
	    "  resolved_tools owner=%s platform=%s link_style=%s resolver=%s toolset=%s cc=%s cxx=%s asm=%s ar=%s "
	    "linker=%s response_files=%s response_style=%s\n",
	    target->label, resolved->platform, resolved->link_style, resolved->resolver,
	    resolved->toolset[0] ? resolved->toolset : "<none>",
	    resolved->cc, resolved->cxx, resolved->asm_, resolved->ar,
	    resolved->linker,
	    resolved->response_files ? "on" : "off", resolved->response_style);
	return 0;
}

/** doctor 출력용으로 path에 separator가 있는지 확인한다. */
static int
doctor_path_has_separator(const char *path)
{
	return path && (strchr(path, '/') || strchr(path, '\\'));
}

/** doctor 출력용으로 absolute path 여부를 확인한다. */
static int
doctor_path_is_absolute(const char *path)
{
	return path && path[0] == '/';
}

/** doctor가 build directory 쓰기 가능 여부를 확인하기 위해 directory를 만든다. */
static int
doctor_mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	char *p;
	struct stat st;

	if (!path || !*path)
		return -1;
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (qstar_platform_mkdir(tmp, 0777) < 0 &&
		    (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode))) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if (qstar_platform_mkdir(tmp, 0777) < 0 &&
	    (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode)))
		return -1;
	return 0;
}

/** doctor path 상태를 package-relative/absolute 양쪽에서 확인한다. */
static int
doctor_path_state(const struct qstar_graph *graph, const char *path, char *full,
    size_t full_len, int *exists, int *executable, int *is_dir)
{
	struct stat st;

	if (exists)
		*exists = 0;
	if (executable)
		*executable = 0;
	if (is_dir)
		*is_dir = 0;
	if (!path || !*path)
		return -1;
	if (doctor_path_is_absolute(path)) {
		if (snprintf(full, full_len, "%s", path) >= (int)full_len)
			return -1;
	} else if (qstar_path_is_package_relative(path)) {
		if (qstar_path_join(graph->package_root ? graph->package_root : ".", path,
		    full, full_len) < 0)
			return -1;
	} else {
		return -1;
	}
	if (stat(full, &st) == 0) {
		if (exists)
			*exists = 1;
		if (is_dir)
			*is_dir = S_ISDIR(st.st_mode);
	}
	if (access(full, X_OK) == 0 && executable)
		*executable = 1;
	return 0;
}

/** doctor가 toolchain role 하나의 발견 상태를 출력한다. */
static void
dump_toolset_tool_doctor(FILE *out, const struct qstar_graph *graph,
    const char *role, const char *tool, int required)
{
	char full[QSTAR_PATH_MAX], found[QSTAR_PATH_MAX];
	const char *mode, *status, *severity;
	int exists, executable, is_dir;

	if (!tool || !*tool) {
		fprintf(out,
		    "toolset-tool role=%s name=<none> required=%s mode=unset status=missing severity=%s path=<none> executable=no\n",
		    role, required ? "true" : "false", required ? "warning" : "info");
		return;
	}
	if (doctor_path_has_separator(tool)) {
		mode = doctor_path_is_absolute(tool) ? "absolute" : "package";
		if (doctor_path_state(graph, tool, full, sizeof(full), &exists,
		    &executable, &is_dir) < 0) {
			fprintf(out,
			    "toolset-tool role=%s name=%s required=%s mode=invalid status=invalid severity=%s path=<none> executable=no\n",
			    role, tool, required ? "true" : "false",
			    required ? "warning" : "info");
			return;
		}
		status = !exists ? "missing" : !executable ? "not-executable" : "found";
		severity = required && strcmp(status, "found") != 0 ? "warning" : "info";
		fprintf(out,
		    "toolset-tool role=%s name=%s required=%s mode=%s status=%s severity=%s path=%s executable=%s\n",
		    role, tool, required ? "true" : "false", mode, status, severity,
		    full, executable ? "yes" : "no");
		(void)is_dir;
		return;
	}
	if (qstar_external_tool_find_path_tool(tool, found, sizeof(found))) {
		fprintf(out,
		    "toolset-tool role=%s name=%s required=%s mode=path status=found severity=info path=%s executable=yes\n",
		    role, tool, required ? "true" : "false", found);
		return;
	}
	fprintf(out,
	    "toolset-tool role=%s name=%s required=%s mode=path status=missing severity=%s path=<none> executable=no\n",
	    role, tool, required ? "true" : "false", required ? "warning" : "info");
}

static void
dump_path_tool_doctor(FILE *out, const char *tool)
{
	char found[QSTAR_PATH_MAX];

	if (qstar_external_tool_find_path_tool(tool, found, sizeof(found)))
		fprintf(out, "external-tool name=%s mode=path status=found path=%s\n",
		    tool, found);
	else
		fprintf(out, "external-tool name=%s mode=path status=missing path=<none>\n",
		    tool);
}

/** toolset external tool discovery 상태를 doctor output에 출력한다. */
static void
dump_external_tool_doctor(FILE *out, const struct qstar_plan *plan)
{
	const struct qstar_graph *graph;
	size_t i, j, path_tool_count;
	int allow_absolute;

	graph = plan->graph;
	path_tool_count = 0;
	allow_absolute = 0;
	(void)plan;
	for (i = 0; i < graph->toolset_len; i++) {
		path_tool_count += graph->toolsets[i].path_tools.len;
		if (graph->toolsets[i].allow_absolute_tools &&
		    strcmp(graph->toolsets[i].allow_absolute_tools, "true") == 0)
			allow_absolute = 1;
	}
	fprintf(out, "external-tool-policy path_tools=%zu allow_absolute=%s source=toolsets\n",
	    path_tool_count, allow_absolute ? "true" : "false");
	for (i = 0; i < graph->toolset_len; i++)
		for (j = 0; j < graph->toolsets[i].path_tools.len; j++)
			dump_path_tool_doctor(out, graph->toolsets[i].path_tools.items[j]);
}

/** target closure를 훑어 doctor에서 필요한 toolchain role을 계산한다. */
static void
collect_doctor_tool_requirements(const struct qstar_plan *plan,
    struct doctor_tool_requirements *req)
{
	struct qstar_source_info source;
	const char *action;
	char role[128];
	int rc;
	size_t i, j;

	memset(req, 0, sizeof(*req));
	for (i = 0; i < plan->len; i++) {
		if (qstar_target_has_provider_final_action(plan->order[i])) {
			for (j = 0; j < plan->order[i]->provider_final.action.argv.len; j++) {
				rc = qstar_provider_tool_token_role(
				    plan->order[i]->provider_final.action.argv.items[j],
				    role, sizeof(role));
				if (rc == 1)
					(void)push_unique_tmp(&req->provider_roles, role);
			}
			continue;
		}
		for (j = 0; j < plan->order[i]->sources.len; j++) {
			if (qstar_target_source_classify(plan->order[i], j,
			    &source) < 0 || !source.compile_input)
				continue;
			if (strcmp(source.language, "cxx") == 0 ||
			    strcmp(source.language, "cxx-module") == 0)
				req->cxx = 1;
			else if (strcmp(source.language, "c") == 0 ||
			    strcmp(source.language, "asm") == 0 ||
			    strcmp(source.language, "asm-cpp") == 0)
				req->cc = 1;
			else if (source.toolset_role && *source.toolset_role)
				(void)push_unique_tmp(&req->provider_roles,
				    source.toolset_role);
		}
		action = qstar_target_final_action(plan->order[i]);
		if (strcmp(action, "archive") == 0)
			req->ar = 1;
		else if (strcmp(action, "link") == 0 ||
		    strcmp(action, "link-shared") == 0)
			req->linker = 1;
	}
}

/** depfile 생성 policy를 build context/compiler 관점에서 doctor에 출력한다. */
static void
dump_depfile_doctor(FILE *out, const struct doctor_tool_requirements *req,
    const struct qstar_resolved_toolchain *toolchain)
{
	const char *compiler, *platform, *behavior, *status;

	if (!req->cc && !req->cxx) {
		fputs("depfile-behavior compiler=<none> platform=<none> flags=-MMD,-MF status=not-required behavior=none\n",
		    out);
		return;
	}
	compiler = req->cc ? toolchain->cc : toolchain->cxx;
#ifdef __APPLE__
	platform = "darwin";
	if (strstr(compiler, "clang") || strcmp(compiler, "cc") == 0) {
		behavior = "appleclang-compatible";
		status = "supported";
	} else {
		behavior = "non-appleclang-tool";
		status = "assumed";
	}
#else
	platform = "other";
	behavior = "gcc-compatible";
	status = "assumed";
#endif
	fprintf(out,
	    "depfile-behavior compiler=%s platform=%s flags=-MMD,-MF status=%s behavior=%s\n",
	    compiler, platform, status, behavior);
}

static void
dump_cxx_strategy_doctor(FILE *out, const struct qstar_plan *plan)
{
	struct qstar_resolved_toolchain toolchain;
	const struct qstar_target *target;
	const char *family, *module_status;
	size_t i;

	for (i = 0; i < plan->len; i++) {
		target = plan->order[i];
		if ((!target->cxx_precompiled_header || !*target->cxx_precompiled_header) &&
		    !target->cxx_unity_enabled && !target->cxx_modules_enabled)
			continue;
		if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
			continue;
		family = qstar_cxx_compiler_family(&toolchain);
		module_status = strcmp(family, "clang") == 0 ? "supported" :
		    "unsupported";
		fprintf(out,
		    "cxx-strategy-capability target=%s compiler=%s family=%s pch=%s unity=%s modules=%s enabled_pch=%s enabled_unity=%s enabled_modules=%s\n",
		    target->label, toolchain.cxx, family,
		    strcmp(family, "clang") == 0 || strcmp(family, "apple-clang") == 0 ||
		    strcmp(family, "gcc") == 0 ? "supported" : "unsupported",
		    strcmp(family, "clang") == 0 || strcmp(family, "apple-clang") == 0 ||
		    strcmp(family, "gcc") == 0 ? "supported" : "unsupported",
		    module_status,
		    target->cxx_precompiled_header && *target->cxx_precompiled_header ?
		    "true" : "false", target->cxx_unity_enabled ? "true" : "false",
		    target->cxx_modules_enabled ? "true" : "false");
	}
}

static void
dump_provider_tool_doctor(FILE *out, const struct qstar_plan *plan,
    const struct doctor_tool_requirements *req)
{
	const struct qstar_string_list *argv;
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	const char *role;
	size_t i, j, k;
	int configured;

	for (i = 0; i < req->provider_roles.len; i++) {
		role = req->provider_roles.items[i];
		argv = NULL;
		configured = 0;
		for (j = 0; j < plan->len && !configured; j++) {
			if (qstar_resolve_toolchain(plan->graph, plan->order[j],
			    &toolchain) < 0)
				continue;
			for (k = 0; k < plan->order[j]->sources.len; k++) {
				if (qstar_target_source_classify(plan->order[j], k,
				    &source) < 0 || !source.compile_input ||
				    !source.toolset_role ||
				    strcmp(source.toolset_role, role) != 0)
					continue;
				argv = plan_resolved_tool_role_argv(plan->graph,
				    plan->order[j], &toolchain, role);
				configured = argv != NULL;
				break;
			}
		}
		fprintf(out,
		    "provider-tool role=%s required=true status=%s argv=",
		    role, configured ? "configured" : "missing");
		if (argv)
			dump_list(out, argv);
		else
			fputs("[]", out);
		fputc('\n', out);
	}
}

/** target 하나의 non-executing command-plan record를 출력한다. */
static int
dump_target_plan(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target,
    size_t order)
{
	char output[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	const char *action;
	const char *final_tool;
	size_t i;

	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  order %zu\n", order);
	fprintf(out, "  kind %s\n", target->kind);
	fprintf(out, "  cacheable %s\n", target->cacheable ? "true" : "false");
	fprintf(out, "  rule provider=%s final_action=%s output_group=%s\n",
	    qstar_target_rule_lookup(target->kind) ?
	    qstar_target_rule_lookup(target->kind)->provider : "generic",
	    qstar_target_final_action(target), qstar_target_output_group(target));
	fputs("  configs ", out);
	dump_list(out, &target->configs);
	fputc('\n', out);
	fputs("  deps ", out);
	dump_list(out, &target->deps);
	fputc('\n', out);
	fputs("  private_deps ", out);
	dump_list(out, &target->private_deps);
	fputc('\n', out);
	dump_dependency_usage(out, plan->graph, target, "  ");
	fputs("  visibility ", out);
	dump_list(out, &target->visibility);
	fputc('\n', out);
	dump_external_deps(out, plan, target);
	dump_generated_edges(out, plan, target);
	dump_consumed_genrules(out, plan, target);
	qstar_target_dump_source_discovery(target, out);
	fputs("  sources ", out);
	dump_list(out, &target->sources);
	fputc('\n', out);
	fputs("  objects ", out);
	dump_list(out, &target->objects);
	fputc('\n', out);
	fprintf(out, "  compile_context %s\n",
	    target->compile_context && *target->compile_context ?
	    target->compile_context : "own");
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
	fputs("  link.frameworks ", out);
	dump_list(out, &target->frameworks);
	fputc('\n', out);
	fputs("  link_options ", out);
	dump_list(out, &target->link_options);
	fputc('\n', out);
	fputs("  link_inputs ", out);
	dump_list(out, &target->link_inputs);
	fputc('\n', out);
	fputs("  cflags ", out);
	dump_list(out, &target->cflags);
	fputc('\n', out);
	fputs("  cxxflags ", out);
	dump_list(out, &target->cxxflags);
	fputc('\n', out);
	fprintf(out, "  cxx_standard %s\n", target->cxx_standard);
	fprintf(out, "  lang.cxx.precompiled_header %s\n",
	    target->cxx_precompiled_header && *target->cxx_precompiled_header ?
	    target->cxx_precompiled_header : "<off>");
	fprintf(out, "  lang.cxx.unity enabled=%s batch_size=%d batches=%zu\n",
	    target->cxx_unity_enabled ? "true" : "false",
	    target->cxx_unity_batch_size, qstar_cxx_unity_batch_count(target));
	fprintf(out, "  lang.cxx.modules enabled=%s interfaces=%s\n",
	    target->cxx_modules_enabled ? "true" : "false",
	    qstar_cxx_target_has_module_interfaces(target) ? "present" : "absent");
	if (strcmp(target->kind, "group") == 0) {
		dump_progress_action_exclusion(out, "  ", target->label, "group");
		fprintf(out, "  action group input=<deps> output=<none>\n");
		return 0;
	}
	if (strcmp(target->kind, "run_target") == 0) {
		fputs("  run.inputs ", out);
		dump_list(out, &target->run_inputs);
		fputc('\n', out);
		fputs("  run.command ", out);
		dump_list(out, &target->run_command);
		fputc('\n', out);
		fprintf(out, "  run.timeout_sec %d\n", target->run_timeout_sec);
		fprintf(out, "  run.expect.contains %s\n",
		    target->run_expect_contains && *target->run_expect_contains ? target->run_expect_contains : "<none>");
		fprintf(out, "  run.expect.file %s\n",
		    target->run_expect_file && *target->run_expect_file ?
		    target->run_expect_file : "<none>");
		snprintf(id, sizeof(id), "%s:run:0", target->label);
		if (qstar_action_description_run(target, description,
		    sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		fprintf(out, "  action run output=build/qstar/out/<run-stamp>\n");
		dump_action_key(out, plan->graph, target, "run", "<run-command>",
		    "build/qstar/out/<run-stamp>", "generic", 0);
		dump_command_skeleton(out, plan->graph, target, "run", "<run-command>",
		    "build/qstar/out/<run-stamp>", "generic", "cli", 0);
		dump_run_argv(out, plan->graph, target);
		return 0;
	}
	if (strcmp(target->kind, "interface") == 0 ||
	    strcmp(target->kind, "imported") == 0 ||
	    strcmp(target->kind, "tool") == 0) {
		qstar_dump_target_artifact_map_text(out, plan->graph, target, "  ");
		fprintf(out,
		    "  action metadata kind=%s input=<declared-artifacts,deps> output=<none>\n",
		    target->kind);
		return 0;
	}
	if (dump_resolved_toolchain(out, plan, target, &toolchain) < 0)
		return -1;
	dump_effective_compile_merge(out, plan->graph, target, &toolchain);
	for (i = 0; !objectlib_uses_consumer_context(target) &&
	    i < target->sources.len; i++) {
		if (qstar_target_provider_final_owns_source(target, i))
			continue;
		qstar_target_source_classify(target, i, &source);
		if (!qstar_source_requires_compile(&source)) {
			fprintf(out,
			    "  action link-input source=%s language=%s output=%s\n",
			    target->sources.items[i], source.language, target->sources.items[i]);
			dump_action_key(out, plan->graph, target, "link-input",
			    target->sources.items[i], target->sources.items[i],
			    source.language, i);
			dump_command_skeleton(out, plan->graph, target, "link-input",
			    target->sources.items[i], target->sources.items[i],
			    source.language, source.tool_role, i);
			continue;
		}
		if (qstar_graph_object_output_path(plan->graph, target, i, output,
		    sizeof(output)) < 0)
			return qstar_set_error(plan->graph, "qstar: object output path too long");
		snprintf(id, sizeof(id), "%s:compile:%zu", target->label, i);
		if (qstar_action_description_compile(target, &source, output,
		    description, sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		fprintf(out, "  action compile source=%s output=%s\n",
		    target->sources.items[i], output);
		dump_action_key(out, plan->graph, target, "compile", target->sources.items[i],
		    output, source.language, i);
		dump_command_skeleton(out, plan->graph, target, "compile",
		    target->sources.items[i], output, source.language, source.tool_role, i);
		dump_compile_argv(out, target, target, plan->graph, &toolchain, &source,
		    target->sources.items[i], output, i, i, NULL);
	}
	if (dump_consumer_objectlib_compile_plan(out, plan, target, &toolchain) < 0)
		return -1;
	if (strcmp(target->kind, "objectlib") == 0) {
		snprintf(id, sizeof(id), "%s:compile-objects:0", target->label);
		dump_progress_action_exclusion(out, "  ", target->label,
		    "objectlib-alias");
		dump_action_description(out, "  ", id, "Collecting objects");
		fprintf(out,
		    "  action compile-objects input=<target-objects> output=<none>\n");
		dump_action_key(out, plan->graph, target, "compile-objects",
		    "<target-objects>", "<none>", "objects", 0);
		dump_command_skeleton(out, plan->graph, target, "compile-objects",
		    "<target-objects>", "<none>", "objects", "object-collector", 0);
		return 0;
	}
	action = qstar_target_final_action(target);
	final_tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	if (qstar_target_has_provider_final_action(target))
		final_tool = target->provider_final.provider;
	if (qstar_graph_artifact_output_path(plan->graph, target, output, sizeof(output)) < 0)
		return qstar_set_error(plan->graph, "qstar: artifact output path too long");
	qstar_dump_target_artifact_map_text(out, plan->graph, target, "  ");
	snprintf(id, sizeof(id), "%s:%s:0", target->label, action);
	if (qstar_action_description_final(target, action, output, description,
	    sizeof(description)) < 0)
		snprintf(description, sizeof(description), "<too-long>");
	dump_action_description(out, "  ", id, description);
	fprintf(out, "  action %s output=%s\n", action, output);
	dump_action_key(out, plan->graph, target, action,
	    qstar_target_has_provider_final_action(target) ?
	    provider_final_input_summary(target) :
	    "<target-objects>", output,
	    "artifact", 0);
	dump_command_skeleton(out, plan->graph, target, action,
	    qstar_target_has_provider_final_action(target) ?
	    provider_final_input_summary(target) :
	    "<target-objects>",
	    output, "artifact", final_tool, 0);
	return dump_lowered_final_argv(out, plan->graph, target, &toolchain);
}

/** QStar target closure와 non-executing command plan을 deterministic text로 출력한다. */
int
qstar_graph_explain_plan(struct qstar_graph *graph, const char *label, FILE *out)
{
	const struct qstar_genrule *genrule;
	struct qstar_plan plan;
	size_t i;
	int rc;

	genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	if (genrule) {
		fputs("qstar command plan v1\n", out);
		fputs("build-plan-ir v1\n", out);
		fprintf(out, "root %s\n", label);
		dump_plan_inputs(out, graph);
		fprintf(out, "closure-order [%s]\n", label);
		dump_direct_genrule_plan(out, graph, genrule, "plan");
		return 0;
	}
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
	char inputs[QSTAR_PATH_MAX], outputs[QSTAR_PATH_MAX], identities[QSTAR_PATH_MAX];
	char resolved_tool[QSTAR_PATH_MAX], tool_mode[64], tool_error[QSTAR_PATH_MAX];
	char id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	size_t i;

	for (i = 0; i < plan->graph->genrule_len; i++) {
		genrule = &plan->graph->genrules[i];
		if (!genrule_referenced_by_target(genrule, target))
			continue;
		format_list_field(inputs, sizeof(inputs), &genrule->inputs);
		format_list_field(outputs, sizeof(outputs), &genrule->outputs);
		if (qstar_genrule_output_identity_list(genrule, identities,
		    sizeof(identities)) < 0)
			snprintf(identities, sizeof(identities), "[<too-long>]");
		if (genrule->config_header) {
			snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
			snprintf(tool_mode, sizeof(tool_mode), "builtin");
		} else if (qstar_resolve_command_tool_for_genrule(plan->graph, target,
		    genrule, genrule->tool, resolved_tool, sizeof(resolved_tool),
		    tool_mode, sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0) {
			snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
			snprintf(tool_mode, sizeof(tool_mode), "invalid");
		}
		snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
		if (qstar_action_description_generate(genrule, description,
		    sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		fprintf(out,
		    "dry_run_step id=%s:generate:0 owner=%s consumer=%s kind=generate "
		    "tool=%s toolset=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s args=",
		    genrule->label, genrule->label, target->label, genrule->tool,
		    genrule->toolset && *genrule->toolset ? genrule->toolset : "<none>",
		    tool_mode, resolved_tool, inputs, outputs, identities);
		dump_list(out, &genrule->args);
		fputs(" execute=no\n", out);
		dump_genrule_artifacts(out, genrule, "");
		dump_genrule_argv(out, plan->graph, target, genrule);
	}
}

/** target의 compile dry-run step을 source 순서대로 출력한다. */
static int
dump_dry_run_compiles(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	char output[QSTAR_PATH_MAX], input[QSTAR_PATH_MAX];
	char id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX], bmi[QSTAR_PATH_MAX];
	size_t i, batch;
	int leader;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	if (qstar_cxx_validate_strategies(plan->graph, target, &toolchain) < 0)
		return -1;
	if (target->cxx_precompiled_header && *target->cxx_precompiled_header) {
		if (qstar_cxx_pch_output_path(plan->graph, target, &toolchain, output,
		    sizeof(output)) < 0)
			return qstar_set_error(plan->graph, "qstar: C++ PCH output path too long");
		snprintf(id, sizeof(id), "%s:cxx-pch:0", target->label);
		fprintf(out,
		    "dry_run_step id=%s owner=%s kind=compile strategy=pch language=cxx tool=cxx-compiler toolset=%s input=%s output=%s execute=no\n",
		    id, target->label,
		    toolchain.toolset[0] ? toolchain.toolset : "<none>",
		    target->cxx_precompiled_header, output);
	}
	for (i = 0; !objectlib_uses_consumer_context(target) &&
	    i < target->sources.len; i++) {
		if (qstar_target_provider_final_owns_source(target, i))
			continue;
		batch = 0;
		leader = 0;
		if (qstar_cxx_unity_source_info(target, i, &batch, &leader) && !leader)
			continue;
		qstar_target_source_classify(target, i, &source);
		if (!qstar_source_requires_compile(&source)) {
			fprintf(out,
			    "dry_run_step id=%s:link-input:%zu owner=%s kind=link-input "
			    "language=%s tool=%s toolset=%s input=%s output=%s execute=no\n",
			    target->label, i, target->label, source.language,
			    source.tool_role,
			    toolchain.toolset[0] ? toolchain.toolset : "<none>",
			    target->sources.items[i],
			    target->sources.items[i]);
			continue;
		}
		snprintf(input, sizeof(input), "%s", target->sources.items[i]);
		if (leader) {
			if (qstar_cxx_unity_source_path(plan->graph, target, batch, input,
			    sizeof(input)) < 0 || qstar_cxx_unity_object_path(plan->graph,
			    target, batch, output, sizeof(output)) < 0)
				return qstar_set_error(plan->graph,
				    "qstar: C++ unity dry-run path too long");
			snprintf(id, sizeof(id), "%s:compile-unity-%zu:0", target->label,
			    batch);
		} else if (qstar_graph_object_output_path(plan->graph, target, i, output,
		    sizeof(output)) < 0) {
			return qstar_set_error(plan->graph, "qstar: object output path too long");
		} else if (qstar_cxx_source_is_module_interface(target, i)) {
			snprintf(id, sizeof(id), "%s:compile-module-interface-%zu:0",
			    target->label, i);
		} else if (qstar_cxx_source_is_implementation(target, i) &&
		    target->cxx_modules_enabled) {
			snprintf(id, sizeof(id), "%s:compile-module-implementation-%zu:0",
			    target->label, i);
		} else {
			snprintf(id, sizeof(id), "%s:compile:%zu", target->label, i);
		}
		if (qstar_action_description_compile(target, &source, output,
		    description, sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		if (leader || qstar_cxx_source_is_module_interface(target, i) ||
		    (target->cxx_modules_enabled &&
		    qstar_cxx_source_is_implementation(target, i))) {
			fprintf(out,
			    "dry_run_step id=%s owner=%s kind=compile strategy=%s language=%s "
			    "tool=%s toolset=%s input=%s output=%s execute=no\n",
			    id, target->label, leader ? "unity" :
			    qstar_cxx_source_is_module_interface(target, i) ?
			    "module-interface" : "module-implementation", source.language,
			    source.tool_role,
			    toolchain.toolset[0] ? toolchain.toolset : "<none>", input, output);
		} else {
			fprintf(out,
			    "dry_run_step id=%s owner=%s kind=compile language=%s tool=%s "
			    "toolset=%s input=%s output=%s execute=no\n",
			    id, target->label, source.language, source.tool_role,
			    toolchain.toolset[0] ? toolchain.toolset : "<none>", input, output);
		}
		if (qstar_cxx_source_is_module_interface(target, i) &&
		    qstar_cxx_module_output_path(plan->graph, target, i, bmi,
		    sizeof(bmi)) == 0)
			fprintf(out, "  module_output role=bmi path=%s\n", bmi);
		dump_compile_argv(out, target, target, plan->graph, &toolchain, &source,
		    input, output, i, i, NULL);
	}
	return 0;
}

/** consumer-context objectlib compile dry-run step을 출력한다. */
static int
dump_dry_run_consumer_objectlib_compiles(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target)
{
	const struct qstar_target *objectlib;
	struct qstar_source_info source;
	struct qstar_resolved_toolchain toolchain;
	char output[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX];
	char description[QSTAR_PATH_MAX];
	size_t i, j, action_index;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	for (i = 0; i < target->objects.len; i++) {
		objectlib = plan_find_target(plan->graph, target->objects.items[i]);
		if (!objectlib || !objectlib_uses_consumer_context(objectlib))
			continue;
		for (j = 0; j < objectlib->sources.len; j++) {
			qstar_target_source_classify(objectlib, j, &source);
			if (!qstar_source_requires_compile(&source))
				continue;
			if (qstar_target_provider_source_unit(objectlib, j))
				return qstar_set_error_origin(plan->graph,
				    objectlib->origin_file, objectlib->origin_line,
				    "sources", objectlib->label,
				    "qstar: provider source token '%s' cannot be used from compile_context = \"consumer\" objectlib yet; use raw provider source strings or compile_context = \"own\"",
				    objectlib->sources.items[j]);
			if (objectlib_source_object_input(plan->graph, target, objectlib, j,
			    output, sizeof(output)) < 0 ||
			    objectlib_source_depfile_output(plan->graph, target, objectlib, j,
			    depfile, sizeof(depfile)) < 0)
				return qstar_set_error(plan->graph,
				    "qstar: consumer objectlib path too long");
			action_index = objectlib_consumer_compile_index(plan->graph, target,
			    i, j);
			snprintf(id, sizeof(id), "%s:compile:%zu", target->label,
			    action_index);
			if (qstar_action_description_compile(objectlib, &source, output,
			    description, sizeof(description)) < 0)
				snprintf(description, sizeof(description), "<too-long>");
			dump_action_description(out, "  ", id, description);
			fprintf(out,
			    "dry_run_step id=%s:compile:%zu owner=%s source_owner=%s "
			    "kind=compile language=%s tool=%s toolset=%s input=%s "
			    "output=%s compile_context=consumer execute=no\n",
			    target->label, action_index, target->label, objectlib->label,
			    source.language, source.tool_role,
			    toolchain.toolset[0] ? toolchain.toolset : "<none>",
			    objectlib->sources.items[j], output);
			dump_compile_argv(out, target, objectlib, plan->graph, &toolchain,
			    &source, objectlib->sources.items[j], output, j,
			    action_index, depfile);
		}
	}
	return 0;
}

/** target의 최종 artifact dry-run step을 출력한다. */
static int
dump_dry_run_final(FILE *out, const struct qstar_plan *plan, const struct qstar_target *target)
{
	char output[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	struct qstar_resolved_toolchain toolchain;
	const char *action, *tool;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	if (strcmp(target->kind, "objectlib") == 0) {
		snprintf(id, sizeof(id), "%s:compile-objects:0", target->label);
		dump_progress_action_exclusion(out, "  ", target->label,
		    "objectlib-alias");
		dump_action_description(out, "  ", id, "Collecting objects");
		fprintf(out,
		    "dry_run_step id=%s:compile-objects:0 owner=%s kind=compile-objects "
		    "tool=object-collector toolset=%s input=<target-objects> "
		    "output=<none> execute=no\n",
		    target->label, target->label,
		    toolchain.toolset[0] ? toolchain.toolset : "<none>");
		return 0;
	}
	action = qstar_target_final_action(target);
	tool = strcmp(action, "archive") == 0 ? "archiver" :
	    strcmp(action, "compile-objects") == 0 ? "object-collector" : "linker";
	if (qstar_target_has_provider_final_action(target))
		tool = target->provider_final.provider;
	if (qstar_graph_artifact_output_path(plan->graph, target, output, sizeof(output)) < 0)
		return qstar_set_error(plan->graph, "qstar: artifact output path too long");
	qstar_dump_target_artifact_map_text(out, plan->graph, target, "");
	snprintf(id, sizeof(id), "%s:%s:0", target->label, action);
	if (qstar_action_description_final(target, action, output, description,
	    sizeof(description)) < 0)
		snprintf(description, sizeof(description), "<too-long>");
	dump_action_description(out, "  ", id, description);
	fprintf(out,
	    "dry_run_step id=%s:%s:0 owner=%s kind=%s tool=%s toolset=%s "
	    "input=%s output=%s execute=no\n",
	    target->label, action, target->label, action, tool,
	    toolchain.toolset[0] ? toolchain.toolset : "<none>",
	    qstar_target_has_provider_final_action(target) ?
	    provider_final_input_summary(target) :
	    "<target-objects>", output);
	return dump_lowered_final_argv(out, plan->graph, target, &toolchain);
}

/** QStar target closure를 실제 실행 없이 dry-run command stream으로 출력한다. */
int
qstar_graph_dry_run(struct qstar_graph *graph, const char *label, FILE *out)
{
	const struct qstar_genrule *genrule;
	struct qstar_plan plan;
	size_t i;
	int rc;

	genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	if (genrule) {
		fputs("qstar dry-run v1\n", out);
		fprintf(out, "root %s\n", label);
		dump_plan_inputs(out, graph);
		fprintf(out, "closure-order [%s]\n", label);
		fprintf(out, "dry_run_generated_action %s order=0 kind=custom_target\n",
		    label);
		dump_direct_genrule_plan(out, graph, genrule, "dry_run");
		return 0;
	}
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
		fprintf(out, "  rule provider=%s final_action=%s output_group=%s\n",
		    qstar_target_rule_lookup(plan.order[i]->kind) ?
		    qstar_target_rule_lookup(plan.order[i]->kind)->provider : "generic",
		    qstar_target_final_action(plan.order[i]),
		    qstar_target_output_group(plan.order[i]));
		fputs("  configs ", out);
		dump_list(out, &plan.order[i]->configs);
		fputc('\n', out);
		fputs("  objects ", out);
		dump_list(out, &plan.order[i]->objects);
		fputc('\n', out);
		fprintf(out, "  compile_context %s\n",
		    plan.order[i]->compile_context && *plan.order[i]->compile_context ?
		    plan.order[i]->compile_context : "own");
		dump_dependency_usage(out, graph, plan.order[i], "  ");
		{
		struct qstar_resolved_toolchain toolchain;
		if (strcmp(plan.order[i]->kind, "run_target") == 0) {
			char id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];

			snprintf(id, sizeof(id), "%s:run:0", plan.order[i]->label);
			if (qstar_action_description_run(plan.order[i], description,
			    sizeof(description)) < 0)
				snprintf(description, sizeof(description), "<too-long>");
			dump_action_description(out, "  ", id, description);
			fprintf(out,
			    "  dry_run_step id=%s:run:0 owner=%s kind=run tool=cli "
			    "input=<deps,run.inputs> output=build/qstar/out/<run-stamp> execute=no\n",
			    plan.order[i]->label, plan.order[i]->label);
			dump_run_argv(out, plan.graph, plan.order[i]);
			dump_dry_run_genrules(out, &plan, plan.order[i]);
			continue;
		}
		if (strcmp(plan.order[i]->kind, "group") == 0) {
			dump_progress_action_exclusion(out, "  ", plan.order[i]->label,
			    "group");
			fprintf(out,
			    "  dry_run_step id=%s:group:0 owner=%s kind=group tool=none input=<deps> output=<none> execute=no\n",
			    plan.order[i]->label, plan.order[i]->label);
			dump_dry_run_genrules(out, &plan, plan.order[i]);
			continue;
		}
		if (strcmp(plan.order[i]->kind, "interface") == 0 ||
		    strcmp(plan.order[i]->kind, "imported") == 0 ||
		    strcmp(plan.order[i]->kind, "tool") == 0) {
			qstar_dump_target_artifact_map_text(out, graph, plan.order[i], "  ");
			fprintf(out,
			    "  dry_run_step id=%s:%s:0 owner=%s kind=metadata tool=none input=<declared-artifacts,deps> output=<none> execute=no\n",
			    plan.order[i]->label, plan.order[i]->kind,
			    plan.order[i]->label);
			dump_dry_run_genrules(out, &plan, plan.order[i]);
			continue;
		}
		if (dump_resolved_toolchain(out, &plan, plan.order[i], &toolchain) < 0) {
			free_plan(&plan);
			return -1;
		}
		dump_effective_compile_merge(out, graph, plan.order[i], &toolchain);
		}
		dump_dry_run_genrules(out, &plan, plan.order[i]);
		if (dump_dry_run_compiles(out, &plan, plan.order[i]) < 0 ||
		    dump_dry_run_consumer_objectlib_compiles(out, &plan,
		    plan.order[i]) < 0 ||
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
	const struct qstar_genrule *genrule;
	const struct qstar_stage *stage;
	struct qstar_plan plan;
	int rc;

	genrule = label && *label ? qstar_graph_find_genrule(graph, label) : NULL;
	if (genrule) {
		fputs("qstar check v1\n", out);
		fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
		fprintf(out, "root %s\n", label);
		dump_plan_inputs(out, graph);
		fprintf(out, "closure-order [%s]\n", label);
		fputs("target-count 0\n", out);
		fputs("generated-action-count 1\n", out);
		fputs("stage-count 0\n", out);
		fputs("status ok\n", out);
		return 0;
	}
	stage = label && *label ? qstar_graph_find_stage(graph, label) : NULL;
	if (stage) {
		fputs("qstar check v1\n", out);
		fprintf(out, "package-root %s\n", graph->package_root ? graph->package_root : ".");
		fprintf(out, "root %s\n", label);
		dump_plan_inputs(out, graph);
		fprintf(out, "closure-order [%s]\n", label);
		fputs("target-count 0\n", out);
		fputs("generated-action-count 0\n", out);
		fputs("stage-count 1\n", out);
		fputs("status ok\n", out);
		return 0;
	}
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
	fprintf(out, "stage-count %zu\n", graph->stage_len);
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
	struct doctor_tool_requirements tool_req;
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
	fprintf(out, "stage-count %zu\n", graph->stage_len);
	memset(&toolchain, 0, sizeof(toolchain));
	memset(&tool_req, 0, sizeof(tool_req));
	if (plan.len > 0 && qstar_resolve_toolchain(graph, plan.order[0], &toolchain) == 0) {
		collect_doctor_tool_requirements(&plan, &tool_req);
		fprintf(out,
		    "toolset-sanity toolset=%s cc=%s cxx=%s ar=%s linker=%s "
		    "platform=%s link_style=%s response_files=%s response_style=%s "
		    "status=resolved\n",
		    toolchain.toolset[0] ? toolchain.toolset : "<none>",
		    toolchain.cc, toolchain.cxx, toolchain.ar,
		    toolchain.linker, toolchain.platform, toolchain.link_style,
		    toolchain.response_files ? "on" : "off", toolchain.response_style);
		fprintf(out,
		    "response-policy source=toolset effective_files=%s effective_style=%s\n",
		    toolchain.response_files ? "on" : "off", toolchain.response_style);
		dump_toolset_tool_doctor(out, graph, "cc", toolchain.cc, tool_req.cc);
		dump_toolset_tool_doctor(out, graph, "cxx", toolchain.cxx,
		    tool_req.cxx);
		dump_toolset_tool_doctor(out, graph, "ar", toolchain.ar, tool_req.ar);
		dump_toolset_tool_doctor(out, graph, "linker", toolchain.linker,
		    tool_req.linker);
		dump_provider_tool_doctor(out, &plan, &tool_req);
		dump_depfile_doctor(out, &tool_req, &toolchain);
	}
	dump_cxx_strategy_doctor(out, &plan);
	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    qstar_graph_build_dir(graph), state_dir, sizeof(state_dir)) == 0) {
		if (doctor_mkdir_p(state_dir) == 0 && access(state_dir, W_OK) == 0)
			fprintf(out, "writable-build-dir yes path=%s\n",
			    qstar_graph_build_dir(graph));
		else
			fprintf(out, "writable-build-dir no path=%s\n",
			    qstar_graph_build_dir(graph));
	}
	fprintf(out, "platform %s\n", qstar_graph_platform(graph));
	dump_external_tool_doctor(out, &plan);
	fputs("diagnostics ok\n", out);
	fputs("file-inputs ok\n", out);
	fputs("status ok\n", out);
	qstar_string_list_free(&tool_req.provider_roles);
	free_plan(&plan);
	return 0;
}
