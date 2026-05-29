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

/** QStar graph가 소유한 모든 동적 메모리를 해제한다. */
void
qstar_graph_free(struct qstar_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->len; i++)
		free_target(&graph->targets[i]);
	free(graph->targets);
	memset(graph, 0, sizeof(*graph));
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

/** QStar graph에 새 target을 추가하고 중복 label을 stable error로 막는다. */
struct qstar_target *
qstar_graph_add_target(struct qstar_graph *graph, const char *label, const char *name,
    const char *kind, const char *fragment_dir)
{
	struct qstar_target *targets, *target;
	size_t cap;

	if (has_target(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate target label '%s'", label);
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
	target->toolchain = qstar_strdup("host");
	target->stdlib_policy = qstar_strdup("system");
	if (!target->label || !target->name || !target->kind || !target->fragment_dir ||
	    !target->toolchain || !target->stdlib_policy) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return target;
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
dump_target(const struct qstar_target *target, FILE *out)
{
	fprintf(out, "target %s\n", target->label);
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
