#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *const init_shapes[] = {
	"app",
	"lib",
	"tool",
	"empty",
	"workspace",
	NULL
};

struct init_context {
	const struct qstar_init_options *options;
	const char *shape;
	const char *directory;
	const char *language;
	char project_name[128];
	char project_lua[256];
	char project_ident[128];
	char header_guard[160];
};

/** Write a stable message to the init error buffer. */
static int
init_error(char *error, size_t error_len, const char *fmt, const char *arg)
{
	if (error && error_len)
		snprintf(error, error_len, fmt, arg);
	return -1;
}

static int
init_error2(char *error, size_t error_len, const char *fmt, const char *a, const char *b)
{
	if (error && error_len)
		snprintf(error, error_len, fmt, a, b);
	return -1;
}

/** Return whether the path already exists. */
static int
path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/** Create a directory and any missing parents. */
static int
mkdir_p(const char *path, char *error, size_t error_len)
{
	char buf[QSTAR_PATH_MAX];
	size_t i, n;

	n = strlen(path);
	if (n >= sizeof(buf))
		return init_error(error, error_len, "qstar: init path too long '%s'", path);
	memcpy(buf, path, n + 1);
	for (i = 1; i < n; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (buf[0] && qstar_platform_mkdir(buf, 0777) < 0 && errno != EEXIST)
			return init_error(error, error_len,
			    "qstar: init could not create directory '%s'", buf);
		buf[i] = '/';
	}
	if (qstar_platform_mkdir(buf, 0777) < 0 && errno != EEXIST)
		return init_error(error, error_len,
		    "qstar: init could not create directory '%s'", buf);
	return 0;
}

/** Create the parent directory for a file path. */
static int
mkdir_parent(const char *path, char *error, size_t error_len)
{
	char dir[QSTAR_PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(dir))
		return init_error(error, error_len, "qstar: init path too long '%s'", path);
	strcpy(dir, path);
	slash = strrchr(dir, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(dir, error, error_len);
}

static int
string_in_list(const char *value, const char *const *list)
{
	size_t i;

	if (!value || !*value)
		return 0;
	for (i = 0; list[i]; i++) {
		if (strcmp(value, list[i]) == 0)
			return 1;
	}
	return 0;
}

static const char *
legacy_shape_replacement(const char *shape)
{
	if (strcmp(shape, "c-app") == 0)
		return "app";
	if (strcmp(shape, "c-lib") == 0)
		return "lib";
	if (strcmp(shape, "generated") == 0)
		return "app";
	return NULL;
}

static const char *
directory_basename(const char *path)
{
	const char *end, *base;

	if (!path || !*path || strcmp(path, ".") == 0)
		return "qstar-project";
	end = path + strlen(path);
	while (end > path && end[-1] == '/')
		end--;
	if (end == path)
		return "qstar-project";
	base = end;
	while (base > path && base[-1] != '/')
		base--;
	if ((size_t)(end - base) == 1 && base[0] == '.')
		return "qstar-project";
	return base;
}

static int
copy_basename(char *dst, size_t dstlen, const char *path)
{
	const char *base, *end;
	size_t n;

	base = directory_basename(path);
	end = base + strlen(base);
	while (end > base && end[-1] == '/')
		end--;
	n = (size_t)(end - base);
	if (n == 0 || n + 1 > dstlen)
		return -1;
	memcpy(dst, base, n);
	dst[n] = '\0';
	return 0;
}

static int
ident_char(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9') || c == '_';
}

static int
ident_start(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static void
make_project_ident(const char *name, char *dst, size_t dstlen)
{
	size_t i, out;
	unsigned char c;

	out = 0;
	if (!name || !*name)
		name = "project";
	if (!ident_start((unsigned char)name[0])) {
		if (dstlen > 8) {
			memcpy(dst, "project_", 8);
			out = 8;
		}
	}
	for (i = 0; name[i] && out + 1 < dstlen; i++) {
		c = (unsigned char)name[i];
		dst[out++] = ident_char(c) ? (char)c : '_';
	}
	if (out == 0 && dstlen > 1)
		dst[out++] = 'p';
	dst[out] = '\0';
}

static void
make_header_guard(const char *ident, char *dst, size_t dstlen)
{
	size_t i, out;
	unsigned char c;

	out = 0;
	for (i = 0; ident && ident[i] && out + 3 < dstlen; i++) {
		c = (unsigned char)ident[i];
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 'a' + 'A');
		dst[out++] = ident_char(c) ? (char)c : '_';
	}
	if (out == 0 && dstlen > 3)
		dst[out++] = 'P';
	if (out + 3 < dstlen) {
		dst[out++] = '_';
		dst[out++] = 'H';
	}
	dst[out] = '\0';
}

static int
escape_lua_string(const char *src, char *dst, size_t dstlen)
{
	size_t i, out;
	char esc;
	unsigned char c;

	out = 0;
	for (i = 0; src && src[i]; i++) {
		c = (unsigned char)src[i];
		esc = '\0';
		if (c == '"' || c == '\\')
			esc = (char)c;
		else if (c == '\n')
			esc = 'n';
		else if (c == '\r')
			esc = 'r';
		else if (c == '\t')
			esc = 't';
		if (esc) {
			if (out + 2 >= dstlen)
				return -1;
			dst[out++] = '\\';
			dst[out++] = esc;
		} else {
			if (out + 1 >= dstlen)
				return -1;
			dst[out++] = c < 0x20 ? '_' : (char)c;
		}
	}
	if (out >= dstlen)
		return -1;
	dst[out] = '\0';
	return 0;
}

static int
format_body(char *dst, size_t dstlen, char *error, size_t error_len, const char *path,
    const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(dst, dstlen, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)dstlen)
		return init_error(error, error_len, "qstar: init template too large for '%s'",
		    path);
	return 0;
}

/** Create one file or print the dry-run plan entry. */
static int
write_text_file(const struct init_context *ctx, const char *rel, const char *body,
    int executable, FILE *out, char *error, size_t error_len)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;
	int failed;

	if (qstar_path_join(ctx->directory, rel, path, sizeof(path)) < 0)
		return init_error(error, error_len, "qstar: init path too long '%s'", rel);
	if (ctx->options->dry_run) {
		fprintf(out, "would_create %s\n", rel);
		return 0;
	}
	if (path_exists(path))
		return init_error(error, error_len,
		    "qstar: init refuses to overwrite existing file '%s'", path);
	if (mkdir_parent(path, error, error_len) < 0)
		return -1;
	f = fopen(path, "wb");
	if (!f)
		return init_error(error, error_len,
		    "qstar: init could not create file '%s'", path);
	failed = fputs(body, f) < 0;
	if (fclose(f) < 0)
		failed = 1;
	if (failed)
		return init_error(error, error_len,
		    "qstar: init could not write file '%s'", path);
	if (executable)
		chmod(path, 0755);
	fprintf(out, "create %s\n", rel);
	return 0;
}

static int
write_gitignore(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	return write_text_file(ctx, ".gitignore",
	    "build/\n"
	    ".qstar/\n"
	    "compile_commands.json\n",
	    0, out, error, error_len);
}

static int
write_app_shape(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	char body[8192];

	if (format_body(body, sizeof(body), error, error_len, "qstar.lua",
	    "qstar.project {\n"
	    "  name = \"%s\",\n"
	    "  version = \"0.1.0\",\n"
	    "  root = \".\",\n"
	    "}\n"
	    "\n"
	    "qstar.toolset \"host\" {\n"
	    "  tools = {\n"
	    "    archive = qstar.cli {\"ar\"},\n"
	    "    link = qstar.cli {\"cc\"},\n"
	    "    c = {\n"
	    "      compiler = qstar.cli {\"cc\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.config \"debug\" {\n"
	    "  toolset = \"//:host\",\n"
	    "}\n"
	    "\n"
	    "qstar.executable \"app\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"src/main.c\"},\n"
	    "}\n",
	    ctx->project_lua) < 0)
		return -1;
	if (write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
	    write_gitignore(ctx, out, error, error_len) < 0)
		return -1;
	return write_text_file(ctx, "src/main.c",
	    "int main(void)\n"
	    "{\n"
	    "\treturn 0;\n"
	    "}\n",
	    0, out, error, error_len);
}

static int
write_lib_shape(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	char body[8192], path[QSTAR_PATH_MAX];

	if (format_body(body, sizeof(body), error, error_len, "qstar.lua",
	    "qstar.project {\n"
	    "  name = \"%s\",\n"
	    "  version = \"0.1.0\",\n"
	    "  root = \".\",\n"
	    "}\n"
	    "\n"
	    "qstar.toolset \"host\" {\n"
	    "  tools = {\n"
	    "    archive = qstar.cli {\"ar\"},\n"
	    "    link = qstar.cli {\"cc\"},\n"
	    "    c = {\n"
	    "      compiler = qstar.cli {\"cc\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.config \"debug\" {\n"
	    "  toolset = \"//:host\",\n"
	    "}\n"
	    "\n"
	    "qstar.staticlib \"core\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"src/%s.c\"},\n"
	    "  lang = {\n"
	    "    c = {\n"
	    "      public_headers = {\"include/%s.h\"},\n"
	    "      public_include_dirs = {\"include\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.test \"unit\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"tests/unit.c\"},\n"
	    "  deps = {\"//:core\"},\n"
	    "}\n",
	    ctx->project_lua, ctx->project_ident, ctx->project_ident) < 0)
		return -1;
	if (write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
	    write_gitignore(ctx, out, error, error_len) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "include/%s.h", ctx->project_ident) >=
	    (int)sizeof(path))
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    ctx->project_ident);
	if (format_body(body, sizeof(body), error, error_len, path,
	    "#ifndef %s\n"
	    "#define %s\n"
	    "\n"
	    "int %s_add(int lhs, int rhs);\n"
	    "\n"
	    "#endif\n",
	    ctx->header_guard, ctx->header_guard, ctx->project_ident) < 0 ||
	    write_text_file(ctx, path, body, 0, out, error, error_len) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "src/%s.c", ctx->project_ident) >=
	    (int)sizeof(path))
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    ctx->project_ident);
	if (format_body(body, sizeof(body), error, error_len, path,
	    "#include \"%s.h\"\n"
	    "\n"
	    "int %s_add(int lhs, int rhs)\n"
	    "{\n"
	    "\treturn lhs + rhs;\n"
	    "}\n",
	    ctx->project_ident, ctx->project_ident) < 0 ||
	    write_text_file(ctx, path, body, 0, out, error, error_len) < 0)
		return -1;
	if (format_body(body, sizeof(body), error, error_len, "tests/unit.c",
	    "#include \"%s.h\"\n"
	    "\n"
	    "int main(void)\n"
	    "{\n"
	    "\treturn %s_add(20, 22) == 42 ? 0 : 1;\n"
	    "}\n",
	    ctx->project_ident, ctx->project_ident) < 0)
		return -1;
	return write_text_file(ctx, "tests/unit.c", body, 0, out, error, error_len);
}

static int
write_tool_shape(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	char body[8192], path[QSTAR_PATH_MAX];

	if (format_body(body, sizeof(body), error, error_len, "qstar.lua",
	    "qstar.project {\n"
	    "  name = \"%s\",\n"
	    "  version = \"0.1.0\",\n"
	    "  root = \".\",\n"
	    "}\n"
	    "\n"
	    "qstar.toolset \"host\" {\n"
	    "  tools = {\n"
	    "    archive = qstar.cli {\"ar\"},\n"
	    "    link = qstar.cli {\"cc\"},\n"
	    "    c = {\n"
	    "      compiler = qstar.cli {\"cc\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.config \"debug\" {\n"
	    "  toolset = \"//:host\",\n"
	    "}\n"
	    "\n"
	    "qstar.executable \"tool\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"tools/%s/main.c\"},\n"
	    "}\n"
	    "\n"
	    "qstar.run_target \"run\" {\n"
	    "  deps = {\"//:tool\"},\n"
	    "  command = qstar.cli {qstar.target_file(\"//:tool\")},\n"
	    "}\n",
	    ctx->project_lua, ctx->project_ident) < 0)
		return -1;
	if (write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
	    write_gitignore(ctx, out, error, error_len) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "tools/%s/main.c", ctx->project_ident) >=
	    (int)sizeof(path))
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    ctx->project_ident);
	return write_text_file(ctx, path,
	    "int main(void)\n"
	    "{\n"
	    "\treturn 0;\n"
	    "}\n",
	    0, out, error, error_len);
}

static int
write_empty_shape(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	char body[1024];

	if (format_body(body, sizeof(body), error, error_len, "qstar.lua",
	    "qstar.project {\n"
	    "  name = \"%s\",\n"
	    "  version = \"0.1.0\",\n"
	    "  root = \".\",\n"
	    "}\n",
	    ctx->project_lua) < 0)
		return -1;
	return write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ?
	    -1 : write_gitignore(ctx, out, error, error_len);
}

static int
write_workspace_shape(const struct init_context *ctx, FILE *out, char *error,
    size_t error_len)
{
	char body[8192];

	if (format_body(body, sizeof(body), error, error_len, "qstar.lua",
	    "qstar.project {\n"
	    "  name = \"%s\",\n"
	    "  version = \"0.1.0\",\n"
	    "  root = \".\",\n"
	    "}\n"
	    "\n"
	    "qstar.toolset \"host\" {\n"
	    "  tools = {\n"
	    "    archive = qstar.cli {\"ar\"},\n"
	    "    link = qstar.cli {\"cc\"},\n"
	    "    c = {\n"
	    "      compiler = qstar.cli {\"cc\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.config \"debug\" {\n"
	    "  toolset = \"//:host\",\n"
	    "}\n"
	    "\n"
	    "qstar.subdir(\"packages/core\")\n"
	    "qstar.subdir(\"packages/app\")\n"
	    "\n"
	    "qstar.group \"all\" {\n"
	    "  deps = {\n"
	    "    \"//packages/core:core\",\n"
	    "    \"//packages/app:app\",\n"
	    "  },\n"
	    "}\n",
	    ctx->project_lua) < 0)
		return -1;
	if (write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
	    write_gitignore(ctx, out, error, error_len) < 0)
		return -1;
	if (write_text_file(ctx, "packages/core/include/core.h",
	    "#ifndef CORE_H\n"
	    "#define CORE_H\n"
	    "\n"
	    "int core_add(int lhs, int rhs);\n"
	    "\n"
	    "#endif\n",
	    0, out, error, error_len) < 0 ||
	    write_text_file(ctx, "packages/core/src/core.c",
	    "#include \"core.h\"\n"
	    "\n"
	    "int core_add(int lhs, int rhs)\n"
	    "{\n"
	    "\treturn lhs + rhs;\n"
	    "}\n",
	    0, out, error, error_len) < 0)
		return -1;
	if (write_text_file(ctx, "packages/core/core.qst",
	    "qstar.staticlib \"core\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"packages/core/src/core.c\"},\n"
	    "  lang = {\n"
	    "    c = {\n"
	    "      public_headers = {\"packages/core/include/core.h\"},\n"
	    "      public_include_dirs = {\"packages/core/include\"},\n"
	    "    },\n"
	    "  },\n"
	    "}\n",
	    0, out, error, error_len) < 0 ||
	    write_text_file(ctx, "packages/app/src/main.c",
	    "#include \"core.h\"\n"
	    "\n"
	    "int main(void)\n"
	    "{\n"
	    "\treturn core_add(20, 22) == 42 ? 0 : 1;\n"
	    "}\n",
	    0, out, error, error_len) < 0 ||
	    write_text_file(ctx, "packages/app/app.qst",
	    "qstar.executable \"app\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"packages/app/src/main.c\"},\n"
	    "  deps = {\"//packages/core:core\"},\n"
	    "}\n",
	    0, out, error, error_len) < 0)
		return -1;
	return 0;
}

/** Print the supported qstar init shapes. */
void
qstar_init_print_shapes(FILE *out)
{
	size_t i;

	fputs("qstar init shapes\n", out);
	for (i = 0; init_shapes[i]; i++)
		fprintf(out, "%s\n", init_shapes[i]);
}

static int
prepare_context(struct init_context *ctx, const struct qstar_init_options *options,
    char *error, size_t error_len)
{
	const char *replacement;

	memset(ctx, 0, sizeof(*ctx));
	ctx->options = options;
	ctx->shape = options && options->shape ? options->shape : "";
	ctx->directory = options && options->directory && *options->directory ?
	    options->directory : ".";
	ctx->language = options && options->use_language && *options->use_language ?
	    options->use_language : "c";
	replacement = legacy_shape_replacement(ctx->shape);
	if (replacement)
		return init_error2(error, error_len,
		    "qstar: init template '%s' was removed; use 'qstar init %s <directory> --use-language=c'",
		    ctx->shape, replacement);
	if (!string_in_list(ctx->shape, init_shapes))
		return init_error(error, error_len, "qstar: unknown init shape '%s'",
		    ctx->shape);
	if (strcmp(ctx->language, "c") != 0)
		return init_error(error, error_len,
		    "qstar: init scaffold for language '%s' is not available yet; use --use-language=c",
		    ctx->language);
	if (options && options->name && *options->name) {
		if (snprintf(ctx->project_name, sizeof(ctx->project_name), "%s",
		    options->name) >= (int)sizeof(ctx->project_name))
			return init_error(error, error_len,
			    "qstar: init project name is too long '%s'", options->name);
	} else if (copy_basename(ctx->project_name, sizeof(ctx->project_name),
	    ctx->directory) < 0) {
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    ctx->directory);
	}
	make_project_ident(ctx->project_name, ctx->project_ident,
	    sizeof(ctx->project_ident));
	make_header_guard(ctx->project_ident, ctx->header_guard,
	    sizeof(ctx->header_guard));
	if (escape_lua_string(ctx->project_name, ctx->project_lua,
	    sizeof(ctx->project_lua)) < 0)
		return init_error(error, error_len,
		    "qstar: init project name is too long '%s'", ctx->project_name);
	return 0;
}

/** Create a qstar init shape in the requested directory. */
int
qstar_init_project(const struct qstar_init_options *options, FILE *out, char *error,
    size_t error_len)
{
	struct init_context ctx;
	int rc;

	if (prepare_context(&ctx, options, error, error_len) < 0)
		return -1;
	if (!ctx.options->dry_run && mkdir_p(ctx.directory, error, error_len) < 0)
		return -1;
	fprintf(out, "qstar init v2\n");
	fprintf(out, "shape %s\n", ctx.shape);
	fprintf(out, "language %s\n", ctx.language);
	fprintf(out, "project %s\n", ctx.project_name);
	fprintf(out, "directory %s\n", ctx.directory);
	if (ctx.options->dry_run)
		fputs("dry_run true\n", out);
	if (strcmp(ctx.shape, "app") == 0)
		rc = write_app_shape(&ctx, out, error, error_len);
	else if (strcmp(ctx.shape, "lib") == 0)
		rc = write_lib_shape(&ctx, out, error, error_len);
	else if (strcmp(ctx.shape, "tool") == 0)
		rc = write_tool_shape(&ctx, out, error, error_len);
	else if (strcmp(ctx.shape, "empty") == 0)
		rc = write_empty_shape(&ctx, out, error, error_len);
	else
		rc = write_workspace_shape(&ctx, out, error, error_len);
	if (rc < 0)
		return -1;
	fputs("status ok\n", out);
	return 0;
}
