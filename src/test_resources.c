#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if QSTAR_PLATFORM_WINDOWS
#include <direct.h>
#define qstar_test_mkdir(path) _mkdir(path)
#else
#define qstar_test_mkdir(path) mkdir(path, 0777)
#endif

#define QSTAR_TEST_DEFAULT_TIMEOUT_SEC 5

enum qstar_test_phase {
	QSTAR_TEST_PENDING = 0,
	QSTAR_TEST_SETUP,
	QSTAR_TEST_BODY,
	QSTAR_TEST_CLEANUP,
	QSTAR_TEST_DONE
};

struct qstar_test_run {
	const struct qstar_target *target;
	enum qstar_test_phase phase;
	qstar_process_id pid;
	time_t start;
	int running;
	int resources_held;
	int attempt;
	int exit_code;
	char status[16];
	char skip_reason[256];
	char action_id[QSTAR_PATH_MAX];
	char stdout_rel[QSTAR_PATH_MAX];
	char stderr_rel[QSTAR_PATH_MAX];
	char result_action_id[QSTAR_PATH_MAX];
	char result_stdout_rel[QSTAR_PATH_MAX];
	char result_stderr_rel[QSTAR_PATH_MAX];
	struct qstar_argv argv;
};

static int
valid_resource_name(const char *name)
{
	const unsigned char *p;

	if (!name || !*name)
		return 0;
	for (p = (const unsigned char *)name; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9')) && *p != '_' && *p != '-' && *p != '.')
			return 0;
	}
	return 1;
}

int
qstar_graph_validate_test_resources(struct qstar_graph *graph)
{
	const struct qstar_test_resource *resource;
	const struct qstar_test_resource_request *request;
	const struct qstar_target *target;
	size_t i, j;

	for (i = 0; i < graph->test_resource_len; i++) {
		resource = &graph->test_resources[i];
		if (!valid_resource_name(resource->name))
			return qstar_set_error_origin(graph, resource->origin_file,
			    resource->origin_line, "name", resource->name,
			    "qstar: test resource id '%s' must use only letters, digits, '_', '-', or '.'",
			    resource->name);
		if (resource->capacity <= 0)
			return qstar_set_error_origin(graph, resource->origin_file,
			    resource->origin_line, "capacity", resource->name,
			    "qstar: test resource '%s' capacity must be greater than zero",
			    resource->name);
	}
	for (i = 0; i < graph->len; i++) {
		target = &graph->targets[i];
		if (target->test_resource_len == 0)
			continue;
		if (strcmp(target->kind, "test") != 0)
			return qstar_set_error_origin(graph, target->origin_file,
			    target->origin_line, "resources", target->label,
			    "qstar: test resources are only valid on qstar.test targets");
		for (j = 0; j < target->test_resource_len; j++) {
			request = &target->test_resources[j];
			resource = qstar_graph_find_test_resource(graph, request->name);
			if (!resource)
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "resources", target->label,
				    "qstar: test '%s' requests unknown resource '%s'",
				    target->label, request->name);
			if (request->amount <= 0 || request->amount > resource->capacity)
				return qstar_set_error_origin(graph, target->origin_file,
				    target->origin_line, "resources", target->label,
				    "qstar: test '%s' resource '%s' request %d exceeds capacity %d",
				    target->label, request->name, request->amount,
				    resource->capacity);
		}
	}
	return 0;
}

static int
mkdir_p(const char *path)
{
	char tmp[QSTAR_PATH_MAX];
	size_t i, n;

	n = strlen(path);
	if (n == 0 || n >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, n + 1);
	for (i = 1; i < n; i++) {
		if (tmp[i] != '/' && tmp[i] != '\\')
			continue;
		if (i == 2 && tmp[1] == ':')
			continue;
		tmp[i] = '\0';
		if (*tmp && qstar_test_mkdir(tmp) < 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}
	return qstar_test_mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
}

static int
full_path(const struct qstar_graph *graph, const char *rel, char *dst,
    size_t dstlen)
{
	const char *root = graph->package_root && *graph->package_root ?
	    graph->package_root : ".";

	if (rel[0] == '/')
		return snprintf(dst, dstlen, "%s", rel) < (int)dstlen ? 0 : -1;
#if QSTAR_PLATFORM_WINDOWS
	if (isalpha((unsigned char)rel[0]) && rel[1] == ':')
		return snprintf(dst, dstlen, "%s", rel) < (int)dstlen ? 0 : -1;
#endif
	return snprintf(dst, dstlen, "%s/%s", root, rel) < (int)dstlen ? 0 : -1;
}

static void
write_shell_arg(FILE *f, const char *s)
{
	const unsigned char *p;
	int simple;

	p = (const unsigned char *)(s ? s : "");
	simple = *p != '\0';
	for (; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
		    *p == '/' || *p == ':' || *p == '=' || *p == '+' || *p == ','))
			simple = 0;
	}
	if (simple) {
		fputs(s, f);
		return;
	}
	fputc('\'', f);
	for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '\'')
			fputs("'\\''", f);
		else
			fputc(*p, f);
	}
	fputc('\'', f);
}

static void
json_string(FILE *f, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	fputc('"', f);
	for (; *p; p++) {
		if (*p == '"' || *p == '\\')
			fprintf(f, "\\%c", *p);
		else if (*p == '\n')
			fputs("\\n", f);
		else if (*p == '\r')
			fputs("\\r", f);
		else if (*p == '\t')
			fputs("\\t", f);
		else if (*p < 0x20)
			fprintf(f, "\\u%04x", *p);
		else
			fputc(*p, f);
	}
	fputc('"', f);
}

static void
xml_string(FILE *f, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	for (; *p; p++) {
		if (*p == '&')
			fputs("&amp;", f);
		else if (*p == '<')
			fputs("&lt;", f);
		else if (*p == '>')
			fputs("&gt;", f);
		else if (*p == '"')
			fputs("&quot;", f);
		else if (*p == '\'')
			fputs("&apos;", f);
		else if (*p >= 0x20 || *p == '\n' || *p == '\t')
			fputc(*p, f);
	}
}

static int
test_log_paths(struct qstar_graph *graph, struct qstar_test_run *run,
    const char *phase)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], logs_rel[QSTAR_PATH_MAX];
	char logs_full[QSTAR_PATH_MAX];
	size_t base_len;

	if (snprintf(run->action_id, sizeof(run->action_id), "%s:test:%s:%d",
	    run->target->label, phase, run->attempt) >= (int)sizeof(run->action_id))
		return qstar_set_error(graph, "qstar: test action id is too long");
	qstar_mangle_label(run->action_id, name, sizeof(name));
	if (strcmp(phase, "test") == 0 && run->attempt == 1) {
		qstar_mangle_label(run->target->label, name, sizeof(name));
		base_len = strlen(name);
		if (snprintf(name + base_len, sizeof(name) - base_len,
		    ".test") >= (int)(sizeof(name) - base_len))
			return qstar_set_error(graph, "qstar: test log name is too long");
	}
	if (qstar_graph_build_path(graph, "logs", logs_rel, sizeof(logs_rel)) < 0 ||
	    full_path(graph, logs_rel, logs_full, sizeof(logs_full)) < 0 ||
	    mkdir_p(logs_full) < 0)
		return qstar_set_error(graph, "qstar: could not create test log directory");
	if (snprintf(sub, sizeof(sub), "logs/%s.stdout", name) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, run->stdout_rel,
	    sizeof(run->stdout_rel)) < 0 ||
	    snprintf(sub, sizeof(sub), "logs/%s.stderr", name) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, run->stderr_rel,
	    sizeof(run->stderr_rel)) < 0)
		return qstar_set_error(graph, "qstar: test log path is too long");
	return 0;
}

static void
free_argv(struct qstar_test_run *run)
{
	qstar_argv_free(&run->argv);
}

static int
make_argv(struct qstar_graph *graph, struct qstar_test_run *run,
    const struct qstar_string_list *command, const char *artifact)
{
	char resolved[QSTAR_PATH_MAX];
	const char *item;
	size_t i, len;

	free_argv(run);
	len = artifact ? 1 : command->len;
	if (len == 0)
		return qstar_set_error(graph, "qstar: test hook command argv is empty");
	if (qstar_argv_reserve(&run->argv, len) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 0; i < len; i++) {
		item = artifact ? artifact : command->items[i];
		if (qstar_graph_resolve_command_token(graph, item, resolved,
		    sizeof(resolved)) < 0)
			return -1;
		if (qstar_argv_push(&run->argv, resolved) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

static void
write_action_log(struct qstar_graph *graph, const struct qstar_test_run *run,
    const char *backend, const char *exit_text, const char *description)
{
	char name[QSTAR_PATH_MAX], sub[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	char path[QSTAR_PATH_MAX];
	FILE *f;
	size_t i;

	qstar_mangle_label(run->action_id, name, sizeof(name));
	if (snprintf(sub, sizeof(sub), "logs/%s.log", name) >= (int)sizeof(sub) ||
	    qstar_graph_build_path(graph, sub, rel, sizeof(rel)) < 0 ||
	    full_path(graph, rel, path, sizeof(path)) < 0)
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "qstar-action-log v2\nexit=%s\nbackend=%s\n", exit_text,
	    backend && *backend ? backend : "stella");
	fputs("description=", f);
	write_shell_arg(f, description);
	fputs("\nenvc=0\noutput_count=0\n", f);
	fprintf(f, "argc=%zu\n", run->argv.len);
	for (i = 0; i < run->argv.len; i++) {
		fprintf(f, "argv[%zu]=", i);
		write_shell_arg(f, run->argv.items[i]);
		fputc('\n', f);
	}
	fputs("argv=", f);
	for (i = 0; i < run->argv.len; i++)
		fprintf(f, "%s%s", i ? " " : "", run->argv.items[i]);
	fputs("\nargv_shell=", f);
	for (i = 0; i < run->argv.len; i++) {
		if (i)
			fputc(' ', f);
		write_shell_arg(f, run->argv.items[i]);
	}
	fputc('\n', f);
	fclose(f);
}

static void
record_spawn_failure(struct qstar_graph *graph, struct qstar_test_run *run,
    const char *backend, const char *phase)
{
	char description[QSTAR_PATH_MAX];

	snprintf(description, sizeof(description), "Test %s %s attempt %d",
	    run->target->label, phase, run->attempt);
	write_action_log(graph, run, backend, "spawn-error", description);
	snprintf(run->result_action_id, sizeof(run->result_action_id), "%s",
	    run->action_id);
	snprintf(run->result_stdout_rel, sizeof(run->result_stdout_rel), "%s",
	    run->stdout_rel);
	snprintf(run->result_stderr_rel, sizeof(run->result_stderr_rel), "%s",
	    run->stderr_rel);
}

static int
start_phase(struct qstar_graph *graph, struct qstar_test_run *run,
    enum qstar_test_phase phase, const char *backend, FILE *out)
{
	const struct qstar_string_list *command = NULL;
	char artifact[QSTAR_PATH_MAX], description[QSTAR_PATH_MAX];
	const char *phase_name, *runner;

	run->phase = phase;
	if (phase == QSTAR_TEST_SETUP) {
		phase_name = "setup";
		command = &run->target->test_setup;
	} else if (phase == QSTAR_TEST_CLEANUP) {
		phase_name = "cleanup";
		command = &run->target->test_cleanup;
	} else {
		phase_name = "test";
		if (qstar_graph_artifact_output_path(graph, run->target, artifact,
		    sizeof(artifact)) < 0)
			return -1;
	}
	if (test_log_paths(graph, run, phase_name) < 0 ||
	    make_argv(graph, run, command, phase == QSTAR_TEST_BODY ? artifact : NULL) < 0)
		return -1;
	fprintf(out, "test_%s label=%s attempt=%d action=%s stdout=%s stderr=%s\n",
	    phase_name, run->target->label, run->attempt, run->action_id,
	    run->stdout_rel, run->stderr_rel);
	if (qstar_platform_process_start_file_output(graph,
	    graph->package_root && *graph->package_root ? graph->package_root : ".",
	    run->argv.items, run->stdout_rel, run->stderr_rel, &run->pid, &runner) < 0)
		return -1;
	(void)backend;
	run->start = time(NULL);
	run->running = 1;
	if (snprintf(description, sizeof(description), "Test %s %s attempt %d",
	    run->target->label, phase_name, run->attempt) >= (int)sizeof(description))
		description[0] = '\0';
	return 0;
}

static int
resource_index(const struct qstar_graph *graph, const char *name)
{
	const struct qstar_test_resource *resource;

	resource = qstar_graph_find_test_resource(graph, name);
	return resource ? (int)(resource - graph->test_resources) : -1;
}

static int
resources_available(const struct qstar_graph *graph, const int *used,
    const struct qstar_target *target)
{
	int index;
	size_t i;

	for (i = 0; i < target->test_resource_len; i++) {
		index = resource_index(graph, target->test_resources[i].name);
		if (index < 0 || used[index] + target->test_resources[i].amount >
		    graph->test_resources[index].capacity)
			return 0;
	}
	return 1;
}

static void
change_resources(const struct qstar_graph *graph, int *used,
    const struct qstar_target *target, int acquire, FILE *out)
{
	int index, delta;
	size_t i;

	for (i = 0; i < target->test_resource_len; i++) {
		index = resource_index(graph, target->test_resources[i].name);
		if (index < 0)
			continue;
		delta = target->test_resources[i].amount;
		used[index] += acquire ? delta : -delta;
		fprintf(out,
		    "test_resource event=%s test=%s resource=%s amount=%d used=%d capacity=%d\n",
		    acquire ? "acquire" : "release", target->label,
		    graph->test_resources[index].name, delta, used[index],
		    graph->test_resources[index].capacity);
	}
}

static int
retry_allowed(const struct qstar_target *target, const char *status)
{
	size_t i;

	for (i = 0; i < target->test_retry_on.len; i++) {
		if (strcmp(target->test_retry_on.items[i], status) == 0)
			return 1;
	}
	return 0;
}

static void
phase_exit_text(int timed_out, int status, char *dst, size_t dstlen)
{
	if (timed_out)
		snprintf(dst, dstlen, "timeout");
	else if (qstar_platform_process_has_exit_code(status))
		snprintf(dst, dstlen, "%d",
		    qstar_platform_process_status_exit_code(status));
	else
		snprintf(dst, dstlen, "signal-%d",
		    qstar_platform_process_signal_number(status));
}

static void
set_phase_result(struct qstar_test_run *run, enum qstar_test_phase phase,
    int status, int timed_out)
{
	if (timed_out) {
		snprintf(run->status, sizeof(run->status), "%s",
		    phase == QSTAR_TEST_CLEANUP ? "error" : "timeout");
		run->exit_code = 124;
		return;
	}
	if (qstar_platform_process_exited_success(status))
		return;
	if (phase == QSTAR_TEST_BODY && qstar_platform_process_has_exit_code(status)) {
		snprintf(run->status, sizeof(run->status), "fail");
		run->exit_code = qstar_platform_process_status_exit_code(status);
	} else {
		snprintf(run->status, sizeof(run->status), "error");
		run->exit_code = qstar_platform_process_has_exit_code(status) ?
		    qstar_platform_process_status_exit_code(status) : 128 +
		    qstar_platform_process_signal_number(status);
	}
}

static int
finish_attempt(struct qstar_graph *graph, struct qstar_test_run *run, int *used,
    size_t *holders, FILE *out)
{
	change_resources(graph, used, run->target, 0, out);
	run->resources_held = 0;
	if (*holders > 0)
		(*holders)--;
	if (run->attempt <= run->target->test_retry_count &&
	    retry_allowed(run->target, run->status)) {
		fprintf(out, "test_retry label=%s status=%s completed_attempt=%d next_attempt=%d\n",
		    run->target->label, run->status, run->attempt, run->attempt + 1);
		run->phase = QSTAR_TEST_PENDING;
		run->status[0] = '\0';
		run->exit_code = 0;
		return 0;
	}
	run->phase = QSTAR_TEST_DONE;
	fprintf(out,
	    "test_result label=%s status=%s exit=%d attempts=%d action=%s stdout=%s stderr=%s\n",
	    run->target->label, run->status, run->exit_code, run->attempt,
	    run->result_action_id, run->result_stdout_rel, run->result_stderr_rel);
	return 0;
}

static int
advance_after_phase(struct qstar_graph *graph, struct qstar_test_run *run,
    enum qstar_test_phase completed, int *used, size_t *holders,
    const char *backend, FILE *out)
{
	char saved_error[sizeof(graph->error)];

	if (completed == QSTAR_TEST_SETUP && run->status[0] == '\0') {
		if (start_phase(graph, run, QSTAR_TEST_BODY, backend, out) == 0)
			return 0;
		snprintf(saved_error, sizeof(saved_error), "%s", graph->error);
		graph->error[0] = '\0';
		snprintf(run->status, sizeof(run->status), "error");
		run->exit_code = 127;
		record_spawn_failure(graph, run, backend, "test");
		fprintf(out, "test_error label=%s phase=test message=%s\n",
		    run->target->label, saved_error);
	}
	if (completed != QSTAR_TEST_CLEANUP && run->target->test_cleanup.len > 0) {
		if (start_phase(graph, run, QSTAR_TEST_CLEANUP, backend, out) == 0)
			return 0;
		snprintf(saved_error, sizeof(saved_error), "%s", graph->error);
		graph->error[0] = '\0';
		snprintf(run->status, sizeof(run->status), "error");
		run->exit_code = 127;
		record_spawn_failure(graph, run, backend, "cleanup");
		fprintf(out, "test_error label=%s phase=cleanup message=%s\n",
		    run->target->label, saved_error);
	}
	if (run->status[0] == '\0')
		snprintf(run->status, sizeof(run->status), "pass");
	return finish_attempt(graph, run, used, holders, out);
}

static int
start_attempt(struct qstar_graph *graph, struct qstar_test_run *run, int *used,
    size_t *holders, const char *backend, FILE *out)
{
	char saved_error[sizeof(graph->error)];
	enum qstar_test_phase phase;

	change_resources(graph, used, run->target, 1, out);
	run->resources_held = 1;
	(*holders)++;
	run->attempt++;
	phase = run->target->test_setup.len > 0 ? QSTAR_TEST_SETUP : QSTAR_TEST_BODY;
	if (start_phase(graph, run, phase, backend, out) == 0)
		return 0;
	snprintf(saved_error, sizeof(saved_error), "%s", graph->error);
	graph->error[0] = '\0';
	snprintf(run->status, sizeof(run->status), "error");
	run->exit_code = 127;
	record_spawn_failure(graph, run, backend,
	    phase == QSTAR_TEST_SETUP ? "setup" : "test");
	fprintf(out, "test_error label=%s phase=%s message=%s\n",
	    run->target->label, phase == QSTAR_TEST_SETUP ? "setup" : "test",
	    saved_error);
	return advance_after_phase(graph, run, phase, used, holders, backend, out);
}

static int
write_json_report(struct qstar_graph *graph, const struct qstar_test_run *runs,
    size_t len, const char *path, const char *backend)
{
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];
	char *slash;
	FILE *f;
	size_t i;

	if (!path || !*path) {
		if (qstar_graph_build_path(graph, "test-results.json", rel,
		    sizeof(rel)) < 0)
			return qstar_set_error(graph, "qstar: test report path is too long");
		path = rel;
	}
	if (!qstar_path_is_package_relative(path))
		return qstar_set_error(graph,
		    "qstar: test report path must be package-relative");
	if (full_path(graph, path, full, sizeof(full)) < 0)
		return qstar_set_error(graph, "qstar: test report path is too long");
	snprintf(parent, sizeof(parent), "%s", full);
	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		if (mkdir_p(parent) < 0)
			return qstar_set_error(graph, "qstar: could not create test report directory");
	}
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write test JSON report '%s'",
		    path);
	fputs("{\"schema\":\"qstar-test-results-v1\",\"backend\":", f);
	json_string(f, backend);
	fprintf(f, ",\"test_count\":%zu,\"tests\":[", len);
	for (i = 0; i < len; i++) {
		if (i)
			fputc(',', f);
		fputs("{\"label\":", f);
		json_string(f, runs[i].target->label);
		fputs(",\"status\":", f);
		json_string(f, runs[i].status);
		fprintf(f, ",\"attempts\":%d,\"exit_code\":%d,\"action_id\":",
		    runs[i].attempt, runs[i].exit_code);
		json_string(f, runs[i].result_action_id);
		fputs(",\"stdout\":", f);
		json_string(f, runs[i].result_stdout_rel);
		fputs(",\"stderr\":", f);
		json_string(f, runs[i].result_stderr_rel);
		fputs(",\"skip_reason\":", f);
		json_string(f, runs[i].skip_reason);
		fputc('}', f);
	}
	fputs("]}\n", f);
	fclose(f);
	return 0;
}

static int
write_junit_report(struct qstar_graph *graph, const struct qstar_test_run *runs,
    size_t len, const char *path)
{
	char full[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX], *slash;
	FILE *f;
	size_t i, failures = 0, errors = 0, skipped = 0;

	if (!path || !*path)
		return 0;
	if (!qstar_path_is_package_relative(path) ||
	    full_path(graph, path, full, sizeof(full)) < 0)
		return qstar_set_error(graph,
		    "qstar: JUnit report path must be package-relative");
	snprintf(parent, sizeof(parent), "%s", full);
	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		if (mkdir_p(parent) < 0)
			return qstar_set_error(graph, "qstar: could not create JUnit report directory");
	}
	for (i = 0; i < len; i++) {
		if (strcmp(runs[i].status, "fail") == 0)
			failures++;
		else if (strcmp(runs[i].status, "error") == 0 ||
		    strcmp(runs[i].status, "timeout") == 0)
			errors++;
		else if (strcmp(runs[i].status, "skip") == 0)
			skipped++;
	}
	f = fopen(full, "w");
	if (!f)
		return qstar_set_error(graph, "qstar: could not write JUnit report '%s'",
		    path);
	fprintf(f,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"qstar\" tests=\"%zu\" failures=\"%zu\" errors=\"%zu\" skipped=\"%zu\">\n",
	    len, failures, errors, skipped);
	for (i = 0; i < len; i++) {
		fputs("  <testcase name=\"", f);
		xml_string(f, runs[i].target->label);
		fputs("\">", f);
		if (strcmp(runs[i].status, "skip") == 0) {
			fputs("<skipped message=\"", f);
			xml_string(f, runs[i].skip_reason);
			fputs("\"/>", f);
		} else if (strcmp(runs[i].status, "fail") == 0) {
			fprintf(f, "<failure message=\"exit %d\"/>", runs[i].exit_code);
		} else if (strcmp(runs[i].status, "error") == 0 ||
		    strcmp(runs[i].status, "timeout") == 0) {
			fputs("<error type=\"", f);
			xml_string(f, runs[i].status);
			fputs("\"/>", f);
		}
		fputs("</testcase>\n", f);
	}
	fputs("</testsuite>\n", f);
	fclose(f);
	return 0;
}

int
qstar_graph_execute_test_batch(struct qstar_graph *graph,
    const struct qstar_target *const *targets, size_t target_len,
    const struct qstar_test_options *options, const char *backend, FILE *out)
{
	struct qstar_test_run *runs;
	int *used;
	size_t i, done_count, holders;
	int jobs, progress, status, process_done, timed_out, timeout_sec, failed;
	char exit_text[64], description[QSTAR_PATH_MAX];
	enum qstar_test_phase completed;

	runs = calloc(target_len ? target_len : 1, sizeof(runs[0]));
	used = calloc(graph->test_resource_len ? graph->test_resource_len : 1,
	    sizeof(used[0]));
	if (!runs || !used) {
		free(runs);
		free(used);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	jobs = options && options->jobs > 0 ? options->jobs : 1;
	for (i = 0; i < target_len; i++) {
		runs[i].target = targets[i];
		if (targets[i]->test_skip_reason && *targets[i]->test_skip_reason) {
			runs[i].phase = QSTAR_TEST_DONE;
			snprintf(runs[i].status, sizeof(runs[i].status), "skip");
			snprintf(runs[i].skip_reason, sizeof(runs[i].skip_reason), "%s",
			    targets[i]->test_skip_reason);
			fprintf(out, "test_result label=%s status=skip attempts=0 reason=%s\n",
			    targets[i]->label, runs[i].skip_reason);
		} else if (targets[i]->test_manual &&
		    !(options && options->include_manual)) {
			runs[i].phase = QSTAR_TEST_DONE;
			snprintf(runs[i].status, sizeof(runs[i].status), "skip");
			snprintf(runs[i].skip_reason, sizeof(runs[i].skip_reason),
			    "manual test excluded from automatic selection");
			fprintf(out, "test_result label=%s status=skip attempts=0 reason=manual\n",
			    targets[i]->label);
		}
	}
	done_count = 0;
	for (i = 0; i < target_len; i++)
		done_count += runs[i].phase == QSTAR_TEST_DONE;
	holders = 0;
	while (done_count < target_len) {
		progress = 0;
		for (i = 0; i < target_len && holders < (size_t)jobs; i++) {
			if (runs[i].phase != QSTAR_TEST_PENDING ||
			    !resources_available(graph, used, runs[i].target))
				continue;
			if (start_attempt(graph, &runs[i], used, &holders, backend, out) < 0)
				goto fail;
			if (runs[i].phase == QSTAR_TEST_DONE)
				done_count++;
			progress = 1;
		}
		for (i = 0; i < target_len; i++) {
			if (!runs[i].running)
				continue;
			if (qstar_platform_process_wait_nohang(runs[i].pid, &status,
			    &process_done) < 0) {
				qstar_set_error(graph, "qstar: test process wait failed");
				goto fail;
			}
			timeout_sec = runs[i].target->test_timeout_sec > 0 ?
			    runs[i].target->test_timeout_sec : QSTAR_TEST_DEFAULT_TIMEOUT_SEC;
			timed_out = !process_done && time(NULL) - runs[i].start >= timeout_sec;
			if (timed_out) {
				qstar_platform_process_terminate(runs[i].pid, &status);
				process_done = 1;
			}
			if (!process_done)
				continue;
			completed = runs[i].phase;
			runs[i].running = 0;
			phase_exit_text(timed_out, status, exit_text, sizeof(exit_text));
			snprintf(description, sizeof(description), "Test %s %s attempt %d",
			    runs[i].target->label,
			    completed == QSTAR_TEST_SETUP ? "setup" :
			    completed == QSTAR_TEST_CLEANUP ? "cleanup" : "test",
			    runs[i].attempt);
			write_action_log(graph, &runs[i], backend, exit_text, description);
			if (completed == QSTAR_TEST_BODY ||
			    (completed == QSTAR_TEST_SETUP && runs[i].status[0] == '\0')) {
				snprintf(runs[i].result_action_id,
				    sizeof(runs[i].result_action_id), "%s", runs[i].action_id);
				snprintf(runs[i].result_stdout_rel,
				    sizeof(runs[i].result_stdout_rel), "%s", runs[i].stdout_rel);
				snprintf(runs[i].result_stderr_rel,
				    sizeof(runs[i].result_stderr_rel), "%s", runs[i].stderr_rel);
			}
			set_phase_result(&runs[i], completed, status, timed_out);
			if (completed == QSTAR_TEST_CLEANUP &&
			    strcmp(runs[i].status, "error") == 0) {
				snprintf(runs[i].result_action_id,
				    sizeof(runs[i].result_action_id), "%s", runs[i].action_id);
				snprintf(runs[i].result_stdout_rel,
				    sizeof(runs[i].result_stdout_rel), "%s", runs[i].stdout_rel);
				snprintf(runs[i].result_stderr_rel,
				    sizeof(runs[i].result_stderr_rel), "%s", runs[i].stderr_rel);
			}
			free_argv(&runs[i]);
			if (advance_after_phase(graph, &runs[i], completed, used, &holders,
			    backend, out) < 0)
				goto fail;
			if (runs[i].phase == QSTAR_TEST_DONE)
				done_count++;
			progress = 1;
		}
		if (!progress)
			qstar_platform_process_sleep_ms(20);
	}
	if (write_json_report(graph, runs, target_len,
	    options ? options->report_json : NULL, backend) < 0 ||
	    write_junit_report(graph, runs, target_len,
	    options ? options->output_junit : NULL) < 0)
		goto fail;
	failed = 0;
	for (i = 0; i < target_len; i++) {
		if (strcmp(runs[i].status, "fail") == 0 ||
		    strcmp(runs[i].status, "error") == 0 ||
		    strcmp(runs[i].status, "timeout") == 0)
			failed++;
		free_argv(&runs[i]);
	}
	free(runs);
	free(used);
	if (failed)
		return qstar_set_error(graph, "qstar: %d test result(s) failed", failed);
	return 0;

fail:
	for (i = 0; i < target_len; i++) {
		if (runs[i].running)
			qstar_platform_process_terminate(runs[i].pid, &status);
		free_argv(&runs[i]);
	}
	free(runs);
	free(used);
	return -1;
}
