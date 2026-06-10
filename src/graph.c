#include "internal.h"

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

/** QStar graph 저장소를 빈 상태로 초기화한다. */
void
qstar_graph_init(struct qstar_graph *graph)
{
	memset(graph, 0, sizeof(*graph));
}

static void
free_target(struct qstar_target *target)
{
	free(target->label);
	free(target->name);
	free(target->kind);
	free(target->fragment_dir);
	free(target->origin_file);
	free(target->modules.root);
	qstar_string_list_free(&target->modules.include);
	qstar_string_list_free(&target->modules.exclude);
	qstar_string_list_free(&target->sources);
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
	qstar_string_list_free(&target->defsyms);
	qstar_string_list_free(&target->cflags);
	qstar_string_list_free(&target->cxxflags);
	qstar_string_list_free(&target->asm_include_dirs);
	qstar_string_list_free(&target->asm_compile_options);
	qstar_string_list_free(&target->cale_hcl_include_dirs);
	qstar_string_list_free(&target->cale_compile_options);
	qstar_string_list_free(&target->run_command);
	free(target->artifact_name);
	free(target->cxx_standard);
	free(target->cale_profile);
	free(target->linker_script);
	free(target->run_marker);
	free(target->toolchain);
	free(target->stdlib_policy);
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
	qstar_string_list_free(&genrule->inputs);
	qstar_string_list_free(&genrule->outputs);
	qstar_string_list_free(&genrule->output_groups);
	qstar_string_list_free(&genrule->output_formats);
	qstar_string_list_free(&genrule->output_addresses);
	qstar_string_list_free(&genrule->output_layouts);
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
	qstar_string_list_free(&stage->srcs);
	qstar_string_list_free(&stage->dsts);
}

/** package alias entry가 소유한 문자열을 해제한다. */
static void
free_package_alias(struct qstar_package_alias *pkg)
{
	free(pkg->alias);
	free(pkg->root);
}

/** profile input이 소유한 문자열을 해제한다. */
static void
free_profile_input(struct qstar_profile_input *profile)
{
	free(profile->name);
	free(profile->target);
	free(profile->toolchain);
	free(profile->stdlib_policy);
	free(profile->freestanding);
	free(profile->arch);
	free(profile->cpu);
	free(profile->abi);
	free(profile->cc);
	free(profile->cxx);
	free(profile->cale);
	free(profile->ar);
	free(profile->linker);
	free(profile->sysroot);
	free(profile->resource_dir);
	free(profile->response_files);
	free(profile->response_style);
	free(profile->linker_script);
	free(profile->allow_absolute_tools);
	qstar_string_list_free(&profile->artifact_names);
	qstar_string_list_free(&profile->include_dirs);
	qstar_string_list_free(&profile->lib_dirs);
	qstar_string_list_free(&profile->link_options);
	qstar_string_list_free(&profile->defsyms);
	qstar_string_list_free(&profile->path_tools);
	qstar_string_list_free(&profile->tool_overrides);
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
	for (i = 0; i < graph->genrule_len; i++)
		free_genrule(&graph->genrules[i]);
	for (i = 0; i < graph->stage_len; i++)
		free_stage(&graph->stages[i]);
	for (i = 0; i < graph->lint_len; i++)
		free_lint_diagnostic(&graph->lint_diagnostics[i]);
	for (i = 0; i < graph->package_len; i++)
		free_package_alias(&graph->packages[i]);
	free_profile_input(&graph->profile);
	free(graph->targets);
	free(graph->packages);
	free(graph->genrules);
	free(graph->stages);
	free(graph->lint_diagnostics);
	qstar_string_list_free(&graph->evaluated_fragments);
	memset(graph, 0, sizeof(*graph));
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

/** QStar explain profile 입력을 graph에 기록한다. */
int
qstar_graph_set_profile_input(struct qstar_graph *graph, const char *name,
    const char *target, const char *toolchain, const char *stdlib_policy)
{
	if (replace_string(&graph->profile.name, name) < 0 ||
	    replace_string(&graph->profile.target, target) < 0 ||
	    replace_string(&graph->profile.toolchain, toolchain) < 0 ||
	    replace_string(&graph->profile.stdlib_policy, stdlib_policy) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
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
	target->cale_profile = qstar_strdup("");
	target->linker_script = qstar_strdup("");
	if (!target->label || !target->name || !target->kind || !target->fragment_dir ||
	    !target->origin_file || !target->toolchain || !target->stdlib_policy ||
	    !target->artifact_name || !target->cxx_standard || !target->cale_profile ||
	    !target->linker_script) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return target;
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
	if (!genrule->label || !genrule->name || !genrule->fragment_dir ||
	    !genrule->origin_file || !genrule->tool) {
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
	if (!stage->label || !stage->name || !stage->fragment_dir ||
	    !stage->origin_file || !stage->root) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return stage;
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
	return strcmp(qstar_genrule_output_format(genrule, index), "raw-binary") == 0 ?
	    "images" : "generated";
}

/** generated output metadata의 address를 기본값 포함해 반환한다. */
const char *
qstar_genrule_output_address(const struct qstar_genrule *genrule, size_t index)
{
	return genrule_meta_or_default(&genrule->output_addresses, index, "<none>");
}

/** generated output metadata의 layout을 기본값 포함해 반환한다. */
const char *
qstar_genrule_output_layout(const struct qstar_genrule *genrule, size_t index)
{
	return genrule_meta_or_default(&genrule->output_layouts, index, "<none>");
}

/** generated output path와 format/address/layout metadata를 action identity로 만든다. */
int
qstar_genrule_output_identity(const struct qstar_genrule *genrule, size_t index,
    char *dst, size_t dstlen)
{
	if (index >= genrule->outputs.len)
		return -1;
	return snprintf(dst, dstlen, "%s|group=%s|format=%s|address=%s|layout=%s",
	    genrule->outputs.items[index], qstar_genrule_output_group(genrule, index),
	    qstar_genrule_output_format(genrule, index),
	    qstar_genrule_output_address(genrule, index),
	    qstar_genrule_output_layout(genrule, index)) < (int)dstlen ? 0 : -1;
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
profile_or_default(const char *s, const char *fallback)
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
dump_target(const struct qstar_target *target, FILE *out)
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
	if (target->modules.present) {
		fprintf(out, "  modules root=%s include=", target->modules.root ? target->modules.root : "");
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
	fputs("  frameworks ", out);
	dump_list(out, &target->frameworks);
	fputc('\n', out);
	fputs("  link_options ", out);
	dump_list(out, &target->link_options);
	fputc('\n', out);
	fprintf(out, "  linker_script %s\n",
	    target->linker_script && *target->linker_script ? target->linker_script : "<none>");
	fputs("  defsyms ", out);
	dump_list(out, &target->defsyms);
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
	fputs("  lang.cale.hcl_include_dirs ", out);
	dump_list(out, &target->cale_hcl_include_dirs);
	fputc('\n', out);
	fputs("  lang.cale.compile_options ", out);
	dump_list(out, &target->cale_compile_options);
	fputc('\n', out);
	fprintf(out, "  lang.cale.profile %s\n", target->cale_profile);
	fputs("  run.command ", out);
	dump_list(out, &target->run_command);
	fputc('\n', out);
	fprintf(out, "  run.timeout_sec %d\n", target->run_timeout_sec);
	fprintf(out, "  run.marker %s\n", target->run_marker ? target->run_marker : "");
	fprintf(out, "  artifact_name %s\n",
	    target->artifact_name && *target->artifact_name ? target->artifact_name :
	    "<default>");
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
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
		    "  output_artifact path=%s group=%s format=%s address=%s layout=%s identity=%s\n",
		    genrule->outputs.items[i], qstar_genrule_output_group(genrule, i),
		    qstar_genrule_output_format(genrule, i),
		    qstar_genrule_output_address(genrule, i),
		    qstar_genrule_output_layout(genrule, i), identity);
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
	for (i = 0; i < stage->srcs.len; i++)
		fprintf(out, "  stage_file src=%s dst=%s\n", stage->srcs.items[i],
		    i < stage->dsts.len ? stage->dsts.items[i] : "<missing>");
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
	fprintf(out, "profile name=%s target=%s toolchain=%s stdlib=%s\n",
	    profile_or_default(graph->profile.name, "default"),
	    profile_or_default(graph->profile.target, "host"),
	    profile_or_default(graph->profile.toolchain, "default"),
	    profile_or_default(graph->profile.stdlib_policy, "default"));
	fprintf(out, "profile_target arch=%s cpu=%s abi=%s freestanding=%s\n",
	    graph->profile.arch ? graph->profile.arch : "<auto>",
	    graph->profile.cpu ? graph->profile.cpu : "<none>",
	    graph->profile.abi ? graph->profile.abi : "<none>",
	    graph->profile.freestanding ? graph->profile.freestanding : "false");
	fprintf(out, "profile_tools cc=%s cxx=%s cale=%s ar=%s linker=%s sysroot=%s resource_dir=%s\n",
	    graph->profile.cc ? graph->profile.cc : "<default>",
	    graph->profile.cxx ? graph->profile.cxx : "<default>",
	    graph->profile.cale ? graph->profile.cale : "<default>",
	    graph->profile.ar ? graph->profile.ar : "<default>",
	    graph->profile.linker ? graph->profile.linker : "<default>",
	    graph->profile.sysroot ? graph->profile.sysroot : "<none>",
	    graph->profile.resource_dir ? graph->profile.resource_dir : "<none>");
	fprintf(out, "profile_link linker_script=%s link_options=",
	    graph->profile.linker_script ? graph->profile.linker_script : "<none>");
	dump_list(out, &graph->profile.link_options);
	fputs(" defsyms=", out);
	dump_list(out, &graph->profile.defsyms);
	fputc('\n', out);
	fprintf(out, "profile_external_tools allow_absolute=%s path_tools=",
	    graph->profile.allow_absolute_tools ? graph->profile.allow_absolute_tools : "false");
	dump_list(out, &graph->profile.path_tools);
	fputs(" tool_overrides=", out);
	dump_list(out, &graph->profile.tool_overrides);
	fputc('\n', out);
	dump_package_aliases(out, graph);
	for (i = 0; i < graph->genrule_len; i++)
		dump_genrule(&graph->genrules[i], out);
	for (i = 0; i < graph->stage_len; i++)
		dump_stage(&graph->stages[i], out);
	found = label == NULL || *label == '\0';
	for (i = 0; i < n; i++) {
		if (label && *label && strcmp(copy[i].label, label) != 0)
			continue;
		found = 1;
		dump_target(&copy[i], out);
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
	const struct qstar_stage **stages;
	size_t i;

	targets = malloc((graph->len ? graph->len : 1) * sizeof(targets[0]));
	stages = malloc((graph->stage_len ? graph->stage_len : 1) * sizeof(stages[0]));
	if (!targets || !stages) {
		free(targets);
		free(stages);
		return -1;
	}
	for (i = 0; i < graph->len; i++)
		targets[i] = &graph->targets[i];
	for (i = 0; i < graph->stage_len; i++)
		stages[i] = &graph->stages[i];
	sort_target_ptrs(targets, graph->len);
	sort_stage_ptrs(stages, graph->stage_len);
	fputs("qstar targets v1\n", out);
	fprintf(out, "target-count %zu\n", graph->len);
	for (i = 0; i < graph->len; i++)
		fprintf(out, "target %s kind=%s origin=%s:%d\n", targets[i]->label,
		    targets[i]->kind,
		    targets[i]->origin_file && *targets[i]->origin_file ?
		    targets[i]->origin_file : "<unknown>",
		    targets[i]->origin_line);
	fprintf(out, "stage-count %zu\n", graph->stage_len);
	for (i = 0; i < graph->stage_len; i++)
		fprintf(out, "stage %s root=%s origin=%s:%d\n", stages[i]->label,
		    stages[i]->root && *stages[i]->root ? stages[i]->root : "<default>",
		    stages[i]->origin_file && *stages[i]->origin_file ?
		    stages[i]->origin_file : "<unknown>",
		    stages[i]->origin_line);
	free(targets);
	free(stages);
	return 0;
}

/** target 하나를 machine-readable JSON record로 출력한다. */
static void
dump_target_json(FILE *out, const struct qstar_target *target)
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
	fputs(",\"artifact_name\":", out);
	dump_json_string(out, target->artifact_name && *target->artifact_name ?
	    target->artifact_name : "");
	fputs(",\"cxx_standard\":", out);
	dump_json_string(out, target->cxx_standard);
	fputs(",\"lang_cxx_standard\":", out);
	dump_json_string(out, target->cxx_standard);
	fputs(",\"lang_asm_preprocess\":", out);
	fprintf(out, "%s", target->asm_preprocess ? "true" : "false");
	fputs(",\"lang_cale_profile\":", out);
	dump_json_string(out, target->cale_profile);
	fputs(",\"run_command\":", out);
	dump_json_list(out, &target->run_command);
	fprintf(out, ",\"run_timeout_sec\":%d", target->run_timeout_sec);
	fputs(",\"run_marker\":", out);
	dump_json_string(out, target->run_marker ? target->run_marker : "");
	fprintf(out, ",\"is_test\":%s", strcmp(target->kind, "test") == 0 ? "true" : "false");
	fprintf(out, ",\"installable\":%s", qstar_target_is_installable(target) ? "true" : "false");
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
		fputs(",\"address\":", out);
		dump_json_string(out, qstar_genrule_output_address(genrule, i));
		fputs(",\"layout\":", out);
		dump_json_string(out, qstar_genrule_output_layout(genrule, i));
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

/** QStar target/generated action 목록을 machine-readable JSON으로 출력한다. */
int
qstar_graph_list_targets_json(const struct qstar_graph *graph, FILE *out)
{
	const struct qstar_target **targets;
	const struct qstar_genrule **genrules;
	const struct qstar_stage **stages;
	size_t i;

	targets = malloc((graph->len ? graph->len : 1) * sizeof(targets[0]));
	genrules = malloc((graph->genrule_len ? graph->genrule_len : 1) * sizeof(genrules[0]));
	stages = malloc((graph->stage_len ? graph->stage_len : 1) * sizeof(stages[0]));
	if (!targets || !genrules || !stages) {
		free(targets);
		free(genrules);
		free(stages);
		return -1;
	}
	for (i = 0; i < graph->len; i++)
		targets[i] = &graph->targets[i];
	for (i = 0; i < graph->genrule_len; i++)
		genrules[i] = &graph->genrules[i];
	for (i = 0; i < graph->stage_len; i++)
		stages[i] = &graph->stages[i];
	sort_target_ptrs(targets, graph->len);
	sort_genrule_ptrs(genrules, graph->genrule_len);
	sort_stage_ptrs(stages, graph->stage_len);
	fputs("{\"schema\":\"qstar-targets-v1\",\"package_root\":", out);
	dump_json_string(out, graph->package_root ? graph->package_root : ".");
	fprintf(out,
	    ",\"target_count\":%zu,\"generated_action_count\":%zu,\"stage_count\":%zu",
	    graph->len, graph->genrule_len, graph->stage_len);
	fputs(",\"targets\":[", out);
	for (i = 0; i < graph->len; i++) {
		if (i)
			fputc(',', out);
		dump_target_json(out, targets[i]);
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
	fputs("]}\n", out);
	free(targets);
	free(genrules);
	free(stages);
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
	fputs("  lang.cale.hcl_include_dirs ", out);
	dump_list(out, &target->cale_hcl_include_dirs);
	fputc('\n', out);
	fputs("  lang.cale.compile_options ", out);
	dump_list(out, &target->cale_compile_options);
	fputc('\n', out);
	fprintf(out, "  lang.cale.profile %s\n", target->cale_profile);
	fputs("  run.command ", out);
	dump_list(out, &target->run_command);
	fputc('\n', out);
	fprintf(out, "  run.timeout_sec %d\n", target->run_timeout_sec);
	fprintf(out, "  run.marker %s\n", target->run_marker ? target->run_marker : "");
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

/** QStar path가 package-relative normalized path인지 검사한다. */
int
qstar_path_is_package_relative(const char *path)
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
