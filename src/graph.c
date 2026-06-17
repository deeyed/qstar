#include "internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 문자열을 QStar 소유 메모리로 복사한다. */
char *
qstar_strdup(const char *s)
{
	char *p;
	size_t n;

	if (!s)
		s = "";
	n = strlen(s) + 1;
	p = malloc(n);
	if (!p)
		return NULL;
	memcpy(p, s, n);
	return p;
}

/** Graph error buffer에 첫 오류만 기록한다. */
int
qstar_set_error(struct qstar_graph *graph, const char *fmt, ...)
{
	va_list ap;

	if (!graph || graph->error[0])
		return -1;
	va_start(ap, fmt);
	vsnprintf(graph->error, sizeof(graph->error), fmt, ap);
	va_end(ap);
	return -1;
}

/** Graph error buffer에 origin metadata와 첫 오류를 함께 기록한다. */
int
qstar_set_error_origin(struct qstar_graph *graph, const char *file, int line,
    const char *field, const char *label, const char *fmt, ...)
{
	va_list ap;

	if (!graph || graph->error[0])
		return -1;
	va_start(ap, fmt);
	vsnprintf(graph->error, sizeof(graph->error), fmt, ap);
	va_end(ap);
	if (file && *file)
		snprintf(graph->error_file, sizeof(graph->error_file), "%s", file);
	if (field && *field)
		snprintf(graph->error_field, sizeof(graph->error_field), "%s", field);
	if (label && *label)
		snprintf(graph->error_label, sizeof(graph->error_label), "%s", label);
	graph->error_line = line;
	return -1;
}

/** 문자열 list에 새 항목을 복사해 추가한다. */
int
qstar_string_list_push(struct qstar_string_list *list, const char *s)
{
	char **items;
	size_t cap;

	if (list->len == list->cap) {
		cap = list->cap ? list->cap * 2 : 4;
		items = realloc(list->items, cap * sizeof(list->items[0]));
		if (!items)
			return -1;
		list->items = items;
		list->cap = cap;
	}
	list->items[list->len] = qstar_strdup(s);
	if (!list->items[list->len])
		return -1;
	list->len++;
	return 0;
}

/** 문자열 list가 소유한 모든 동적 메모리를 해제한다. */
void
qstar_string_list_free(struct qstar_string_list *list)
{
	size_t i;

	for (i = 0; i < list->len; i++)
		free(list->items[i]);
	free(list->items);
	memset(list, 0, sizeof(*list));
}

static int
copy_string_list(struct qstar_string_list *dst, const struct qstar_string_list *src);

static void
free_provider_action_template(struct qstar_provider_action_template *action)
{
	if (!action)
		return;
	qstar_string_list_free(&action->argv);
	qstar_string_list_free(&action->env);
	qstar_string_list_free(&action->inputs);
	qstar_string_list_free(&action->outputs);
	free(action->depfile);
	memset(action, 0, sizeof(*action));
}

static int
copy_provider_action_template(struct qstar_graph *graph,
    struct qstar_provider_action_template *dst,
    const struct qstar_provider_action_template *src)
{
	if (!src) {
		dst->depfile = qstar_strdup("");
		return dst->depfile ? 0 : qstar_set_error(graph, "qstar: out of memory");
	}
	dst->depfile = qstar_strdup(src->depfile ? src->depfile : "");
	dst->wants_depfile = src->wants_depfile ? 1 : 0;
	if (!dst->depfile ||
	    copy_string_list(&dst->argv, &src->argv) < 0 ||
	    copy_string_list(&dst->env, &src->env) < 0 ||
	    copy_string_list(&dst->inputs, &src->inputs) < 0 ||
	    copy_string_list(&dst->outputs, &src->outputs) < 0) {
		free_provider_action_template(dst);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

/** QStar graph 저장소를 빈 상태로 초기화한다. */
void
qstar_graph_init(struct qstar_graph *graph)
{
	memset(graph, 0, sizeof(*graph));
}

static void
free_target(struct qstar_target *target)
{
	size_t i;

	free(target->label);
	free(target->name);
	free(target->kind);
	free(target->fragment_dir);
	free(target->origin_file);
	free(target->modules.root);
	qstar_string_list_free(&target->modules.include);
	qstar_string_list_free(&target->modules.exclude);
	qstar_string_list_free(&target->configs);
	qstar_string_list_free(&target->sources);
	for (i = 0; i < target->provider_source_len; i++) {
		free(target->provider_sources[i].path);
		free(target->provider_sources[i].provider);
		free(target->provider_sources[i].unit);
		free(target->provider_sources[i].emits);
		free(target->provider_sources[i].lower);
		free(target->provider_sources[i].toolset_role);
		free_provider_action_template(&target->provider_sources[i].action);
	}
	free(target->provider_sources);
	free(target->provider_final.provider);
	free(target->provider_final.kind);
	free(target->provider_final.lower);
	free_provider_action_template(&target->provider_final.action);
	qstar_string_list_free(&target->public_headers);
	qstar_string_list_free(&target->private_headers);
	qstar_string_list_free(&target->include_dirs);
	qstar_string_list_free(&target->public_include_dirs);
	qstar_string_list_free(&target->private_include_dirs);
	qstar_string_list_free(&target->interface_include_dirs);
	qstar_string_list_free(&target->system_include_dirs);
	qstar_string_list_free(&target->deps);
	qstar_string_list_free(&target->private_deps);
	qstar_string_list_free(&target->visibility);
	qstar_string_list_free(&target->libs);
	qstar_string_list_free(&target->lib_dirs);
	qstar_string_list_free(&target->frameworks);
	qstar_string_list_free(&target->link_options);
	qstar_string_list_free(&target->link_inputs);
	qstar_string_list_free(&target->cflags);
	qstar_string_list_free(&target->cxxflags);
	qstar_string_list_free(&target->asm_include_dirs);
	qstar_string_list_free(&target->asm_compile_options);
	for (i = 0; i < target->provider_option_len; i++) {
		free(target->provider_options[i].provider);
		free(target->provider_options[i].name);
		free(target->provider_options[i].type);
		free(target->provider_options[i].value);
		qstar_string_list_free(&target->provider_options[i].list);
	}
	free(target->provider_options);
	qstar_string_list_free(&target->run_inputs);
	qstar_string_list_free(&target->run_command);
	free(target->description);
	free(target->artifact_name);
	free(target->cxx_standard);
	free(target->run_expect_contains);
	free(target->run_expect_file);
	free(target->toolset);
	free(target->toolchain);
	free(target->stdlib_policy);
}

/** reusable config declaration이 소유한 문자열과 option skeleton을 해제한다. */
static void
free_config(struct qstar_config *config)
{
	free(config->label);
	free(config->name);
	free(config->fragment_dir);
	free(config->origin_file);
	free_target(&config->options);
}

/** toolset declaration이 소유한 문자열과 argv list를 해제한다. */
static void
free_toolset(struct qstar_toolset *toolset)
{
	size_t i;

	free(toolset->label);
	free(toolset->name);
	free(toolset->fragment_dir);
	free(toolset->origin_file);
	for (i = 0; i < toolset->role_len; i++) {
		free(toolset->roles[i].role);
		qstar_string_list_free(&toolset->roles[i].argv);
	}
	free(toolset->roles);
	qstar_string_list_free(&toolset->path_tools);
	free(toolset->response_files);
	free(toolset->response_style);
	free(toolset->allow_absolute_tools);
}

/** activated language provider entry가 소유한 문자열을 해제한다. */
static void
free_language_provider(struct qstar_language_provider *provider)
{
	size_t i;

	free(provider->api);
	free(provider->id);
	free(provider->namespace);
	free(provider->version);
	free(provider->dir);
	free(provider->manifest);
	free(provider->implementation);
	for (i = 0; i < provider->option_len; i++) {
		free(provider->options[i].name);
		free(provider->options[i].type);
		free(provider->options[i].default_value);
		qstar_string_list_free(&provider->options[i].default_list);
		qstar_string_list_free(&provider->options[i].values);
	}
	free(provider->options);
	for (i = 0; i < provider->unit_len; i++) {
		free(provider->units[i].name);
		qstar_string_list_free(&provider->units[i].suffixes);
		free(provider->units[i].emits);
		free(provider->units[i].lower);
		free(provider->units[i].deps);
	}
	free(provider->units);
	for (i = 0; i < provider->final_len; i++) {
		free(provider->finals[i].kind);
		free(provider->finals[i].lower);
	}
	free(provider->finals);
}

/** generated action skeleton이 소유한 문자열과 list를 해제한다. */
static void
free_genrule(struct qstar_genrule *genrule)
{
	free(genrule->label);
	free(genrule->name);
	free(genrule->fragment_dir);
	free(genrule->origin_file);
	free(genrule->tool);
	free(genrule->description);
	qstar_string_list_free(&genrule->inputs);
	qstar_string_list_free(&genrule->outputs);
	qstar_string_list_free(&genrule->output_groups);
	qstar_string_list_free(&genrule->output_formats);
	qstar_string_list_free(&genrule->args);
	qstar_string_list_free(&genrule->command);
}

/** copy-only staging rule이 소유한 문자열과 list를 해제한다. */
static void
free_stage(struct qstar_stage *stage)
{
	free(stage->label);
	free(stage->name);
	free(stage->fragment_dir);
	free(stage->origin_file);
	free(stage->root);
	free(stage->description);
	qstar_string_list_free(&stage->srcs);
	qstar_string_list_free(&stage->dsts);
}

/** target family primitive가 소유한 문자열과 list를 해제한다. */
static void
free_target_family(struct qstar_target_family *family)
{
	free(family->name);
	free(family->fragment_dir);
	free(family->origin_file);
	qstar_string_list_free(&family->variants);
	qstar_string_list_free(&family->targets);
}

/** qstar.project metadata가 소유한 문자열을 해제한다. */
static void
free_project(struct qstar_project *project)
{
	free(project->name);
	free(project->version);
	free(project->root);
	free(project->build_dir);
	free(project->generated_dir);
	free(project->compile_commands);
	memset(project, 0, sizeof(*project));
}

/** package alias entry가 소유한 문자열을 해제한다. */
static void
free_package_alias(struct qstar_package_alias *pkg)
{
	free(pkg->alias);
	free(pkg->root);
}

/** build context input이 소유한 문자열을 해제한다. */
static void
free_build_context(struct qstar_build_context *context)
{
	free(context->name);
	free(context->target);
	free(context->platform);
	free(context->toolchain);
	free(context->stdlib_policy);
	free(context->cc);
	free(context->cxx);
	free(context->ar);
	free(context->linker);
	free(context->sysroot);
	free(context->resource_dir);
	free(context->response_files);
	free(context->response_style);
	free(context->allow_absolute_tools);
	qstar_string_list_free(&context->artifact_names);
	qstar_string_list_free(&context->compile_options);
	qstar_string_list_free(&context->include_dirs);
	qstar_string_list_free(&context->lib_dirs);
	qstar_string_list_free(&context->link_options);
	qstar_string_list_free(&context->path_tools);
	qstar_string_list_free(&context->tool_overrides);
}

/** cached lowered action entry가 소유한 문자열과 list를 해제한다. */
static void
free_cached_action(struct qstar_cached_action *action)
{
	free(action->id);
	free(action->kind);
	free(action->target_label);
	free(action->description);
	free(action->depfile);
	free(action->source_path);
	qstar_string_list_free(&action->argv);
	qstar_string_list_free(&action->env);
	qstar_string_list_free(&action->outputs);
	qstar_string_list_free(&action->inputs);
	qstar_string_list_free(&action->depfile_inputs);
}

/** lint diagnostic entry가 소유한 문자열을 해제한다. */
static void
free_lint_diagnostic(struct qstar_lint_diagnostic *diag)
{
	free(diag->code);
	free(diag->severity);
	free(diag->file);
	free(diag->field);
	free(diag->label);
	free(diag->message);
}

/** QStar graph가 소유한 모든 동적 메모리를 해제한다. */
void
qstar_graph_free(struct qstar_graph *graph)
{
	size_t i;

	free(graph->package_root);
	for (i = 0; i < graph->len; i++)
		free_target(&graph->targets[i]);
	for (i = 0; i < graph->config_len; i++)
		free_config(&graph->configs[i]);
	for (i = 0; i < graph->toolset_len; i++)
		free_toolset(&graph->toolsets[i]);
	for (i = 0; i < graph->language_provider_len; i++)
		free_language_provider(&graph->language_providers[i]);
	for (i = 0; i < graph->genrule_len; i++)
		free_genrule(&graph->genrules[i]);
	for (i = 0; i < graph->stage_len; i++)
		free_stage(&graph->stages[i]);
	for (i = 0; i < graph->family_len; i++)
		free_target_family(&graph->families[i]);
	for (i = 0; i < graph->lint_len; i++)
		free_lint_diagnostic(&graph->lint_diagnostics[i]);
	for (i = 0; i < graph->package_len; i++)
		free_package_alias(&graph->packages[i]);
	for (i = 0; i < graph->cached_action_len; i++)
		free_cached_action(&graph->cached_actions[i]);
	free_project(&graph->project);
	free(graph->generator);
	free(graph->requested_generator);
	free(graph->build_dir_override);
	free_build_context(&graph->build_context);
	free(graph->targets);
	free(graph->configs);
	free(graph->toolsets);
	free(graph->language_providers);
	free(graph->packages);
	free(graph->genrules);
	free(graph->stages);
	free(graph->families);
	free(graph->lint_diagnostics);
	free(graph->cached_actions);
	qstar_string_list_free(&graph->evaluated_fragments);
	memset(graph, 0, sizeof(*graph));
}

/** Graph에 저장된 cached lowered action plan을 비운다. */
void
qstar_graph_clear_cached_actions(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->cached_action_len; i++)
		free_cached_action(&graph->cached_actions[i]);
	free(graph->cached_actions);
	graph->cached_actions = NULL;
	graph->cached_action_len = 0;
	graph->cached_action_cap = 0;
	graph->cached_action_plan_loaded = 0;
}

/** Graph의 cached lowered action plan에 새 action slot을 추가한다. */
struct qstar_cached_action *
qstar_graph_add_cached_action(struct qstar_graph *graph)
{
	struct qstar_cached_action *items;
	size_t ncap;

	if (graph->cached_action_len == graph->cached_action_cap) {
		ncap = graph->cached_action_cap ? graph->cached_action_cap * 2 : 64;
		items = realloc(graph->cached_actions, ncap * sizeof(items[0]));
		if (!items) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		memset(items + graph->cached_action_cap, 0,
		    (ncap - graph->cached_action_cap) * sizeof(items[0]));
		graph->cached_actions = items;
		graph->cached_action_cap = ncap;
	}
	return &graph->cached_actions[graph->cached_action_len++];
}

/** QStar package root를 graph에 기록한다. */
int
qstar_graph_set_package_root(struct qstar_graph *graph, const char *root)
{
	char *copy;

	copy = qstar_strdup(root && *root ? root : ".");
	if (!copy)
		return qstar_set_error(graph, "qstar: out of memory");
	free(graph->package_root);
	graph->package_root = copy;
	return 0;
}

static int
valid_project_relative(const char *path)
{
	return path && *path && qstar_path_is_package_relative(path);
}

/** QStar project의 effective build directory를 반환한다. */
const char *
qstar_graph_build_dir(const struct qstar_graph *graph)
{
	if (graph && graph->build_dir_override && *graph->build_dir_override)
		return graph->build_dir_override;
	if (graph && graph->project.build_dir && *graph->project.build_dir)
		return graph->project.build_dir;
	return "build/qstar";
}

/** QStar project의 effective generated output directory를 반환한다. */
const char *
qstar_graph_generated_dir(const struct qstar_graph *graph)
{
	if (graph && graph->project.generated_dir && *graph->project.generated_dir)
		return graph->project.generated_dir;
	return "generated";
}

/** path가 현재 project의 generated output root 아래 있는지 검사한다. */
int
qstar_graph_path_is_generated(const struct qstar_graph *graph, const char *path)
{
	const char *root;
	size_t n;

	if (!path || !*path)
		return 0;
	root = qstar_graph_generated_dir(graph);
	n = strlen(root);
	return strncmp(path, root, n) == 0 && path[n] == '/' && path[n + 1] != '\0';
}

/** QStar project의 effective generator를 반환한다. */
const char *
qstar_graph_generator(const struct qstar_graph *graph)
{
	if (graph && graph->generator && *graph->generator)
		return graph->generator;
	return "stella";
}

/** CLI가 요청한 generator 값을 반환한다. */
const char *
qstar_graph_requested_generator(const struct qstar_graph *graph)
{
	if (graph && graph->requested_generator && *graph->requested_generator)
		return graph->requested_generator;
	return "auto";
}

/** QStar project의 effective compile_commands policy를 반환한다. */
const char *
qstar_graph_compile_commands_policy(const struct qstar_graph *graph)
{
	if (graph && graph->project.compile_commands &&
	    *graph->project.compile_commands)
		return graph->project.compile_commands;
	return "build";
}

/** build directory 아래 상대 path를 deterministic package-relative path로 만든다. */
int
qstar_graph_build_path(const struct qstar_graph *graph, const char *subpath, char *dst,
    size_t dstlen)
{
	const char *build_dir;
	int n;

	build_dir = qstar_graph_build_dir(graph);
	if (!subpath || !*subpath)
		n = snprintf(dst, dstlen, "%s", build_dir);
	else
		n = snprintf(dst, dstlen, "%s/%s", build_dir, subpath);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

static int
valid_generator(const char *generator)
{
	return !generator || !*generator || strcmp(generator, "stella") == 0 ||
	    strcmp(generator, "ninja") == 0 || strcmp(generator, "auto") == 0;
}

/** CLI generator/build directory override를 graph effective option으로 기록한다. */
int
qstar_graph_set_cli_overrides(struct qstar_graph *graph, const char *generator,
    const char *build_dir)
{
	char *generator_copy, *requested_copy, *build_copy;
	const char *requested, *effective;

	if (!valid_generator(generator))
		return qstar_set_error(graph,
		    "qstar: invalid generator '%s'; expected stella, ninja, or auto",
		    generator ? generator : "");
	if (build_dir && *build_dir && !valid_project_relative(build_dir))
		return qstar_set_error(graph,
		    "qstar: CLI build directory override must be package-relative");
	requested = generator && *generator ? generator : "auto";
	effective = strcmp(requested, "auto") == 0 ? "stella" : requested;
	requested_copy = qstar_strdup(requested);
	generator_copy = qstar_strdup(effective);
	build_copy = build_dir && *build_dir ? qstar_strdup(build_dir) : NULL;
	if (!requested_copy || !generator_copy || (build_dir && *build_dir && !build_copy)) {
		free(requested_copy);
		free(generator_copy);
		free(build_copy);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	free(graph->requested_generator);
	free(graph->generator);
	free(graph->build_dir_override);
	graph->requested_generator = requested_copy;
	graph->generator = generator_copy;
	graph->build_dir_override = build_copy;
	return 0;
}

/** qstar.project metadata를 graph에 기록한다. */
int
qstar_graph_set_project(struct qstar_graph *graph, const char *name,
    const char *version, const char *root, const char *build_dir,
    const char *generated_dir, const char *compile_commands)
{
	char *name_copy, *version_copy, *root_copy, *build_copy, *generated_copy;
	char *compile_copy;
	const char *effective_build_dir, *effective_generated_dir, *effective_compile;
	size_t generated_len;

	if (graph->project.present)
		return qstar_set_error(graph, "qstar: qstar.project already declared");
	if (root && *root && strcmp(root, ".") != 0)
		return qstar_set_error(graph, "qstar: qstar.project root must be \".\" in v1");
	effective_build_dir = build_dir && *build_dir ? build_dir : "build/qstar";
	if (!valid_project_relative(effective_build_dir))
		return qstar_set_error(graph,
		    "qstar: qstar.project build_dir must be package-relative");
	effective_generated_dir = generated_dir && *generated_dir ? generated_dir :
	    "generated";
	if (!valid_project_relative(effective_generated_dir))
		return qstar_set_error(graph,
		    "qstar: qstar.project generated_dir must be package-relative");
	generated_len = strlen(effective_generated_dir);
	if (strcmp(effective_generated_dir, ".") == 0 ||
	    effective_generated_dir[generated_len - 1] == '/')
		return qstar_set_error(graph,
		    "qstar: qstar.project generated_dir must name a directory without a trailing slash");
	effective_compile = compile_commands && *compile_commands ? compile_commands :
	    "build";
	if (strcmp(effective_compile, "build") != 0 &&
	    strcmp(effective_compile, "root") != 0 &&
	    strcmp(effective_compile, "off") != 0)
		return qstar_set_error(graph,
		    "qstar: qstar.project compile_commands must be \"root\", \"build\", or \"off\"");
	name_copy = qstar_strdup(name ? name : "");
	version_copy = qstar_strdup(version ? version : "");
	root_copy = qstar_strdup(root && *root ? root : ".");
	build_copy = qstar_strdup(effective_build_dir);
	generated_copy = qstar_strdup(effective_generated_dir);
	compile_copy = qstar_strdup(effective_compile);
	if (!name_copy || !version_copy || !root_copy || !build_copy ||
	    !generated_copy || !compile_copy) {
		free(name_copy);
		free(version_copy);
		free(root_copy);
		free(build_copy);
		free(generated_copy);
		free(compile_copy);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	graph->project.present = 1;
	graph->project.name = name_copy;
	graph->project.version = version_copy;
	graph->project.root = root_copy;
	graph->project.build_dir = build_copy;
	graph->project.generated_dir = generated_copy;
	graph->project.compile_commands = compile_copy;
	return 0;
}

static int
target_cmp(const void *a, const void *b)
{
	const struct qstar_target *ta = a;
	const struct qstar_target *tb = b;

	return strcmp(ta->label, tb->label);
}

static int
has_target(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return 1;
	}
	return 0;
}

static int
has_genrule(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (strcmp(graph->genrules[i].label, label) == 0)
			return 1;
	}
	return 0;
}

static int
has_stage(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->stage_len; i++) {
		if (strcmp(graph->stages[i].label, label) == 0)
			return 1;
	}
	return 0;
}

static int
has_config(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->config_len; i++) {
		if (strcmp(graph->configs[i].label, label) == 0)
			return 1;
	}
	return 0;
}

static int
has_toolset(const struct qstar_graph *graph, const char *label)
{
	return qstar_graph_find_toolset(graph, label) != NULL;
}

/** Graph에서 canonical toolset label을 찾는다. */
const struct qstar_toolset *
qstar_graph_find_toolset(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	if (!graph || !label || !*label)
		return NULL;
	for (i = 0; i < graph->toolset_len; i++) {
		if (graph->toolsets[i].label && strcmp(graph->toolsets[i].label, label) == 0)
			return &graph->toolsets[i];
	}
	return NULL;
}

const struct qstar_language_provider *
qstar_graph_find_language_provider(const struct qstar_graph *graph, const char *namespace)
{
	size_t i;

	if (!graph || !namespace || !*namespace)
		return NULL;
	for (i = 0; i < graph->language_provider_len; i++) {
		if (graph->language_providers[i].namespace &&
		    strcmp(graph->language_providers[i].namespace, namespace) == 0)
			return &graph->language_providers[i];
	}
	return NULL;
}

const struct qstar_language_provider *
qstar_graph_find_language_provider_manifest(const struct qstar_graph *graph,
    const char *manifest)
{
	size_t i;

	if (!graph || !manifest || !*manifest)
		return NULL;
	for (i = 0; i < graph->language_provider_len; i++) {
		if (graph->language_providers[i].manifest &&
		    strcmp(graph->language_providers[i].manifest, manifest) == 0)
			return &graph->language_providers[i];
	}
	return NULL;
}

int
qstar_graph_language_provider_is_available(const struct qstar_graph *graph,
    const char *namespace)
{
	return qstar_language_provider_is_preloaded(namespace) ||
	    qstar_graph_find_language_provider(graph, namespace) != NULL;
}

/** toolset에서 compile/archive/link role argv-vector를 찾는다. */
const struct qstar_string_list *
qstar_toolset_role_argv(const struct qstar_toolset *toolset, const char *role)
{
	size_t i;

	if (!toolset || !role)
		return NULL;
	for (i = 0; i < toolset->role_len; i++) {
		if (toolset->roles[i].role && strcmp(toolset->roles[i].role, role) == 0)
			return toolset->roles[i].argv.len ? &toolset->roles[i].argv : NULL;
	}
	return NULL;
}

struct qstar_string_list *
qstar_toolset_add_role(struct qstar_graph *graph, struct qstar_toolset *toolset,
    const char *role)
{
	struct qstar_tool_role *roles;
	size_t cap, i;

	if (!toolset || !role || !*role) {
		qstar_set_error(graph, "qstar: empty toolset tool role");
		return NULL;
	}
	for (i = 0; i < toolset->role_len; i++) {
		if (toolset->roles[i].role && strcmp(toolset->roles[i].role, role) == 0) {
			qstar_set_error(graph, "qstar: duplicate toolset tool role '%s'",
			    role);
			return NULL;
		}
	}
	if (toolset->role_len == toolset->role_cap) {
		cap = toolset->role_cap ? toolset->role_cap * 2 : 4;
		roles = realloc(toolset->roles, cap * sizeof(toolset->roles[0]));
		if (!roles) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		toolset->roles = roles;
		toolset->role_cap = cap;
	}
	memset(&toolset->roles[toolset->role_len], 0,
	    sizeof(toolset->roles[toolset->role_len]));
	toolset->roles[toolset->role_len].role = qstar_strdup(role);
	if (!toolset->roles[toolset->role_len].role) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return &toolset->roles[toolset->role_len++].argv;
}

static int
compare_tool_role(const void *a, const void *b)
{
	const struct qstar_tool_role *ra = a, *rb = b;

	return strcmp(ra->role ? ra->role : "", rb->role ? rb->role : "");
}

void
qstar_toolset_sort_roles(struct qstar_toolset *toolset)
{
	if (!toolset || toolset->role_len < 2)
		return;
	qsort(toolset->roles, toolset->role_len, sizeof(toolset->roles[0]),
	    compare_tool_role);
}

/** target에 연결된 toolset role argv-vector를 찾는다. */
const struct qstar_string_list *
qstar_target_tool_role_argv(const struct qstar_graph *graph,
    const struct qstar_target *target, const char *role)
{
	const struct qstar_toolset *toolset;

	if (!target || !target->toolset || !*target->toolset)
		return NULL;
	toolset = qstar_graph_find_toolset(graph, target->toolset);
	return qstar_toolset_role_argv(toolset, role);
}

static int
valid_alias(const char *alias)
{
	const unsigned char *p;

	if (!alias || alias[0] != '@' || !alias[1])
		return 0;
	for (p = (const unsigned char *)alias + 1; *p; p++) {
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
			return 0;
	}
	return 1;
}

/** QStar package alias map에서 alias를 찾는다. */
const struct qstar_package_alias *
qstar_graph_find_package_alias(const struct qstar_graph *graph, const char *alias)
{
	size_t i;

	for (i = 0; i < graph->package_len; i++) {
		if (strcmp(graph->packages[i].alias, alias) == 0)
			return &graph->packages[i];
	}
	return NULL;
}

/** QStar package alias를 추가하고 중복 alias를 stable error로 막는다. */
int
qstar_graph_add_package_alias(struct qstar_graph *graph, const char *alias, const char *root)
{
	struct qstar_package_alias *packages, *pkg;
	size_t cap;

	if (!valid_alias(alias))
		return qstar_set_error(graph, "qstar: invalid package alias '%s'", alias ? alias : "");
	if (!root || !*root)
		return qstar_set_error(graph, "qstar: package alias '%s' has empty root", alias);
	if (qstar_graph_find_package_alias(graph, alias))
		return qstar_set_error(graph, "qstar: duplicate package alias '%s'", alias);
	if (graph->package_len == graph->package_cap) {
		cap = graph->package_cap ? graph->package_cap * 2 : 4;
		packages = realloc(graph->packages, cap * sizeof(graph->packages[0]));
		if (!packages)
			return qstar_set_error(graph, "qstar: out of memory");
		graph->packages = packages;
		graph->package_cap = cap;
	}
	pkg = &graph->packages[graph->package_len++];
	memset(pkg, 0, sizeof(*pkg));
	pkg->alias = qstar_strdup(alias);
	pkg->root = qstar_strdup(root);
	if (!pkg->alias || !pkg->root)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

static int
replace_string(char **slot, const char *value)
{
	char *copy;

	if (!value)
		return 0;
	copy = qstar_strdup(value);
	if (!copy)
		return -1;
	free(*slot);
	*slot = copy;
	return 0;
}

static const char *
canonical_platform_context(const char *platform)
{
	if (!platform || !*platform || strcmp(platform, "default") == 0)
		return NULL;
	if (strcmp(platform, "host") == 0)
		return "host";
	if (strcmp(platform, "windows") == 0 || strcmp(platform, "win32") == 0)
		return "windows";
	if (strcmp(platform, "darwin") == 0 || strcmp(platform, "macos") == 0)
		return "darwin";
	if (strcmp(platform, "linux") == 0)
		return "linux";
	if (strcmp(platform, "generic") == 0)
		return "generic";
	return NULL;
}

/** explicit platform context를 graph에 기록한다. */
int
qstar_graph_set_platform_context(struct qstar_graph *graph, const char *platform)
{
	const char *canonical;

	if (!platform)
		return 0;
	canonical = canonical_platform_context(platform);
	if (!canonical)
		return qstar_set_error(graph,
		    "qstar: invalid platform context '%s'; expected host, windows, darwin, linux, or generic",
		    platform);
	if (replace_string(&graph->build_context.platform, canonical) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** graph의 explicit platform context를 반환한다. */
const char *
qstar_graph_platform(const struct qstar_graph *graph)
{
	if (graph && graph->build_context.platform && *graph->build_context.platform)
		return graph->build_context.platform;
	return qstar_host_platform();
}

/** QStar explain build context 입력을 graph에 기록한다. */
int
qstar_graph_set_build_context_input(struct qstar_graph *graph, const char *name,
    const char *target, const char *toolchain, const char *stdlib_policy)
{
	if (replace_string(&graph->build_context.name, name) < 0 ||
	    replace_string(&graph->build_context.target, target) < 0 ||
	    replace_string(&graph->build_context.toolchain, toolchain) < 0 ||
	    replace_string(&graph->build_context.stdlib_policy, stdlib_policy) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** 문자열 list field를 graph-owned deep copy로 복사한다. */
static int
copy_string_list(struct qstar_string_list *dst, const struct qstar_string_list *src)
{
	size_t i;

	for (i = 0; i < src->len; i++) {
		if (qstar_string_list_push(dst, src->items[i]) < 0)
			return -1;
	}
	return 0;
}

/** QStar graph에 새 target을 추가하고 중복 label을 stable error로 막는다. */
struct qstar_target *
qstar_graph_add_target(struct qstar_graph *graph, const char *label, const char *name,
    const char *kind, const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_target *targets, *target;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate target label '%s'", label);
		return NULL;
	}
	if (has_genrule(graph, label)) {
		qstar_set_error(graph, "qstar: label '%s' is already used by generated action", label);
		return NULL;
	}
	if (has_stage(graph, label)) {
		qstar_set_error(graph, "qstar: label '%s' is already used by stage rule", label);
		return NULL;
	}
	if (has_config(graph, label)) {
		qstar_set_error(graph, "qstar: label '%s' is already used by config", label);
		return NULL;
	}
	if (has_toolset(graph, label)) {
		qstar_set_error(graph, "qstar: label '%s' is already used by toolset", label);
		return NULL;
	}
	if (graph->len == graph->cap) {
		cap = graph->cap ? graph->cap * 2 : 8;
		targets = realloc(graph->targets, cap * sizeof(graph->targets[0]));
		if (!targets) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->targets = targets;
		graph->cap = cap;
	}
	target = &graph->targets[graph->len++];
	memset(target, 0, sizeof(*target));
	target->label = qstar_strdup(label);
	target->name = qstar_strdup(name);
	target->kind = qstar_strdup(kind);
	target->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	target->origin_file = qstar_strdup(origin_file ? origin_file : "");
	target->origin_line = origin_line;
	target->toolchain = qstar_strdup("host");
	target->stdlib_policy = qstar_strdup("system");
	target->artifact_name = qstar_strdup("");
	target->cxx_standard = qstar_strdup("");
	target->run_expect_contains = qstar_strdup("");
	target->run_expect_file = qstar_strdup("");
	target->description = qstar_strdup("");
	target->toolset = qstar_strdup("");
	if (!target->label || !target->name || !target->kind || !target->fragment_dir ||
	    !target->origin_file || !target->toolchain || !target->stdlib_policy ||
	    !target->artifact_name || !target->cxx_standard ||
	    !target->run_expect_contains || !target->run_expect_file ||
	    !target->description || !target->toolset) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return target;
}

/** QStar graph에 reusable config declaration을 추가하고 label 충돌을 막는다. */
struct qstar_config *
qstar_graph_add_config(struct qstar_graph *graph, const char *label, const char *name,
    const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_config *configs, *config;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: config label '%s' conflicts with target", label);
		return NULL;
	}
	if (has_genrule(graph, label)) {
		qstar_set_error(graph,
		    "qstar: config label '%s' conflicts with generated action", label);
		return NULL;
	}
	if (has_stage(graph, label)) {
		qstar_set_error(graph, "qstar: config label '%s' conflicts with stage rule",
		    label);
		return NULL;
	}
	if (has_config(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate config label '%s'", label);
		return NULL;
	}
	if (has_toolset(graph, label)) {
		qstar_set_error(graph, "qstar: config label '%s' conflicts with toolset", label);
		return NULL;
	}
	if (graph->config_len == graph->config_cap) {
		cap = graph->config_cap ? graph->config_cap * 2 : 4;
		configs = realloc(graph->configs, cap * sizeof(graph->configs[0]));
		if (!configs) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->configs = configs;
		graph->config_cap = cap;
	}
	config = &graph->configs[graph->config_len++];
	memset(config, 0, sizeof(*config));
	config->label = qstar_strdup(label);
	config->name = qstar_strdup(name);
	config->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	config->origin_file = qstar_strdup(origin_file ? origin_file : "");
	config->origin_line = origin_line;
	config->options.label = qstar_strdup(label);
	config->options.name = qstar_strdup(name);
	config->options.kind = qstar_strdup("config");
	config->options.fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	config->options.origin_file = qstar_strdup(origin_file ? origin_file : "");
	config->options.origin_line = origin_line;
	if (!config->label || !config->name || !config->fragment_dir ||
	    !config->origin_file || !config->options.label || !config->options.name ||
	    !config->options.kind || !config->options.fragment_dir ||
	    !config->options.origin_file) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return config;
}

/** QStar graph에 toolset declaration을 추가하고 label 충돌을 막는다. */
struct qstar_toolset *
qstar_graph_add_toolset(struct qstar_graph *graph, const char *label, const char *name,
    const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_toolset *toolsets, *toolset;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: toolset label '%s' conflicts with target", label);
		return NULL;
	}
	if (has_genrule(graph, label)) {
		qstar_set_error(graph,
		    "qstar: toolset label '%s' conflicts with generated action", label);
		return NULL;
	}
	if (has_stage(graph, label)) {
		qstar_set_error(graph, "qstar: toolset label '%s' conflicts with stage rule",
		    label);
		return NULL;
	}
	if (has_config(graph, label)) {
		qstar_set_error(graph, "qstar: toolset label '%s' conflicts with config", label);
		return NULL;
	}
	if (has_toolset(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate toolset label '%s'", label);
		return NULL;
	}
	if (graph->toolset_len == graph->toolset_cap) {
		cap = graph->toolset_cap ? graph->toolset_cap * 2 : 4;
		toolsets = realloc(graph->toolsets, cap * sizeof(graph->toolsets[0]));
		if (!toolsets) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->toolsets = toolsets;
		graph->toolset_cap = cap;
	}
	toolset = &graph->toolsets[graph->toolset_len++];
	memset(toolset, 0, sizeof(*toolset));
	toolset->label = qstar_strdup(label);
	toolset->name = qstar_strdup(name);
	toolset->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	toolset->origin_file = qstar_strdup(origin_file ? origin_file : "");
	toolset->origin_line = origin_line;
	toolset->response_files = qstar_strdup("auto");
	toolset->response_style = qstar_strdup("auto");
	toolset->allow_absolute_tools = qstar_strdup("false");
	if (!toolset->label || !toolset->name || !toolset->fragment_dir ||
	    !toolset->origin_file || !toolset->response_files ||
	    !toolset->response_style || !toolset->allow_absolute_tools) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return toolset;
}

struct qstar_language_provider *
qstar_graph_add_language_provider(struct qstar_graph *graph, const char *api,
    const char *id, const char *namespace, const char *version, const char *dir,
    const char *manifest, const char *implementation)
{
	struct qstar_language_provider *providers, *provider;
	size_t cap;

	if (!api || !*api || !id || !*id || !namespace || !*namespace ||
	    !dir || !*dir || !manifest || !*manifest || !implementation ||
	    !*implementation) {
		qstar_set_error(graph, "qstar: invalid language provider manifest");
		return NULL;
	}
	if (qstar_language_provider_is_preloaded(namespace)) {
		qstar_set_error(graph,
		    "qstar: language namespace lang.%s is preloaded by QStar", namespace);
		return NULL;
	}
	if (qstar_graph_find_language_provider(graph, namespace)) {
		qstar_set_error(graph,
		    "qstar: duplicate language provider namespace lang.%s", namespace);
		return NULL;
	}
	if (qstar_graph_find_language_provider_manifest(graph, manifest)) {
		qstar_set_error(graph, "qstar: duplicate language provider '%s'",
		    manifest);
		return NULL;
	}
	if (graph->language_provider_len == graph->language_provider_cap) {
		cap = graph->language_provider_cap ? graph->language_provider_cap * 2 : 4;
		providers = realloc(graph->language_providers,
		    cap * sizeof(graph->language_providers[0]));
		if (!providers) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->language_providers = providers;
		graph->language_provider_cap = cap;
	}
	provider = &graph->language_providers[graph->language_provider_len++];
	memset(provider, 0, sizeof(*provider));
	provider->api = qstar_strdup(api);
	provider->id = qstar_strdup(id);
	provider->namespace = qstar_strdup(namespace);
	provider->version = qstar_strdup(version ? version : "");
	provider->dir = qstar_strdup(dir);
	provider->manifest = qstar_strdup(manifest);
	provider->implementation = qstar_strdup(implementation);
	if (!provider->api || !provider->id || !provider->namespace ||
	    !provider->version || !provider->dir || !provider->manifest ||
	    !provider->implementation) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return provider;
}

int
qstar_language_provider_add_option_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *name, const char *type,
    const struct qstar_string_list *values, int has_default, const char *default_value,
    const struct qstar_string_list *default_list)
{
	struct qstar_language_option_schema *options, *option;
	size_t cap;

	if (!provider || !name || !*name || !type || !*type)
		return qstar_set_error(graph, "qstar: invalid language provider option schema");
	if (provider->option_len == provider->option_cap) {
		cap = provider->option_cap ? provider->option_cap * 2 : 4;
		options = realloc(provider->options, cap * sizeof(provider->options[0]));
		if (!options)
			return qstar_set_error(graph, "qstar: out of memory");
		provider->options = options;
		provider->option_cap = cap;
	}
	option = &provider->options[provider->option_len++];
	memset(option, 0, sizeof(*option));
	option->name = qstar_strdup(name);
	option->type = qstar_strdup(type);
	option->has_default = has_default ? 1 : 0;
	option->default_value = qstar_strdup(default_value ? default_value : "");
	if (!option->name || !option->type || !option->default_value ||
	    copy_string_list(&option->values, values) < 0 ||
	    copy_string_list(&option->default_list, default_list) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

int
qstar_language_provider_add_unit_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *name,
    const struct qstar_string_list *suffixes, const char *emits, const char *lower,
    const char *deps)
{
	struct qstar_language_unit_schema *units, *unit;
	size_t cap, i;

	if (!provider || !name || !*name || !suffixes || suffixes->len == 0 ||
	    !emits || !*emits || !lower || !*lower)
		return qstar_set_error(graph, "qstar: invalid language provider unit schema");
	for (i = 0; i < provider->unit_len; i++) {
		if (provider->units[i].name && strcmp(provider->units[i].name, name) == 0)
			return qstar_set_error(graph,
			    "qstar: duplicate language provider unit '%s.%s'",
			    provider->namespace, name);
	}
	if (provider->unit_len == provider->unit_cap) {
		cap = provider->unit_cap ? provider->unit_cap * 2 : 4;
		units = realloc(provider->units, cap * sizeof(provider->units[0]));
		if (!units)
			return qstar_set_error(graph, "qstar: out of memory");
		provider->units = units;
		provider->unit_cap = cap;
	}
	unit = &provider->units[provider->unit_len++];
	memset(unit, 0, sizeof(*unit));
	unit->name = qstar_strdup(name);
	unit->emits = qstar_strdup(emits);
	unit->lower = qstar_strdup(lower);
	unit->deps = qstar_strdup(deps ? deps : "");
	if (!unit->name || !unit->emits || !unit->lower || !unit->deps ||
	    copy_string_list(&unit->suffixes, suffixes) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

int
qstar_language_provider_add_final_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *kind, const char *lower)
{
	struct qstar_language_final_schema *finals, *final;
	size_t cap, i;

	if (!provider || !kind || !*kind || !lower || !*lower)
		return qstar_set_error(graph, "qstar: invalid language provider final schema");
	for (i = 0; i < provider->final_len; i++) {
		if (provider->finals[i].kind &&
		    strcmp(provider->finals[i].kind, kind) == 0)
			return qstar_set_error(graph,
			    "qstar: duplicate language provider final '%s.%s'",
			    provider->namespace, kind);
	}
	if (provider->final_len == provider->final_cap) {
		cap = provider->final_cap ? provider->final_cap * 2 : 4;
		finals = realloc(provider->finals, cap * sizeof(provider->finals[0]));
		if (!finals)
			return qstar_set_error(graph, "qstar: out of memory");
		provider->finals = finals;
		provider->final_cap = cap;
	}
	final = &provider->finals[provider->final_len++];
	memset(final, 0, sizeof(*final));
	final->kind = qstar_strdup(kind);
	final->lower = qstar_strdup(lower);
	if (!final->kind || !final->lower)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

int
qstar_target_add_provider_source_unit(struct qstar_graph *graph,
    struct qstar_target *target, size_t source_index, const char *path,
    const char *provider, const char *unit, const char *emits, const char *lower,
    const struct qstar_provider_action_template *action)
{
	struct qstar_provider_source_unit *items, *item;
	char role[128];
	size_t cap, i;

	if (!target || !path || !*path || !provider || !*provider || !unit ||
	    !*unit || !emits || !*emits || !lower || !*lower)
		return qstar_set_error(graph, "qstar: invalid provider source unit");
	for (i = 0; i < target->provider_source_len; i++) {
		if (target->provider_sources[i].source_index == source_index)
			return qstar_set_error(graph,
			    "qstar: duplicate provider source metadata for '%s'",
			    target->label ? target->label : "<target>");
	}
	if (snprintf(role, sizeof(role), "%s.compiler", provider) >=
	    (int)sizeof(role))
		return qstar_set_error(graph, "qstar: provider source tool role is too long");
	if (target->provider_source_len == target->provider_source_cap) {
		cap = target->provider_source_cap ? target->provider_source_cap * 2 : 4;
		items = realloc(target->provider_sources,
		    cap * sizeof(target->provider_sources[0]));
		if (!items)
			return qstar_set_error(graph, "qstar: out of memory");
		target->provider_sources = items;
		target->provider_source_cap = cap;
	}
	item = &target->provider_sources[target->provider_source_len++];
	memset(item, 0, sizeof(*item));
	item->source_index = source_index;
	item->path = qstar_strdup(path);
	item->provider = qstar_strdup(provider);
	item->unit = qstar_strdup(unit);
	item->emits = qstar_strdup(emits);
	item->lower = qstar_strdup(lower);
	item->toolset_role = qstar_strdup(role);
	if (!item->path || !item->provider || !item->unit || !item->emits ||
	    !item->lower || !item->toolset_role)
		return qstar_set_error(graph, "qstar: out of memory");
	if (copy_provider_action_template(graph, &item->action, action) < 0)
		return -1;
	return 0;
}

int
qstar_target_set_provider_final_action(struct qstar_graph *graph,
    struct qstar_target *target, const char *provider, const char *kind,
    const char *lower, const struct qstar_provider_action_template *action)
{
	if (!target || !provider || !*provider || !kind || !*kind || !lower || !*lower)
		return qstar_set_error(graph, "qstar: invalid provider final action");
	if (target->provider_final.present)
		return qstar_set_error(graph,
		    "qstar: duplicate provider final action for '%s'",
		    target->label ? target->label : "<target>");
	target->provider_final.present = 1;
	target->provider_final.provider = qstar_strdup(provider);
	target->provider_final.kind = qstar_strdup(kind);
	target->provider_final.lower = qstar_strdup(lower);
	if (!target->provider_final.provider || !target->provider_final.kind ||
	    !target->provider_final.lower)
		return qstar_set_error(graph, "qstar: out of memory");
	if (copy_provider_action_template(graph, &target->provider_final.action,
	    action) < 0)
		return -1;
	return 0;
}

int
qstar_target_set_provider_option(struct qstar_graph *graph,
    struct qstar_target *target, const char *provider, const char *name,
    const char *type, const char *value, const struct qstar_string_list *list)
{
	struct qstar_provider_option_value *items, *item;
	size_t cap, i;

	if (!target || !provider || !*provider || !name || !*name || !type || !*type)
		return qstar_set_error(graph, "qstar: invalid provider option");
	for (i = 0; i < target->provider_option_len; i++) {
		item = &target->provider_options[i];
		if (strcmp(item->provider, provider) == 0 &&
		    strcmp(item->name, name) == 0)
			goto replace;
	}
	if (target->provider_option_len == target->provider_option_cap) {
		cap = target->provider_option_cap ? target->provider_option_cap * 2 : 8;
		items = realloc(target->provider_options,
		    cap * sizeof(target->provider_options[0]));
		if (!items)
			return qstar_set_error(graph, "qstar: out of memory");
		target->provider_options = items;
		target->provider_option_cap = cap;
	}
	item = &target->provider_options[target->provider_option_len++];
	memset(item, 0, sizeof(*item));
	item->provider = qstar_strdup(provider);
	item->name = qstar_strdup(name);
	if (!item->provider || !item->name)
		return qstar_set_error(graph, "qstar: out of memory");

replace:
	free(item->type);
	free(item->value);
	qstar_string_list_free(&item->list);
	item->type = qstar_strdup(type);
	item->value = qstar_strdup(value ? value : "");
	if (!item->type || !item->value)
		return qstar_set_error(graph, "qstar: out of memory");
	if (list && copy_string_list(&item->list, list) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

const struct qstar_provider_option_value *
qstar_target_provider_option(const struct qstar_target *target,
    const char *provider, const char *name)
{
	size_t i;

	if (!target || !provider || !name)
		return NULL;
	for (i = 0; i < target->provider_option_len; i++) {
		if (strcmp(target->provider_options[i].provider, provider) == 0 &&
		    strcmp(target->provider_options[i].name, name) == 0)
			return &target->provider_options[i];
	}
	return NULL;
}

const struct qstar_language_option_schema *
qstar_language_provider_find_option(const struct qstar_language_provider *provider,
    const char *name)
{
	size_t i;

	if (!provider || !name || !*name)
		return NULL;
	for (i = 0; i < provider->option_len; i++) {
		if (provider->options[i].name &&
		    strcmp(provider->options[i].name, name) == 0)
			return &provider->options[i];
	}
	return NULL;
}

const struct qstar_language_unit_schema *
qstar_language_provider_find_unit(const struct qstar_language_provider *provider,
    const char *name)
{
	size_t i;

	if (!provider || !name || !*name)
		return NULL;
	for (i = 0; i < provider->unit_len; i++) {
		if (provider->units[i].name &&
		    strcmp(provider->units[i].name, name) == 0)
			return &provider->units[i];
	}
	return NULL;
}

const struct qstar_language_final_schema *
qstar_language_provider_find_final(const struct qstar_language_provider *provider,
    const char *kind)
{
	size_t i;

	if (!provider || !kind || !*kind)
		return NULL;
	for (i = 0; i < provider->final_len; i++) {
		if (provider->finals[i].kind &&
		    strcmp(provider->finals[i].kind, kind) == 0)
			return &provider->finals[i];
	}
	return NULL;
}

int
qstar_target_has_provider_final_action(const struct qstar_target *target)
{
	return target && target->provider_final.present;
}

const struct qstar_provider_source_unit *
qstar_target_provider_source_unit(const struct qstar_target *target,
    size_t source_index)
{
	size_t i;

	if (!target)
		return NULL;
	for (i = 0; i < target->provider_source_len; i++) {
		if (target->provider_sources[i].source_index == source_index)
			return &target->provider_sources[i];
	}
	return NULL;
}

/** config label로 reusable config declaration을 찾는다. */
static const struct qstar_config *
find_config(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->config_len; i++) {
		if (strcmp(graph->configs[i].label, label) == 0)
			return &graph->configs[i];
	}
	return NULL;
}

/** config의 lang.cxx.modules option을 target modules에 병합한다. */
static int
merge_modules(struct qstar_graph *graph, struct qstar_target *target,
    const struct qstar_target *options)
{
	if (!options->modules.present)
		return 0;
	target->modules.present = 1;
	if (options->modules.root) {
		if (replace_string(&target->modules.root, options->modules.root) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	if (copy_string_list(&target->modules.include, &options->modules.include) < 0 ||
	    copy_string_list(&target->modules.exclude, &options->modules.exclude) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** config의 list option을 target에 선언 순서대로 append한다. */
static int
merge_config_lists(struct qstar_graph *graph, struct qstar_target *target,
    const struct qstar_target *options)
{
#define MERGE_LIST(field) \
	do { \
		if (copy_string_list(&target->field, &options->field) < 0) \
			return qstar_set_error(graph, "qstar: out of memory"); \
	} while (0)

	MERGE_LIST(public_headers);
	MERGE_LIST(private_headers);
	MERGE_LIST(include_dirs);
	MERGE_LIST(public_include_dirs);
	MERGE_LIST(private_include_dirs);
	MERGE_LIST(interface_include_dirs);
	MERGE_LIST(system_include_dirs);
	MERGE_LIST(libs);
	MERGE_LIST(lib_dirs);
	MERGE_LIST(frameworks);
	MERGE_LIST(link_options);
	MERGE_LIST(link_inputs);
	MERGE_LIST(cflags);
	MERGE_LIST(cxxflags);
	MERGE_LIST(asm_include_dirs);
	MERGE_LIST(asm_compile_options);
#undef MERGE_LIST
	for (size_t i = 0; i < options->provider_option_len; i++) {
		const struct qstar_provider_option_value *option;

		option = &options->provider_options[i];
		if (qstar_target_set_provider_option(graph, target, option->provider,
		    option->name, option->type, option->value, &option->list) < 0)
			return -1;
	}
	return 0;
}

/** config의 scalar option을 target에 적용한다. 이후 target local scalar가 다시 override한다. */
static int
merge_config_scalars(struct qstar_graph *graph, struct qstar_target *target,
    const struct qstar_config *config)
{
	const struct qstar_target *options;

	options = &config->options;
	if (config->has_artifact_name &&
	    replace_string(&target->artifact_name, options->artifact_name) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (config->has_cxx_standard &&
	    replace_string(&target->cxx_standard, options->cxx_standard) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (config->has_toolset &&
	    replace_string(&target->toolset, options->toolset) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (config->has_toolchain &&
	    replace_string(&target->toolchain, options->toolchain) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (config->has_stdlib_policy &&
	    replace_string(&target->stdlib_policy, options->stdlib_policy) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (config->has_asm_preprocess)
		target->asm_preprocess = options->asm_preprocess;
	if (config->has_cxx_modules) {
		target->cxx_modules_present = options->cxx_modules_present;
		target->cxx_modules_enabled = options->cxx_modules_enabled;
	}
	return 0;
}

/** target의 configs label list를 reusable config 선언과 병합한다. */
int
qstar_graph_apply_target_configs(struct qstar_graph *graph, struct qstar_target *target)
{
	const struct qstar_config *config;
	size_t i;

	for (i = 0; i < target->configs.len; i++) {
		config = find_config(graph, target->configs.items[i]);
		if (!config) {
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "configs", target->label,
			    "qstar: target '%s' references unknown config '%s'",
			    target->label, target->configs.items[i]);
		}
		if (merge_config_lists(graph, target, &config->options) < 0 ||
		    merge_modules(graph, target, &config->options) < 0 ||
		    merge_config_scalars(graph, target, config) < 0)
			return -1;
	}
	return 0;
}

/** target/config의 toolset scalar가 선언된 qstar.toolset label을 가리키는지 검증한다. */
int
qstar_graph_validate_toolsets(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->config_len; i++) {
		if (graph->configs[i].has_toolset && graph->configs[i].options.toolset &&
		    *graph->configs[i].options.toolset &&
		    !has_toolset(graph, graph->configs[i].options.toolset)) {
			return qstar_set_error_origin(graph, graph->configs[i].origin_file,
			    graph->configs[i].origin_line, "toolset",
			    graph->configs[i].label,
			    "qstar: config '%s' references unknown toolset '%s'",
			    graph->configs[i].label, graph->configs[i].options.toolset);
		}
	}
	for (i = 0; i < graph->len; i++) {
		if (graph->targets[i].toolset && *graph->targets[i].toolset &&
		    !has_toolset(graph, graph->targets[i].toolset)) {
			return qstar_set_error_origin(graph, graph->targets[i].origin_file,
			    graph->targets[i].origin_line, "toolset",
			    graph->targets[i].label,
			    "qstar: target '%s' references unknown toolset '%s'",
			    graph->targets[i].label, graph->targets[i].toolset);
		}
	}
	return 0;
}

/** QStar graph에 generated action skeleton을 추가하고 중복 label을 막는다. */
struct qstar_genrule *
qstar_graph_add_genrule(struct qstar_graph *graph, const char *label, const char *name,
    const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_genrule *genrules, *genrule;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: generated action label '%s' conflicts with target",
		    label);
		return NULL;
	}
	if (has_genrule(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate generated action label '%s'", label);
		return NULL;
	}
	if (has_stage(graph, label)) {
		qstar_set_error(graph,
		    "qstar: generated action label '%s' conflicts with stage rule", label);
		return NULL;
	}
	if (has_config(graph, label)) {
		qstar_set_error(graph,
		    "qstar: generated action label '%s' conflicts with config", label);
		return NULL;
	}
	if (has_toolset(graph, label)) {
		qstar_set_error(graph,
		    "qstar: generated action label '%s' conflicts with toolset", label);
		return NULL;
	}
	if (graph->genrule_len == graph->genrule_cap) {
		cap = graph->genrule_cap ? graph->genrule_cap * 2 : 4;
		genrules = realloc(graph->genrules, cap * sizeof(graph->genrules[0]));
		if (!genrules) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->genrules = genrules;
		graph->genrule_cap = cap;
	}
	genrule = &graph->genrules[graph->genrule_len++];
	memset(genrule, 0, sizeof(*genrule));
	genrule->label = qstar_strdup(label);
	genrule->name = qstar_strdup(name);
	genrule->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	genrule->origin_file = qstar_strdup(origin_file ? origin_file : "");
	genrule->origin_line = origin_line;
	genrule->tool = qstar_strdup("generator");
	genrule->description = qstar_strdup("");
	if (!genrule->label || !genrule->name || !genrule->fragment_dir ||
	    !genrule->origin_file || !genrule->tool || !genrule->description) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return genrule;
}

/** QStar graph에 copy-only staging rule을 추가하고 중복 label을 막는다. */
struct qstar_stage *
qstar_graph_add_stage(struct qstar_graph *graph, const char *label, const char *name,
    const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_stage *stages, *stage;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: stage label '%s' conflicts with target", label);
		return NULL;
	}
	if (has_genrule(graph, label)) {
		qstar_set_error(graph,
		    "qstar: stage label '%s' conflicts with generated action", label);
		return NULL;
	}
	if (has_stage(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate stage label '%s'", label);
		return NULL;
	}
	if (has_config(graph, label)) {
		qstar_set_error(graph, "qstar: stage label '%s' conflicts with config", label);
		return NULL;
	}
	if (has_toolset(graph, label)) {
		qstar_set_error(graph, "qstar: stage label '%s' conflicts with toolset", label);
		return NULL;
	}
	if (graph->stage_len == graph->stage_cap) {
		cap = graph->stage_cap ? graph->stage_cap * 2 : 4;
		stages = realloc(graph->stages, cap * sizeof(graph->stages[0]));
		if (!stages) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->stages = stages;
		graph->stage_cap = cap;
	}
	stage = &graph->stages[graph->stage_len++];
	memset(stage, 0, sizeof(*stage));
	stage->label = qstar_strdup(label);
	stage->name = qstar_strdup(name);
	stage->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	stage->origin_file = qstar_strdup(origin_file ? origin_file : "");
	stage->origin_line = origin_line;
	stage->root = qstar_strdup("");
	stage->description = qstar_strdup("");
	if (!stage->label || !stage->name || !stage->fragment_dir ||
	    !stage->origin_file || !stage->root || !stage->description) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return stage;
}

/** target_family name이 이미 선언되었는지 확인한다. */
static int
has_target_family(const struct qstar_graph *graph, const char *name)
{
	size_t i;

	for (i = 0; i < graph->family_len; i++) {
		if (strcmp(graph->families[i].name, name) == 0)
			return 1;
	}
	return 0;
}

/** QStar target family lint grouping primitive를 추가한다. */
struct qstar_target_family *
qstar_graph_add_target_family(struct qstar_graph *graph, const char *name,
    const char *fragment_dir, const char *origin_file, int origin_line)
{
	struct qstar_target_family *families, *family;
	size_t cap;

	if (!name || !*name) {
		qstar_set_error(graph, "qstar: target_family name is empty");
		return NULL;
	}
	if (has_target_family(graph, name)) {
		qstar_set_error(graph, "qstar: duplicate target_family '%s'", name);
		return NULL;
	}
	if (graph->family_len == graph->family_cap) {
		cap = graph->family_cap ? graph->family_cap * 2 : 4;
		families = realloc(graph->families, cap * sizeof(graph->families[0]));
		if (!families) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->families = families;
		graph->family_cap = cap;
	}
	family = &graph->families[graph->family_len++];
	memset(family, 0, sizeof(*family));
	family->name = qstar_strdup(name);
	family->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	family->origin_file = qstar_strdup(origin_file ? origin_file : "");
	family->origin_line = origin_line;
	if (!family->name || !family->fragment_dir || !family->origin_file) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return family;
}

/** generated action label로 action을 찾는다. */
const struct qstar_genrule *
qstar_graph_find_genrule(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (strcmp(graph->genrules[i].label, label) == 0)
			return &graph->genrules[i];
	}
	return NULL;
}

/** qstar.target_file placeholder token에서 label과 artifact selector를 추출한다. */
int
qstar_target_file_token_parse(const char *arg, char *label, size_t labellen,
    char *artifact, size_t artifactlen)
{
	const char *prefix = "<qstar-target-file:";
	const char *payload_start, *selector;
	size_t n, payload, label_len, artifact_len;

	if (!arg || strncmp(arg, prefix, strlen(prefix)) != 0)
		return 0;
	n = strlen(arg);
	if (n <= strlen(prefix) + 1 || arg[n - 1] != '>')
		return -1;
	payload = n - strlen(prefix) - 1;
	payload_start = arg + strlen(prefix);
	selector = memchr(payload_start, '#', payload);
	label_len = selector ? (size_t)(selector - payload_start) : payload;
	artifact_len = selector ? payload - label_len - 1 : 0;
	if (label_len == 0 || label_len + 1 > labellen ||
	    (selector && artifact_len == 0) ||
	    (artifact && artifact_len + 1 > artifactlen))
		return -1;
	memcpy(label, payload_start, label_len);
	label[label_len] = '\0';
	if (artifact && artifactlen) {
		if (selector) {
			memcpy(artifact, selector + 1, artifact_len);
			artifact[artifact_len] = '\0';
		} else {
			artifact[0] = '\0';
		}
	}
	return 1;
}

/** qstar.target_file placeholder token에서 canonical label을 추출한다. */
int
qstar_target_file_token_label(const char *arg, char *label, size_t labellen)
{
	return qstar_target_file_token_parse(arg, label, labellen, NULL, 0);
}

/** qstar.stage_dir placeholder token에서 canonical stage label을 추출한다. */
int
qstar_stage_dir_token_label(const char *arg, char *label, size_t labellen)
{
	const char *prefix = "<qstar-stage-dir:";
	const char *payload_start;
	size_t n, payload;

	if (!arg || strncmp(arg, prefix, strlen(prefix)) != 0)
		return 0;
	n = strlen(arg);
	if (n <= strlen(prefix) + 1 || arg[n - 1] != '>')
		return -1;
	payload = n - strlen(prefix) - 1;
	payload_start = arg + strlen(prefix);
	if (payload == 0 || payload + 1 > labellen)
		return -1;
	memcpy(label, payload_start, payload);
	label[payload] = '\0';
	return 1;
}

int
qstar_provider_tool_token_role(const char *arg, char *role, size_t rolelen)
{
	const char *prefix = "<qstar-tool:";
	size_t n, payload;

	if (!arg || strncmp(arg, prefix, strlen(prefix)) != 0)
		return 0;
	n = strlen(arg);
	if (n <= strlen(prefix) + 1 || arg[n - 1] != '>')
		return -1;
	payload = n - strlen(prefix) - 1;
	if (payload + 1 > rolelen)
		return -1;
	memcpy(role, arg + strlen(prefix), payload);
	role[payload] = '\0';
	return 1;
}

/** stage/package rule label로 staging rule을 찾는다. */
const struct qstar_stage *
qstar_graph_find_stage(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->stage_len; i++) {
		if (strcmp(graph->stages[i].label, label) == 0)
			return &graph->stages[i];
	}
	return NULL;
}

/** metadata list에서 output index에 맞는 값을 가져온다. */
static const char *
genrule_meta_or_default(const struct qstar_string_list *list, size_t index,
    const char *fallback)
{
	if (index < list->len && list->items[index] && *list->items[index])
		return list->items[index];
	return fallback;
}

/** generated output metadata의 format을 기본값 포함해 반환한다. */
const char *
qstar_genrule_output_format(const struct qstar_genrule *genrule, size_t index)
{
	return genrule_meta_or_default(&genrule->output_formats, index, "file");
}

/** generated output metadata의 output group을 기본값 포함해 반환한다. */
const char *
qstar_genrule_output_group(const struct qstar_genrule *genrule, size_t index)
{
	const char *group;

	group = genrule_meta_or_default(&genrule->output_groups, index, "");
	if (*group)
		return group;
	if (strcmp(qstar_genrule_output_format(genrule, index), "object") == 0)
		return "objects";
	return "generated";
}

/** generated output path와 generic metadata를 action identity로 만든다. */
int
qstar_genrule_output_identity(const struct qstar_genrule *genrule, size_t index,
    char *dst, size_t dstlen)
{
	if (index >= genrule->outputs.len)
		return -1;
	return snprintf(dst, dstlen, "%s|group=%s|format=%s",
	    genrule->outputs.items[index], qstar_genrule_output_group(genrule, index),
	    qstar_genrule_output_format(genrule, index)) < (int)dstlen ? 0 : -1;
}

/** generated output identity list를 action key material로 만든다. */
int
qstar_genrule_output_identity_list(const struct qstar_genrule *genrule, char *dst,
    size_t dstlen)
{
	char identity[QSTAR_PATH_MAX];
	size_t i, used, n;

	if (!dstlen)
		return -1;
	used = 0;
	dst[used++] = '[';
	dst[used] = '\0';
	for (i = 0; i < genrule->outputs.len; i++) {
		if (qstar_genrule_output_identity(genrule, i, identity, sizeof(identity)) < 0)
			return -1;
		n = snprintf(dst + used, dstlen - used, "%s%s", i ? "," : "", identity);
		if (n >= dstlen - used)
			return -1;
		used += n;
	}
	if (used + 2 > dstlen)
		return -1;
	dst[used++] = ']';
	dst[used] = '\0';
	return 0;
}

/** generated output path를 생산하는 action skeleton을 찾는다. */
const struct qstar_genrule *
qstar_graph_find_output_owner(const struct qstar_graph *graph, const char *path)
{
	size_t i, j;

	for (i = 0; i < graph->genrule_len; i++) {
		for (j = 0; j < graph->genrules[i].outputs.len; j++) {
			if (strcmp(graph->genrules[i].outputs.items[j], path) == 0)
				return &graph->genrules[i];
		}
	}
	return NULL;
}

static const char *
context_or_default(const char *s, const char *fallback)
{
	return s && *s ? s : fallback;
}

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

/** JSON string literal을 escaping해 출력한다. */
static void
dump_json_string(FILE *out, const char *s)
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

/** string list를 JSON array로 출력한다. */
static void
dump_json_list(FILE *out, const struct qstar_string_list *list)
{
	size_t i;

	fputc('[', out);
	for (i = 0; i < list->len; i++) {
		if (i)
			fputc(',', out);
		dump_json_string(out, list->items[i]);
	}
	fputc(']', out);
}

void
qstar_dump_target_artifact_map_text(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target, const char *indent)
{
	struct qstar_target_artifact_map map;
	size_t i;

	if (qstar_graph_target_artifact_map(graph, target, &map) < 0)
		return;
	for (i = 0; i < map.len; i++) {
		fprintf(out,
		    "%sartifact id=%s role=%s path=%s install_dir=%s primary=%s installable=%s\n",
		    indent ? indent : "", map.items[i].id, map.items[i].role,
		    map.items[i].path,
		    map.items[i].install_dir[0] ? map.items[i].install_dir : "<none>",
		    map.items[i].primary ? "true" : "false",
		    map.items[i].installable ? "true" : "false");
	}
}

static void
dump_target_artifact_map_json(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	struct qstar_target_artifact_map map;
	size_t i;

	fputc('[', out);
	if (qstar_graph_target_artifact_map(graph, target, &map) == 0) {
		for (i = 0; i < map.len; i++) {
			if (i)
				fputc(',', out);
			fputs("{\"id\":", out);
			dump_json_string(out, map.items[i].id);
			fputs(",\"role\":", out);
			dump_json_string(out, map.items[i].role);
			fputs(",\"path\":", out);
			dump_json_string(out, map.items[i].path);
			fputs(",\"install_dir\":", out);
			dump_json_string(out, map.items[i].install_dir);
			fprintf(out, ",\"primary\":%s,\"installable\":%s}",
			    map.items[i].primary ? "true" : "false",
			    map.items[i].installable ? "true" : "false");
		}
	}
	fputc(']', out);
}

static void
dump_package_aliases(FILE *out, const struct qstar_graph *graph)
{
	size_t i;

	fputs("package_aliases [", out);
	for (i = 0; i < graph->package_len; i++) {
		if (i)
			fputs(", ", out);
		fprintf(out, "%s=%s", graph->packages[i].alias, graph->packages[i].root);
	}
	fputs("]\n", out);
}

static void
dump_target(const struct qstar_graph *graph, const struct qstar_target *target, FILE *out)
{
	char package[QSTAR_PATH_MAX];

	if (qstar_label_package_path(target->label, package, sizeof(package)) < 0)
		snprintf(package, sizeof(package), "<external>");
	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    target->origin_file && *target->origin_file ? target->origin_file : "<unknown>",
	    target->origin_line);
	fprintf(out, "  package %s\n", package[0] ? package : "<root>");
	fprintf(out, "  kind %s\n", target->kind);
	fprintf(out, "  rule provider=%s final_action=%s output_group=%s\n",
	    qstar_target_rule_lookup(target->kind) ?
	    qstar_target_rule_lookup(target->kind)->provider : "generic",
	    qstar_target_final_action(target), qstar_target_output_group(target));
	if (qstar_target_has_provider_final_action(target))
		fprintf(out, "  provider_final provider=%s kind=%s lower=%s\n",
		    target->provider_final.provider ? target->provider_final.provider :
		    "<unknown>",
		    target->provider_final.kind ? target->provider_final.kind : "<unknown>",
		    target->provider_final.lower ? target->provider_final.lower :
		    "<unknown>");
	fputs("  configs ", out);
	dump_list(out, &target->configs);
	fputc('\n', out);
	if (target->modules.present) {
		fprintf(out, "  lang.cxx.modules root=%s include=",
		    target->modules.root ? target->modules.root : "");
		dump_list(out, &target->modules.include);
		fputs(" exclude=", out);
		dump_list(out, &target->modules.exclude);
		fputc('\n', out);
	}
	fputs("  sources ", out);
	dump_list(out, &target->sources);
	fputc('\n', out);
	fputs("  public_headers ", out);
	dump_list(out, &target->public_headers);
	fputc('\n', out);
	fputs("  private_headers ", out);
	dump_list(out, &target->private_headers);
	fputc('\n', out);
	fputs("  include_dirs ", out);
	dump_list(out, &target->include_dirs);
	fputc('\n', out);
	fputs("  public_include_dirs ", out);
	dump_list(out, &target->public_include_dirs);
	fputc('\n', out);
	fputs("  private_include_dirs ", out);
	dump_list(out, &target->private_include_dirs);
	fputc('\n', out);
	fputs("  interface_include_dirs ", out);
	dump_list(out, &target->interface_include_dirs);
	fputc('\n', out);
	fputs("  system_include_dirs ", out);
	dump_list(out, &target->system_include_dirs);
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
	fputs("  lang.asm.include_dirs ", out);
	dump_list(out, &target->asm_include_dirs);
	fputc('\n', out);
	fputs("  lang.asm.compile_options ", out);
	dump_list(out, &target->asm_compile_options);
	fputc('\n', out);
	fprintf(out, "  lang.asm.preprocess %s\n", target->asm_preprocess ? "true" : "false");
	fprintf(out, "  lang.cxx.modules enabled=%s\n",
	    target->cxx_modules_enabled ? "true" : "false");
	fputs("  run.inputs ", out);
	dump_list(out, &target->run_inputs);
	fputc('\n', out);
	fputs("  run.command ", out);
	dump_list(out, &target->run_command);
	fputc('\n', out);
	fprintf(out, "  description %s\n",
	    target->description && *target->description ? target->description : "<default>");
	fprintf(out, "  run.timeout_sec %d\n", target->run_timeout_sec);
	fprintf(out, "  run.expect.contains %s\n",
	    target->run_expect_contains ? target->run_expect_contains : "");
	fprintf(out, "  run.expect.file %s\n",
	    target->run_expect_file && *target->run_expect_file ? target->run_expect_file : "");
	fprintf(out, "  artifact_name %s\n",
	    target->artifact_name && *target->artifact_name ? target->artifact_name :
	    "<default>");
	qstar_dump_target_artifact_map_text(out, graph, target, "  ");
	fprintf(out, "  toolset %s\n",
	    target->toolset && *target->toolset ? target->toolset : "<default>");
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
}

/** reusable config declaration을 Graph IR dump 형식으로 출력한다. */
static void
dump_config(const struct qstar_config *config, FILE *out)
{
	const struct qstar_target *options;

	options = &config->options;
	fprintf(out, "config %s\n", config->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    config->origin_file && *config->origin_file ? config->origin_file : "<unknown>",
	    config->origin_line);
	fprintf(out, "  fragment_dir %s\n", config->fragment_dir);
	fputs("  public_headers ", out);
	dump_list(out, &options->public_headers);
	fputc('\n', out);
	fputs("  private_headers ", out);
	dump_list(out, &options->private_headers);
	fputc('\n', out);
	fputs("  include_dirs ", out);
	dump_list(out, &options->include_dirs);
	fputc('\n', out);
	fputs("  public_include_dirs ", out);
	dump_list(out, &options->public_include_dirs);
	fputc('\n', out);
	fputs("  private_include_dirs ", out);
	dump_list(out, &options->private_include_dirs);
	fputc('\n', out);
	fputs("  system_include_dirs ", out);
	dump_list(out, &options->system_include_dirs);
	fputc('\n', out);
	fputs("  libs ", out);
	dump_list(out, &options->libs);
	fputc('\n', out);
	fputs("  lib_dirs ", out);
	dump_list(out, &options->lib_dirs);
	fputc('\n', out);
	fputs("  link.frameworks ", out);
	dump_list(out, &options->frameworks);
	fputc('\n', out);
	fputs("  link_options ", out);
	dump_list(out, &options->link_options);
	fputc('\n', out);
	fputs("  link_inputs ", out);
	dump_list(out, &options->link_inputs);
	fputc('\n', out);
	fputs("  cflags ", out);
	dump_list(out, &options->cflags);
	fputc('\n', out);
	fputs("  cxxflags ", out);
	dump_list(out, &options->cxxflags);
	fputc('\n', out);
	fprintf(out, "  cxx_standard %s\n",
	    config->has_cxx_standard && options->cxx_standard ?
	    options->cxx_standard : "<unset>");
	fputs("  lang.asm.include_dirs ", out);
	dump_list(out, &options->asm_include_dirs);
	fputc('\n', out);
	fputs("  lang.asm.compile_options ", out);
	dump_list(out, &options->asm_compile_options);
	fputc('\n', out);
	fprintf(out, "  lang.asm.preprocess %s\n",
	    config->has_asm_preprocess ? (options->asm_preprocess ? "true" : "false") :
	    "<unset>");
	fprintf(out, "  lang.cxx.modules enabled=%s\n",
	    config->has_cxx_modules ? (options->cxx_modules_enabled ? "true" : "false") :
	    "<unset>");
	if (options->modules.present) {
		fprintf(out, "  lang.cxx.modules root=%s include=",
		    options->modules.root ? options->modules.root : "");
		dump_list(out, &options->modules.include);
		fputs(" exclude=", out);
		dump_list(out, &options->modules.exclude);
		fputc('\n', out);
	}
	fprintf(out, "  artifact_name %s\n",
	    config->has_artifact_name && options->artifact_name && *options->artifact_name ?
	    options->artifact_name : "<unset>");
	fprintf(out, "  toolset %s\n",
	    config->has_toolset && options->toolset && *options->toolset ? options->toolset :
	    "<unset>");
	fprintf(out, "  toolchain %s\n",
	    config->has_toolchain && options->toolchain ? options->toolchain : "<unset>");
	fprintf(out, "  stdlib %s\n",
	    config->has_stdlib_policy && options->stdlib_policy ? options->stdlib_policy :
	    "<unset>");
}

/** generated action skeleton을 Graph IR dump 형식으로 출력한다. */
static void
dump_genrule(const struct qstar_genrule *genrule, FILE *out)
{
	char identity[QSTAR_PATH_MAX];
	size_t i;

	fprintf(out, "generated_action %s\n", genrule->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    genrule->origin_file && *genrule->origin_file ? genrule->origin_file : "<unknown>",
	    genrule->origin_line);
	fprintf(out, "  tool %s\n", genrule->tool);
	fprintf(out, "  description %s\n",
	    genrule->description && *genrule->description ? genrule->description : "<default>");
	fprintf(out, "  config_header %s\n", genrule->config_header ? "yes" : "no");
	fputs("  inputs ", out);
	dump_list(out, &genrule->inputs);
	fputc('\n', out);
	fputs("  outputs ", out);
	dump_list(out, &genrule->outputs);
	fputc('\n', out);
	for (i = 0; i < genrule->outputs.len; i++) {
		if (qstar_genrule_output_identity(genrule, i, identity,
		    sizeof(identity)) < 0)
			snprintf(identity, sizeof(identity), "<too-long>");
		fprintf(out,
		    "  output_artifact path=%s group=%s format=%s identity=%s\n",
		    genrule->outputs.items[i], qstar_genrule_output_group(genrule, i),
		    qstar_genrule_output_format(genrule, i), identity);
	}
	fputs("  args ", out);
	dump_list(out, &genrule->args);
	fputc('\n', out);
	fputs("  command ", out);
	dump_list(out, &genrule->command);
	fputc('\n', out);
}

/** copy-only stage rule을 Graph IR dump 형식으로 출력한다. */
static void
dump_stage(const struct qstar_stage *stage, FILE *out)
{
	size_t i;

	fprintf(out, "stage %s\n", stage->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    stage->origin_file && *stage->origin_file ? stage->origin_file : "<unknown>",
	    stage->origin_line);
	fprintf(out, "  root %s\n", stage->root && *stage->root ? stage->root : "<default>");
	fprintf(out, "  description %s\n",
	    stage->description && *stage->description ? stage->description : "<default>");
	for (i = 0; i < stage->srcs.len; i++)
		fprintf(out, "  stage_file src=%s dst=%s\n", stage->srcs.items[i],
		    i < stage->dsts.len ? stage->dsts.items[i] : "<missing>");
}

/** target_family lint grouping primitive를 Graph IR dump 형식으로 출력한다. */
static void
dump_target_family(const struct qstar_target_family *family, FILE *out)
{
	fprintf(out, "target_family %s\n", family->name);
	fprintf(out, "  origin file=%s line=%d\n",
	    family->origin_file && *family->origin_file ? family->origin_file : "<unknown>",
	    family->origin_line);
	fprintf(out, "  allow_shared_sources %s\n",
	    family->allow_shared_sources ? "true" : "false");
	fputs("  variants ", out);
	dump_list(out, &family->variants);
	fputc('\n', out);
	fputs("  targets ", out);
	dump_list(out, &family->targets);
	fputc('\n', out);
}

/** toolset declaration을 Graph IR dump 형식으로 출력한다. */
static void
dump_toolset(const struct qstar_toolset *toolset, FILE *out)
{
	size_t i;

	fprintf(out, "toolset %s\n", toolset->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    toolset->origin_file && *toolset->origin_file ? toolset->origin_file :
	    "<unknown>", toolset->origin_line);
	fprintf(out, "  fragment_dir %s\n", toolset->fragment_dir);
	for (i = 0; i < toolset->role_len; i++) {
		fprintf(out, "  tools.%s ", toolset->roles[i].role);
		dump_list(out, &toolset->roles[i].argv);
		fputc('\n', out);
	}
	fprintf(out, "  response_files %s\n",
	    toolset->response_files && *toolset->response_files ? toolset->response_files :
	    "auto");
	fprintf(out, "  response_style %s\n",
	    toolset->response_style && *toolset->response_style ? toolset->response_style :
	    "auto");
	fprintf(out, "  allow_absolute_tools %s\n",
	    toolset->allow_absolute_tools && *toolset->allow_absolute_tools ?
	    toolset->allow_absolute_tools : "false");
	fputs("  path_tools ", out);
	dump_list(out, &toolset->path_tools);
	fputc('\n', out);
}

/** activated language provider registry entry를 Graph IR dump 형식으로 출력한다. */
static void
dump_language_provider(const struct qstar_language_provider *provider, FILE *out)
{
	size_t i;

	fprintf(out,
	    "language_provider namespace=%s id=%s api=%s version=%s dir=%s manifest=%s implementation=%s\n",
	    provider->namespace && *provider->namespace ? provider->namespace : "<unknown>",
	    provider->id && *provider->id ? provider->id : "<unknown>",
	    provider->api && *provider->api ? provider->api : "<unknown>",
	    provider->version && *provider->version ? provider->version : "<unspecified>",
	    provider->dir && *provider->dir ? provider->dir : "<unknown>",
	    provider->manifest && *provider->manifest ? provider->manifest : "<unknown>",
	    provider->implementation && *provider->implementation ?
	    provider->implementation : "<unknown>");
	for (i = 0; i < provider->unit_len; i++) {
		fprintf(out, "  unit %s emits=%s lower=%s deps=%s suffixes ",
		    provider->units[i].name ? provider->units[i].name : "<unknown>",
		    provider->units[i].emits ? provider->units[i].emits : "<unknown>",
		    provider->units[i].lower ? provider->units[i].lower : "<unknown>",
		    provider->units[i].deps && *provider->units[i].deps ?
		    provider->units[i].deps : "<none>");
		dump_list(out, &provider->units[i].suffixes);
		fputc('\n', out);
	}
	for (i = 0; i < provider->final_len; i++)
		fprintf(out, "  final %s lower=%s\n",
		    provider->finals[i].kind ? provider->finals[i].kind : "<unknown>",
		    provider->finals[i].lower ? provider->finals[i].lower : "<unknown>");
}

/** QStar Graph IR를 deterministic explain text로 출력한다. */
int
qstar_graph_dump(const struct qstar_graph *graph, const char *label, FILE *out)
{
	struct qstar_target *copy;
	size_t i, n;
	int found;

	copy = NULL;
	n = graph->len;
	if (n) {
		copy = malloc(n * sizeof(copy[0]));
		if (!copy)
			return -1;
		memcpy(copy, graph->targets, n * sizeof(copy[0]));
		qsort(copy, n, sizeof(copy[0]), target_cmp);
	}
	fputs("qstar graph v1\n", out);
	fprintf(out,
	    "project name=%s version=%s root=%s build_dir=%s generated_dir=%s compile_commands=%s generator=%s requested_generator=%s\n",
	    graph->project.name && *graph->project.name ? graph->project.name : "<unnamed>",
	    graph->project.version && *graph->project.version ?
	    graph->project.version : "<unspecified>",
	    graph->project.root && *graph->project.root ? graph->project.root : ".",
	    qstar_graph_build_dir(graph), qstar_graph_generated_dir(graph),
	    qstar_graph_compile_commands_policy(graph), qstar_graph_generator(graph),
	    qstar_graph_requested_generator(graph));
	fprintf(out, "build_context name=%s target=%s platform=%s toolchain=%s stdlib=%s\n",
	    context_or_default(graph->build_context.name, "default"),
	    context_or_default(graph->build_context.target, "host"),
	    qstar_graph_platform(graph),
	    context_or_default(graph->build_context.toolchain, "default"),
	    context_or_default(graph->build_context.stdlib_policy, "default"));
	fprintf(out, "build_context_tools cc=%s cxx=%s ar=%s linker=%s sysroot=%s resource_dir=%s\n",
	    graph->build_context.cc ? graph->build_context.cc : "<default>",
	    graph->build_context.cxx ? graph->build_context.cxx : "<default>",
	    graph->build_context.ar ? graph->build_context.ar : "<default>",
	    graph->build_context.linker ? graph->build_context.linker : "<default>",
	    graph->build_context.sysroot ? graph->build_context.sysroot : "<none>",
	    graph->build_context.resource_dir ? graph->build_context.resource_dir : "<none>");
	fprintf(out, "build_context_response response_files=%s response_style=%s\n",
	    graph->build_context.response_files ? graph->build_context.response_files : "auto",
	    graph->build_context.response_style ? graph->build_context.response_style : "auto");
	fputs("build_context_link link_options=", out);
	dump_list(out, &graph->build_context.link_options);
	fputc('\n', out);
	fputs("build_context_compile compile_options=", out);
	dump_list(out, &graph->build_context.compile_options);
	fputs(" include_dirs=", out);
	dump_list(out, &graph->build_context.include_dirs);
	fputc('\n', out);
	fprintf(out, "external_tool_policy allow_absolute=%s path_tools=",
	    graph->build_context.allow_absolute_tools ? graph->build_context.allow_absolute_tools : "false");
	dump_list(out, &graph->build_context.path_tools);
	fputs(" tool_overrides=", out);
	dump_list(out, &graph->build_context.tool_overrides);
	fputc('\n', out);
	dump_package_aliases(out, graph);
	for (i = 0; i < graph->language_provider_len; i++)
		dump_language_provider(&graph->language_providers[i], out);
	for (i = 0; i < graph->toolset_len; i++)
		dump_toolset(&graph->toolsets[i], out);
	for (i = 0; i < graph->config_len; i++)
		dump_config(&graph->configs[i], out);
	for (i = 0; i < graph->genrule_len; i++)
		dump_genrule(&graph->genrules[i], out);
	for (i = 0; i < graph->stage_len; i++)
		dump_stage(&graph->stages[i], out);
	for (i = 0; i < graph->family_len; i++)
		dump_target_family(&graph->families[i], out);
	found = label == NULL || *label == '\0';
	for (i = 0; i < n; i++) {
		if (label && *label && strcmp(copy[i].label, label) != 0)
			continue;
		found = 1;
		dump_target(graph, &copy[i], out);
	}
	free(copy);
	return found ? 0 : -1;
}

/** target pointer list를 canonical label 순서로 정렬한다. */
static void
sort_target_ptrs(const struct qstar_target **targets, size_t n)
{
	size_t i, j;
	const struct qstar_target *v;

	for (i = 1; i < n; i++) {
		v = targets[i];
		j = i;
		while (j > 0 && strcmp(targets[j - 1]->label, v->label) > 0) {
			targets[j] = targets[j - 1];
			j--;
		}
		targets[j] = v;
	}
}

/** toolset pointer list를 canonical label 순서로 정렬한다. */
static void
sort_toolset_ptrs(const struct qstar_toolset **toolsets, size_t n)
{
	size_t i, j;
	const struct qstar_toolset *v;

	for (i = 1; i < n; i++) {
		v = toolsets[i];
		j = i;
		while (j > 0 && strcmp(toolsets[j - 1]->label, v->label) > 0) {
			toolsets[j] = toolsets[j - 1];
			j--;
		}
		toolsets[j] = v;
	}
}

/** language provider pointer list를 namespace 순서로 정렬한다. */
static void
sort_language_provider_ptrs(const struct qstar_language_provider **providers, size_t n)
{
	size_t i, j;
	const struct qstar_language_provider *v;

	for (i = 1; i < n; i++) {
		v = providers[i];
		j = i;
		while (j > 0 && strcmp(providers[j - 1]->namespace, v->namespace) > 0) {
			providers[j] = providers[j - 1];
			j--;
		}
		providers[j] = v;
	}
}

/** config pointer list를 canonical label 순서로 정렬한다. */
static void
sort_config_ptrs(const struct qstar_config **configs, size_t n)
{
	size_t i, j;
	const struct qstar_config *v;

	for (i = 1; i < n; i++) {
		v = configs[i];
		j = i;
		while (j > 0 && strcmp(configs[j - 1]->label, v->label) > 0) {
			configs[j] = configs[j - 1];
			j--;
		}
		configs[j] = v;
	}
}

/** generated action pointer list를 canonical label 순서로 정렬한다. */
static void
sort_genrule_ptrs(const struct qstar_genrule **genrules, size_t n)
{
	size_t i, j;
	const struct qstar_genrule *v;

	for (i = 1; i < n; i++) {
		v = genrules[i];
		j = i;
		while (j > 0 && strcmp(genrules[j - 1]->label, v->label) > 0) {
			genrules[j] = genrules[j - 1];
			j--;
		}
		genrules[j] = v;
	}
}

/** stage pointer list를 canonical label 순서로 정렬한다. */
static void
sort_stage_ptrs(const struct qstar_stage **stages, size_t n)
{
	size_t i, j;
	const struct qstar_stage *v;

	for (i = 1; i < n; i++) {
		v = stages[i];
		j = i;
		while (j > 0 && strcmp(stages[j - 1]->label, v->label) > 0) {
			stages[j] = stages[j - 1];
			j--;
		}
		stages[j] = v;
	}
}

/** target_family pointer list를 canonical name 순서로 정렬한다. */
static void
sort_family_ptrs(const struct qstar_target_family **families, size_t n)
{
	size_t i, j;
	const struct qstar_target_family *v;

	for (i = 1; i < n; i++) {
		v = families[i];
		j = i;
		while (j > 0 && strcmp(families[j - 1]->name, v->name) > 0) {
			families[j] = families[j - 1];
			j--;
		}
		families[j] = v;
	}
}

/** target label에 대응하는 target을 찾는다. */
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

/** QStar target 목록을 deterministic text로 출력한다. */
int
qstar_graph_list_targets(const struct qstar_graph *graph, FILE *out)
{
	const struct qstar_target **targets;
	const struct qstar_config **configs;
	const struct qstar_toolset **toolsets;
	const struct qstar_language_provider **providers;
	const struct qstar_stage **stages;
	const struct qstar_target_family **families;
	size_t i;

	targets = malloc((graph->len ? graph->len : 1) * sizeof(targets[0]));
	configs = malloc((graph->config_len ? graph->config_len : 1) * sizeof(configs[0]));
	toolsets = malloc((graph->toolset_len ? graph->toolset_len : 1) *
	    sizeof(toolsets[0]));
	providers = malloc((graph->language_provider_len ? graph->language_provider_len : 1) *
	    sizeof(providers[0]));
	stages = malloc((graph->stage_len ? graph->stage_len : 1) * sizeof(stages[0]));
	families = malloc((graph->family_len ? graph->family_len : 1) * sizeof(families[0]));
	if (!targets || !configs || !toolsets || !providers || !stages || !families) {
		free(targets);
		free(configs);
		free(toolsets);
		free(providers);
		free(stages);
		free(families);
		return -1;
	}
	for (i = 0; i < graph->len; i++)
		targets[i] = &graph->targets[i];
	for (i = 0; i < graph->config_len; i++)
		configs[i] = &graph->configs[i];
	for (i = 0; i < graph->toolset_len; i++)
		toolsets[i] = &graph->toolsets[i];
	for (i = 0; i < graph->language_provider_len; i++)
		providers[i] = &graph->language_providers[i];
	for (i = 0; i < graph->stage_len; i++)
		stages[i] = &graph->stages[i];
	for (i = 0; i < graph->family_len; i++)
		families[i] = &graph->families[i];
	sort_target_ptrs(targets, graph->len);
	sort_config_ptrs(configs, graph->config_len);
	sort_toolset_ptrs(toolsets, graph->toolset_len);
	sort_language_provider_ptrs(providers, graph->language_provider_len);
	sort_stage_ptrs(stages, graph->stage_len);
	sort_family_ptrs(families, graph->family_len);
	fputs("qstar targets v1\n", out);
	fprintf(out, "target-count %zu\n", graph->len);
	for (i = 0; i < graph->len; i++) {
		fprintf(out, "target %s kind=%s origin=%s:%d\n", targets[i]->label,
		    targets[i]->kind,
		    targets[i]->origin_file && *targets[i]->origin_file ?
		    targets[i]->origin_file : "<unknown>",
		    targets[i]->origin_line);
		qstar_dump_target_artifact_map_text(out, graph, targets[i], "  ");
	}
	fprintf(out, "config-count %zu\n", graph->config_len);
	for (i = 0; i < graph->config_len; i++)
		fprintf(out, "config %s origin=%s:%d\n", configs[i]->label,
		    configs[i]->origin_file && *configs[i]->origin_file ?
		    configs[i]->origin_file : "<unknown>",
		    configs[i]->origin_line);
	fprintf(out, "toolset-count %zu\n", graph->toolset_len);
	for (i = 0; i < graph->toolset_len; i++)
		fprintf(out, "toolset %s origin=%s:%d\n", toolsets[i]->label,
		    toolsets[i]->origin_file && *toolsets[i]->origin_file ?
		    toolsets[i]->origin_file : "<unknown>",
		    toolsets[i]->origin_line);
	fprintf(out, "language-provider-count %zu\n", graph->language_provider_len);
	for (i = 0; i < graph->language_provider_len; i++)
		fprintf(out, "language_provider namespace=%s id=%s manifest=%s implementation=%s\n",
		    providers[i]->namespace, providers[i]->id, providers[i]->manifest,
		    providers[i]->implementation);
	fprintf(out, "stage-count %zu\n", graph->stage_len);
	for (i = 0; i < graph->stage_len; i++)
		fprintf(out, "stage %s root=%s origin=%s:%d\n", stages[i]->label,
		    stages[i]->root && *stages[i]->root ? stages[i]->root : "<default>",
		    stages[i]->origin_file && *stages[i]->origin_file ?
		    stages[i]->origin_file : "<unknown>",
		    stages[i]->origin_line);
	fprintf(out, "target-family-count %zu\n", graph->family_len);
	for (i = 0; i < graph->family_len; i++)
		fprintf(out, "target_family %s allow_shared_sources=%s origin=%s:%d\n",
		    families[i]->name, families[i]->allow_shared_sources ? "true" : "false",
		    families[i]->origin_file && *families[i]->origin_file ?
			    families[i]->origin_file : "<unknown>",
			    families[i]->origin_line);
	free(targets);
	free(configs);
	free(toolsets);
	free(providers);
	free(stages);
	free(families);
	return 0;
}

/** target 하나를 machine-readable JSON record로 출력한다. */
static void
dump_target_json(FILE *out, const struct qstar_graph *graph,
    const struct qstar_target *target)
{
	fputs("{\"label\":", out);
	dump_json_string(out, target->label);
	fputs(",\"name\":", out);
	dump_json_string(out, target->name);
	fputs(",\"kind\":", out);
	dump_json_string(out, target->kind);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, target->origin_file && *target->origin_file ?
	    target->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", target->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, target->fragment_dir);
	fputs(",\"configs\":", out);
	dump_json_list(out, &target->configs);
	fputs(",\"sources\":", out);
	dump_json_list(out, &target->sources);
	fputs(",\"public_headers\":", out);
	dump_json_list(out, &target->public_headers);
	fputs(",\"deps\":", out);
	dump_json_list(out, &target->deps);
	fputs(",\"private_deps\":", out);
	dump_json_list(out, &target->private_deps);
	fputs(",\"toolchain\":", out);
	dump_json_string(out, target->toolchain);
	fputs(",\"toolset\":", out);
	dump_json_string(out, target->toolset && *target->toolset ? target->toolset : "");
	fputs(",\"artifact_name\":", out);
	dump_json_string(out, target->artifact_name && *target->artifact_name ?
	    target->artifact_name : "");
	fputs(",\"artifacts\":", out);
	dump_target_artifact_map_json(out, graph, target);
	fputs(",\"cxx_standard\":", out);
	dump_json_string(out, target->cxx_standard);
	fputs(",\"lang_cxx_standard\":", out);
	dump_json_string(out, target->cxx_standard);
	fputs(",\"lang_asm_preprocess\":", out);
	fprintf(out, "%s", target->asm_preprocess ? "true" : "false");
	fputs(",\"description\":", out);
	dump_json_string(out, target->description && *target->description ?
	    target->description : "");
	fputs(",\"run_command\":", out);
	dump_json_list(out, &target->run_command);
	fputs(",\"run_inputs\":", out);
	dump_json_list(out, &target->run_inputs);
	fprintf(out, ",\"run_timeout_sec\":%d", target->run_timeout_sec);
	fputs(",\"run_expect_contains\":", out);
	dump_json_string(out, target->run_expect_contains ? target->run_expect_contains : "");
	fputs(",\"run_expect_file\":", out);
	dump_json_string(out, target->run_expect_file ? target->run_expect_file : "");
	fprintf(out, ",\"is_test\":%s", strcmp(target->kind, "test") == 0 ? "true" : "false");
	fprintf(out, ",\"installable\":%s", qstar_target_is_installable(target) ? "true" : "false");
	fputc('}', out);
}

/** reusable config 하나를 machine-readable JSON record로 출력한다. */
static void
dump_config_json(FILE *out, const struct qstar_config *config)
{
	const struct qstar_target *options;

	options = &config->options;
	fputs("{\"label\":", out);
	dump_json_string(out, config->label);
	fputs(",\"name\":", out);
	dump_json_string(out, config->name);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, config->origin_file && *config->origin_file ?
	    config->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", config->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, config->fragment_dir);
	fputs(",\"public_headers\":", out);
	dump_json_list(out, &options->public_headers);
	fputs(",\"private_headers\":", out);
	dump_json_list(out, &options->private_headers);
	fputs(",\"include_dirs\":", out);
	dump_json_list(out, &options->include_dirs);
	fputs(",\"public_include_dirs\":", out);
	dump_json_list(out, &options->public_include_dirs);
	fputs(",\"private_include_dirs\":", out);
	dump_json_list(out, &options->private_include_dirs);
	fputs(",\"system_include_dirs\":", out);
	dump_json_list(out, &options->system_include_dirs);
	fputs(",\"cflags\":", out);
	dump_json_list(out, &options->cflags);
	fputs(",\"cxxflags\":", out);
	dump_json_list(out, &options->cxxflags);
	fputs(",\"asm_include_dirs\":", out);
	dump_json_list(out, &options->asm_include_dirs);
	fputs(",\"asm_compile_options\":", out);
	dump_json_list(out, &options->asm_compile_options);
	fputs(",\"libs\":", out);
	dump_json_list(out, &options->libs);
	fputs(",\"lib_dirs\":", out);
	dump_json_list(out, &options->lib_dirs);
	fputs(",\"link\":{\"frameworks\":", out);
	dump_json_list(out, &options->frameworks);
	fputc('}', out);
	fputs(",\"link_options\":", out);
	dump_json_list(out, &options->link_options);
	fputs(",\"link_inputs\":", out);
	dump_json_list(out, &options->link_inputs);
	fputs(",\"has_toolchain\":", out);
	fprintf(out, "%s", config->has_toolchain ? "true" : "false");
	fputs(",\"has_toolset\":", out);
	fprintf(out, "%s", config->has_toolset ? "true" : "false");
	fputs(",\"toolset\":", out);
	dump_json_string(out, config->has_toolset && options->toolset ? options->toolset : "");
	fputs(",\"toolchain\":", out);
	dump_json_string(out, config->has_toolchain && options->toolchain ?
	    options->toolchain : "");
	fputs(",\"has_stdlib\":", out);
	fprintf(out, "%s", config->has_stdlib_policy ? "true" : "false");
	fputs(",\"stdlib\":", out);
	dump_json_string(out, config->has_stdlib_policy && options->stdlib_policy ?
	    options->stdlib_policy : "");
	fputs(",\"has_artifact_name\":", out);
	fprintf(out, "%s", config->has_artifact_name ? "true" : "false");
	fputs(",\"artifact_name\":", out);
	dump_json_string(out, config->has_artifact_name && options->artifact_name ?
	    options->artifact_name : "");
	fputs(",\"has_cxx_standard\":", out);
	fprintf(out, "%s", config->has_cxx_standard ? "true" : "false");
	fputs(",\"cxx_standard\":", out);
	dump_json_string(out, config->has_cxx_standard && options->cxx_standard ?
	    options->cxx_standard : "");
	fputs(",\"has_asm_preprocess\":", out);
	fprintf(out, "%s", config->has_asm_preprocess ? "true" : "false");
	fputs(",\"asm_preprocess\":", out);
	fprintf(out, "%s", options->asm_preprocess ? "true" : "false");
	fputs(",\"has_cxx_modules\":", out);
	fprintf(out, "%s", config->has_cxx_modules ? "true" : "false");
	fputs(",\"cxx_modules_enabled\":", out);
	fprintf(out, "%s", options->cxx_modules_enabled ? "true" : "false");
	fputc('}', out);
}

/** toolset 하나를 machine-readable JSON record로 출력한다. */
static void
dump_toolset_json(FILE *out, const struct qstar_toolset *toolset)
{
	size_t i;

	fputs("{\"label\":", out);
	dump_json_string(out, toolset->label);
	fputs(",\"name\":", out);
	dump_json_string(out, toolset->name);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, toolset->origin_file && *toolset->origin_file ?
	    toolset->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", toolset->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, toolset->fragment_dir);
	fputs(",\"tools\":{", out);
	for (i = 0; i < toolset->role_len; i++) {
		if (i)
			fputc(',', out);
		dump_json_string(out, toolset->roles[i].role);
		fputc(':', out);
		dump_json_list(out, &toolset->roles[i].argv);
	}
	fputc('}', out);
	fputs(",\"response_files\":", out);
	dump_json_string(out, toolset->response_files && *toolset->response_files ?
	    toolset->response_files : "auto");
	fputs(",\"response_style\":", out);
	dump_json_string(out, toolset->response_style && *toolset->response_style ?
	    toolset->response_style : "auto");
	fputs(",\"allow_absolute_tools\":", out);
	dump_json_string(out, toolset->allow_absolute_tools && *toolset->allow_absolute_tools ?
	    toolset->allow_absolute_tools : "false");
	fputs(",\"path_tools\":", out);
	dump_json_list(out, &toolset->path_tools);
	fputc('}', out);
}

/** activated language provider 하나를 machine-readable JSON record로 출력한다. */
static void
dump_language_provider_json(FILE *out, const struct qstar_language_provider *provider)
{
	fputs("{\"namespace\":", out);
	dump_json_string(out, provider->namespace);
	fputs(",\"id\":", out);
	dump_json_string(out, provider->id);
	fputs(",\"api\":", out);
	dump_json_string(out, provider->api);
	fputs(",\"version\":", out);
	dump_json_string(out, provider->version && *provider->version ?
	    provider->version : "");
	fputs(",\"dir\":", out);
	dump_json_string(out, provider->dir);
	fputs(",\"manifest\":", out);
	dump_json_string(out, provider->manifest);
	fputs(",\"implementation\":", out);
	dump_json_string(out, provider->implementation);
	fputc('}', out);
}

/** generated action 하나를 machine-readable JSON record로 출력한다. */
static void
dump_genrule_json(FILE *out, const struct qstar_genrule *genrule)
{
	char identity[QSTAR_PATH_MAX];
	size_t i;

	fputs("{\"label\":", out);
	dump_json_string(out, genrule->label);
	fputs(",\"name\":", out);
	dump_json_string(out, genrule->name);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, genrule->origin_file && *genrule->origin_file ?
	    genrule->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", genrule->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, genrule->fragment_dir);
	fputs(",\"tool\":", out);
	dump_json_string(out, genrule->tool);
	fputs(",\"description\":", out);
	dump_json_string(out, genrule->description && *genrule->description ?
	    genrule->description : "");
	fprintf(out, ",\"config_header\":%s", genrule->config_header ? "true" : "false");
	fputs(",\"inputs\":", out);
	dump_json_list(out, &genrule->inputs);
	fputs(",\"outputs\":", out);
	dump_json_list(out, &genrule->outputs);
	fputs(",\"output_artifacts\":[", out);
	for (i = 0; i < genrule->outputs.len; i++) {
		if (i)
			fputc(',', out);
		if (qstar_genrule_output_identity(genrule, i, identity,
		    sizeof(identity)) < 0)
			snprintf(identity, sizeof(identity), "<too-long>");
		fputs("{\"path\":", out);
		dump_json_string(out, genrule->outputs.items[i]);
		fputs(",\"group\":", out);
		dump_json_string(out, qstar_genrule_output_group(genrule, i));
		fputs(",\"format\":", out);
		dump_json_string(out, qstar_genrule_output_format(genrule, i));
		fputs(",\"identity\":", out);
		dump_json_string(out, identity);
		fputc('}', out);
	}
	fputc(']', out);
	fputs(",\"command\":", out);
	dump_json_list(out, &genrule->command);
	fputc('}', out);
}

/** stage rule 하나를 machine-readable JSON record로 출력한다. */
static void
dump_stage_json(FILE *out, const struct qstar_stage *stage)
{
	size_t i;

	fputs("{\"label\":", out);
	dump_json_string(out, stage->label);
	fputs(",\"name\":", out);
	dump_json_string(out, stage->name);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, stage->origin_file && *stage->origin_file ?
	    stage->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", stage->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, stage->fragment_dir);
	fputs(",\"root\":", out);
	dump_json_string(out, stage->root && *stage->root ? stage->root : "");
	fputs(",\"description\":", out);
	dump_json_string(out, stage->description && *stage->description ?
	    stage->description : "");
	fputs(",\"files\":[", out);
	for (i = 0; i < stage->srcs.len; i++) {
		if (i)
			fputc(',', out);
		fputs("{\"src\":", out);
		dump_json_string(out, stage->srcs.items[i]);
		fputs(",\"dst\":", out);
		dump_json_string(out, i < stage->dsts.len ? stage->dsts.items[i] : "");
		fputc('}', out);
	}
	fputs("]}", out);
}

/** target_family 하나를 machine-readable JSON record로 출력한다. */
static void
dump_target_family_json(FILE *out, const struct qstar_target_family *family)
{
	fputs("{\"name\":", out);
	dump_json_string(out, family->name);
	fputs(",\"origin_file\":", out);
	dump_json_string(out, family->origin_file && *family->origin_file ?
	    family->origin_file : "<unknown>");
	fprintf(out, ",\"origin_line\":%d", family->origin_line);
	fputs(",\"fragment_dir\":", out);
	dump_json_string(out, family->fragment_dir);
	fprintf(out, ",\"allow_shared_sources\":%s",
	    family->allow_shared_sources ? "true" : "false");
	fputs(",\"variants\":", out);
	dump_json_list(out, &family->variants);
	fputs(",\"targets\":", out);
	dump_json_list(out, &family->targets);
	fputc('}', out);
}

/** QStar target/generated action 목록을 machine-readable JSON으로 출력한다. */
int
qstar_graph_list_targets_json(const struct qstar_graph *graph, FILE *out)
{
	const struct qstar_target **targets;
	const struct qstar_config **configs;
	const struct qstar_toolset **toolsets;
	const struct qstar_language_provider **providers;
	const struct qstar_genrule **genrules;
	const struct qstar_stage **stages;
	const struct qstar_target_family **families;
	size_t i;

	targets = malloc((graph->len ? graph->len : 1) * sizeof(targets[0]));
	configs = malloc((graph->config_len ? graph->config_len : 1) * sizeof(configs[0]));
	toolsets = malloc((graph->toolset_len ? graph->toolset_len : 1) *
	    sizeof(toolsets[0]));
	providers = malloc((graph->language_provider_len ? graph->language_provider_len : 1) *
	    sizeof(providers[0]));
	genrules = malloc((graph->genrule_len ? graph->genrule_len : 1) * sizeof(genrules[0]));
	stages = malloc((graph->stage_len ? graph->stage_len : 1) * sizeof(stages[0]));
	families = malloc((graph->family_len ? graph->family_len : 1) * sizeof(families[0]));
	if (!targets || !configs || !toolsets || !providers || !genrules || !stages ||
	    !families) {
		free(targets);
		free(configs);
		free(toolsets);
		free(providers);
		free(genrules);
		free(stages);
		free(families);
		return -1;
	}
	for (i = 0; i < graph->len; i++)
		targets[i] = &graph->targets[i];
	for (i = 0; i < graph->config_len; i++)
		configs[i] = &graph->configs[i];
	for (i = 0; i < graph->toolset_len; i++)
		toolsets[i] = &graph->toolsets[i];
	for (i = 0; i < graph->language_provider_len; i++)
		providers[i] = &graph->language_providers[i];
	for (i = 0; i < graph->genrule_len; i++)
		genrules[i] = &graph->genrules[i];
	for (i = 0; i < graph->stage_len; i++)
		stages[i] = &graph->stages[i];
	for (i = 0; i < graph->family_len; i++)
		families[i] = &graph->families[i];
	sort_target_ptrs(targets, graph->len);
	sort_config_ptrs(configs, graph->config_len);
	sort_toolset_ptrs(toolsets, graph->toolset_len);
	sort_language_provider_ptrs(providers, graph->language_provider_len);
	sort_genrule_ptrs(genrules, graph->genrule_len);
	sort_stage_ptrs(stages, graph->stage_len);
	sort_family_ptrs(families, graph->family_len);
	fputs("{\"schema\":\"qstar-targets-v1\",\"package_root\":", out);
	dump_json_string(out, graph->package_root ? graph->package_root : ".");
	fputs(",\"project\":{\"name\":", out);
	dump_json_string(out, graph->project.name && *graph->project.name ?
	    graph->project.name : "");
	fputs(",\"version\":", out);
	dump_json_string(out, graph->project.version && *graph->project.version ?
	    graph->project.version : "");
	fputs(",\"root\":", out);
	dump_json_string(out, graph->project.root && *graph->project.root ?
	    graph->project.root : ".");
	fputs(",\"build_dir\":", out);
	dump_json_string(out, qstar_graph_build_dir(graph));
	fputs(",\"generated_dir\":", out);
	dump_json_string(out, qstar_graph_generated_dir(graph));
	fputs(",\"compile_commands\":", out);
	dump_json_string(out, qstar_graph_compile_commands_policy(graph));
	fputs(",\"generator\":", out);
	dump_json_string(out, qstar_graph_generator(graph));
	fputs(",\"requested_generator\":", out);
	dump_json_string(out, qstar_graph_requested_generator(graph));
	fputc('}', out);
	fprintf(out,
	    ",\"target_count\":%zu,\"config_count\":%zu,\"toolset_count\":%zu,"
	    "\"language_provider_count\":%zu,"
	    "\"generated_action_count\":%zu,\"stage_count\":%zu,\"target_family_count\":%zu",
	    graph->len, graph->config_len, graph->toolset_len,
	    graph->language_provider_len, graph->genrule_len, graph->stage_len,
	    graph->family_len);
	fputs(",\"targets\":[", out);
	for (i = 0; i < graph->len; i++) {
		if (i)
			fputc(',', out);
		dump_target_json(out, graph, targets[i]);
	}
	fputs("],\"configs\":[", out);
	for (i = 0; i < graph->config_len; i++) {
		if (i)
			fputc(',', out);
		dump_config_json(out, configs[i]);
	}
	fputs("],\"toolsets\":[", out);
	for (i = 0; i < graph->toolset_len; i++) {
		if (i)
			fputc(',', out);
		dump_toolset_json(out, toolsets[i]);
	}
	fputs("],\"language_providers\":[", out);
	for (i = 0; i < graph->language_provider_len; i++) {
		if (i)
			fputc(',', out);
		dump_language_provider_json(out, providers[i]);
	}
	fputs("],\"generated_actions\":[", out);
	for (i = 0; i < graph->genrule_len; i++) {
		if (i)
			fputc(',', out);
		dump_genrule_json(out, genrules[i]);
	}
	fputs("],\"stages\":[", out);
	for (i = 0; i < graph->stage_len; i++) {
		if (i)
			fputc(',', out);
		dump_stage_json(out, stages[i]);
	}
	fputs("],\"target_families\":[", out);
	for (i = 0; i < graph->family_len; i++) {
		if (i)
			fputc(',', out);
		dump_target_family_json(out, families[i]);
	}
	fputs("]}\n", out);
	free(targets);
	free(configs);
	free(toolsets);
	free(providers);
	free(genrules);
	free(stages);
	free(families);
	return 0;
}

/** QStar target 하나를 authoring query text로 출력한다. */
int
qstar_graph_query(const struct qstar_graph *graph, const char *label, FILE *out)
{
	const struct qstar_target *target;

	if (!label || !*label)
		return -1;
	target = find_target(graph, label);
	if (!target)
		return -1;
	fputs("qstar query v1\n", out);
	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    target->origin_file && *target->origin_file ? target->origin_file : "<unknown>",
	    target->origin_line);
	fprintf(out, "  fragment_dir %s\n", target->fragment_dir);
	{
	char package[QSTAR_PATH_MAX];
	if (qstar_label_package_path(target->label, package, sizeof(package)) < 0)
		snprintf(package, sizeof(package), "<external>");
	fprintf(out, "  package %s\n", package[0] ? package : "<root>");
	}
	fprintf(out, "  kind %s\n", target->kind);
	fprintf(out, "  rule provider=%s final_action=%s output_group=%s\n",
	    qstar_target_rule_lookup(target->kind) ?
	    qstar_target_rule_lookup(target->kind)->provider : "generic",
	    qstar_target_final_action(target), qstar_target_output_group(target));
	if (qstar_target_has_provider_final_action(target))
		fprintf(out, "  provider_final provider=%s kind=%s lower=%s\n",
		    target->provider_final.provider ? target->provider_final.provider :
		    "<unknown>",
		    target->provider_final.kind ? target->provider_final.kind : "<unknown>",
		    target->provider_final.lower ? target->provider_final.lower :
		    "<unknown>");
	fputs("  configs ", out);
	dump_list(out, &target->configs);
	fputc('\n', out);
	fputs("  sources ", out);
	dump_list(out, &target->sources);
	fputc('\n', out);
	fputs("  public_headers ", out);
	dump_list(out, &target->public_headers);
	fputc('\n', out);
	fputs("  private_headers ", out);
	dump_list(out, &target->private_headers);
	fputc('\n', out);
	fputs("  include_dirs ", out);
	dump_list(out, &target->include_dirs);
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
	fputs("  cflags ", out);
	dump_list(out, &target->cflags);
	fputc('\n', out);
	fputs("  cxxflags ", out);
	dump_list(out, &target->cxxflags);
	fputc('\n', out);
	fprintf(out, "  cxx_standard %s\n", target->cxx_standard);
	fputs("  lang.asm.include_dirs ", out);
	dump_list(out, &target->asm_include_dirs);
	fputc('\n', out);
	fputs("  lang.asm.compile_options ", out);
	dump_list(out, &target->asm_compile_options);
	fputc('\n', out);
	fprintf(out, "  lang.asm.preprocess %s\n", target->asm_preprocess ? "true" : "false");
	fprintf(out, "  lang.cxx.modules enabled=%s\n",
	    target->cxx_modules_enabled ? "true" : "false");
	fputs("  run.inputs ", out);
	dump_list(out, &target->run_inputs);
	fputc('\n', out);
	fputs("  run.command ", out);
	dump_list(out, &target->run_command);
	fputc('\n', out);
	fprintf(out, "  run.timeout_sec %d\n", target->run_timeout_sec);
	fprintf(out, "  run.expect.contains %s\n",
	    target->run_expect_contains ? target->run_expect_contains : "");
	fprintf(out, "  run.expect.file %s\n",
	    target->run_expect_file && *target->run_expect_file ? target->run_expect_file : "");
	fprintf(out, "  artifact_name %s\n",
	    target->artifact_name && *target->artifact_name ? target->artifact_name :
	    "<default>");
	qstar_dump_target_artifact_map_text(out, graph, target, "  ");
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
	return 0;
}

/** 경로에서 package root로 쓸 dirname을 계산한다. */
int
qstar_dirname(const char *path, char *dst, size_t dstlen)
{
	const char *slash;
	size_t n;

	slash = strrchr(path, '/');
	if (!slash) {
		if (dstlen < 2)
			return -1;
		strcpy(dst, ".");
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

/** 두 path 조각을 slash 기준으로 결합한다. */
int
qstar_path_join(const char *a, const char *b, char *dst, size_t dstlen)
{
	size_t na, nb, need;

	if (!a || !*a || strcmp(a, ".") == 0) {
		need = strlen(b) + 1;
		if (need > dstlen)
			return -1;
		memcpy(dst, b, need);
		return 0;
	}
	na = strlen(a);
	nb = strlen(b);
	need = na + 1 + nb + 1;
	if (need > dstlen)
		return -1;
	memcpy(dst, a, na);
	dst[na] = '/';
	memcpy(dst + na + 1, b, nb + 1);
	return 0;
}

/** normalized slash path를 component pointer 배열로 분해한다. */
static int
split_path_components(char *path, char **parts, size_t *len, size_t cap)
{
	char *p;

	*len = 0;
	if (!path || !*path || strcmp(path, ".") == 0)
		return 0;
	p = path;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		if (*len >= cap)
			return -1;
		parts[(*len)++] = p;
		while (*p && *p != '/')
			p++;
		if (*p)
			*p++ = '\0';
	}
	return 0;
}

/** 상대 path buffer에 component 하나를 slash로 이어 붙인다. */
static int
append_relative_component(char *dst, size_t dstlen, size_t *used, const char *part)
{
	size_t len, need;

	len = strlen(part);
	need = *used + (*used ? 1 : 0) + len + 1;
	if (need > dstlen)
		return -1;
	if (*used)
		dst[(*used)++] = '/';
	memcpy(dst + *used, part, len + 1);
	*used += len;
	return 0;
}

/** 두 package-relative directory 사이의 상대 directory path를 계산한다. */
int
qstar_path_relative_between_dirs(const char *from_dir, const char *to_dir,
    char *dst, size_t dstlen)
{
	char from_buf[QSTAR_PATH_MAX], to_buf[QSTAR_PATH_MAX];
	char *from_parts[128], *to_parts[128];
	size_t from_len, to_len, common, used, i;

	if (!dstlen)
		return -1;
	if (snprintf(from_buf, sizeof(from_buf), "%s", from_dir ? from_dir : ".") >=
	    (int)sizeof(from_buf) ||
	    snprintf(to_buf, sizeof(to_buf), "%s", to_dir ? to_dir : ".") >=
	    (int)sizeof(to_buf))
		return -1;
	if (split_path_components(from_buf, from_parts, &from_len,
	    sizeof(from_parts) / sizeof(from_parts[0])) < 0 ||
	    split_path_components(to_buf, to_parts, &to_len,
	    sizeof(to_parts) / sizeof(to_parts[0])) < 0)
		return -1;
	common = 0;
	while (common < from_len && common < to_len &&
	    strcmp(from_parts[common], to_parts[common]) == 0)
		common++;
	dst[0] = '\0';
	used = 0;
	for (i = common; i < from_len; i++) {
		if (append_relative_component(dst, dstlen, &used, "..") < 0)
			return -1;
	}
	for (i = common; i < to_len; i++) {
		if (append_relative_component(dst, dstlen, &used, to_parts[i]) < 0)
			return -1;
	}
	if (!used) {
		if (dstlen < 2)
			return -1;
		snprintf(dst, dstlen, ".");
	}
	return 0;
}

/** QStar path가 package-relative normalized path인지 검사한다. */
int
qstar_path_is_package_relative(const char *path)
{
	const unsigned char *p;

	if (!path || !*path || path[0] == '/')
		return 0;
	if (isalpha((unsigned char)path[0]) && path[1] == ':')
		return 0;
	for (p = (const unsigned char *)path; *p; p++) {
		if (*p == '\\' || *p == ':')
			return 0;
	}
	if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0)
		return 0;
	if (strncmp(path, "../", 3) == 0 || strstr(path, "/../") ||
	    strstr(path, "/./") || strstr(path, "//"))
		return 0;
	return 1;
}

/** package-relative path 검증 실패 이유를 사용자-facing 문구로 반환한다. */
const char *
qstar_path_package_relative_reason(const char *path)
{
	if (!path || !*path)
		return "path is empty";
	if (path[0] == '/')
		return "absolute paths are not allowed";
	if (isalpha((unsigned char)path[0]) && path[1] == ':')
		return "drive-letter paths are not allowed in package paths; write project files as slash-normalized paths like 'src/main.c' and keep absolute tool locations in toolsets";
	if (strchr(path, '\\'))
		return "backslash paths are not normalized; use '/' separators like 'src/main.c'";
	if (strchr(path, ':'))
		return "colon characters are reserved in package paths";
	if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0 ||
	    strncmp(path, "../", 3) == 0 || strstr(path, "/../"))
		return "parent-directory paths are not allowed";
	if (strstr(path, "/./"))
		return "current-directory segments are not allowed";
	if (strstr(path, "//"))
		return "duplicate path separators are not allowed";
	return "use package-relative slash-normalized paths";
}
