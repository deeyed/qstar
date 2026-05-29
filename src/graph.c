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
	qstar_string_list_free(&target->system_include_dirs);
	qstar_string_list_free(&target->deps);
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
	qstar_string_list_free(&genrule->args);
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
	for (i = 0; i < graph->package_len; i++)
		free_package_alias(&graph->packages[i]);
	free_profile_input(&graph->profile);
	free(graph->targets);
	free(graph->packages);
	free(graph->genrules);
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
	if (!target->label || !target->name || !target->kind || !target->fragment_dir ||
	    !target->origin_file || !target->toolchain || !target->stdlib_policy) {
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
	fprintf(out, "target %s\n", target->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    target->origin_file && *target->origin_file ? target->origin_file : "<unknown>",
	    target->origin_line);
	fprintf(out, "  kind %s\n", target->kind);
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
	fputs("  system_include_dirs ", out);
	dump_list(out, &target->system_include_dirs);
	fputc('\n', out);
	fputs("  deps ", out);
	dump_list(out, &target->deps);
	fputc('\n', out);
	fprintf(out, "  toolchain %s\n", target->toolchain);
	fprintf(out, "  stdlib %s\n", target->stdlib_policy);
}

/** generated action skeleton을 Graph IR dump 형식으로 출력한다. */
static void
dump_genrule(const struct qstar_genrule *genrule, FILE *out)
{
	fprintf(out, "generated_action %s\n", genrule->label);
	fprintf(out, "  origin file=%s line=%d\n",
	    genrule->origin_file && *genrule->origin_file ? genrule->origin_file : "<unknown>",
	    genrule->origin_line);
	fprintf(out, "  tool %s\n", genrule->tool);
	fputs("  inputs ", out);
	dump_list(out, &genrule->inputs);
	fputc('\n', out);
	fputs("  outputs ", out);
	dump_list(out, &genrule->outputs);
	fputc('\n', out);
	fputs("  args ", out);
	dump_list(out, &genrule->args);
	fputc('\n', out);
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
	dump_package_aliases(out, graph);
	for (i = 0; i < graph->genrule_len; i++)
		dump_genrule(&graph->genrules[i], out);
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
	size_t i;

	targets = malloc((graph->len ? graph->len : 1) * sizeof(targets[0]));
	if (!targets)
		return -1;
	for (i = 0; i < graph->len; i++)
		targets[i] = &graph->targets[i];
	sort_target_ptrs(targets, graph->len);
	fputs("qstar targets v1\n", out);
	fprintf(out, "target-count %zu\n", graph->len);
	for (i = 0; i < graph->len; i++)
		fprintf(out, "target %s kind=%s origin=%s:%d\n", targets[i]->label,
		    targets[i]->kind,
		    targets[i]->origin_file && *targets[i]->origin_file ?
		    targets[i]->origin_file : "<unknown>",
		    targets[i]->origin_line);
	free(targets);
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
	fprintf(out, "  kind %s\n", target->kind);
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
