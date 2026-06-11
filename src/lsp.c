#include "internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qstar_lsp_doc {
	char *uri;
	char *path;
	char *text;
	struct qstar_lsp_doc *next;
};

struct qstar_lsp_server {
	struct qstar_lsp_doc *docs;
	int shutdown;
};

struct qstar_lsp_buf {
	char *data;
	size_t len;
	size_t cap;
};

struct qstar_lsp_hover_entry {
	const char *name;
	const char *text;
};

static const struct qstar_lsp_hover_entry qstar_lsp_symbols[] = {
	{ "qstar.project", "Declare package-root project metadata, build_dir, generated_dir, and compile database policy." },
	{ "qstar.profile", "Declare an in-DSL toolchain/profile policy for qstar.lua." },
	{ "qstar.config", "Declare a reusable target option bundle for configs = { ... }." },
	{ "qstar.executable", "Create an executable target." },
	{ "qstar.staticlib", "Create a static library target." },
	{ "qstar.sharedlib", "Create a shared library target, if the profile supports it." },
	{ "qstar.test", "Create a test executable target." },
	{ "qstar.custom_target", "Create a package-local generated action." },
	{ "qstar.run_target", "Declare a named external run action." },
	{ "qstar.group", "Declare a deps-only aggregate target with no artifact." },
	{ "qstar.configure_file", "Generate a deterministic config header or configured file." },
	{ "qstar.stage", "Create a copy-only package/boot staging rule." },
	{ "qstar.stage_file", "Map a source artifact or file into a staging destination path." },
	{ "qstar.target_family", "Group profile variants for lint policy such as shared source suppression." },
	{ "qstar.cli", "Build an argv-vector command for custom_target or run_target." },
	{ "qstar.input", "Reference a custom_target input by index inside qstar.cli." },
	{ "qstar.subdir", "Load a canonical subdir fragment named <folder>.qst." },
	{ "qstar.import_file", "Load a package-relative .qst graph fragment exactly once." },
	{ "qstar.import_module", "Load a package-relative module folder as <name>/<name>.qsm and return its table." },
	{ "qstar.files", "Return an explicit file list for target fields." },
	{ "qstar.output", "Declare a generated output path, or reference an output by index inside qstar.cli." },
	{ "qstar.target_file", "Reference another target artifact path inside qstar.cli." },
	{ "qstar.version", "QStar authoring/runtime version string." },
	{ "qstar.host.os", "Host operating system identifier used by the QStar evaluator." },
	{ "qstar.host.arch", "Host architecture identifier used by the QStar evaluator." },
	{ "qstar.project.root", "Resolved package/project root path for the current qstar.lua." },
	{ "QSTAR_VERSION", "QStar authoring/runtime version string." },
	{ "QSTAR_VERSION_MAJOR", "QStar major version integer." },
	{ "QSTAR_VERSION_MINOR", "QStar minor version integer." },
	{ "QSTAR_VERSION_PATCH", "QStar patch version integer." },
	{ "QSTAR_HOST_OS", "Host operating system identifier." },
	{ "QSTAR_HOST_ARCH", "Host architecture identifier." },
	{ "QSTAR_PACKAGE_ROOT", "Resolved package root path." },
	{ "QSTAR_PROJECT_ROOT", "Resolved project root path." },
	{ "QSTAR_PROFILE", "Active profile name." },
	{ "QSTAR_TARGET", "Active target triple or host." },
};

static const struct qstar_lsp_hover_entry qstar_lsp_fields[] = {
	{ "sources", "Compile or generated source inputs for this target." },
	{ "configs", "Reusable qstar.config labels merged before target-local fields." },
	{ "generated_dir", "Project-level package-relative root for qstar.output generated artifacts." },
	{ "deps", "Public dependency edges used for build, link, and include propagation." },
	{ "public_deps", "Alias for public dependency edges." },
	{ "private_deps", "Private dependency edges used for build/link without public include propagation." },
	{ "public_headers", "Language-local exported headers under lang.c, lang.cxx, or lang.cale." },
	{ "private_headers", "Language-local private headers under lang.c, lang.cxx, or lang.cale." },
	{ "lang", "Per-language option namespace." },
	{ "c", "C language options under lang.c." },
	{ "cxx", "C++ language options under lang.cxx." },
	{ "asm", "Assembly language options under lang.asm." },
	{ "cale", "Cale language options under lang.cale." },
	{ "include_dirs", "Language-local include directories under lang.<language>." },
	{ "public_include_dirs", "Language include directories propagated to public dependents." },
	{ "private_include_dirs", "Language include directories used only by this target." },
	{ "system_include_dirs", "Language system include directories under lang.c or lang.cxx." },
	{ "compile_options", "Language-specific compiler options." },
	{ "defines", "Language-specific preprocessor defines." },
	{ "standard", "C++ standard string under lang.cxx.standard." },
	{ "preprocess", "Assembly preprocessing switch under lang.asm.preprocess." },
	{ "modules", "Language module skeleton under lang.cxx or lang.cale." },
	{ "enabled", "Boolean switch for lang.cxx.modules; true is currently unsupported." },
	{ "profile", "Cale language profile under lang.cale.profile." },
	{ "command", "argv-vector command built with qstar.cli." },
	{ "timeout", "run_target timeout in seconds." },
	{ "marker", "run_target stdout/stderr/marker_log marker string." },
	{ "marker_log", "Optional package-relative serial log file scanned for run_target marker." },
	{ "root", "Stage root path for qstar.stage, package-relative." },
	{ "files", "Stage file mappings built with qstar.stage_file." },
	{ "variants", "Target family variant names such as x86_64, aarch64, or rv64." },
	{ "targets", "Explicit target labels that belong to a target_family." },
	{ "allow_shared_sources", "Allow duplicate source use inside this target_family lint group." },
	{ "visibility", "Package visibility patterns that may depend on this target." },
	{ "toolchain", "Toolchain profile name for this target." },
	{ "artifact_name", "Target artifact filename override, for example BOOTX64.EFI." },
	{ "libs", "System libraries rendered by the selected target profile." },
	{ "link_options", "Raw target linker options appended before object inputs." },
	{ "linker_script", "Package-relative linker script rendered as -T <script>." },
	{ "defsyms", "Linker --defsym=NAME=VALUE entries." },
	{ "frameworks", "Darwin frameworks rendered by the selected target profile." },
};

/** 동적 문자열 buffer를 빈 상태로 초기화한다. */
static void
lsp_buf_init(struct qstar_lsp_buf *buf)
{
	memset(buf, 0, sizeof(*buf));
}

/** 동적 문자열 buffer가 소유한 메모리를 해제한다. */
static void
lsp_buf_free(struct qstar_lsp_buf *buf)
{
	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}

/** 동적 문자열 buffer에 raw byte를 덧붙인다. */
static int
lsp_buf_append_n(struct qstar_lsp_buf *buf, const char *s, size_t n)
{
	char *data;
	size_t cap;

	if (buf->len + n + 1 > buf->cap) {
		cap = buf->cap ? buf->cap * 2 : 256;
		while (cap < buf->len + n + 1)
			cap *= 2;
		data = realloc(buf->data, cap);
		if (!data)
			return -1;
		buf->data = data;
		buf->cap = cap;
	}
	memcpy(buf->data + buf->len, s, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
	return 0;
}

/** 동적 문자열 buffer에 C string을 덧붙인다. */
static int
lsp_buf_append(struct qstar_lsp_buf *buf, const char *s)
{
	return lsp_buf_append_n(buf, s ? s : "", strlen(s ? s : ""));
}

/** printf 형식으로 동적 문자열 buffer에 추가한다. */
static int
lsp_buf_printf(struct qstar_lsp_buf *buf, const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	return lsp_buf_append_n(buf, tmp, (size_t)n);
}

/** JSON string literal을 buffer에 escaping해 추가한다. */
static int
lsp_json_string(struct qstar_lsp_buf *buf, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	if (lsp_buf_append(buf, "\"") < 0)
		return -1;
	while (*p) {
		if (*p == '"' || *p == '\\') {
			if (lsp_buf_printf(buf, "\\%c", *p) < 0)
				return -1;
		} else if (*p == '\n') {
			if (lsp_buf_append(buf, "\\n") < 0)
				return -1;
		} else if (*p == '\r') {
			if (lsp_buf_append(buf, "\\r") < 0)
				return -1;
		} else if (*p == '\t') {
			if (lsp_buf_append(buf, "\\t") < 0)
				return -1;
		} else if (*p < 0x20) {
			if (lsp_buf_printf(buf, "\\u%04x", *p) < 0)
				return -1;
		} else if (lsp_buf_append_n(buf, (const char *)p, 1) < 0) {
			return -1;
		}
		p++;
	}
	return lsp_buf_append(buf, "\"");
}

/** LSP 응답/notification payload를 Content-Length framing으로 출력한다. */
static int
lsp_send(FILE *out, const char *payload)
{
	fprintf(out, "Content-Length: %zu\r\n\r\n%s", strlen(payload), payload);
	fflush(out);
	return ferror(out) ? -1 : 0;
}

/** LSP header에서 Content-Length 값을 추출한다. */
static int
parse_content_length(const char *header)
{
	const char *p;

	p = strstr(header, "Content-Length:");
	if (!p)
		return -1;
	p += strlen("Content-Length:");
	while (*p == ' ' || *p == '\t')
		p++;
	return atoi(p);
}

/** stdin에서 LSP message 하나를 읽어 JSON body를 반환한다. */
static char *
lsp_read_message(FILE *in)
{
	char header[8192];
	int c, content_length;
	size_t len;
	char *body;

	len = 0;
	while ((c = fgetc(in)) != EOF) {
		if (len + 1 >= sizeof(header))
			return NULL;
		header[len++] = (char)c;
		header[len] = '\0';
		if (len >= 4 && strcmp(header + len - 4, "\r\n\r\n") == 0)
			break;
		if (len >= 2 && strcmp(header + len - 2, "\n\n") == 0)
			break;
	}
	if (len == 0 && c == EOF)
		return NULL;
	content_length = parse_content_length(header);
	if (content_length < 0)
		return NULL;
	body = malloc((size_t)content_length + 1);
	if (!body)
		return NULL;
	if (fread(body, 1, (size_t)content_length, in) != (size_t)content_length) {
		free(body);
		return NULL;
	}
	body[content_length] = '\0';
	return body;
}

/** JSON string escape를 작게 해제한다. */
static char *
json_decode_string(const char *start, const char **endp)
{
	struct qstar_lsp_buf out;
	const char *p;
	char ch;

	if (!start || *start != '"')
		return NULL;
	lsp_buf_init(&out);
	for (p = start + 1; *p && *p != '"'; p++) {
		if (*p == '\\') {
			p++;
			if (!*p)
				break;
			if (*p == 'n')
				ch = '\n';
			else if (*p == 'r')
				ch = '\r';
			else if (*p == 't')
				ch = '\t';
			else
				ch = *p;
			if (lsp_buf_append_n(&out, &ch, 1) < 0) {
				lsp_buf_free(&out);
				return NULL;
			}
		} else if (lsp_buf_append_n(&out, p, 1) < 0) {
			lsp_buf_free(&out);
			return NULL;
		}
	}
	if (*p == '"')
		p++;
	if (endp)
		*endp = p;
	if (!out.data) {
		out.data = qstar_strdup("");
		if (!out.data)
			return NULL;
	}
	return out.data;
}

/** JSON object 안에서 첫 번째 key string 값을 찾는다. */
static char *
json_get_string(const char *json, const char *key)
{
	char needle[128];
	const char *p;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return NULL;
	p += strlen(needle);
	while (*p && *p != ':')
		p++;
	if (*p != ':')
		return NULL;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	return json_decode_string(p, NULL);
}

/** JSON object 안에서 첫 번째 key integer 값을 찾는다. */
static int
json_get_int(const char *json, const char *key, int fallback)
{
	char needle[128];
	const char *p;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return fallback;
	p += strlen(needle);
	while (*p && *p != ':')
		p++;
	if (*p != ':')
		return fallback;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	return atoi(p);
}

/** JSON-RPC id 값을 안전하게 복사한다. */
static char *
json_copy_id_raw(const char *json)
{
	const char *p, *start;
	char *id;
	size_t n;

	p = strstr(json, "\"id\"");
	if (!p)
		return NULL;
	p += 4;
	while (*p && *p != ':')
		p++;
	if (*p != ':')
		return NULL;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	start = p;
	if (*p == '"') {
		p++;
		while (*p && (*p != '"' || p[-1] == '\\'))
			p++;
		if (*p == '"')
			p++;
	} else {
		while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
			p++;
	}
	n = (size_t)(p - start);
	id = malloc(n + 1);
	if (!id)
		return NULL;
	memcpy(id, start, n);
	id[n] = '\0';
	return id;
}

/** file URI percent escape를 filesystem path로 변환한다. */
static char *
uri_to_path(const char *uri)
{
	struct qstar_lsp_buf out;
	const char *p;
	char hex[3], ch;

	if (!uri)
		return NULL;
	p = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;
	lsp_buf_init(&out);
	while (*p) {
		if (*p == '%' && isxdigit((unsigned char)p[1]) &&
		    isxdigit((unsigned char)p[2])) {
			hex[0] = p[1];
			hex[1] = p[2];
			hex[2] = '\0';
			ch = (char)strtol(hex, NULL, 16);
			if (lsp_buf_append_n(&out, &ch, 1) < 0) {
				lsp_buf_free(&out);
				return NULL;
			}
			p += 3;
		} else {
			if (lsp_buf_append_n(&out, p, 1) < 0) {
				lsp_buf_free(&out);
				return NULL;
			}
			p++;
		}
	}
	return out.data ? out.data : qstar_strdup("");
}

/** LSP 문서 table에서 URI를 찾는다. */
static struct qstar_lsp_doc *
find_doc(struct qstar_lsp_server *server, const char *uri)
{
	struct qstar_lsp_doc *doc;

	for (doc = server->docs; doc; doc = doc->next) {
		if (strcmp(doc->uri, uri) == 0)
			return doc;
	}
	return NULL;
}

/** LSP 문서 text를 저장하거나 갱신한다. */
static int
upsert_doc(struct qstar_lsp_server *server, const char *uri, const char *text)
{
	struct qstar_lsp_doc *doc;
	char *copy, *path;

	doc = find_doc(server, uri);
	copy = qstar_strdup(text ? text : "");
	if (!copy)
		return -1;
	if (doc) {
		free(doc->text);
		doc->text = copy;
		return 0;
	}
	path = uri_to_path(uri);
	if (!path) {
		free(copy);
		return -1;
	}
	doc = calloc(1, sizeof(*doc));
	if (!doc) {
		free(path);
		free(copy);
		return -1;
	}
	doc->uri = qstar_strdup(uri);
	doc->path = path;
	doc->text = copy;
	doc->next = server->docs;
	server->docs = doc;
	return doc->uri ? 0 : -1;
}

/** LSP 문서 table이 소유한 메모리를 해제한다. */
static void
free_docs(struct qstar_lsp_server *server)
{
	struct qstar_lsp_doc *doc, *next;

	for (doc = server->docs; doc; doc = next) {
		next = doc->next;
		free(doc->uri);
		free(doc->path);
		free(doc->text);
		free(doc);
	}
	server->docs = NULL;
}

/** path가 실제 파일인지 확인한다. */
static int
file_exists(const char *path)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** 현재 파일 기준으로 평가할 root qstar.lua를 찾는다. */
static int
find_root_file(const char *path, char *root_file, size_t root_file_len)
{
	char dir[QSTAR_PATH_MAX], candidate[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];
	const char *base;

	if (qstar_dirname(path, dir, sizeof(dir)) < 0)
		return -1;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strcmp(base, "qstar.lua") == 0) {
		return snprintf(root_file, root_file_len, "%s", path) < (int)root_file_len ?
		    0 : -1;
	}
	for (;;) {
		if (qstar_path_join(dir, "qstar.lua", candidate, sizeof(candidate)) == 0 &&
		    file_exists(candidate)) {
			return snprintf(root_file, root_file_len, "%s", candidate) <
			    (int)root_file_len ? 0 : -1;
		}
		if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0)
			break;
		if (qstar_dirname(dir, parent, sizeof(parent)) < 0 ||
		    strcmp(parent, dir) == 0)
			break;
		snprintf(dir, sizeof(dir), "%s", parent);
	}
	return -1;
}

/** LSP diagnostic용 lint graph를 디스크 상태 기준으로 평가한다. */
static int
load_lint_graph(const char *root_file, struct qstar_graph *graph)
{
	int rc;

	qstar_graph_init(graph);
	rc = qstar_graph_set_profile_input(graph, NULL, NULL, NULL, NULL);
	if (rc == 0)
		rc = qstar_lua_eval_file(graph, root_file);
	if (rc == 0)
		rc = qstar_graph_apply_selected_profile(graph);
	if (rc == 0)
		rc = qstar_graph_set_profile_input(graph, NULL, NULL, NULL, NULL);
	if (rc == 0)
		rc = qstar_graph_validate_profile(graph);
	if (rc == 0)
		rc = qstar_graph_validate_packages(graph);
	if (rc == 0)
		rc = qstar_graph_validate_generated_outputs(graph);
	if (rc == 0)
		rc = qstar_graph_validate_sources(graph);
	if (rc == 0)
		rc = qstar_graph_validate_headers(graph);
	if (rc == 0)
		rc = qstar_graph_validate_file_inputs(graph);
	if (rc < 0 && graph->error[0])
		(void)qstar_graph_add_lint_from_error(graph);
	return rc;
}

/** lint diagnostic이 현재 파일에 해당하는지 확인한다. */
static int
diag_matches_path(const struct qstar_lint_diagnostic *diag, const char *path)
{
	return diag->file && path && strcmp(diag->file, path) == 0;
}

/** filesystem path를 file URI로 변환한다. */
static int
path_to_uri(struct qstar_lsp_buf *buf, const char *path)
{
	const unsigned char *p = (const unsigned char *)(path ? path : "");

	if (lsp_buf_append(buf, "\"file://") < 0)
		return -1;
	while (*p) {
		if (*p == '"' || *p == '\\' || *p == ' ' || *p == '#') {
			if (lsp_buf_printf(buf, "%%%02X", *p) < 0)
				return -1;
		} else if (lsp_buf_append_n(buf, (const char *)p, 1) < 0) {
			return -1;
		}
		p++;
	}
	return lsp_buf_append(buf, "\"");
}

/** LSP Location object를 buffer에 추가한다. */
static int
append_location(struct qstar_lsp_buf *buf, const char *file, int line)
{
	int lnum;

	lnum = line > 0 ? line - 1 : 0;
	if (lsp_buf_append(buf, "{\"uri\":") < 0 ||
	    path_to_uri(buf, file && *file ? file : "<unknown>") < 0 ||
	    lsp_buf_append(buf, ",\"range\":{\"start\":{\"line\":") < 0 ||
	    lsp_buf_printf(buf, "%d", lnum) < 0 ||
	    lsp_buf_append(buf, ",\"character\":0},\"end\":{\"line\":") < 0 ||
	    lsp_buf_printf(buf, "%d", lnum) < 0 ||
	    lsp_buf_append(buf, ",\"character\":1}}}") < 0)
		return -1;
	return 0;
}

/** graph target label에 대응하는 target을 찾는다. */
static const struct qstar_target *
lsp_find_target(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->len; i++) {
		if (strcmp(graph->targets[i].label, label) == 0)
			return &graph->targets[i];
	}
	return NULL;
}

/** graph generated action label에 대응하는 action을 찾는다. */
static const struct qstar_genrule *
lsp_find_genrule(const struct qstar_graph *graph, const char *label)
{
	size_t i;

	for (i = 0; i < graph->genrule_len; i++) {
		if (strcmp(graph->genrules[i].label, label) == 0)
			return &graph->genrules[i];
	}
	return NULL;
}

/** LSP publishDiagnostics notification을 출력한다. */
static int
publish_diagnostics(FILE *out, const char *uri, const char *path)
{
	struct qstar_graph graph;
	struct qstar_lsp_buf payload;
	char root_file[QSTAR_PATH_MAX];
	size_t i;
	int first;

	lsp_buf_init(&payload);
	if (lsp_buf_append(&payload,
	    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":") < 0 ||
	    lsp_json_string(&payload, uri) < 0 ||
	    lsp_buf_append(&payload, ",\"diagnostics\":[") < 0) {
		lsp_buf_free(&payload);
		return -1;
	}
	first = 1;
	if (find_root_file(path, root_file, sizeof(root_file)) == 0) {
		(void)load_lint_graph(root_file, &graph);
		for (i = 0; i < graph.lint_len; i++) {
			const struct qstar_lint_diagnostic *diag = &graph.lint_diagnostics[i];
			int severity = strcmp(diag->severity, "warning") == 0 ? 2 :
			    strcmp(diag->severity, "info") == 0 ? 3 : 1;
			int line = diag->line > 0 ? diag->line - 1 : 0;

			if (!diag_matches_path(diag, path))
				continue;
			if (!first && lsp_buf_append(&payload, ",") < 0)
				goto fail_graph;
			first = 0;
			if (lsp_buf_append(&payload,
			    "{\"range\":{\"start\":{\"line\":") < 0 ||
			    lsp_buf_printf(&payload, "%d", line) < 0 ||
			    lsp_buf_append(&payload, ",\"character\":0},\"end\":{\"line\":") < 0 ||
			    lsp_buf_printf(&payload, "%d", line) < 0 ||
			    lsp_buf_append(&payload, ",\"character\":1}},\"severity\":") < 0 ||
			    lsp_buf_printf(&payload, "%d", severity) < 0 ||
			    lsp_buf_append(&payload, ",\"code\":") < 0 ||
			    lsp_json_string(&payload, diag->code) < 0 ||
			    lsp_buf_append(&payload, ",\"source\":\"qstar\",\"message\":") < 0 ||
			    lsp_json_string(&payload, diag->message) < 0 ||
			    lsp_buf_append(&payload, "}") < 0)
				goto fail_graph;
		}
		qstar_graph_free(&graph);
	} else {
		if (lsp_buf_append(&payload,
		    "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},\"severity\":1,\"code\":\"QSTAR001\",\"source\":\"qstar\",\"message\":\"root entry must be qstar.lua\"}") < 0)
			goto fail;
	}
	if (lsp_buf_append(&payload, "]}}") < 0)
		goto fail;
	i = (size_t)lsp_send(out, payload.data);
	lsp_buf_free(&payload);
	return (int)i;

fail_graph:
	qstar_graph_free(&graph);
fail:
	lsp_buf_free(&payload);
	return -1;
}

/** 한 줄의 시작 pointer와 길이를 찾는다. */
static const char *
line_at_position(const char *text, int line, size_t *len)
{
	const char *p, *start;
	int cur;

	p = text ? text : "";
	cur = 0;
	while (*p && cur < line) {
		if (*p++ == '\n')
			cur++;
	}
	start = p;
	while (*p && *p != '\n' && *p != '\r')
		p++;
	*len = (size_t)(p - start);
	return start;
}

/** hover/completion용 token 문자 범위를 판정한다. */
static int
is_token_char(int c)
{
	return isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' ||
	    c == '/' || c == '@';
}

/** 현재 위치 주변 token을 안전하게 복사한다. */
static char *
copy_token_at(const char *text, int line, int character)
{
	const char *row;
	char *token;
	size_t len, start, end, pos;

	row = line_at_position(text, line, &len);
	pos = character < 0 ? 0 : (size_t)character;
	if (pos > len)
		pos = len;
	start = pos;
	while (start > 0 && is_token_char((unsigned char)row[start - 1]))
		start--;
	end = pos;
	while (end < len && is_token_char((unsigned char)row[end]))
		end++;
	if (end <= start)
		return qstar_strdup("");
	token = malloc(end - start + 1);
	if (!token)
		return NULL;
	memcpy(token, row + start, end - start);
	token[end - start] = '\0';
	return token;
}

/** 정적 hover table에서 token 설명을 찾는다. */
static const char *
lookup_static_hover(const char *token)
{
	size_t i;

	for (i = 0; i < sizeof(qstar_lsp_symbols) / sizeof(qstar_lsp_symbols[0]); i++) {
		if (strcmp(qstar_lsp_symbols[i].name, token) == 0)
			return qstar_lsp_symbols[i].text;
	}
	for (i = 0; i < sizeof(qstar_lsp_fields) / sizeof(qstar_lsp_fields[0]); i++) {
		if (strcmp(qstar_lsp_fields[i].name, token) == 0)
			return qstar_lsp_fields[i].text;
	}
	return NULL;
}

/** label hover를 graph에서 찾아 markdown 설명으로 만든다. */
static char *
label_hover(const char *path, const char *token)
{
	struct qstar_graph graph;
	struct qstar_lsp_buf text;
	char root_file[QSTAR_PATH_MAX];
	size_t i;

	if (strncmp(token, "//", 2) != 0 || !strchr(token, ':'))
		return NULL;
	if (find_root_file(path, root_file, sizeof(root_file)) < 0)
		return NULL;
	if (load_lint_graph(root_file, &graph) < 0 && graph.len == 0) {
		qstar_graph_free(&graph);
		return NULL;
	}
	lsp_buf_init(&text);
	for (i = 0; i < graph.len; i++) {
		const struct qstar_target *target = &graph.targets[i];
		if (strcmp(target->label, token) != 0)
			continue;
		if (lsp_buf_printf(&text,
		    "**%s**\n\nkind: `%s`\n\norigin: `%s:%d`\n\nsources: `%zu`",
		    target->label, target->kind, target->origin_file, target->origin_line,
		    target->sources.len) < 0) {
			lsp_buf_free(&text);
			qstar_graph_free(&graph);
			return NULL;
		}
		qstar_graph_free(&graph);
		return text.data;
	}
	qstar_graph_free(&graph);
	return NULL;
}

/** textDocument/hover 요청에 응답한다. */
static int
handle_hover(struct qstar_lsp_server *server, FILE *out, const char *id, const char *body)
{
	struct qstar_lsp_doc *doc;
	struct qstar_lsp_buf payload;
	char *uri, *token, *dynamic;
	const char *desc;
	int line, character, rc;

	uri = json_get_string(body, "uri");
	line = json_get_int(body, "line", 0);
	character = json_get_int(body, "character", 0);
	doc = uri ? find_doc(server, uri) : NULL;
	token = doc ? copy_token_at(doc->text, line, character) : qstar_strdup("");
	desc = token ? lookup_static_hover(token) : NULL;
	dynamic = (!desc && doc && token) ? label_hover(doc->path, token) : NULL;
	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":",
	    id ? id : "null") < 0)
		goto fail;
	if (desc) {
		if (lsp_buf_append(&payload, "{\"contents\":{\"kind\":\"markdown\",\"value\":") < 0 ||
		    lsp_json_string(&payload, desc) < 0 ||
		    lsp_buf_append(&payload, "}}") < 0)
			goto fail;
	} else if (dynamic) {
		if (lsp_buf_append(&payload, "{\"contents\":{\"kind\":\"markdown\",\"value\":") < 0 ||
		    lsp_json_string(&payload, dynamic) < 0 ||
		    lsp_buf_append(&payload, "}}") < 0)
			goto fail;
	} else if (lsp_buf_append(&payload, "null") < 0) {
		goto fail;
	}
	if (lsp_buf_append(&payload, "}") < 0)
		goto fail;
	rc = lsp_send(out, payload.data);
	free(uri);
	free(token);
	free(dynamic);
	lsp_buf_free(&payload);
	return rc;

fail:
	free(uri);
	free(token);
	free(dynamic);
	lsp_buf_free(&payload);
	return -1;
}

/** CompletionItem 하나를 JSON으로 추가한다. */
static int
append_completion_item(struct qstar_lsp_buf *payload, const char *label,
    const char *detail, int first)
{
	if (!first && lsp_buf_append(payload, ",") < 0)
		return -1;
	if (lsp_buf_append(payload, "{\"label\":") < 0 ||
	    lsp_json_string(payload, label) < 0 ||
	    lsp_buf_append(payload, ",\"kind\":14,\"detail\":") < 0 ||
	    lsp_json_string(payload, detail) < 0 ||
	    lsp_buf_append(payload, "}") < 0)
		return -1;
	return 0;
}

/** textDocument/completion 요청에 정적 QStar surface를 반환한다. */
static int
handle_completion(FILE *out, const char *id)
{
	struct qstar_lsp_buf payload;
	size_t i;
	int first;

	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload,
	    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"isIncomplete\":false,\"items\":[",
	    id ? id : "null") < 0)
		goto fail;
	first = 1;
	for (i = 0; i < sizeof(qstar_lsp_symbols) / sizeof(qstar_lsp_symbols[0]); i++) {
		if (append_completion_item(&payload, qstar_lsp_symbols[i].name,
		    qstar_lsp_symbols[i].text, first) < 0)
			goto fail;
		first = 0;
	}
	for (i = 0; i < sizeof(qstar_lsp_fields) / sizeof(qstar_lsp_fields[0]); i++) {
		if (append_completion_item(&payload, qstar_lsp_fields[i].name,
		    qstar_lsp_fields[i].text, first) < 0)
			goto fail;
		first = 0;
	}
	if (lsp_buf_append(&payload, "]}}") < 0)
		goto fail;
	i = (size_t)lsp_send(out, payload.data);
	lsp_buf_free(&payload);
	return (int)i;

fail:
	lsp_buf_free(&payload);
	return -1;
}

/** textDocument/definition 요청에 label 선언 위치를 반환한다. */
static int
handle_definition(struct qstar_lsp_server *server, FILE *out, const char *id,
    const char *body)
{
	struct qstar_lsp_doc *doc;
	struct qstar_graph graph;
	struct qstar_lsp_buf payload;
	const struct qstar_target *target;
	const struct qstar_genrule *genrule;
	char root_file[QSTAR_PATH_MAX];
	char *uri, *token;
	int line, character, rc;

	uri = json_get_string(body, "uri");
	line = json_get_int(body, "line", 0);
	character = json_get_int(body, "character", 0);
	doc = uri ? find_doc(server, uri) : NULL;
	token = doc ? copy_token_at(doc->text, line, character) : qstar_strdup("");
	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":",
	    id ? id : "null") < 0)
		goto fail;
	if (!doc || !token || token[0] == '\0' ||
	    find_root_file(doc->path, root_file, sizeof(root_file)) < 0 ||
	    (load_lint_graph(root_file, &graph) < 0 && graph.len == 0 &&
	    graph.genrule_len == 0)) {
		if (lsp_buf_append(&payload, "null}") < 0)
			goto fail;
		rc = lsp_send(out, payload.data);
		free(uri);
		free(token);
		lsp_buf_free(&payload);
		return rc;
	}
	target = lsp_find_target(&graph, token);
	genrule = target ? NULL : lsp_find_genrule(&graph, token);
	if (target) {
		if (append_location(&payload, target->origin_file, target->origin_line) < 0)
			goto fail_graph;
	} else if (genrule) {
		if (append_location(&payload, genrule->origin_file, genrule->origin_line) < 0)
			goto fail_graph;
	} else if (lsp_buf_append(&payload, "null") < 0) {
		goto fail_graph;
	}
	if (lsp_buf_append(&payload, "}") < 0)
		goto fail_graph;
	rc = lsp_send(out, payload.data);
	qstar_graph_free(&graph);
	free(uri);
	free(token);
	lsp_buf_free(&payload);
	return rc;

fail_graph:
	qstar_graph_free(&graph);
fail:
	free(uri);
	free(token);
	lsp_buf_free(&payload);
	return -1;
}

/** dependency list에서 label이 쓰인 위치를 LSP reference location으로 추가한다. */
static int
append_references_from_list(struct qstar_lsp_buf *payload,
    const struct qstar_target *target, const struct qstar_string_list *list,
    const char *label, int *first)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], label) != 0)
			continue;
		if (!*first && lsp_buf_append(payload, ",") < 0)
			return -1;
		*first = 0;
		if (append_location(payload, target->origin_file, target->origin_line) < 0)
			return -1;
	}
	return 0;
}

/** textDocument/references 요청에 target dependency usage를 반환한다. */
static int
handle_references(struct qstar_lsp_server *server, FILE *out, const char *id,
    const char *body)
{
	struct qstar_lsp_doc *doc;
	struct qstar_graph graph;
	struct qstar_lsp_buf payload;
	char root_file[QSTAR_PATH_MAX];
	char *uri, *token;
	size_t i;
	int line, character, first, rc;

	uri = json_get_string(body, "uri");
	line = json_get_int(body, "line", 0);
	character = json_get_int(body, "character", 0);
	doc = uri ? find_doc(server, uri) : NULL;
	token = doc ? copy_token_at(doc->text, line, character) : qstar_strdup("");
	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":[",
	    id ? id : "null") < 0)
		goto fail;
	first = 1;
	if (doc && token && token[0] != '\0' &&
	    find_root_file(doc->path, root_file, sizeof(root_file)) == 0 &&
	    !(load_lint_graph(root_file, &graph) < 0 && graph.len == 0)) {
		for (i = 0; i < graph.len; i++) {
			if (append_references_from_list(&payload, &graph.targets[i],
			    &graph.targets[i].deps, token, &first) < 0 ||
			    append_references_from_list(&payload, &graph.targets[i],
			    &graph.targets[i].private_deps, token, &first) < 0)
				goto fail_graph;
		}
		qstar_graph_free(&graph);
	}
	if (lsp_buf_append(&payload, "]}") < 0)
		goto fail;
	rc = lsp_send(out, payload.data);
	free(uri);
	free(token);
	lsp_buf_free(&payload);
	return rc;

fail_graph:
	qstar_graph_free(&graph);
fail:
	free(uri);
	free(token);
	lsp_buf_free(&payload);
	return -1;
}

/** DocumentSymbol 하나를 추가한다. */
static int
append_document_symbol(struct qstar_lsp_buf *payload, const char *name,
    int kind, const char *detail, int line, int first)
{
	int lnum;

	lnum = line > 0 ? line - 1 : 0;
	if (!first && lsp_buf_append(payload, ",") < 0)
		return -1;
	if (lsp_buf_append(payload, "{\"name\":") < 0 ||
	    lsp_json_string(payload, name) < 0 ||
	    lsp_buf_append(payload, ",\"detail\":") < 0 ||
	    lsp_json_string(payload, detail) < 0 ||
	    lsp_buf_printf(payload, ",\"kind\":%d", kind) < 0 ||
	    lsp_buf_append(payload, ",\"range\":{\"start\":{\"line\":") < 0 ||
	    lsp_buf_printf(payload, "%d", lnum) < 0 ||
	    lsp_buf_append(payload, ",\"character\":0},\"end\":{\"line\":") < 0 ||
	    lsp_buf_printf(payload, "%d", lnum) < 0 ||
	    lsp_buf_append(payload, ",\"character\":1}},\"selectionRange\":{\"start\":{\"line\":") < 0 ||
	    lsp_buf_printf(payload, "%d", lnum) < 0 ||
	    lsp_buf_append(payload, ",\"character\":0},\"end\":{\"line\":") < 0 ||
	    lsp_buf_printf(payload, "%d", lnum) < 0 ||
	    lsp_buf_append(payload, ",\"character\":1}}}") < 0)
		return -1;
	return 0;
}

/** textDocument/documentSymbol 요청에 현재 fragment의 target/action symbols를 반환한다. */
static int
handle_document_symbols(struct qstar_lsp_server *server, FILE *out, const char *id,
    const char *body)
{
	struct qstar_lsp_doc *doc;
	struct qstar_graph graph;
	struct qstar_lsp_buf payload;
	char root_file[QSTAR_PATH_MAX];
	char *uri;
	size_t i;
	int first, rc;

	uri = json_get_string(body, "uri");
	doc = uri ? find_doc(server, uri) : NULL;
	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":[",
	    id ? id : "null") < 0)
		goto fail;
	first = 1;
	if (doc && find_root_file(doc->path, root_file, sizeof(root_file)) == 0 &&
	    !(load_lint_graph(root_file, &graph) < 0 && graph.len == 0 &&
	    graph.config_len == 0 && graph.genrule_len == 0)) {
		for (i = 0; i < graph.len; i++) {
			if (strcmp(graph.targets[i].origin_file, doc->path) != 0)
				continue;
			if (append_document_symbol(&payload, graph.targets[i].label,
			    12, graph.targets[i].kind, graph.targets[i].origin_line,
			    first) < 0)
				goto fail_graph;
			first = 0;
		}
		for (i = 0; i < graph.config_len; i++) {
			if (strcmp(graph.configs[i].origin_file, doc->path) != 0)
				continue;
			if (append_document_symbol(&payload, graph.configs[i].label,
			    5, "config", graph.configs[i].origin_line, first) < 0)
				goto fail_graph;
			first = 0;
		}
		for (i = 0; i < graph.genrule_len; i++) {
			if (strcmp(graph.genrules[i].origin_file, doc->path) != 0)
				continue;
			if (append_document_symbol(&payload, graph.genrules[i].label,
			    13, graph.genrules[i].config_header ? "configure_file" : "custom_target",
			    graph.genrules[i].origin_line, first) < 0)
				goto fail_graph;
			first = 0;
		}
		qstar_graph_free(&graph);
	}
	if (lsp_buf_append(&payload, "]}") < 0)
		goto fail;
	rc = lsp_send(out, payload.data);
	free(uri);
	lsp_buf_free(&payload);
	return rc;

fail_graph:
	qstar_graph_free(&graph);
fail:
	free(uri);
	lsp_buf_free(&payload);
	return -1;
}

/** SymbolInformation 하나를 추가한다. */
static int
append_workspace_symbol(struct qstar_lsp_buf *payload, const char *name,
    int kind, const char *container, const char *file, int line, int first)
{
	if (!first && lsp_buf_append(payload, ",") < 0)
		return -1;
	if (lsp_buf_append(payload, "{\"name\":") < 0 ||
	    lsp_json_string(payload, name) < 0 ||
	    lsp_buf_printf(payload, ",\"kind\":%d,\"containerName\":", kind) < 0 ||
	    lsp_json_string(payload, container) < 0 ||
	    lsp_buf_append(payload, ",\"location\":") < 0 ||
	    append_location(payload, file, line) < 0 ||
	    lsp_buf_append(payload, "}") < 0)
		return -1;
	return 0;
}

/** workspace/symbol 요청에 graph-wide target/action symbols를 반환한다. */
static int
handle_workspace_symbols(struct qstar_lsp_server *server, FILE *out, const char *id,
    const char *body)
{
	struct qstar_lsp_doc *doc;
	struct qstar_graph graph;
	struct qstar_lsp_buf payload;
	char root_file[QSTAR_PATH_MAX];
	char *query;
	size_t i;
	int first, rc;

	query = json_get_string(body, "query");
	doc = server->docs;
	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":[",
	    id ? id : "null") < 0)
		goto fail;
	first = 1;
	if (doc && find_root_file(doc->path, root_file, sizeof(root_file)) == 0 &&
	    !(load_lint_graph(root_file, &graph) < 0 && graph.len == 0 &&
	    graph.config_len == 0 && graph.genrule_len == 0)) {
		for (i = 0; i < graph.len; i++) {
			if (query && *query && !strstr(graph.targets[i].label, query))
				continue;
			if (append_workspace_symbol(&payload, graph.targets[i].label,
			    12, graph.targets[i].kind, graph.targets[i].origin_file,
			    graph.targets[i].origin_line, first) < 0)
				goto fail_graph;
			first = 0;
		}
		for (i = 0; i < graph.config_len; i++) {
			if (query && *query && !strstr(graph.configs[i].label, query))
				continue;
			if (append_workspace_symbol(&payload, graph.configs[i].label,
			    5, "config", graph.configs[i].origin_file,
			    graph.configs[i].origin_line, first) < 0)
				goto fail_graph;
			first = 0;
		}
		for (i = 0; i < graph.genrule_len; i++) {
			if (query && *query && !strstr(graph.genrules[i].label, query))
				continue;
			if (append_workspace_symbol(&payload, graph.genrules[i].label,
			    13, graph.genrules[i].config_header ? "configure_file" : "custom_target",
			    graph.genrules[i].origin_file, graph.genrules[i].origin_line,
			    first) < 0)
				goto fail_graph;
			first = 0;
		}
		qstar_graph_free(&graph);
	}
	if (lsp_buf_append(&payload, "]}") < 0)
		goto fail;
	rc = lsp_send(out, payload.data);
	free(query);
	lsp_buf_free(&payload);
	return rc;

fail_graph:
	qstar_graph_free(&graph);
fail:
	free(query);
	lsp_buf_free(&payload);
	return -1;
}

/** initialize 요청에 LSP capability를 반환한다. */
static int
handle_initialize(FILE *out, const char *id)
{
	struct qstar_lsp_buf payload;
	int rc;

	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload,
	    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"documentSymbolProvider\":true,\"workspaceSymbolProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\"q\",\".\",\"/\",\":\",\"\\\"\"]}},\"serverInfo\":{\"name\":\"qstar-lsp\",\"version\":\"v1\"}}}",
	    id ? id : "null") < 0) {
		lsp_buf_free(&payload);
		return -1;
	}
	rc = lsp_send(out, payload.data);
	lsp_buf_free(&payload);
	return rc;
}

/** 요청 id에 null result를 반환한다. */
static int
send_null_result(FILE *out, const char *id)
{
	struct qstar_lsp_buf payload;
	int rc;

	lsp_buf_init(&payload);
	if (lsp_buf_printf(&payload, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}",
	    id ? id : "null") < 0) {
		lsp_buf_free(&payload);
		return -1;
	}
	rc = lsp_send(out, payload.data);
	lsp_buf_free(&payload);
	return rc;
}

/** method notification에서 textDocument URI를 찾아 diagnostics를 publish한다. */
static int
publish_for_body(struct qstar_lsp_server *server, FILE *out, const char *body)
{
	struct qstar_lsp_doc *doc;
	char *uri, *text;
	int rc;

	uri = json_get_string(body, "uri");
	text = json_get_string(body, "text");
	if (uri && text && upsert_doc(server, uri, text) < 0) {
		free(uri);
		free(text);
		return -1;
	}
	doc = uri ? find_doc(server, uri) : NULL;
	rc = doc ? publish_diagnostics(out, doc->uri, doc->path) : 0;
	free(uri);
	free(text);
	return rc;
}

/** JSON-RPC method 하나를 처리한다. */
static int
handle_message(struct qstar_lsp_server *server, FILE *out, const char *body)
{
	char *method, *id;
	int rc;

	method = json_get_string(body, "method");
	id = json_copy_id_raw(body);
	if (!method) {
		free(id);
		return 0;
	}
	if (strcmp(method, "initialize") == 0) {
		rc = handle_initialize(out, id);
	} else if (strcmp(method, "shutdown") == 0) {
		server->shutdown = 1;
		rc = send_null_result(out, id);
	} else if (strcmp(method, "exit") == 0) {
		free(method);
		free(id);
		return 1;
	} else if (strcmp(method, "textDocument/didOpen") == 0 ||
	    strcmp(method, "textDocument/didChange") == 0 ||
	    strcmp(method, "textDocument/didSave") == 0) {
		rc = publish_for_body(server, out, body);
	} else if (strcmp(method, "textDocument/hover") == 0) {
		rc = handle_hover(server, out, id, body);
	} else if (strcmp(method, "textDocument/completion") == 0) {
		rc = handle_completion(out, id);
	} else if (strcmp(method, "textDocument/definition") == 0) {
		rc = handle_definition(server, out, id, body);
	} else if (strcmp(method, "textDocument/references") == 0) {
		rc = handle_references(server, out, id, body);
	} else if (strcmp(method, "textDocument/documentSymbol") == 0) {
		rc = handle_document_symbols(server, out, id, body);
	} else if (strcmp(method, "workspace/symbol") == 0) {
		rc = handle_workspace_symbols(server, out, id, body);
	} else if (id) {
		rc = send_null_result(out, id);
	} else {
		rc = 0;
	}
	free(method);
	free(id);
	return rc;
}

/** stdio 기반 QStar Language Server Protocol v1 loop를 실행한다. */
int
qstar_lsp_stdio(FILE *in, FILE *out)
{
	struct qstar_lsp_server server;
	char *body;
	int rc;

	memset(&server, 0, sizeof(server));
	setvbuf(out, NULL, _IONBF, 0);
	while ((body = lsp_read_message(in)) != NULL) {
		rc = handle_message(&server, out, body);
		free(body);
		if (rc > 0)
			break;
		if (rc < 0) {
			free_docs(&server);
			return 1;
		}
	}
	rc = server.shutdown ? 0 : 0;
	free_docs(&server);
	return rc;
}
