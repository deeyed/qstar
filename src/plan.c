#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QSTAR_PLAN_HASH_INIT 1469598103934665603ULL
#define QSTAR_PLAN_HASH_PRIME 1099511628211ULL
#define QSTAR_RSP_SKELETON_THRESHOLD 240U

struct qstar_plan {
	struct qstar_graph *graph;
	const struct qstar_target **order;
	unsigned char *state;
	size_t len;
	size_t cap;
};

struct qstar_argv_dump {
	const char *id;
	unsigned long long digest;
	unsigned long long response_digest;
	size_t seen;
	size_t text_len;
	int response_files;
	char response_style[32];
};

struct doctor_tool_requirements {
	int cc;
	int cxx;
	int ar;
	int linker;
	struct qstar_string_list provider_roles;
};

/** build context 문자열이 없을 때 explain dump에 쓸 기본값을 반환한다. */
static const char *
context_or_default(const char *s, const char *fallback)
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

/** build context input과 package alias map을 command-plan header에 출력한다. */
static void
dump_plan_inputs(FILE *out, const struct qstar_graph *graph)
{
	size_t i;

	fprintf(out, "build_context name=%s target=%s platform=%s toolchain=%s stdlib=%s\n",
	    context_or_default(graph->build_context.name, "default"),
	    context_or_default(graph->build_context.target, "host"),
	    qstar_graph_platform(graph),
	    context_or_default(graph->build_context.toolchain, "default"),
	    context_or_default(graph->build_context.stdlib_policy, "default"));
	fprintf(out, "build_context_tools cc=%s cxx=%s ar=%s linker=%s sysroot=%s resource_dir=%s\n",
	    context_or_default(graph->build_context.cc, "<default>"),
	    context_or_default(graph->build_context.cxx, "<default>"),
	    context_or_default(graph->build_context.ar, "<default>"),
	    context_or_default(graph->build_context.linker, "<default>"),
	    context_or_default(graph->build_context.sysroot, "<none>"),
	    context_or_default(graph->build_context.resource_dir, "<none>"));
	fprintf(out, "build_context_response response_files=%s response_style=%s\n",
	    context_or_default(graph->build_context.response_files, "auto"),
	    context_or_default(graph->build_context.response_style, "auto"));
	fputs("build_context_link link_options=", out);
	dump_list(out, &graph->build_context.link_options);
	fputc('\n', out);
	fputs("build_context_compile compile_options=", out);
	dump_list(out, &graph->build_context.compile_options);
	fputs(" include_dirs=", out);
	dump_list(out, &graph->build_context.include_dirs);
	fputc('\n', out);
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
	fprintf(out, "external_tool_policy allow_absolute=%s path_tools=",
	    context_or_default(graph->build_context.allow_absolute_tools, "false"));
	dump_list(out, &graph->build_context.path_tools);
	fputs(" tool_overrides=", out);
	dump_list(out, &graph->build_context.tool_overrides);
	fputc('\n', out);
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
	    "language=%s build_context=%s target=%s toolchain=%s stdlib=%s deps=",
	    target->label, kind, index, kind, target->label, input, output,
	    language,
	    context_or_default(graph->build_context.name, "default"),
	    context_or_default(graph->build_context.target, "host"),
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
	    context_or_default(graph->build_context.target, "host"), target->stdlib_policy,
	    input, output);
}

/** command digest용 FNV-1a hash에 문자열을 섞는다. */
static void
digest_str(unsigned long long *h, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	while (*p) {
		*h ^= (unsigned long long)*p++;
		*h *= QSTAR_PLAN_HASH_PRIME;
	}
	*h ^= 0xffU;
	*h *= QSTAR_PLAN_HASH_PRIME;
}

/** response file skeleton digest에 argv tail 값을 섞는다. */
static void
digest_response_value(struct qstar_argv_dump *dump, const char *value)
{
	if (dump->seen == 0)
		return;
	digest_str(&dump->response_digest, value);
	digest_str(&dump->response_digest, "\n");
}

/** 실제 argv plan의 list header를 출력한다. */
static void
begin_argv(FILE *out, struct qstar_argv_dump *dump, const char *id, size_t argc,
    const struct qstar_resolved_toolchain *toolchain)
{
	memset(dump, 0, sizeof(*dump));
	dump->id = id;
	dump->digest = QSTAR_PLAN_HASH_INIT;
	dump->response_digest = QSTAR_PLAN_HASH_INIT;
	dump->response_files = toolchain ? toolchain->response_files : 0;
	snprintf(dump->response_style, sizeof(dump->response_style), "%s",
	    toolchain ? toolchain->response_style : "none");
	digest_str(&dump->digest, id);
	digest_str(&dump->response_digest, id);
	digest_str(&dump->response_digest, dump->response_style);
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
	digest_str(&dump->digest, value);
	digest_response_value(dump, value);
	dump->text_len += strlen(value) + 1;
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
end_argv(FILE *out, const struct qstar_argv_dump *dump)
{
	char rsp[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX];

	qstar_mangle_label(dump->id, name, sizeof(name));
	snprintf(rsp, sizeof(rsp), "build/qstar/rsp/%s.rsp", name);
	fprintf(out, "] digest=%016llx response=%s",
	    dump->digest,
	    dump->text_len > QSTAR_RSP_SKELETON_THRESHOLD ?
	    (dump->response_files ? "skeleton" : "unsupported") : "none");
	if (dump->text_len > QSTAR_RSP_SKELETON_THRESHOLD && dump->response_files)
		fprintf(out, " response_file=%s response_style=%s response_digest=%016llx",
		    rsp, dump->response_style, dump->response_digest);
	else if (dump->text_len > QSTAR_RSP_SKELETON_THRESHOLD)
		fputs(" response_capability=off", out);
	fputc('\n', out);
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

/** target compile option이 build context/config/local merge 뒤 어떻게 보이는지 설명한다. */
static void
dump_effective_compile_merge(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_resolved_toolchain *toolchain)
{
	fprintf(out,
	    "  effective_compile_merge owner=%s build_context=%s target=%s response_files=%s response_style=%s order=build_context,target build_context_compile_options=",
	    target->label, context_or_default(graph->build_context.name, "default"),
	    context_or_default(graph->build_context.target, "host"),
	    toolchain->response_files ? "on" : "off", toolchain->response_style);
	dump_list(out, &graph->build_context.compile_options);
	fputs(" build_context_include_dirs=", out);
	dump_list(out, &graph->build_context.include_dirs);
	fputs(" target_c_compile_options=", out);
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

/** artifact path에서 basename component를 반환한다. */
static const char *
artifact_basename(const char *path)
{
	const char *slash;

	slash = strrchr(path ? path : "", '/');
	return slash ? slash + 1 : path;
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
    const struct qstar_graph *graph,
    const struct qstar_resolved_toolchain *toolchain, const struct qstar_source_info *source,
    const char *input, const char *output, size_t index)
{
	const struct qstar_provider_source_unit *provider_unit;
	char id[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], std_arg[128];
	const char *role;
	const char *tool;
	struct qstar_string_list includes;
	struct qstar_argv_dump dump;
	size_t argc, i;
	int is_asm, is_cxx, provider_source, wants_depfile;

	memset(&includes, 0, sizeof(includes));
	collect_compile_include_dirs(graph, target, &includes);
	snprintf(id, sizeof(id), "%s:compile:%zu", target->label, index);
	qstar_graph_depfile_output_path(graph, target, index, depfile, sizeof(depfile));
	is_asm = qstar_source_is_asm(source);
	is_cxx = strcmp(source->provider, "cxx") == 0;
	provider_unit = qstar_target_provider_source_unit(target, index);
	provider_source = provider_unit != NULL;
	wants_depfile = !provider_source &&
	    (strcmp(source->provider, "c") == 0 || is_cxx ||
	    qstar_source_uses_asm_preprocessor(target, source));
	if (provider_unit) {
		argc = plan_provider_template_argc(graph, target, toolchain,
		    &provider_unit->action.argv);
		begin_argv(out, &dump, id, argc, toolchain);
		plan_argv_provider_template(out, &dump, graph, target, toolchain,
		    &provider_unit->action.argv);
		end_argv(out, &dump);
		qstar_string_list_free(&includes);
		return;
	}
	role = qstar_source_toolset_role(source);
	tool = qstar_resolved_toolchain_provider_tool(toolchain, source->provider,
	    source->provider_role);
	if (!tool)
		tool = toolchain->cc;
	argc = 5 + plan_tool_role_argc(graph, target, toolchain, role) - 1 +
	    (provider_source ? 0 : graph->build_context.compile_options.len) +
	    (provider_source ? 0 : graph->build_context.include_dirs.len * 2) +
	    (provider_source ? 0 :
	    (is_asm ? target->asm_include_dirs.len * 2 : includes.len * 2)) +
	    (provider_source || is_asm ? 0 : target->system_include_dirs.len * 2) +
	    (wants_depfile ? 3 : 0) +
	    (!provider_source && is_asm ? 2 : 0) +
	    (strcmp(target->kind, "sharedlib") == 0 &&
	    qstar_platform_supports_sharedlib(toolchain->platform) &&
	    !qstar_platform_is_windows(toolchain->platform) && !provider_source &&
	    !is_asm ? 1 : 0) +
	    (!provider_source && is_cxx && target->cxx_standard[0] ? 1 : 0) +
	    (provider_source ? 0 : is_asm ? target->asm_compile_options.len :
	    is_cxx ? target->cxxflags.len : target->cflags.len);
	snprintf(std_arg, sizeof(std_arg), "-std=%s", target->cxx_standard);
	begin_argv(out, &dump, id, argc, toolchain);
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
	for (i = 0; !provider_source && !is_asm && !is_cxx && i < target->cflags.len; i++)
		argv_item(out, &dump, target->cflags.items[i]);
	for (i = 0; !provider_source && is_cxx && i < target->cxxflags.len; i++)
		argv_item(out, &dump, target->cxxflags.items[i]);
	for (i = 0; !provider_source && is_asm && i < target->asm_compile_options.len; i++)
		argv_item(out, &dump, target->asm_compile_options.items[i]);
	for (i = 0; !provider_source && i < graph->build_context.compile_options.len; i++)
		argv_item(out, &dump, graph->build_context.compile_options.items[i]);
	for (i = 0; !provider_source && i < graph->build_context.include_dirs.len; i++) {
		argv_item(out, &dump, "-I");
		argv_item(out, &dump, graph->build_context.include_dirs.items[i]);
	}
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
}

/** MSVC target에서 clang-cl style driver가 link flag boundary를 필요로 하는지 본다. */
static int
toolchain_needs_msvc_link_boundary(const struct qstar_resolved_toolchain *toolchain,
    const struct qstar_target *target)
{
	const char *tool;

	if (strcmp(toolchain->link_style, "msvc") != 0)
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

/** target/build context link_options가 argv에 추가할 argument 수를 계산한다. */
static size_t
link_policy_arg_count(const struct qstar_graph *graph, const struct qstar_target *target)
{
	return graph->build_context.link_options.len + target->link_options.len;
}

/** target/build context link_options를 deterministic command argv dump에 추가한다. */
static void
dump_link_policy_argv(FILE *out, struct qstar_argv_dump *dump,
    const struct qstar_graph *graph, const struct qstar_target *target)
{
	size_t i;

	for (i = 0; i < graph->build_context.link_options.len; i++)
		argv_item(out, dump, graph->build_context.link_options.items[i]);
	for (i = 0; i < target->link_options.len; i++)
		argv_item(out, dump, target->link_options.items[i]);
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
	char id[QSTAR_PATH_MAX], resolved_tool[QSTAR_PATH_MAX], tool_mode[64];
	char resolved_arg[QSTAR_PATH_MAX];
	char tool_error[QSTAR_PATH_MAX];

	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	begin_argv(out, &dump, id, 1 + genrule->args.len, NULL);
	if (genrule->config_header) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "builtin");
		argv_item(out, &dump, resolved_tool);
	} else if (qstar_resolve_command_tool_for_target(graph, target, genrule->tool,
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
	char id[QSTAR_PATH_MAX], resolved[QSTAR_PATH_MAX];
	size_t i;

	snprintf(id, sizeof(id), "%s:run:0", target->label);
	begin_argv(out, &dump, id, target->run_command.len, NULL);
	for (i = 0; i < target->run_command.len; i++)
		argv_item(out, &dump, plan_resolve_run_arg(graph,
		    target->run_command.items[i], resolved, sizeof(resolved)));
	end_argv(out, &dump);
}

/** target final action의 실제 argv plan을 출력한다. */
static void
dump_final_argv(FILE *out, const struct qstar_target *target,
    const struct qstar_graph *graph, const struct qstar_resolved_toolchain *toolchain,
    const char *action, const char *output)
{
	char id[QSTAR_PATH_MAX];
	char buf[QSTAR_PATH_MAX];
	char import_lib[QSTAR_PATH_MAX], install_name[QSTAR_PATH_MAX];
	char soname[QSTAR_PATH_MAX];
	size_t argc, i;
	struct qstar_argv_dump dump;
	int darwin;
	int msvc;
	int msvc_out;
	int windows;

	snprintf(id, sizeof(id), "%s:%s:0", target->label, action);
	msvc = strcmp(toolchain->link_style, "msvc") == 0;
	darwin = qstar_platform_is_darwin(toolchain->platform);
	windows = qstar_platform_is_windows(toolchain->platform);
	msvc_out = strcmp(action, "archive") != 0 &&
	    toolchain_uses_msvc_out_arg(toolchain, target);
	argc = strcmp(action, "archive") == 0 ?
	    3 + plan_tool_role_argc(graph, target, toolchain, "archive") :
	    strcmp(action, "link-shared") == 0 ?
	    (qstar_platform_is_darwin(toolchain->platform) ? 6 : 5) +
	    plan_tool_role_argc(graph, target, toolchain, "link") :
	    3 + plan_tool_role_argc(graph, target, toolchain, "link");
	if (strcmp(action, "archive") != 0)
		argc += target->lib_dirs.len +
		    graph->build_context.lib_dirs.len + target->libs.len +
		    (darwin ? target->frameworks.len * 2 : 0) +
		    link_policy_arg_count(graph, target) +
		    (toolchain_needs_msvc_link_boundary(toolchain, target) ? 1 : 0);
	if (msvc_out)
		argc--;
	begin_argv(out, &dump, id, argc, toolchain);
	if (strcmp(action, "archive") == 0) {
		plan_argv_tool_role(out, &dump, graph, target, toolchain, "archive",
		    toolchain->ar);
		argv_item(out, &dump, "rcs");
		argv_item(out, &dump, output);
		argv_item(out, &dump, "<target-objects>");
	} else {
		plan_argv_tool_role(out, &dump, graph, target, toolchain, "link",
		    qstar_target_has_compile_provider(target, "cxx") ? toolchain->cxx :
		    toolchain->linker);
		if (msvc_out) {
			snprintf(buf, sizeof(buf), "/out:%s", output);
			argv_item(out, &dump, buf);
		} else {
			argv_item(out, &dump, "-o");
			argv_item(out, &dump, output);
		}
		if (strcmp(action, "link-shared") == 0) {
			if (windows) {
				if (qstar_graph_target_artifact_path(
				    (struct qstar_graph *)graph, target,
				    "import_lib", import_lib, sizeof(import_lib)) == 0) {
					if (msvc) {
						argv_item(out, &dump, "/DLL");
						snprintf(buf, sizeof(buf), "/IMPLIB:%s",
						    import_lib);
						argv_item(out, &dump, buf);
					} else {
						argv_item(out, &dump, "-shared");
						snprintf(buf, sizeof(buf),
						    "-Wl,--out-implib,%s", import_lib);
						argv_item(out, &dump, buf);
					}
				}
			} else if (darwin) {
				snprintf(install_name, sizeof(install_name), "@rpath/%s",
				    artifact_basename(output));
				argv_item(out, &dump, "-dynamiclib");
				argv_item(out, &dump, "-install_name");
				argv_item(out, &dump, install_name);
			} else {
				snprintf(soname, sizeof(soname), "-Wl,-soname,%s",
				    artifact_basename(output));
				argv_item(out, &dump, "-shared");
				argv_item(out, &dump, soname);
			}
		}
		dump_link_policy_argv(out, &dump, graph, target);
		argv_item(out, &dump, "<target-objects>");
		if (toolchain_needs_msvc_link_boundary(toolchain, target))
			argv_item(out, &dump, "/link");
		for (i = 0; i < graph->build_context.lib_dirs.len; i++) {
			if (msvc) {
				snprintf(buf, sizeof(buf), "/LIBPATH:%s",
				    graph->build_context.lib_dirs.items[i]);
				argv_item(out, &dump, buf);
			} else {
				snprintf(buf, sizeof(buf), "-L%s",
				    graph->build_context.lib_dirs.items[i]);
				argv_item(out, &dump, buf);
			}
		}
		for (i = 0; i < target->lib_dirs.len; i++) {
			if (msvc) {
				snprintf(buf, sizeof(buf), "/LIBPATH:%s", target->lib_dirs.items[i]);
				argv_item(out, &dump, buf);
			} else {
				snprintf(buf, sizeof(buf), "-L%s", target->lib_dirs.items[i]);
				argv_item(out, &dump, buf);
			}
		}
		for (i = 0; i < target->libs.len; i++) {
			if (msvc) {
				snprintf(buf, sizeof(buf), "%s.lib", target->libs.items[i]);
				argv_item(out, &dump, buf);
			} else {
				snprintf(buf, sizeof(buf), "-l%s", target->libs.items[i]);
				argv_item(out, &dump, buf);
			}
		}
		if (darwin) {
			for (i = 0; i < target->frameworks.len; i++) {
				argv_item(out, &dump, "-framework");
				argv_item(out, &dump, target->frameworks.items[i]);
			}
		}
	}
	end_argv(out, &dump);
}

static void
dump_provider_final_argv(FILE *out, const struct qstar_target *target,
    const struct qstar_graph *graph, const struct qstar_resolved_toolchain *toolchain,
    const char *action)
{
	struct qstar_argv_dump dump;
	char id[QSTAR_PATH_MAX];
	size_t argc;

	snprintf(id, sizeof(id), "%s:%s:0", target->label, action);
	argc = plan_provider_template_argc(graph, target, toolchain,
	    &target->provider_final.action.argv);
	begin_argv(out, &dump, id, argc, toolchain);
	plan_argv_provider_template(out, &dump, graph, target, toolchain,
	    &target->provider_final.action.argv);
	end_argv(out, &dump);
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
		} else if (qstar_resolve_command_tool_for_target(plan->graph, target,
		    genrule->tool, resolved_tool, sizeof(resolved_tool), tool_mode,
		    sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0) {
			snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
			snprintf(tool_mode, sizeof(tool_mode), "invalid");
		}
		snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
		if (qstar_action_description_generate(genrule, description,
		    sizeof(description)) < 0)
			snprintf(description, sizeof(description), "<too-long>");
		dump_action_description(out, "  ", id, description);
		fprintf(out,
		    "  generated_action id=%s tool=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s args=",
		    genrule->label, genrule->tool, tool_mode, resolved_tool, inputs, outputs,
		    identities);
		dump_list(out, &genrule->args);
		fputs(" execute=no\n", out);
		dump_genrule_input_edges(out, plan->graph, genrule, "  ");
		dump_genrule_artifacts(out, genrule, "  ");
		fprintf(out,
		    "  action_key id=%s:generate:0 kind=generate owner=%s consumer=%s "
		    "input=%s output=%s language=generated build_context=%s target=%s "
		    "toolchain=%s stdlib=%s deps=[] packages=",
		    genrule->label, genrule->label, target->label, inputs, identities,
		    context_or_default(plan->graph->build_context.name, "default"),
		    context_or_default(plan->graph->build_context.target, "host"),
		    target->toolchain, target->stdlib_policy);
		dump_package_aliases(out, plan->graph);
		fputc('\n', out);
		fprintf(out,
		    "  command_skeleton id=%s:generate:0 phase=generate language=generated "
		    "tool=%s resolved_tool=%s tool_mode=%s toolchain=%s target=%s stdlib=%s input=%s output=%s "
		    "consumer=%s execute=no\n",
		    genrule->label, genrule->tool, resolved_tool, tool_mode,
		    target->toolchain, context_or_default(plan->graph->build_context.target, "host"),
		    target->stdlib_policy, inputs, identities, target->label);
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
	} else if (qstar_external_tool_resolve_command_tool(graph, genrule->tool,
	    resolved_tool, sizeof(resolved_tool), tool_mode, sizeof(tool_mode),
	    tool_error, sizeof(tool_error)) < 0) {
		snprintf(resolved_tool, sizeof(resolved_tool), "%s", genrule->tool);
		snprintf(tool_mode, sizeof(tool_mode), "invalid");
	}
	fprintf(out,
	    "%s_generated_action %s tool=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s\n",
	    mode, genrule->label, genrule->tool, tool_mode, resolved_tool, inputs, outputs,
	    identities);
	snprintf(id, sizeof(id), "%s:generate:0", genrule->label);
	if (qstar_action_description_generate(genrule, description,
	    sizeof(description)) < 0)
		snprintf(description, sizeof(description), "<too-long>");
	dump_action_description(out, "  ", id, description);
	dump_genrule_input_edges(out, graph, genrule, "  ");
	dump_genrule_artifacts(out, genrule, "  ");
	fprintf(out,
	    "  action_key id=%s:generate:0 kind=generate owner=%s consumer=<direct> "
	    "input=%s output=%s language=generated build_context=%s target=%s toolchain=custom stdlib=none deps=[] packages=",
	    genrule->label, genrule->label, inputs, identities,
	    context_or_default(graph->build_context.name, "default"),
	    context_or_default(graph->build_context.target, "host"));
	dump_package_aliases(out, graph);
	fputc('\n', out);
	fprintf(out,
	    "  command_skeleton id=%s:generate:0 phase=generate language=generated "
	    "tool=%s resolved_tool=%s tool_mode=%s toolchain=custom target=%s stdlib=none input=%s output=%s consumer=<direct> execute=no\n",
	    genrule->label, genrule->tool, resolved_tool, tool_mode,
	    context_or_default(graph->build_context.target, "host"), inputs, identities);
	dump_genrule_argv(out, graph, NULL, genrule);
}

/** target-local toolchain/build context resolver skeleton을 출력한다. */
static int
dump_resolved_toolchain(FILE *out, const struct qstar_plan *plan,
    const struct qstar_target *target, struct qstar_resolved_toolchain *resolved)
{
	if (qstar_resolve_toolchain(plan->graph, target, resolved) < 0)
		return -1;
	fprintf(out,
	    "  resolved_toolchain owner=%s toolchain=%s build_context=%s target=%s "
	    "platform=%s link_style=%s stdlib=%s resolver=%s toolset=%s cc=%s cxx=%s asm=%s ar=%s "
	    "linker=%s sysroot=%s resource_dir=%s response_files=%s response_style=%s\n",
	    target->label, resolved->name,
	    context_or_default(plan->graph->build_context.name, "default"),
	    resolved->target, resolved->platform, resolved->link_style,
	    resolved->stdlib_policy, resolved->resolver,
	    resolved->toolset[0] ? resolved->toolset : "<none>",
	    resolved->cc, resolved->cxx, resolved->asm_, resolved->ar,
	    resolved->linker,
	    resolved->sysroot[0] ? resolved->sysroot : "<none>",
	    resolved->resource_dir[0] ? resolved->resource_dir : "<none>",
	    resolved->response_files ? "on" : "off", resolved->response_style);
	return 0;
}

/** tool override entry의 NAME=VALUE를 doctor 출력용으로 분리한다. */
static int
split_tool_override_for_doctor(const char *entry, char *name, size_t name_len,
    char *value, size_t value_len)
{
	const char *eq;
	size_t n;

	eq = entry ? strchr(entry, '=') : NULL;
	if (!eq || eq == entry || eq[1] == '\0')
		return 0;
	n = (size_t)(eq - entry);
	if (n + 1 > name_len || strlen(eq + 1) + 1 > value_len)
		return 0;
	memcpy(name, entry, n);
	name[n] = '\0';
	snprintf(value, value_len, "%s", eq + 1);
	return 1;
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

/** doctor path 상태를 stable text로 출력한다. */
static void
dump_build_context_path_doctor(FILE *out, const struct qstar_graph *graph, const char *name,
    const char *path, int want_dir)
{
	char full[QSTAR_PATH_MAX];
	const char *mode, *status, *type;
	int exists, executable, is_dir;

	if (!path || !*path) {
		fprintf(out,
		    "build-context-path name=%s path=<none> mode=unset status=not-set type=none\n",
		    name);
		return;
	}
	mode = doctor_path_is_absolute(path) ? "absolute" : "package";
	if (doctor_path_state(graph, path, full, sizeof(full), &exists, &executable,
	    &is_dir) < 0) {
		fprintf(out,
		    "build-context-path name=%s path=%s mode=invalid status=invalid type=unknown\n",
		    name, path);
		return;
	}
	type = exists ? is_dir ? "directory" : "file" : "missing";
	status = !exists ? "missing" : want_dir && !is_dir ? "not-directory" : "found";
	fprintf(out, "build-context-path name=%s path=%s mode=%s status=%s type=%s full=%s\n",
	    name, path, mode, status, type, full);
}

/** doctor가 toolchain role 하나의 발견 상태를 출력한다. */
static void
dump_toolchain_tool_doctor(FILE *out, const struct qstar_graph *graph,
    const char *role, const char *tool, int required)
{
	char full[QSTAR_PATH_MAX], found[QSTAR_PATH_MAX];
	const char *mode, *status, *severity;
	int exists, executable, is_dir;

	if (!tool || !*tool) {
		fprintf(out,
		    "toolchain-tool role=%s name=<none> required=%s mode=unset status=missing severity=%s path=<none> executable=no\n",
		    role, required ? "true" : "false", required ? "warning" : "info");
		return;
	}
	if (doctor_path_has_separator(tool)) {
		mode = doctor_path_is_absolute(tool) ? "absolute" : "package";
		if (doctor_path_state(graph, tool, full, sizeof(full), &exists,
		    &executable, &is_dir) < 0) {
			fprintf(out,
			    "toolchain-tool role=%s name=%s required=%s mode=invalid status=invalid severity=%s path=<none> executable=no\n",
			    role, tool, required ? "true" : "false",
			    required ? "warning" : "info");
			return;
		}
		status = !exists ? "missing" : !executable ? "not-executable" : "found";
		severity = required && strcmp(status, "found") != 0 ? "warning" : "info";
		fprintf(out,
		    "toolchain-tool role=%s name=%s required=%s mode=%s status=%s severity=%s path=%s executable=%s\n",
		    role, tool, required ? "true" : "false", mode, status, severity,
		    full, executable ? "yes" : "no");
		(void)is_dir;
		return;
	}
	if (qstar_external_tool_find_path_tool(tool, found, sizeof(found))) {
		fprintf(out,
		    "toolchain-tool role=%s name=%s required=%s mode=path status=found severity=info path=%s executable=yes\n",
		    role, tool, required ? "true" : "false", found);
		return;
	}
	fprintf(out,
	    "toolchain-tool role=%s name=%s required=%s mode=path status=missing severity=%s path=<none> executable=no\n",
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

/** build context/toolset external tool discovery 상태를 doctor output에 출력한다. */
static void
dump_external_tool_doctor(FILE *out, const struct qstar_plan *plan)
{
	const struct qstar_graph *graph;
	char found[QSTAR_PATH_MAX], name[QSTAR_PATH_MAX], value[QSTAR_PATH_MAX];
	char full[QSTAR_PATH_MAX];
	const char *mode, *status;
	size_t i, j, path_tool_count;
	int exists, executable, is_dir;

	graph = plan->graph;
	path_tool_count = graph->build_context.path_tools.len;
	(void)plan;
	for (i = 0; i < graph->toolset_len; i++)
		path_tool_count += graph->toolsets[i].path_tools.len;
	fprintf(out, "external-tool-policy path_tools=%zu tool_overrides=%zu allow_absolute=%s\n",
	    path_tool_count, graph->build_context.tool_overrides.len,
	    context_or_default(graph->build_context.allow_absolute_tools, "false"));
	for (i = 0; i < graph->build_context.path_tools.len; i++)
		dump_path_tool_doctor(out, graph->build_context.path_tools.items[i]);
	for (i = 0; i < graph->toolset_len; i++)
		for (j = 0; j < graph->toolsets[i].path_tools.len; j++)
			dump_path_tool_doctor(out, graph->toolsets[i].path_tools.items[j]);
	for (i = 0; i < graph->build_context.tool_overrides.len; i++) {
		if (!split_tool_override_for_doctor(graph->build_context.tool_overrides.items[i],
		    name, sizeof(name), value, sizeof(value))) {
			fprintf(out, "external-tool-override entry=%s status=invalid\n",
			    graph->build_context.tool_overrides.items[i]);
			continue;
		}
		if (strchr(value, '/') || strchr(value, '\\')) {
			mode = value[0] == '/' ? "absolute" : "package";
			if (doctor_path_state(graph, value, full, sizeof(full), &exists,
			    &executable, &is_dir) == 0) {
				status = !exists ? "missing" :
				    !executable ? "not-executable" : "found";
				fprintf(out,
				    "external-tool-override name=%s value=%s mode=%s status=%s path=%s executable=%s\n",
				    name, value, mode, status, full,
				    executable ? "yes" : "no");
				(void)is_dir;
			} else {
				fprintf(out,
				    "external-tool-override name=%s value=%s mode=invalid status=invalid path=<none> executable=no\n",
				    name, value);
			}
		} else if (qstar_external_tool_find_path_tool(value, found, sizeof(found))) {
			fprintf(out,
			    "external-tool-override name=%s value=%s mode=path status=found path=%s\n",
			    name, value, found);
		} else {
			fprintf(out,
			    "external-tool-override name=%s value=%s mode=path status=missing path=<none>\n",
			    name, value);
		}
	}
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
			if (strcmp(source.language, "cxx") == 0)
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
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
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
	if (dump_resolved_toolchain(out, plan, target, &toolchain) < 0)
		return -1;
	dump_effective_compile_merge(out, plan->graph, target, &toolchain);
	for (i = 0; !qstar_target_has_provider_final_action(target) &&
	    i < target->sources.len; i++) {
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
		dump_compile_argv(out, target, plan->graph, &toolchain, &source,
		    target->sources.items[i], output, i);
	}
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
	    qstar_target_has_provider_final_action(target) ? "<provider-sources>" :
	    "<target-objects>", output,
	    "artifact", 0);
	dump_command_skeleton(out, plan->graph, target, action,
	    qstar_target_has_provider_final_action(target) ? "<provider-sources>" :
	    "<target-objects>",
	    output, "artifact", final_tool, 0);
	if (qstar_target_has_provider_final_action(target))
		dump_provider_final_argv(out, target, plan->graph, &toolchain, action);
	else
		dump_final_argv(out, target, plan->graph, &toolchain, action, output);
	return 0;
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
		} else if (qstar_resolve_command_tool_for_target(plan->graph, target,
		    genrule->tool, resolved_tool, sizeof(resolved_tool), tool_mode,
		    sizeof(tool_mode), tool_error, sizeof(tool_error)) < 0) {
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
		    "tool=%s tool_mode=%s resolved_tool=%s inputs=%s outputs=%s output_identities=%s args=",
		    genrule->label, genrule->label, target->label, genrule->tool,
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
	char output[QSTAR_PATH_MAX], id[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	size_t i;

	if (qstar_resolve_toolchain(plan->graph, target, &toolchain) < 0)
		return -1;
	for (i = 0; !qstar_target_has_provider_final_action(target) &&
	    i < target->sources.len; i++) {
		qstar_target_source_classify(target, i, &source);
		if (!qstar_source_requires_compile(&source)) {
			fprintf(out,
			    "dry_run_step id=%s:link-input:%zu owner=%s kind=link-input "
			    "language=%s tool=%s toolchain=%s input=%s output=%s execute=no\n",
			    target->label, i, target->label, source.language,
			    source.tool_role, toolchain.name, target->sources.items[i],
			    target->sources.items[i]);
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
		    "tool=object-collector toolchain=%s input=<target-objects> "
		    "output=<none> execute=no\n",
		    target->label, target->label, toolchain.name);
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
	    "dry_run_step id=%s:%s:0 owner=%s kind=%s tool=%s toolchain=%s "
	    "input=%s output=%s execute=no\n",
	    target->label, action, target->label, action, tool, toolchain.name,
	    qstar_target_has_provider_final_action(target) ? "<provider-sources>" :
	    "<target-objects>", output);
	if (qstar_target_has_provider_final_action(target))
		dump_provider_final_argv(out, target, plan->graph, &toolchain, action);
	else
		dump_final_argv(out, target, plan->graph, &toolchain, action, output);
	return 0;
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
		if (dump_resolved_toolchain(out, &plan, plan.order[i], &toolchain) < 0) {
			free_plan(&plan);
			return -1;
		}
		dump_effective_compile_merge(out, graph, plan.order[i], &toolchain);
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
			    "toolchain-sanity name=%s cc=%s cxx=%s ar=%s linker=%s "
			    "platform=%s link_style=%s sysroot=%s resource_dir=%s response_files=%s response_style=%s "
			    "status=resolved\n",
			    toolchain.name, toolchain.cc, toolchain.cxx, toolchain.ar,
			    toolchain.linker, toolchain.platform, toolchain.link_style,
		    toolchain.sysroot[0] ? toolchain.sysroot : "<none>",
		    toolchain.resource_dir[0] ? toolchain.resource_dir : "<none>",
		    toolchain.response_files ? "on" : "off", toolchain.response_style);
		fprintf(out,
		    "response-policy configured_files=%s configured_style=%s effective_files=%s effective_style=%s\n",
		    context_or_default(graph->build_context.response_files, "auto"),
		    context_or_default(graph->build_context.response_style, "auto"),
		    toolchain.response_files ? "on" : "off", toolchain.response_style);
		dump_toolchain_tool_doctor(out, graph, "cc", toolchain.cc, tool_req.cc);
			dump_toolchain_tool_doctor(out, graph, "cxx", toolchain.cxx,
			    tool_req.cxx);
			dump_toolchain_tool_doctor(out, graph, "ar", toolchain.ar, tool_req.ar);
		dump_toolchain_tool_doctor(out, graph, "linker", toolchain.linker,
		    tool_req.linker);
		dump_provider_tool_doctor(out, &plan, &tool_req);
		dump_build_context_path_doctor(out, graph, "sysroot", graph->build_context.sysroot, 1);
		dump_build_context_path_doctor(out, graph, "resource_dir",
		    graph->build_context.resource_dir, 1);
		dump_depfile_doctor(out, &tool_req, &toolchain);
	}
	if (qstar_path_join(graph->package_root ? graph->package_root : ".",
	    qstar_graph_build_dir(graph), state_dir, sizeof(state_dir)) == 0) {
		if (doctor_mkdir_p(state_dir) == 0 && access(state_dir, W_OK) == 0)
			fprintf(out, "writable-build-dir yes path=%s\n",
			    qstar_graph_build_dir(graph));
		else
			fprintf(out, "writable-build-dir no path=%s\n",
			    qstar_graph_build_dir(graph));
	}
	fprintf(out, "build-context name=%s target=%s toolchain=%s stdlib=%s\n",
	    graph->build_context.name ? graph->build_context.name : "default",
	    graph->build_context.target ? graph->build_context.target : "host",
	    graph->build_context.toolchain ? graph->build_context.toolchain : "host",
	    graph->build_context.stdlib_policy ? graph->build_context.stdlib_policy : "system");
	fprintf(out, "build-context-options include_dirs=%zu lib_dirs=%zu\n",
	    graph->build_context.include_dirs.len, graph->build_context.lib_dirs.len);
	dump_external_tool_doctor(out, &plan);
	fputs("diagnostics ok\n", out);
	fputs("file-inputs ok\n", out);
	fputs("status ok\n", out);
	qstar_string_list_free(&tool_req.provider_roles);
	free_plan(&plan);
	return 0;
}
