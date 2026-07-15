#include "internal.h"

#include <stdlib.h>
#include <string.h>

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

const struct qstar_test_suite *
qstar_graph_find_test_suite(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	if (!graph || !label)
		return NULL;
	for (i = 0; i < graph->test_suite_len; i++) {
		if (strcmp(graph->test_suites[i].label, label) == 0)
			return &graph->test_suites[i];
	}
	return NULL;
}

struct qstar_test_suite *
qstar_graph_add_test_suite(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir, const char *origin_file,
    int origin_line)
{
	struct qstar_test_suite *items, *suite;
	size_t cap;

	if (!label || !*label || !name || !*name) {
		qstar_set_error(graph, "qstar: test_suite label and name must not be empty");
		return NULL;
	}
	if (qstar_graph_find_test_suite(graph, label)) {
		qstar_set_error(graph, "qstar: duplicate test_suite label '%s'", label);
		return NULL;
	}
	if (graph->test_suite_len == graph->test_suite_cap) {
		cap = graph->test_suite_cap ? graph->test_suite_cap * 2 : 8;
		items = realloc(graph->test_suites, cap * sizeof(items[0]));
		if (!items) {
			qstar_set_error(graph, "qstar: out of memory");
			return NULL;
		}
		graph->test_suites = items;
		graph->test_suite_cap = cap;
	}
	suite = &graph->test_suites[graph->test_suite_len++];
	memset(suite, 0, sizeof(*suite));
	suite->label = qstar_strdup(label);
	suite->name = qstar_strdup(name);
	suite->fragment_dir = qstar_strdup(fragment_dir ? fragment_dir : "");
	suite->origin_file = qstar_strdup(origin_file ? origin_file : "");
	suite->origin_line = origin_line;
	suite->description = qstar_strdup("");
	if (!suite->label || !suite->name || !suite->fragment_dir ||
	    !suite->origin_file || !suite->description) {
		qstar_set_error(graph, "qstar: out of memory");
		return NULL;
	}
	return suite;
}

static int
string_list_contains(const struct qstar_string_list *list, const char *value)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], value) == 0)
			return 1;
	}
	return 0;
}

static int
push_unique(struct qstar_string_list *list, const char *value)
{
	if (string_list_contains(list, value))
		return 0;
	return qstar_string_list_push(list, value);
}

static int
tag_valid(const char *tag)
{
	const unsigned char *p;

	if (!tag || !*tag)
		return 0;
	for (p = (const unsigned char *)tag; *p; p++) {
		if (*p == '\n' || *p == '\r')
			return 0;
	}
	return 1;
}

static int
suite_label_conflicts(const struct qstar_graph *graph,
    const struct qstar_test_suite *suite, const char **kind)
{
	size_t i;

	if (find_target(graph, suite->label)) {
		*kind = "target";
		return 1;
	}
	for (i = 0; i < graph->genrule_len; i++) {
		if (strcmp(graph->genrules[i].label, suite->label) == 0) {
			*kind = "generated action";
			return 1;
		}
	}
	for (i = 0; i < graph->stage_len; i++) {
		if (strcmp(graph->stages[i].label, suite->label) == 0) {
			*kind = "stage";
			return 1;
		}
	}
	for (i = 0; i < graph->config_len; i++) {
		if (strcmp(graph->configs[i].label, suite->label) == 0) {
			*kind = "config";
			return 1;
		}
	}
	for (i = 0; i < graph->toolset_len; i++) {
		if (strcmp(graph->toolsets[i].label, suite->label) == 0) {
			*kind = "toolset";
			return 1;
		}
	}
	return 0;
}

static int
validate_suite_cycle(struct qstar_graph *graph, size_t index,
    unsigned char *state)
{
	const struct qstar_test_suite *suite, *nested;
	size_t i, nested_index;

	if (state[index] == 2)
		return 0;
	if (state[index] == 1)
		return qstar_set_error_origin(graph,
		    graph->test_suites[index].origin_file,
		    graph->test_suites[index].origin_line, "tests",
		    graph->test_suites[index].label,
		    "qstar: test_suite cycle includes '%s'",
		    graph->test_suites[index].label);
	state[index] = 1;
	suite = &graph->test_suites[index];
	for (i = 0; i < suite->tests.len; i++) {
		nested = qstar_graph_find_test_suite(graph, suite->tests.items[i]);
		if (!nested)
			continue;
		nested_index = (size_t)(nested - graph->test_suites);
		if (validate_suite_cycle(graph, nested_index, state) < 0)
			return -1;
	}
	state[index] = 2;
	return 0;
}

int
qstar_graph_validate_test_suites(struct qstar_graph *graph)
{
	const struct qstar_test_suite *suite, *nested;
	const struct qstar_target *target;
	const char *conflict_kind;
	unsigned char *state;
	size_t i, j, k;

	for (i = 0; i < graph->test_suite_len; i++) {
		suite = &graph->test_suites[i];
		if (suite_label_conflicts(graph, suite, &conflict_kind))
			return qstar_set_error_origin(graph, suite->origin_file,
			    suite->origin_line, "label", suite->label,
			    "qstar: test_suite label '%s' conflicts with %s",
			    suite->label, conflict_kind);
		if (suite->tests.len == 0)
			return qstar_set_error_origin(graph, suite->origin_file,
			    suite->origin_line, "tests", suite->label,
			    "qstar: test_suite '%s' requires at least one test, run_target, or nested suite label",
			    suite->label);
		for (j = 0; j < suite->tags.len; j++) {
			if (!tag_valid(suite->tags.items[j]))
				return qstar_set_error_origin(graph, suite->origin_file,
				    suite->origin_line, "tags", suite->label,
				    "qstar: test_suite '%s' tag %zu must be a non-empty one-line string",
				    suite->label, j + 1);
			for (k = 0; k < j; k++) {
				if (strcmp(suite->tags.items[j], suite->tags.items[k]) == 0)
					return qstar_set_error_origin(graph,
					    suite->origin_file, suite->origin_line, "tags",
					    suite->label,
					    "qstar: duplicate tag '%s' in test_suite '%s'",
					    suite->tags.items[j], suite->label);
			}
		}
		for (j = 0; j < suite->tests.len; j++) {
			for (k = 0; k < j; k++) {
				if (strcmp(suite->tests.items[j], suite->tests.items[k]) == 0)
					return qstar_set_error_origin(graph,
					    suite->origin_file, suite->origin_line, "tests",
					    suite->label,
					    "qstar: duplicate member '%s' in test_suite '%s'",
					    suite->tests.items[j], suite->label);
			}
			nested = qstar_graph_find_test_suite(graph, suite->tests.items[j]);
			if (nested)
				continue;
			target = find_target(graph, suite->tests.items[j]);
			if (!target)
				return qstar_set_error_origin(graph, suite->origin_file,
				    suite->origin_line, "tests", suite->label,
				    "qstar: test_suite '%s' references unknown label '%s'",
				    suite->label, suite->tests.items[j]);
			if (strcmp(target->kind, "test") != 0 &&
			    strcmp(target->kind, "run_target") != 0)
				return qstar_set_error_origin(graph, suite->origin_file,
				    suite->origin_line, "tests", suite->label,
				    "qstar: test_suite '%s' member '%s' must be qstar.test, run_target, or another test_suite",
				    suite->label, target->label);
		}
	}
	state = calloc(graph->test_suite_len ? graph->test_suite_len : 1,
	    sizeof(state[0]));
	if (!state)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 0; i < graph->test_suite_len; i++) {
		if (validate_suite_cycle(graph, i, state) < 0) {
			free(state);
			return -1;
		}
	}
	free(state);
	return 0;
}

static int
option_tag_matches(const char *const *tags, size_t len,
    const struct qstar_test_suite *suite)
{
	size_t i, j;

	for (i = 0; i < len; i++) {
		for (j = 0; j < suite->tags.len; j++) {
			if (strcmp(tags[i], suite->tags.items[j]) == 0)
				return 1;
		}
	}
	return 0;
}

static int
suite_is_excluded(const struct qstar_test_suite *suite,
    const struct qstar_test_options *options)
{
	return options && options->exclude_tag_len &&
	    option_tag_matches(options->exclude_tags, options->exclude_tag_len, suite);
}

static int
resolve_suite_recursive(const struct qstar_graph *graph,
    const struct qstar_test_suite *suite, const struct qstar_test_options *options,
    unsigned char *stack, struct qstar_string_list *labels)
{
	const struct qstar_test_suite *nested;
	size_t i, index;

	if (suite_is_excluded(suite, options))
		return 0;
	index = (size_t)(suite - graph->test_suites);
	if (index >= graph->test_suite_len || stack[index])
		return -1;
	stack[index] = 1;
	for (i = 0; i < suite->tests.len; i++) {
		nested = qstar_graph_find_test_suite(graph, suite->tests.items[i]);
		if (nested) {
			if (resolve_suite_recursive(graph, nested, options, stack,
			    labels) < 0) {
				stack[index] = 0;
				return -1;
			}
		} else if (push_unique(labels, suite->tests.items[i]) < 0) {
			stack[index] = 0;
			return -1;
		}
	}
	stack[index] = 0;
	return 0;
}

int
qstar_graph_resolve_test_suite_members(const struct qstar_graph *graph,
    const struct qstar_test_suite *suite, const struct qstar_test_options *options,
    struct qstar_string_list *labels)
{
	unsigned char *stack;
	int rc;

	stack = calloc(graph->test_suite_len ? graph->test_suite_len : 1,
	    sizeof(stack[0]));
	if (!stack)
		return -1;
	rc = resolve_suite_recursive(graph, suite, options, stack, labels);
	free(stack);
	return rc;
}

static int
suite_ptr_cmp(const void *a, const void *b)
{
	const struct qstar_test_suite *const *sa = a;
	const struct qstar_test_suite *const *sb = b;

	return strcmp((*sa)->label, (*sb)->label);
}

int
qstar_graph_resolve_test_selection(struct qstar_graph *graph,
    const struct qstar_test_options *options, struct qstar_string_list *labels)
{
	const struct qstar_test_suite *suite;
	const struct qstar_test_suite **sorted;
	char canonical[QSTAR_PATH_MAX];
	size_t i;

	if (!options)
		return 0;
	for (i = 0; i < options->tag_len; i++) {
		if (!tag_valid(options->tags[i]))
			return qstar_set_error(graph,
			    "qstar: test tag filters must be non-empty one-line strings");
	}
	for (i = 0; i < options->exclude_tag_len; i++) {
		if (!tag_valid(options->exclude_tags[i]))
			return qstar_set_error(graph,
			    "qstar: excluded test tag filters must be non-empty one-line strings");
	}
	for (i = 0; i < options->suite_len; i++) {
		if (!options->suites[i] || !*options->suites[i])
			return qstar_set_error(graph,
			    "qstar: test suite filters must not be empty");
		if (qstar_label_canonicalize(options->suites[i], "", canonical,
		    sizeof(canonical)) < 0)
			return qstar_set_error(graph,
			    "qstar: invalid test suite label '%s'", options->suites[i]);
		suite = qstar_graph_find_test_suite(graph, canonical);
		if (!suite)
			return qstar_set_error(graph,
			    "qstar: unknown test_suite label '%s'", canonical);
		if (qstar_graph_resolve_test_suite_members(graph, suite, options,
		    labels) < 0)
			return qstar_set_error(graph,
			    "qstar: could not resolve test_suite '%s'", canonical);
	}
	if (options->tag_len == 0 && options->exclude_tag_len == 0)
		return 0;
	sorted = malloc((graph->test_suite_len ? graph->test_suite_len : 1) *
	    sizeof(sorted[0]));
	if (!sorted)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 0; i < graph->test_suite_len; i++)
		sorted[i] = &graph->test_suites[i];
	qsort(sorted, graph->test_suite_len, sizeof(sorted[0]), suite_ptr_cmp);
	for (i = 0; i < graph->test_suite_len; i++) {
		suite = sorted[i];
		if (suite->manual || suite_is_excluded(suite, options))
			continue;
		if (options->tag_len && !option_tag_matches(options->tags,
		    options->tag_len, suite))
			continue;
		if (qstar_graph_resolve_test_suite_members(graph, suite, options,
		    labels) < 0) {
			free(sorted);
			return qstar_set_error(graph,
			    "qstar: could not resolve test_suite '%s'", suite->label);
		}
	}
	free(sorted);
	return 0;
}

int
qstar_graph_collect_test_suite_memberships(const struct qstar_graph *graph,
    const char *target_label, struct qstar_string_list *direct,
    struct qstar_string_list *transitive)
{
	const struct qstar_test_suite **sorted;
	struct qstar_string_list members;
	size_t i;

	memset(&members, 0, sizeof(members));
	sorted = malloc((graph->test_suite_len ? graph->test_suite_len : 1) *
	    sizeof(sorted[0]));
	if (!sorted)
		return -1;
	for (i = 0; i < graph->test_suite_len; i++)
		sorted[i] = &graph->test_suites[i];
	qsort(sorted, graph->test_suite_len, sizeof(sorted[0]), suite_ptr_cmp);
	for (i = 0; i < graph->test_suite_len; i++) {
		if (direct && string_list_contains(&sorted[i]->tests, target_label) &&
		    qstar_string_list_push(direct, sorted[i]->label) < 0)
			goto fail;
		qstar_string_list_free(&members);
		memset(&members, 0, sizeof(members));
		if (qstar_graph_resolve_test_suite_members(graph, sorted[i], NULL,
		    &members) < 0)
			goto fail;
		if (transitive && string_list_contains(&members, target_label) &&
		    qstar_string_list_push(transitive, sorted[i]->label) < 0)
			goto fail;
	}
	qstar_string_list_free(&members);
	free(sorted);
	return 0;

fail:
	qstar_string_list_free(&members);
	free(sorted);
	return -1;
}
