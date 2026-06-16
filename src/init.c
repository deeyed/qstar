#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <mach-o/dyld.h>
#endif

#define QSTAR_INIT_MAX_LANGUAGES 16
#define QSTAR_INIT_LANGUAGE_MAX 64

static const char *const init_shapes[] = {
	"app",
	"lib",
	"tool",
	"empty",
	"workspace",
	NULL
};

struct init_language {
	char id[QSTAR_INIT_LANGUAGE_MAX];
	char local_name[QSTAR_INIT_LANGUAGE_MAX + 8];
	char source_dir[QSTAR_PATH_MAX];
	int builtin;
	int external;
	int vendored;
};

struct init_context {
	const struct qstar_init_options *options;
	const char *shape;
	const char *directory;
	const char *primary_language;
	struct init_language languages[QSTAR_INIT_MAX_LANGUAGES];
	size_t language_len;
	char project_name[128];
	char project_lua[256];
	char project_ident[128];
	char header_guard[160];
	char activation_block[4096];
	char tool_entries[8192];
};

static int appendf(char *dst, size_t dstlen, char *error, size_t error_len,
    const char *fmt, ...);

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

static int
path_is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
path_is_regular(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int
copy_existing_path(const char *path, char *dst, size_t dstlen)
{
	if (snprintf(dst, dstlen, "%s", path) >= (int)dstlen)
		return -1;
	return 0;
}

static int
current_executable_path(char *dst, size_t dstlen)
{
	char raw[QSTAR_PATH_MAX];
#if defined(__APPLE__) && defined(__MACH__)
	uint32_t len;

	len = (uint32_t)sizeof(raw);
	if (_NSGetExecutablePath(raw, &len) != 0)
		return -1;
	return copy_existing_path(raw, dst, dstlen);
#elif defined(__linux__)
	ssize_t n;

	n = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
	if (n < 0 || (size_t)n >= sizeof(raw))
		return -1;
	raw[n] = '\0';
	return copy_existing_path(raw, dst, dstlen);
#else
	(void)dst;
	(void)dstlen;
	(void)raw;
	return -1;
#endif
}

static int
valid_language_id(const char *s)
{
	const unsigned char *p;

	if (!s || !*s)
		return 0;
	for (p = (const unsigned char *)s; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-'))
			return 0;
	}
	return 1;
}

static int
language_id_is_builtin(const char *id)
{
	return strcmp(id, "c") == 0 || strcmp(id, "cxx") == 0 ||
	    strcmp(id, "asm") == 0;
}

static const char *
default_compiler_for_language(const char *id)
{
	if (strcmp(id, "c") == 0)
		return "cc";
	if (strcmp(id, "cxx") == 0)
		return "c++";
	if (strcmp(id, "asm") == 0)
		return "cc";
	if (strcmp(id, "rust") == 0)
		return "rustc";
	if (strcmp(id, "cuda") == 0)
		return "nvcc";
	return id && *id ? id : "cc";
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
		if (buf[0] && qstar_platform_mkdir(buf, 0777) < 0) {
			if (errno != EEXIST)
				return init_error(error, error_len,
				    "qstar: init could not create directory '%s'", buf);
			if (!path_is_dir(buf))
				return init_error(error, error_len,
				    "qstar: init path exists but is not a directory '%s'",
				    buf);
		}
		buf[i] = '/';
	}
	if (qstar_platform_mkdir(buf, 0777) < 0) {
		if (errno != EEXIST)
			return init_error(error, error_len,
			    "qstar: init could not create directory '%s'", buf);
		if (!path_is_dir(buf))
			return init_error(error, error_len,
			    "qstar: init path exists but is not a directory '%s'", buf);
	}
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

static int
provider_manifest_from_base(const char *base, const char *id, char *dir, size_t dir_len)
{
	char candidate_dir[QSTAR_PATH_MAX], manifest[QSTAR_PATH_MAX];

	if (!base || !*base)
		return 0;
	if (qstar_path_join(base, id, candidate_dir, sizeof(candidate_dir)) < 0 ||
	    snprintf(manifest, sizeof(manifest), "%s/%s.qsm", candidate_dir, id) >=
	    (int)sizeof(manifest))
		return -1;
	if (!path_is_regular(manifest))
		return 0;
	return copy_existing_path(candidate_dir, dir, dir_len) < 0 ? -1 : 1;
}

static int
resolve_provider_source_dir(const char *id, char *dir, size_t dir_len)
{
	const char *env_dir;
	char exe[QSTAR_PATH_MAX], bin_dir[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX];
	char source_build[QSTAR_PATH_MAX], source_root[QSTAR_PATH_MAX];
	char base[QSTAR_PATH_MAX];
	int rc;

	env_dir = getenv("QSTAR_PROVIDER_DIR");
	rc = provider_manifest_from_base(env_dir, id, dir, dir_len);
	if (rc != 0)
		return rc;
	if (current_executable_path(exe, sizeof(exe)) == 0 &&
	    qstar_dirname(exe, bin_dir, sizeof(bin_dir)) == 0) {
		if (qstar_dirname(bin_dir, prefix, sizeof(prefix)) == 0 &&
		    qstar_path_join(prefix, "share/qstar/languages", base,
		    sizeof(base)) == 0) {
			rc = provider_manifest_from_base(base, id, dir, dir_len);
			if (rc != 0)
				return rc;
		}
		if (qstar_dirname(bin_dir, source_build, sizeof(source_build)) == 0 &&
		    qstar_dirname(source_build, source_root, sizeof(source_root)) == 0 &&
		    qstar_path_join(source_root, "qstar/languages", base,
		    sizeof(base)) == 0) {
			rc = provider_manifest_from_base(base, id, dir, dir_len);
			if (rc != 0)
				return rc;
		}
	}
	rc = provider_manifest_from_base("qstar/languages", id, dir, dir_len);
	if (rc != 0)
		return rc;
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

static void
make_language_local_name(const char *id, char *dst, size_t dstlen)
{
	size_t i, out;
	unsigned char c;
	int direct;

	direct = id && (ident_start((unsigned char)id[0]));
	for (i = 0; direct && id[i]; i++) {
		c = (unsigned char)id[i];
		if (!(ident_char(c) && c != '-'))
			direct = 0;
	}
	if (direct) {
		snprintf(dst, dstlen, "%s", id);
		return;
	}
	out = 0;
	if (dstlen > 5) {
		memcpy(dst, "lang_", 5);
		out = 5;
	}
	for (i = 0; id && id[i] && out + 1 < dstlen; i++) {
		c = (unsigned char)id[i];
		dst[out++] = (ident_char(c) && c != '-') ? (char)c : '_';
	}
	if (out == 5 && out + 1 < dstlen)
		dst[out++] = 'x';
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
language_selected(const struct init_context *ctx, const char *id)
{
	size_t i;

	for (i = 0; i < ctx->language_len; i++) {
		if (strcmp(ctx->languages[i].id, id) == 0)
			return 1;
	}
	return 0;
}

static int
copy_trimmed_language(char *dst, size_t dstlen, const char *start, size_t n)
{
	while (n > 0 && isspace((unsigned char)*start)) {
		start++;
		n--;
	}
	while (n > 0 && isspace((unsigned char)start[n - 1]))
		n--;
	if (n == 0 || n + 1 > dstlen)
		return -1;
	memcpy(dst, start, n);
	dst[n] = '\0';
	return 0;
}

static int
add_init_language(struct init_context *ctx, const char *id, char *error,
    size_t error_len)
{
	struct init_language *language;
	char source_dir[QSTAR_PATH_MAX];
	int rc;

	if (!valid_language_id(id))
		return init_error(error, error_len,
		    "qstar: invalid init language id '%s'", id);
	if (language_selected(ctx, id))
		return 0;
	if (ctx->language_len >= QSTAR_INIT_MAX_LANGUAGES)
		return init_error(error, error_len,
		    "qstar: too many init languages; first unsupported id was '%s'", id);
	language = &ctx->languages[ctx->language_len];
	memset(language, 0, sizeof(*language));
	if (snprintf(language->id, sizeof(language->id), "%s", id) >=
	    (int)sizeof(language->id))
		return init_error(error, error_len,
		    "qstar: init language id is too long '%s'", id);
	language->builtin = language_id_is_builtin(id);
	language->external = !language->builtin;
	if (language->external) {
		rc = resolve_provider_source_dir(id, source_dir, sizeof(source_dir));
		if (rc < 0)
			return init_error(error, error_len,
			    "qstar: standard language provider path for '%s' is too long",
			    id);
		if (rc == 0) {
			if (error && error_len)
				snprintf(error, error_len,
				    "qstar: language provider '%s' not found; expected provider manifest in qstar/languages/%s, QSTAR_PROVIDER_DIR/%s, installed share/qstar/languages/%s, or dev checkout qstar/languages/%s",
				    id, id, id, id, id);
			return -1;
		}
		if (snprintf(language->source_dir, sizeof(language->source_dir), "%s",
		    source_dir) >= (int)sizeof(language->source_dir))
			return init_error(error, error_len,
			    "qstar: standard language provider path for '%s' is too long",
			    id);
		make_language_local_name(id, language->local_name,
		    sizeof(language->local_name));
	}
	ctx->language_len++;
	return 0;
}

static int
parse_init_languages(struct init_context *ctx, const char *raw, char *error,
    size_t error_len)
{
	const char *p, *start;
	char id[QSTAR_INIT_LANGUAGE_MAX];
	size_t n;

	if (!raw || !*raw)
		raw = "c";
	p = raw;
	while (1) {
		start = p;
		while (*p && *p != ',')
			p++;
		n = (size_t)(p - start);
		if (copy_trimmed_language(id, sizeof(id), start, n) < 0)
			return init_error(error, error_len,
			    "qstar: init --use-language contains an empty or too-long language id near '%s'",
			    start);
		if (add_init_language(ctx, id, error, error_len) < 0)
			return -1;
		if (!*p)
			break;
		p++;
	}
	if (ctx->language_len == 0)
		return init_error(error, error_len,
		    "qstar: init --use-language did not name any languages", raw);
	ctx->primary_language = ctx->languages[0].id;
	return 0;
}

static int
append_builtin_tool_entry(struct init_context *ctx, const char *id, char *error,
    size_t error_len)
{
	const char *tool;

	tool = default_compiler_for_language(id);
	return appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error, error_len,
	    "    %s = {\n"
	    "      compiler = qstar.cli {\"%s\"},\n"
	    "    },\n",
	    id, tool);
}

static int
build_language_snippets(struct init_context *ctx, char *error, size_t error_len)
{
	size_t i;
	char id_lua[QSTAR_INIT_LANGUAGE_MAX * 2];

	ctx->activation_block[0] = '\0';
	ctx->tool_entries[0] = '\0';
	if (append_builtin_tool_entry(ctx, "c", error, error_len) < 0)
		return -1;
	if (language_selected(ctx, "cxx") &&
	    append_builtin_tool_entry(ctx, "cxx", error, error_len) < 0)
		return -1;
	if (language_selected(ctx, "asm") &&
	    append_builtin_tool_entry(ctx, "asm", error, error_len) < 0)
		return -1;
	for (i = 0; i < ctx->language_len; i++) {
		if (!ctx->languages[i].external)
			continue;
		if (escape_lua_string(ctx->languages[i].id, id_lua,
		    sizeof(id_lua)) < 0)
			return init_error(error, error_len,
			    "qstar: init language id is too long '%s'",
			    ctx->languages[i].id);
		if (appendf(ctx->activation_block, sizeof(ctx->activation_block),
		    error, error_len, "local %s = qstar.use_language(\"%s\")\n",
		    ctx->languages[i].local_name, id_lua) < 0)
			return -1;
		if (appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error,
		    error_len,
		    "    [\"%s\"] = %s.tools {\n"
		    "      compiler = qstar.cli {\"%s\"},\n"
		    "    },\n",
		    id_lua, ctx->languages[i].local_name,
		    default_compiler_for_language(ctx->languages[i].id)) < 0)
			return -1;
	}
	return 0;
}

static int
appendf(char *dst, size_t dstlen, char *error, size_t error_len, const char *fmt, ...)
{
	va_list ap;
	size_t used;
	int n;

	used = strlen(dst);
	if (used >= dstlen)
		return init_error(error, error_len, "qstar: init template too large for '%s'",
		    "qstar.lua");
	va_start(ap, fmt);
	n = vsnprintf(dst + used, dstlen - used, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= dstlen - used)
		return init_error(error, error_len, "qstar: init template too large for '%s'",
		    "qstar.lua");
	return 0;
}

static int
string_item_compare(const void *a, const void *b)
{
	const char *const *sa = (const char *const *)a;
	const char *const *sb = (const char *const *)b;

	return strcmp(*sa, *sb);
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

static int
format_qstar_lua(const struct init_context *ctx, char *dst, size_t dstlen,
    char *error, size_t error_len, const char *target_body)
{
	const char *gap;
	int n;

	gap = ctx->activation_block[0] ? "\n" : "";
	n = snprintf(dst, dstlen,
	    "%s%s"
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
	    "%s"
	    "  },\n"
	    "}\n"
	    "\n"
	    "qstar.config \"debug\" {\n"
	    "  toolset = \"//:host\",\n"
	    "}\n"
	    "%s",
	    ctx->activation_block, gap, ctx->project_lua, ctx->tool_entries,
	    target_body ? target_body : "");
	if (n < 0 || (size_t)n >= dstlen)
		return init_error(error, error_len, "qstar: init template too large for '%s'",
		    "qstar.lua");
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
ensure_dir(const struct init_context *ctx, const char *rel, FILE *out, char *error,
    size_t error_len)
{
	char path[QSTAR_PATH_MAX];

	if (qstar_path_join(ctx->directory, rel, path, sizeof(path)) < 0)
		return init_error(error, error_len, "qstar: init path too long '%s'", rel);
	if (ctx->options->dry_run) {
		fprintf(out, "would_create_dir %s\n", rel);
		return 0;
	}
	if (path_exists(path)) {
		if (!path_is_dir(path))
			return init_error(error, error_len,
			    "qstar: init path exists but is not a directory '%s'", path);
		return 0;
	}
	if (mkdir_p(path, error, error_len) < 0)
		return -1;
	fprintf(out, "create_dir %s\n", rel);
	return 0;
}

static int
copy_regular_file(const struct init_context *ctx, const char *src, const char *rel,
    FILE *out, char *error, size_t error_len)
{
	char dst[QSTAR_PATH_MAX], buf[8192];
	FILE *in, *f;
	size_t n;
	int failed;

	if (qstar_path_join(ctx->directory, rel, dst, sizeof(dst)) < 0)
		return init_error(error, error_len, "qstar: init path too long '%s'", rel);
	if (ctx->options->dry_run) {
		fprintf(out, "would_create %s\n", rel);
		return 0;
	}
	if (path_exists(dst))
		return init_error(error, error_len,
		    "qstar: init refuses to overwrite existing file '%s'", dst);
	if (mkdir_parent(dst, error, error_len) < 0)
		return -1;
	in = fopen(src, "rb");
	if (!in)
		return init_error(error, error_len,
		    "qstar: init could not read provider file '%s'", src);
	f = fopen(dst, "wb");
	if (!f) {
		fclose(in);
		return init_error(error, error_len,
		    "qstar: init could not create file '%s'", dst);
	}
	failed = 0;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, f) != n) {
			failed = 1;
			break;
		}
	}
	if (ferror(in))
		failed = 1;
	if (fclose(in) < 0)
		failed = 1;
	if (fclose(f) < 0)
		failed = 1;
	if (failed)
		return init_error(error, error_len,
		    "qstar: init could not copy provider file '%s'", src);
	fprintf(out, "create %s\n", rel);
	return 0;
}

static int
copy_provider_tree(const struct init_context *ctx, const char *src_dir,
    const char *dst_rel, FILE *out, char *error, size_t error_len)
{
	struct qstar_string_list names;
	struct dirent *entry;
	DIR *dir;
	size_t i;
	char src_child[QSTAR_PATH_MAX], dst_child[QSTAR_PATH_MAX];
	struct stat st;

	if (ensure_dir(ctx, dst_rel, out, error, error_len) < 0)
		return -1;
	memset(&names, 0, sizeof(names));
	dir = opendir(src_dir);
	if (!dir)
		return init_error(error, error_len,
		    "qstar: init could not read provider directory '%s'", src_dir);
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (qstar_string_list_push(&names, entry->d_name) < 0) {
			closedir(dir);
			qstar_string_list_free(&names);
			return init_error(error, error_len, "qstar: out of memory", "");
		}
	}
	closedir(dir);
	qsort(names.items, names.len, sizeof(names.items[0]), string_item_compare);
	for (i = 0; i < names.len; i++) {
		if (qstar_path_join(src_dir, names.items[i], src_child,
		    sizeof(src_child)) < 0 ||
		    qstar_path_join(dst_rel, names.items[i], dst_child,
		    sizeof(dst_child)) < 0) {
			qstar_string_list_free(&names);
			return init_error(error, error_len,
			    "qstar: init provider path too long '%s'", names.items[i]);
		}
		if (stat(src_child, &st) < 0) {
			qstar_string_list_free(&names);
			return init_error(error, error_len,
			    "qstar: init could not stat provider path '%s'", src_child);
		}
		if (S_ISDIR(st.st_mode)) {
			if (copy_provider_tree(ctx, src_child, dst_child, out, error,
			    error_len) < 0) {
				qstar_string_list_free(&names);
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			if (copy_regular_file(ctx, src_child, dst_child, out, error,
			    error_len) < 0) {
				qstar_string_list_free(&names);
				return -1;
			}
		} else {
			qstar_string_list_free(&names);
			return init_error(error, error_len,
			    "qstar: init provider path is not a regular file or directory '%s'",
			    src_child);
		}
	}
	qstar_string_list_free(&names);
	return 0;
}

static int
has_external_languages(const struct init_context *ctx)
{
	size_t i;

	for (i = 0; i < ctx->language_len; i++) {
		if (ctx->languages[i].external)
			return 1;
	}
	return 0;
}

static int
vendor_language_providers(struct init_context *ctx, FILE *out, char *error,
    size_t error_len)
{
	size_t i;
	char dst_rel[QSTAR_PATH_MAX];

	if (!has_external_languages(ctx))
		return 0;
	if (ensure_dir(ctx, "qstar", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "qstar/languages", out, error, error_len) < 0)
		return -1;
	for (i = 0; i < ctx->language_len; i++) {
		if (!ctx->languages[i].external)
			continue;
		if (snprintf(dst_rel, sizeof(dst_rel), "qstar/languages/%s",
		    ctx->languages[i].id) >= (int)sizeof(dst_rel))
			return init_error(error, error_len,
			    "qstar: init provider path too long '%s'",
			    ctx->languages[i].id);
		fprintf(out, "vendor %s\n", dst_rel);
		if (copy_provider_tree(ctx, ctx->languages[i].source_dir, dst_rel,
		    out, error, error_len) < 0)
			return -1;
		ctx->languages[i].vendored = 1;
		fprintf(out, "activate %s\n", ctx->languages[i].id);
	}
	return 0;
}

static int
write_app_shape(const struct init_context *ctx, FILE *out, char *error, size_t error_len)
{
	char body[16384], target_body[2048];

	if (format_body(target_body, sizeof(target_body), error, error_len, "qstar.lua",
	    "\n"
	    "qstar.executable \"app\" {\n"
	    "  configs = {\"//:debug\"},\n"
	    "  sources = {\"src/main.c\"},\n"
	    "}\n",
	    "") < 0 ||
	    format_qstar_lua(ctx, body, sizeof(body), error, error_len,
	    target_body) < 0)
		return -1;
	if (ensure_dir(ctx, "src", out, error, error_len) < 0 ||
	    write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
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
	char body[16384], target_body[4096], path[QSTAR_PATH_MAX];

	if (format_body(target_body, sizeof(target_body), error, error_len, "qstar.lua",
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
	    ctx->project_ident, ctx->project_ident) < 0 ||
	    format_qstar_lua(ctx, body, sizeof(body), error, error_len,
	    target_body) < 0)
		return -1;
	if (ensure_dir(ctx, "include", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "src", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "tests", out, error, error_len) < 0 ||
	    write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
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
	char body[16384], target_body[4096], path[QSTAR_PATH_MAX];

	if (format_body(target_body, sizeof(target_body), error, error_len, "qstar.lua",
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
	    ctx->project_ident) < 0 ||
	    format_qstar_lua(ctx, body, sizeof(body), error, error_len,
	    target_body) < 0)
		return -1;
	if (ensure_dir(ctx, "tools", out, error, error_len) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "tools/%s", ctx->project_ident) >=
	    (int)sizeof(path))
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    ctx->project_ident);
	if (ensure_dir(ctx, path, out, error, error_len) < 0 ||
	    write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
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
	char body[16384];

	if (format_qstar_lua(ctx, body, sizeof(body), error, error_len, "") < 0)
		return -1;
	return write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ?
	    -1 : write_gitignore(ctx, out, error, error_len);
}

static int
write_workspace_shape(const struct init_context *ctx, FILE *out, char *error,
    size_t error_len)
{
	char body[16384], target_body[4096];

	if (format_body(target_body, sizeof(target_body), error, error_len, "qstar.lua",
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
	    "") < 0 ||
	    format_qstar_lua(ctx, body, sizeof(body), error, error_len,
	    target_body) < 0)
		return -1;
	if (ensure_dir(ctx, "packages", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "packages/core", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "packages/core/include", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "packages/core/src", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "packages/app", out, error, error_len) < 0 ||
	    ensure_dir(ctx, "packages/app/src", out, error, error_len) < 0 ||
	    write_text_file(ctx, "qstar.lua", body, 0, out, error, error_len) < 0 ||
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
id_list_contains(const struct qstar_string_list *list, const char *id)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], id) == 0)
			return 1;
	}
	return 0;
}

static int
collect_provider_ids_from_base(const char *base, struct qstar_string_list *ids,
    char *error, size_t error_len)
{
	struct qstar_string_list names;
	struct dirent *entry;
	DIR *dir;
	size_t i;
	char manifest[QSTAR_PATH_MAX], provider_dir[QSTAR_PATH_MAX];

	if (!base || !*base || !path_is_dir(base))
		return 0;
	memset(&names, 0, sizeof(names));
	dir = opendir(base);
	if (!dir)
		return init_error(error, error_len,
		    "qstar: init could not read provider directory '%s'", base);
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (qstar_string_list_push(&names, entry->d_name) < 0) {
			closedir(dir);
			qstar_string_list_free(&names);
			return init_error(error, error_len, "qstar: out of memory", "");
		}
	}
	closedir(dir);
	qsort(names.items, names.len, sizeof(names.items[0]), string_item_compare);
	for (i = 0; i < names.len; i++) {
		if (!valid_language_id(names.items[i]) || id_list_contains(ids, names.items[i]))
			continue;
		if (qstar_path_join(base, names.items[i], provider_dir,
		    sizeof(provider_dir)) < 0 ||
		    snprintf(manifest, sizeof(manifest), "%s/%s.qsm", provider_dir,
		    names.items[i]) >= (int)sizeof(manifest)) {
			qstar_string_list_free(&names);
			return init_error(error, error_len,
			    "qstar: init provider path too long '%s'", names.items[i]);
		}
		if (path_is_regular(manifest) &&
		    qstar_string_list_push(ids, names.items[i]) < 0) {
			qstar_string_list_free(&names);
			return init_error(error, error_len, "qstar: out of memory", "");
		}
	}
	qstar_string_list_free(&names);
	return 0;
}

static int
collect_provider_ids(struct qstar_string_list *ids, char *error, size_t error_len)
{
	const char *env_dir;
	char exe[QSTAR_PATH_MAX], bin_dir[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX];
	char source_build[QSTAR_PATH_MAX], source_root[QSTAR_PATH_MAX];
	char base[QSTAR_PATH_MAX];

	env_dir = getenv("QSTAR_PROVIDER_DIR");
	if (collect_provider_ids_from_base(env_dir, ids, error, error_len) < 0)
		return -1;
	if (current_executable_path(exe, sizeof(exe)) == 0 &&
	    qstar_dirname(exe, bin_dir, sizeof(bin_dir)) == 0) {
		if (qstar_dirname(bin_dir, prefix, sizeof(prefix)) == 0 &&
		    qstar_path_join(prefix, "share/qstar/languages", base,
		    sizeof(base)) == 0 &&
		    collect_provider_ids_from_base(base, ids, error, error_len) < 0)
			return -1;
		if (qstar_dirname(bin_dir, source_build, sizeof(source_build)) == 0 &&
		    qstar_dirname(source_build, source_root, sizeof(source_root)) == 0 &&
		    qstar_path_join(source_root, "qstar/languages", base,
		    sizeof(base)) == 0 &&
		    collect_provider_ids_from_base(base, ids, error, error_len) < 0)
			return -1;
	}
	if (collect_provider_ids_from_base("qstar/languages", ids, error,
	    error_len) < 0)
		return -1;
	return 0;
}

int
qstar_init_print_languages(FILE *out, char *error, size_t error_len)
{
	struct qstar_string_list ids;
	char source_dir[QSTAR_PATH_MAX];
	size_t i;

	memset(&ids, 0, sizeof(ids));
	if (collect_provider_ids(&ids, error, error_len) < 0) {
		qstar_string_list_free(&ids);
		return -1;
	}
	qsort(ids.items, ids.len, sizeof(ids.items[0]), string_item_compare);
	fputs("qstar init languages\n", out);
	fputs("builtin c\n", out);
	fputs("builtin cxx\n", out);
	fputs("builtin asm\n", out);
	for (i = 0; i < ids.len; i++) {
		if (resolve_provider_source_dir(ids.items[i], source_dir,
		    sizeof(source_dir)) > 0)
			fprintf(out, "provider %s source=%s\n", ids.items[i], source_dir);
	}
	qstar_string_list_free(&ids);
	return 0;
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
	replacement = legacy_shape_replacement(ctx->shape);
	if (replacement)
		return init_error2(error, error_len,
		    "qstar: init template '%s' was removed; use 'qstar init %s <directory> --use-language=c'",
		    ctx->shape, replacement);
	if (!string_in_list(ctx->shape, init_shapes))
		return init_error(error, error_len, "qstar: unknown init shape '%s'",
		    ctx->shape);
	if (parse_init_languages(ctx, options ? options->use_language : NULL, error,
	    error_len) < 0)
		return -1;
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
	if (build_language_snippets(ctx, error, error_len) < 0)
		return -1;
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
	fprintf(out, "language %s\n", ctx.primary_language);
	fprintf(out, "project %s\n", ctx.project_name);
	fprintf(out, "directory %s\n", ctx.directory);
	if (ctx.options->dry_run)
		fputs("dry_run true\n", out);
	if (strcmp(ctx.primary_language, "c") != 0)
		fprintf(out,
		    "warning language '%s' has no init scaffold for shape '%s'; using builtin c scaffold\n",
		    ctx.primary_language, ctx.shape);
	if (vendor_language_providers(&ctx, out, error, error_len) < 0)
		return -1;
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
