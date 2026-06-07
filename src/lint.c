#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 메시지 문자열이 부분 문자열을 포함하는지 검사한다. */
static int
contains_text(const char *message, const char *needle)
{
	return message && needle && strstr(message, needle) != NULL;
}

/** 기존 graph error 메시지를 QSTAR lint code로 보수적으로 분류한다. */
static const char *
classify_error_code(const char *message)
{
	if (contains_text(message, "leaks private include") ||
	    contains_text(message, "exposes private header"))
		return "QSTAR030";
	if (contains_text(message, "must be package-relative") ||
	    contains_text(message, "escapes package root") ||
	    contains_text(message, "outside package root"))
		return "QSTAR020";
	if (contains_text(message, "unknown target label") ||
	    contains_text(message, "invalid label") ||
	    contains_text(message, "invalid target name") ||
	    contains_text(message, "owned by package") ||
	    contains_text(message, "not visible"))
		return "QSTAR010";
	if (contains_text(message, "duplicate target label") ||
	    contains_text(message, "duplicate generated action label") ||
	    contains_text(message, "already used by generated action") ||
	    contains_text(message, "conflicts with target"))
		return "QSTAR011";
	return "QSTAR900";
}

/** QStar lint diagnostic을 graph에 추가한다. */
int
qstar_graph_add_lint(struct qstar_graph *graph, const char *code,
    const char *severity, const char *file, int line, const char *field,
    const char *label, const char *fmt, ...)
{
	struct qstar_lint_diagnostic *items, *diag;
	size_t cap;
	char message[1024];
	va_list ap;

	if (!graph)
		return -1;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	if (graph->lint_len == graph->lint_cap) {
		cap = graph->lint_cap ? graph->lint_cap * 2 : 8;
		items = realloc(graph->lint_diagnostics, cap * sizeof(items[0]));
		if (!items)
			return qstar_set_error(graph, "qstar: out of memory");
		graph->lint_diagnostics = items;
		graph->lint_cap = cap;
	}
	diag = &graph->lint_diagnostics[graph->lint_len++];
	memset(diag, 0, sizeof(*diag));
	diag->code = qstar_strdup(code ? code : "QSTAR900");
	diag->severity = qstar_strdup(severity ? severity : "error");
	diag->file = qstar_strdup(file && *file ? file : "<unknown>");
	diag->field = qstar_strdup(field && *field ? field : "<none>");
	diag->label = qstar_strdup(label && *label ? label : "<none>");
	diag->message = qstar_strdup(message);
	diag->line = line;
	if (!diag->code || !diag->severity || !diag->file || !diag->field ||
	    !diag->label || !diag->message)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** 현재 graph error buffer를 lint diagnostic으로 변환해 추가한다. */
int
qstar_graph_add_lint_from_error(struct qstar_graph *graph)
{
	if (!graph || !graph->error[0])
		return 0;
	return qstar_graph_add_lint(graph, classify_error_code(graph->error), "error",
	    graph->error_file[0] ? graph->error_file : "<unknown>",
	    graph->error_line,
	    graph->error_field[0] ? graph->error_field : "<none>",
	    graph->error_label[0] ? graph->error_label : "<none>",
	    "%s", graph->error);
}

/** label이 graph target으로 존재하는지 확인한다. */
static int
target_exists(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return 1;
	}
	return 0;
}

/** JSON 문자열을 LSP가 읽을 수 있게 escaping한다. */
static void
json_string(FILE *out, const char *s)
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

/** diagnostic severity별 개수를 계산한다. */
static void
count_diagnostics(const struct qstar_graph *graph, int *errors, int *warnings)
{
	size_t i;

	*errors = 0;
	*warnings = 0;
	for (i = 0; i < graph->lint_len; i++) {
		if (strcmp(graph->lint_diagnostics[i].severity, "warning") == 0)
			(*warnings)++;
		else
			(*errors)++;
	}
}

/** text lint output을 deterministic line format으로 출력한다. */
static void
emit_text(const struct qstar_graph *graph, FILE *out)
{
	int errors, warnings;
	size_t i;

	count_diagnostics(graph, &errors, &warnings);
	fputs("qstar lint v1\n", out);
	for (i = 0; i < graph->lint_len; i++) {
		const struct qstar_lint_diagnostic *diag = &graph->lint_diagnostics[i];
		fprintf(out,
		    "diagnostic code=%s severity=%s file=%s line=%d field=%s label=%s message=%s\n",
		    diag->code, diag->severity, diag->file, diag->line,
		    diag->field, diag->label, diag->message);
	}
	fprintf(out, "summary errors=%d warnings=%d\n", errors, warnings);
	fprintf(out, "status %s\n", errors ? "error" : warnings ? "warning" : "ok");
}

/** JSON lint output을 단일 object로 출력한다. */
static void
emit_json(const struct qstar_graph *graph, FILE *out)
{
	int errors, warnings;
	size_t i;

	count_diagnostics(graph, &errors, &warnings);
	fputs("{\"schema\":\"qstar-lint-v1\",\"diagnostics\":[", out);
	for (i = 0; i < graph->lint_len; i++) {
		const struct qstar_lint_diagnostic *diag = &graph->lint_diagnostics[i];
		if (i)
			fputc(',', out);
		fputs("{\"code\":", out);
		json_string(out, diag->code);
		fputs(",\"severity\":", out);
		json_string(out, diag->severity);
		fputs(",\"file\":", out);
		json_string(out, diag->file);
		fprintf(out, ",\"line\":%d,\"field\":", diag->line);
		json_string(out, diag->field);
		fputs(",\"label\":", out);
		json_string(out, diag->label);
		fputs(",\"message\":", out);
		json_string(out, diag->message);
		fputc('}', out);
	}
	fprintf(out,
	    "],\"summary\":{\"errors\":%d,\"warnings\":%d},\"status\":",
	    errors, warnings);
	json_string(out, errors ? "error" : warnings ? "warning" : "ok");
	fputs("}\n", out);
}

/** QStar lint diagnostic을 text 또는 LSP-ready JSON으로 출력한다. */
int
qstar_graph_lint(struct qstar_graph *graph, const char *label, const char *format, FILE *out)
{
	int errors, warnings;

	if (label && *label && strcmp(label, "//...") != 0 && !target_exists(graph, label)) {
		if (qstar_graph_add_lint(graph, "QSTAR010", "error", "<command-line>", 0,
		    "label", label, "unknown target label '%s'", label) < 0)
			return -1;
	}
	if (!format || strcmp(format, "text") == 0)
		emit_text(graph, out);
	else if (strcmp(format, "json") == 0)
		emit_json(graph, out);
	else
		return qstar_set_error(graph, "qstar: unsupported lint format '%s'", format);
	count_diagnostics(graph, &errors, &warnings);
	(void)warnings;
	return errors ? -1 : 0;
}
