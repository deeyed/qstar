#include "internal.h"

#include "lauxlib.h"
#include "lua.h"

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
	char namespace[QSTAR_INIT_LANGUAGE_MAX];
	char local_name[QSTAR_INIT_LANGUAGE_MAX + 8];
	char source_dir[QSTAR_PATH_MAX];
	char source_ext[32];
	int builtin;
	int external;
	int vendored;
	int scaffold_loaded;
	int has_scaffold;
	int has_shape;
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
	char config_lang_entries[8192];
};

static int appendf(char *dst, size_t dstlen, char *error, size_t error_len,
    const char *fmt, ...);
static int string_item_compare(const void *a, const void *b);

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
lua_identifier(const char *s)
{
	size_t i;

	if (!s || !ident_start((unsigned char)s[0]))
		return 0;
	for (i = 1; s[i]; i++) {
		if (!ident_char((unsigned char)s[i]))
			return 0;
	}
	return 1;
}

static int
append_lua_key(char *dst, size_t dstlen, char *error, size_t error_len,
    const char *key)
{
	char escaped[QSTAR_INIT_LANGUAGE_MAX * 2];

	if (lua_identifier(key))
		return appendf(dst, dstlen, error, error_len, "%s", key);
	if (escape_lua_string(key, escaped, sizeof(escaped)) < 0)
		return init_error(error, error_len, "qstar: init key is too long '%s'",
		    key);
	return appendf(dst, dstlen, error, error_len, "[\"%s\"]", escaped);
}

static int
init_lua_language_provider(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushvalue(L, 1);
	return 1;
}

static lua_State *
load_provider_manifest(const struct init_language *language, char *error,
    size_t error_len)
{
	lua_State *L;
	char manifest[QSTAR_PATH_MAX];

	if (!language || !language->external)
		return NULL;
	if (snprintf(manifest, sizeof(manifest), "%s/%s.qsm", language->source_dir,
	    language->id) >= (int)sizeof(manifest)) {
		init_error(error, error_len,
		    "qstar: init provider manifest path too long for '%s'",
		    language->id);
		return NULL;
	}
	L = luaL_newstate();
	if (!L) {
		init_error(error, error_len, "qstar: out of memory", "");
		return NULL;
	}
	lua_newtable(L);
	lua_pushcfunction(L, init_lua_language_provider);
	lua_setfield(L, -2, "language_provider");
	lua_setglobal(L, "qstar");
	if (luaL_loadfilex(L, manifest, "t") != LUA_OK ||
	    lua_pcall(L, 0, 1, 0) != LUA_OK) {
		if (error && error_len)
			snprintf(error, error_len,
			    "qstar: init could not load language provider '%s': %s",
			    language->id, lua_tostring(L, -1));
		lua_close(L);
		return NULL;
	}
	if (!lua_istable(L, -1)) {
		init_error(error, error_len,
		    "qstar: init language provider '%s' must return a table",
		    language->id);
		lua_close(L);
		return NULL;
	}
	return L;
}

static void
manifest_string_field(lua_State *L, int table, const char *field, char *dst,
    size_t dstlen, const char *fallback)
{
	const char *value;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
	snprintf(dst, dstlen, "%s", value ? value : "");
	lua_pop(L, 1);
}

static void
manifest_source_ext(lua_State *L, int manifest, char *dst, size_t dstlen)
{
	if (dstlen)
		dst[0] = '\0';
	if (manifest < 0)
		manifest = lua_gettop(L) + manifest + 1;
	lua_getfield(L, manifest, "units");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "object");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "suffixes");
			if (lua_istable(L, -1)) {
				lua_rawgeti(L, -1, 1);
				if (lua_isstring(L, -1))
					snprintf(dst, dstlen, "%s",
					    lua_tostring(L, -1));
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

static int
scaffold_shape_exists(lua_State *L, int manifest, const char *shape)
{
	int exists;

	if (manifest < 0)
		manifest = lua_gettop(L) + manifest + 1;
	exists = 0;
	lua_getfield(L, manifest, "scaffold");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "shapes");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, shape);
			exists = lua_istable(L, -1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return exists;
}

static int
load_language_scaffold_metadata(struct init_context *ctx, char *error,
    size_t error_len)
{
	size_t i;
	lua_State *L;

	for (i = 0; i < ctx->language_len; i++) {
		if (ctx->languages[i].builtin) {
			snprintf(ctx->languages[i].namespace,
			    sizeof(ctx->languages[i].namespace), "%s",
			    ctx->languages[i].id);
			continue;
		}
		L = load_provider_manifest(&ctx->languages[i], error, error_len);
		if (!L)
			return -1;
		manifest_string_field(L, -1, "namespace",
		    ctx->languages[i].namespace, sizeof(ctx->languages[i].namespace),
		    ctx->languages[i].id);
		manifest_source_ext(L, -1, ctx->languages[i].source_ext,
		    sizeof(ctx->languages[i].source_ext));
		lua_getfield(L, -1, "scaffold");
		ctx->languages[i].has_scaffold = lua_istable(L, -1);
		lua_pop(L, 1);
		ctx->languages[i].has_shape = scaffold_shape_exists(L, -1,
		    ctx->shape);
		ctx->languages[i].scaffold_loaded = 1;
		lua_close(L);
	}
	return 0;
}

static const char *
init_default_target_name(const struct init_context *ctx)
{
	if (strcmp(ctx->shape, "lib") == 0)
		return "core";
	if (strcmp(ctx->shape, "tool") == 0)
		return "tool";
	if (strcmp(ctx->shape, "workspace") == 0)
		return "all";
	if (strcmp(ctx->shape, "empty") == 0)
		return "";
	return "app";
}

static int
expand_template(const struct init_context *ctx, const struct init_language *language,
    const char *src, const char *target_name, char *dst, size_t dstlen,
    char *error, size_t error_len)
{
	size_t i, out, n;
	const char *value, *end, *name;

	out = 0;
	for (i = 0; src && src[i];) {
		if (src[i] != '$') {
			if (out + 1 >= dstlen)
				return init_error(error, error_len,
				    "qstar: init template expansion too large for '%s'",
				    src);
			dst[out++] = src[i++];
			continue;
		}
		if (src[i + 1] != '{')
			return init_error(error, error_len,
			    "qstar: init scaffold template contains unsupported '$' in '%s'",
			    src);
		name = src + i + 2;
		end = strchr(name, '}');
		if (!end)
			return init_error(error, error_len,
			    "qstar: init scaffold template has unterminated variable in '%s'",
			    src);
		n = (size_t)(end - name);
		value = NULL;
		if (n == strlen("project_name") &&
		    strncmp(name, "project_name", n) == 0)
			value = ctx->project_name;
		else if (n == strlen("project_ident") &&
		    strncmp(name, "project_ident", n) == 0)
			value = ctx->project_ident;
		else if (n == strlen("namespace") && strncmp(name, "namespace", n) == 0)
			value = language->namespace;
		else if (n == strlen("shape") && strncmp(name, "shape", n) == 0)
			value = ctx->shape;
		else if (n == strlen("target_name") &&
		    strncmp(name, "target_name", n) == 0)
			value = target_name ? target_name : init_default_target_name(ctx);
		else if (n == strlen("source_ext") &&
		    strncmp(name, "source_ext", n) == 0)
			value = language->source_ext;
		if (!value)
			return init_error(error, error_len,
			    "qstar: init scaffold template has unknown variable in '%s'",
			    src);
		n = strlen(value);
		if (out + n >= dstlen)
			return init_error(error, error_len,
			    "qstar: init template expansion too large for '%s'", src);
		memcpy(dst + out, value, n);
		out += n;
		i = (size_t)(end - src) + 1;
	}
	if (out >= dstlen)
		return init_error(error, error_len,
		    "qstar: init template expansion too large for '%s'", src ? src : "");
	dst[out] = '\0';
	return 0;
}

static int
expand_scaffold_path(const struct init_context *ctx,
    const struct init_language *language, const char *src, const char *target_name,
    int file_path, char *dst, size_t dstlen, char *error, size_t error_len)
{
	const char *reason;
	size_t n;

	if (expand_template(ctx, language, src, target_name, dst, dstlen, error,
	    error_len) < 0)
		return -1;
	if (!qstar_path_is_package_relative(dst)) {
		reason = qstar_path_package_relative_reason(dst);
		if (error && error_len)
			snprintf(error, error_len,
			    "qstar: init scaffold path '%s' must be package-relative%s%s",
			    dst, reason && *reason ? ": " : "",
			    reason && *reason ? reason : "");
		return -1;
	}
	n = strlen(dst);
	if (file_path && n > 0 && dst[n - 1] == '/')
		return init_error(error, error_len,
		    "qstar: init scaffold file path must not end with '/' in '%s'",
		    dst);
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
append_lua_string_list_literal(lua_State *L, int table, char *dst, size_t dstlen,
    char *error, size_t error_len)
{
	size_t i, n;
	char escaped[1024];
	const char *value;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return appendf(dst, dstlen, error, error_len, "{}");
	if (appendf(dst, dstlen, error, error_len, "{") < 0)
		return -1;
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		value = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (escape_lua_string(value, escaped, sizeof(escaped)) < 0) {
			lua_pop(L, 1);
			return init_error(error, error_len,
			    "qstar: init scaffold string is too long '%s'", value);
		}
		if (appendf(dst, dstlen, error, error_len, "%s\"%s\"",
		    i == 1 ? "" : ", ", escaped) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return appendf(dst, dstlen, error, error_len, "}");
}

static int
append_scaffold_value(lua_State *L, int value, char *dst, size_t dstlen,
    char *error, size_t error_len)
{
	const char *s;
	char escaped[1024];

	if (value < 0)
		value = lua_gettop(L) + value + 1;
	if (lua_isboolean(L, value))
		return appendf(dst, dstlen, error, error_len, "%s",
		    lua_toboolean(L, value) ? "true" : "false");
	if (lua_isstring(L, value)) {
		s = lua_tostring(L, value);
		if (escape_lua_string(s, escaped, sizeof(escaped)) < 0)
			return init_error(error, error_len,
			    "qstar: init scaffold string is too long '%s'", s);
		return appendf(dst, dstlen, error, error_len, "\"%s\"", escaped);
	}
	if (lua_istable(L, value))
		return append_lua_string_list_literal(L, value, dst, dstlen, error,
		    error_len);
	return appendf(dst, dstlen, error, error_len, "nil");
}

static int
collect_table_string_keys(lua_State *L, int table, struct qstar_string_list *keys,
    char *error, size_t error_len)
{
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return 0;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (key && qstar_string_list_push(keys, key) < 0) {
			lua_pop(L, 2);
			return init_error(error, error_len, "qstar: out of memory", "");
		}
		lua_pop(L, 1);
	}
	qsort(keys->items, keys->len, sizeof(keys->items[0]), string_item_compare);
	return 0;
}

static int
append_provider_tool_entry(struct init_context *ctx, struct init_language *language,
    lua_State *L, int scaffold, char *error, size_t error_len)
{
	struct qstar_string_list roles;
	size_t i;

	memset(&roles, 0, sizeof(roles));
	if (appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error, error_len,
	    "    ") < 0 ||
	    append_lua_key(ctx->tool_entries, sizeof(ctx->tool_entries), error,
	    error_len, language->namespace) < 0 ||
	    appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error, error_len,
	    " = %s.tools {\n", language->local_name) < 0)
		return -1;
	if (scaffold < 0)
		scaffold = lua_gettop(L) + scaffold + 1;
	if (lua_istable(L, scaffold))
		lua_getfield(L, scaffold, "tools");
	else
		lua_pushnil(L);
	if (lua_istable(L, -1) && collect_table_string_keys(L, -1, &roles, error,
	    error_len) < 0) {
		qstar_string_list_free(&roles);
		lua_pop(L, 1);
		return -1;
	}
	if (roles.len == 0) {
		if (appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error,
		    error_len,
		    "      compiler = qstar.cli {\"%s\"},\n",
		    default_compiler_for_language(language->id)) < 0) {
			qstar_string_list_free(&roles);
			lua_pop(L, 1);
			return -1;
		}
	} else {
		for (i = 0; i < roles.len; i++) {
			lua_getfield(L, -1, roles.items[i]);
			if (appendf(ctx->tool_entries, sizeof(ctx->tool_entries),
			    error, error_len, "      %s = qstar.cli ",
			    roles.items[i]) < 0 ||
			    append_lua_string_list_literal(L, -1, ctx->tool_entries,
			    sizeof(ctx->tool_entries), error, error_len) < 0 ||
			    appendf(ctx->tool_entries, sizeof(ctx->tool_entries),
			    error, error_len, ",\n") < 0) {
				lua_pop(L, 2);
				qstar_string_list_free(&roles);
				return -1;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	qstar_string_list_free(&roles);
	return appendf(ctx->tool_entries, sizeof(ctx->tool_entries), error,
	    error_len, "    },\n");
}

static int
append_provider_config_options(struct init_context *ctx,
    struct init_language *language, lua_State *L, int scaffold, char *error,
    size_t error_len)
{
	struct qstar_string_list keys;
	size_t i;

	memset(&keys, 0, sizeof(keys));
	if (scaffold < 0)
		scaffold = lua_gettop(L) + scaffold + 1;
	if (lua_istable(L, scaffold))
		lua_getfield(L, scaffold, "options");
	else
		lua_pushnil(L);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (collect_table_string_keys(L, -1, &keys, error, error_len) < 0) {
		qstar_string_list_free(&keys);
		lua_pop(L, 1);
		return -1;
	}
	if (keys.len == 0) {
		qstar_string_list_free(&keys);
		lua_pop(L, 1);
		return 0;
	}
	if (appendf(ctx->config_lang_entries, sizeof(ctx->config_lang_entries),
	    error, error_len, "    ") < 0 ||
	    append_lua_key(ctx->config_lang_entries,
	    sizeof(ctx->config_lang_entries), error, error_len,
	    language->namespace) < 0 ||
	    appendf(ctx->config_lang_entries, sizeof(ctx->config_lang_entries),
	    error, error_len, " = %s.options {\n", language->local_name) < 0) {
		qstar_string_list_free(&keys);
		lua_pop(L, 1);
		return -1;
	}
	for (i = 0; i < keys.len; i++) {
		lua_getfield(L, -1, keys.items[i]);
		if (appendf(ctx->config_lang_entries, sizeof(ctx->config_lang_entries),
		    error, error_len, "      %s = ", keys.items[i]) < 0 ||
		    append_scaffold_value(L, -1, ctx->config_lang_entries,
		    sizeof(ctx->config_lang_entries), error, error_len) < 0 ||
		    appendf(ctx->config_lang_entries, sizeof(ctx->config_lang_entries),
		    error, error_len, ",\n") < 0) {
			lua_pop(L, 2);
			qstar_string_list_free(&keys);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	qstar_string_list_free(&keys);
	return appendf(ctx->config_lang_entries, sizeof(ctx->config_lang_entries),
	    error, error_len, "    },\n");
}

static int
build_language_snippets(struct init_context *ctx, char *error, size_t error_len)
{
	size_t i;
	char id_lua[QSTAR_INIT_LANGUAGE_MAX * 2];
	lua_State *L;
	int need_c_tool;

	ctx->activation_block[0] = '\0';
	ctx->tool_entries[0] = '\0';
	ctx->config_lang_entries[0] = '\0';
	need_c_tool = language_selected(ctx, "c") ||
	    (ctx->languages[0].external && !ctx->languages[0].has_shape);
	if (need_c_tool && append_builtin_tool_entry(ctx, "c", error, error_len) < 0)
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
		L = load_provider_manifest(&ctx->languages[i], error, error_len);
		if (!L)
			return -1;
		lua_getfield(L, -1, "scaffold");
		if (append_provider_tool_entry(ctx, &ctx->languages[i], L, -1,
		    error, error_len) < 0 ||
		    append_provider_config_options(ctx, &ctx->languages[i], L, -1,
		    error, error_len) < 0) {
			lua_close(L);
			return -1;
		}
		lua_close(L);
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
	if (ctx->config_lang_entries[0]) {
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
		    "  lang = {\n"
		    "%s"
		    "  },\n"
		    "}\n"
		    "%s",
		    ctx->activation_block, gap, ctx->project_lua,
		    ctx->tool_entries, ctx->config_lang_entries,
		    target_body ? target_body : "");
	} else {
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
		    ctx->activation_block, gap, ctx->project_lua,
		    ctx->tool_entries, target_body ? target_body : "");
	}
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

static const char *
scaffold_target_function(const char *kind)
{
	if (strcmp(kind, "executable") == 0)
		return "qstar.executable";
	if (strcmp(kind, "staticlib") == 0)
		return "qstar.staticlib";
	if (strcmp(kind, "sharedlib") == 0)
		return "qstar.sharedlib";
	if (strcmp(kind, "test") == 0)
		return "qstar.test";
	if (strcmp(kind, "run_target") == 0)
		return "qstar.run_target";
	return "qstar.group";
}

static int
append_scaffold_option_table(lua_State *L, int table, char *dst, size_t dstlen,
    char *error, size_t error_len, const char *indent)
{
	struct qstar_string_list keys;
	size_t i;

	memset(&keys, 0, sizeof(keys));
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return appendf(dst, dstlen, error, error_len, "{}");
	if (collect_table_string_keys(L, table, &keys, error, error_len) < 0) {
		qstar_string_list_free(&keys);
		return -1;
	}
	if (appendf(dst, dstlen, error, error_len, "{\n") < 0) {
		qstar_string_list_free(&keys);
		return -1;
	}
	for (i = 0; i < keys.len; i++) {
		lua_getfield(L, table, keys.items[i]);
		if (appendf(dst, dstlen, error, error_len, "%s  %s = ", indent,
		    keys.items[i]) < 0 ||
		    append_scaffold_value(L, -1, dst, dstlen, error, error_len) < 0 ||
		    appendf(dst, dstlen, error, error_len, ",\n") < 0) {
			lua_pop(L, 1);
			qstar_string_list_free(&keys);
			return -1;
		}
		lua_pop(L, 1);
	}
	qstar_string_list_free(&keys);
	return appendf(dst, dstlen, error, error_len, "%s}", indent);
}

static int
append_scaffold_deps(lua_State *L, int table, const struct init_context *ctx,
    const struct init_language *language, const char *target_name, char *dst,
    size_t dstlen, char *error, size_t error_len)
{
	size_t i, n;
	const char *dep;
	char expanded[QSTAR_PATH_MAX], escaped[QSTAR_PATH_MAX * 2];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || lua_rawlen(L, table) == 0)
		return 0;
	if (appendf(dst, dstlen, error, error_len, "  deps = {\n") < 0)
		return -1;
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		dep = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (expand_template(ctx, language, dep, target_name, expanded,
		    sizeof(expanded), error, error_len) < 0 ||
		    escape_lua_string(expanded, escaped, sizeof(escaped)) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		if (appendf(dst, dstlen, error, error_len, "    \"%s\",\n",
		    escaped) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return appendf(dst, dstlen, error, error_len, "  },\n");
}

static int
append_scaffold_sources(lua_State *L, int table, const struct init_context *ctx,
    const struct init_language *language, const char *target_name, char *dst,
    size_t dstlen, char *error, size_t error_len)
{
	size_t i, n;
	const char *path, *helper;
	char expanded[QSTAR_PATH_MAX], escaped[QSTAR_PATH_MAX * 2];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || lua_rawlen(L, table) == 0)
		return 0;
	if (appendf(dst, dstlen, error, error_len, "  sources = {\n") < 0)
		return -1;
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			path = lua_tostring(L, -1);
			if (expand_scaffold_path(ctx, language, path, target_name, 1,
			    expanded, sizeof(expanded), error, error_len) < 0 ||
			    escape_lua_string(expanded, escaped, sizeof(escaped)) < 0) {
				lua_pop(L, 1);
				return -1;
			}
			if (appendf(dst, dstlen, error, error_len,
			    "    \"%s\",\n", escaped) < 0) {
				lua_pop(L, 1);
				return -1;
			}
		} else if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "helper");
			helper = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
			lua_pop(L, 1);
			lua_getfield(L, -1, "path");
			path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
			if (expand_scaffold_path(ctx, language, path, target_name, 1,
			    expanded, sizeof(expanded), error, error_len) < 0 ||
			    escape_lua_string(expanded, escaped, sizeof(escaped)) < 0) {
				lua_pop(L, 2);
				return -1;
			}
			lua_pop(L, 1);
			if (!helper || !lua_identifier(helper)) {
				lua_pop(L, 1);
				return init_error(error, error_len,
				    "qstar: init scaffold helper is invalid '%s'",
				    helper ? helper : "<missing>");
			}
			if (appendf(dst, dstlen, error, error_len,
			    "    %s.%s(\"%s\"", language->local_name, helper,
			    escaped) < 0) {
				lua_pop(L, 1);
				return -1;
			}
			lua_getfield(L, -1, "options");
			if (lua_istable(L, -1)) {
				if (appendf(dst, dstlen, error, error_len, ", ") < 0 ||
				    append_scaffold_option_table(L, -1, dst,
				    dstlen, error, error_len, "    ") < 0) {
					lua_pop(L, 2);
					return -1;
				}
			}
			lua_pop(L, 1);
			if (appendf(dst, dstlen, error, error_len, "),\n") < 0) {
				lua_pop(L, 1);
				return -1;
			}
		}
		lua_pop(L, 1);
	}
	return appendf(dst, dstlen, error, error_len, "  },\n");
}

static int
append_scaffold_lang(lua_State *L, int table, const struct init_language *language,
    char *dst, size_t dstlen, char *error, size_t error_len)
{
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return 0;
	lua_getfield(L, table, language->namespace);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (appendf(dst, dstlen, error, error_len, "  lang = {\n    ") < 0 ||
	    append_lua_key(dst, dstlen, error, error_len, language->namespace) < 0 ||
	    appendf(dst, dstlen, error, error_len, " = %s.options ",
	    language->local_name) < 0 ||
	    append_scaffold_option_table(L, -1, dst, dstlen, error, error_len,
	    "    ") < 0 ||
	    appendf(dst, dstlen, error, error_len, ",\n  },\n") < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
append_scaffold_target(lua_State *L, int target, const struct init_context *ctx,
    const struct init_language *language, char *dst, size_t dstlen, char *error,
    size_t error_len)
{
	const char *kind, *name;
	char expanded_name[128], escaped_name[256];

	if (target < 0)
		target = lua_gettop(L) + target + 1;
	lua_getfield(L, target, "kind");
	kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "executable";
	lua_pop(L, 1);
	lua_getfield(L, target, "name");
	name = lua_isstring(L, -1) ? lua_tostring(L, -1) :
	    init_default_target_name(ctx);
	if (expand_template(ctx, language, name, name, expanded_name,
	    sizeof(expanded_name), error, error_len) < 0 ||
	    escape_lua_string(expanded_name, escaped_name, sizeof(escaped_name)) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	if (appendf(dst, dstlen, error, error_len, "\n%s \"%s\" {\n",
	    scaffold_target_function(kind), escaped_name) < 0)
		return -1;
	if (strcmp(kind, "group") != 0 && strcmp(kind, "run_target") != 0 &&
	    appendf(dst, dstlen, error, error_len,
	    "  configs = {\"//:debug\"},\n") < 0)
		return -1;
	lua_getfield(L, target, "sources");
	if (append_scaffold_sources(L, -1, ctx, language, expanded_name, dst,
	    dstlen, error, error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, target, "deps");
	if (append_scaffold_deps(L, -1, ctx, language, expanded_name, dst, dstlen,
	    error, error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, target, "lang");
	if (append_scaffold_lang(L, -1, language, dst, dstlen, error,
	    error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return appendf(dst, dstlen, error, error_len, "}\n");
}

static int
append_scaffold_group(lua_State *L, int group, const struct init_context *ctx,
    const struct init_language *language, char *dst, size_t dstlen, char *error,
    size_t error_len)
{
	const char *name;
	char expanded_name[128], escaped_name[256];

	if (group < 0)
		group = lua_gettop(L) + group + 1;
	lua_getfield(L, group, "name");
	name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "all";
	if (expand_template(ctx, language, name, name, expanded_name,
	    sizeof(expanded_name), error, error_len) < 0 ||
	    escape_lua_string(expanded_name, escaped_name, sizeof(escaped_name)) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	if (appendf(dst, dstlen, error, error_len, "\nqstar.group \"%s\" {\n",
	    escaped_name) < 0)
		return -1;
	lua_getfield(L, group, "deps");
	if (append_scaffold_deps(L, -1, ctx, language, expanded_name, dst, dstlen,
	    error, error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return appendf(dst, dstlen, error, error_len, "}\n");
}

static int
append_scaffold_targets(lua_State *L, int shape, const struct init_context *ctx,
    const struct init_language *language, char *dst, size_t dstlen, char *error,
    size_t error_len)
{
	size_t i, n;

	if (shape < 0)
		shape = lua_gettop(L) + shape + 1;
	lua_getfield(L, shape, "targets");
	if (lua_istable(L, -1)) {
		n = lua_rawlen(L, -1);
		for (i = 1; i <= n; i++) {
			lua_rawgeti(L, -1, (lua_Integer)i);
			if (append_scaffold_target(L, -1, ctx, language, dst,
			    dstlen, error, error_len) < 0) {
				lua_pop(L, 2);
				return -1;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	lua_getfield(L, shape, "target");
	if (lua_istable(L, -1) && append_scaffold_target(L, -1, ctx, language,
	    dst, dstlen, error, error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, shape, "group");
	if (lua_istable(L, -1) && append_scaffold_group(L, -1, ctx, language,
	    dst, dstlen, error, error_len) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
ensure_scaffold_directories(lua_State *L, int shape, const struct init_context *ctx,
    const struct init_language *language, FILE *out, char *error,
    size_t error_len)
{
	size_t i, n;
	const char *path;
	char expanded[QSTAR_PATH_MAX];

	if (shape < 0)
		shape = lua_gettop(L) + shape + 1;
	lua_getfield(L, shape, "directories");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (expand_scaffold_path(ctx, language, path,
		    init_default_target_name(ctx), 0, expanded, sizeof(expanded),
		    error, error_len) < 0 ||
		    ensure_dir(ctx, expanded, out, error, error_len) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
write_scaffold_files(lua_State *L, int shape, const struct init_context *ctx,
    const struct init_language *language, FILE *out, char *error,
    size_t error_len)
{
	size_t i, n;
	const char *path, *body;
	char expanded_path[QSTAR_PATH_MAX], expanded_body[16384];
	int executable;

	if (shape < 0)
		shape = lua_gettop(L) + shape + 1;
	lua_getfield(L, shape, "files");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		lua_getfield(L, -1, "path");
		path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (expand_scaffold_path(ctx, language, path,
		    init_default_target_name(ctx), 1, expanded_path,
		    sizeof(expanded_path), error, error_len) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "body");
		body = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (expand_template(ctx, language, body, init_default_target_name(ctx),
		    expanded_body, sizeof(expanded_body), error, error_len) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "executable");
		executable = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : 0;
		lua_pop(L, 1);
		if (write_text_file(ctx, expanded_path, expanded_body, executable, out,
		    error, error_len) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
fragment_import_line(const char *fragment_path, char *dst, size_t dstlen,
    char *error, size_t error_len)
{
	char dir[QSTAR_PATH_MAX], escaped[QSTAR_PATH_MAX * 2];
	char *slash;

	if (strlen(fragment_path) >= sizeof(dir))
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    fragment_path);
	strcpy(dir, fragment_path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (escape_lua_string(dir, escaped, sizeof(escaped)) < 0)
			return init_error(error, error_len,
			    "qstar: init path too long '%s'", dir);
		return snprintf(dst, dstlen, "qstar.subdir(\"%s\")\n", escaped) >=
		    (int)dstlen ? -1 : 0;
	}
	if (escape_lua_string(fragment_path, escaped, sizeof(escaped)) < 0)
		return init_error(error, error_len, "qstar: init path too long '%s'",
		    fragment_path);
	return snprintf(dst, dstlen, "qstar.import_file(\"%s\")\n", escaped) >=
	    (int)dstlen ? -1 : 0;
}

static int
write_scaffold_fragments(lua_State *L, int shape, const struct init_context *ctx,
    const struct init_language *language, FILE *out, char *root_body,
    size_t root_body_len, char *error, size_t error_len)
{
	size_t i, n;
	const char *path;
	char fragment_path[QSTAR_PATH_MAX], fragment_body[32768], import_line[QSTAR_PATH_MAX * 2];

	if (shape < 0)
		shape = lua_gettop(L) + shape + 1;
	lua_getfield(L, shape, "fragments");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		lua_getfield(L, -1, "path");
		path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		if (expand_scaffold_path(ctx, language, path,
		    init_default_target_name(ctx), 1, fragment_path,
		    sizeof(fragment_path), error, error_len) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		if (fragment_import_line(fragment_path, import_line,
		    sizeof(import_line), error, error_len) < 0 ||
		    appendf(root_body, root_body_len, error, error_len, "%s",
		    import_line) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		if (ensure_scaffold_directories(L, -1, ctx, language, out, error,
		    error_len) < 0 ||
		    write_scaffold_files(L, -1, ctx, language, out, error,
		    error_len) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		fragment_body[0] = '\0';
		if (appendf(fragment_body, sizeof(fragment_body), error, error_len,
		    "local %s = qstar.use_language(\"%s\")\n",
		    language->local_name, language->id) < 0 ||
		    append_scaffold_targets(L, -1, ctx, language, fragment_body,
		    sizeof(fragment_body), error, error_len) < 0 ||
		    write_text_file(ctx, fragment_path, fragment_body, 0, out, error,
		    error_len) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
write_provider_shape(const struct init_context *ctx,
    const struct init_language *language, FILE *out, char *error,
    size_t error_len)
{
	lua_State *L;
	char root_body[65536], qstar_body[65536];

	L = load_provider_manifest(language, error, error_len);
	if (!L)
		return -1;
	lua_getfield(L, -1, "scaffold");
	lua_getfield(L, -1, "shapes");
	lua_getfield(L, -1, ctx->shape);
	if (!lua_istable(L, -1)) {
		lua_close(L);
		return init_error(error, error_len,
		    "qstar: init scaffold for language '%s' shape '%s' disappeared",
		    language->id);
	}
	fprintf(out, "scaffold %s %s\n", language->namespace, ctx->shape);
	root_body[0] = '\0';
	if (ensure_scaffold_directories(L, -1, ctx, language, out, error,
	    error_len) < 0 ||
	    write_scaffold_fragments(L, -1, ctx, language, out, root_body,
	    sizeof(root_body), error, error_len) < 0 ||
	    append_scaffold_targets(L, -1, ctx, language, root_body,
	    sizeof(root_body), error, error_len) < 0 ||
	    format_qstar_lua(ctx, qstar_body, sizeof(qstar_body), error, error_len,
	    root_body) < 0 ||
	    write_text_file(ctx, "qstar.lua", qstar_body, 0, out, error,
	    error_len) < 0 ||
	    write_gitignore(ctx, out, error, error_len) < 0 ||
	    write_scaffold_files(L, -1, ctx, language, out, error, error_len) < 0) {
		lua_close(L);
		return -1;
	}
	lua_close(L);
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
	if (load_language_scaffold_metadata(ctx, error, error_len) < 0)
		return -1;
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
	const struct init_language *primary;
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
	primary = &ctx.languages[0];
	if (primary->external && !primary->has_shape)
		fprintf(out,
		    "warning language '%s' has no init scaffold for shape '%s'; using builtin c scaffold\n",
		    ctx.primary_language, ctx.shape);
	if (vendor_language_providers(&ctx, out, error, error_len) < 0)
		return -1;
	if (primary->external && primary->has_shape)
		rc = write_provider_shape(&ctx, primary, out, error, error_len);
	else if (strcmp(ctx.shape, "app") == 0)
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
