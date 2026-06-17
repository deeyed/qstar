#include "internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
#include <mach-o/dyld.h>
#endif

struct qstar_lua_context {
	struct qstar_graph *graph;
	char root_dir[QSTAR_PATH_MAX];
	char current_dir[QSTAR_PATH_MAX];
	struct qstar_string_list import_stack;
	struct qstar_string_list provider_stack;
	int module_depth;
};

struct qstar_provider_lowering_ctx {
	struct qstar_graph *graph;
	const struct qstar_target *target;
	const struct qstar_language_provider *provider;
	const struct qstar_language_unit_schema *unit;
	const char *final_kind;
	const char *source;
	const struct qstar_string_list *sources;
	const char *object;
	const char *artifact;
	const char *depfile;
	char cache_base[QSTAR_PATH_MAX];
	int source_options_ref;
};

#define QSTAR_STATUS_DESCRIPTION_MAX 240

static int lower_provider_source_unit(lua_State *L, int source_token,
    struct qstar_target *target, struct qstar_graph *graph, size_t source_index,
    const char *path, const char *provider, const char *unit_name,
    const struct qstar_language_unit_schema *unit,
    struct qstar_provider_action_template *action);
static int lower_provider_final_action(lua_State *L, struct qstar_target *target,
    struct qstar_graph *graph);
static int qstar_lua_abs_index(lua_State *L, int idx);
static int readonly_table_assignment_forbidden(lua_State *L);
static int string_in_set(const char *s, const char *const *items);
static int valid_tool_role_name(const char *s);

static int
fs_path_is_absolute(const char *path)
{
	if (!path || !*path)
		return 0;
	if (path[0] == '/')
		return 1;
	return isalpha((unsigned char)path[0]) && path[1] == ':' &&
	    (path[2] == '/' || path[2] == '\\');
}

static int
path_join_root_or_absolute(const char *root, const char *path, char *dst,
    size_t dstlen)
{
	if (fs_path_is_absolute(path)) {
		if (snprintf(dst, dstlen, "%s", path) >= (int)dstlen)
			return -1;
		return 0;
	}
	return qstar_path_join(root, path, dst, dstlen);
}

static const char *
qstar_host_os(void)
{
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
	return "macos";
#elif defined(__linux__)
	return "linux";
#elif defined(__FreeBSD__)
	return "freebsd";
#else
	return "unknown";
#endif
}

static const char *
qstar_host_arch(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
	return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#elif defined(__riscv) && __riscv_xlen == 64
	return "riscv64";
#elif defined(__arm__) || defined(_M_ARM)
	return "arm";
#else
	return "unknown";
#endif
}

/** 현재 Lua 호출자의 source 위치를 QStar origin으로 가져온다. */
static void
current_origin(lua_State *L, char *file, size_t filelen, int *line)
{
	lua_Debug ar;
	const char *src;

	if (filelen)
		file[0] = '\0';
	if (line)
		*line = 0;
	if (!lua_getstack(L, 1, &ar) || !lua_getinfo(L, "Sl", &ar))
		return;
	src = ar.source ? ar.source : ar.short_src;
	if (src && src[0] == '@')
		src++;
	if (src && filelen)
		snprintf(file, filelen, "%s", src);
	if (line)
		*line = ar.currentline;
}

static struct qstar_lua_context *
get_context(lua_State *L)
{
	struct qstar_lua_context *ctx;

	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.context");
	ctx = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ctx;
}

/** 현재 평가 중인 .qsm module에서 graph 선언 API가 호출되지 않게 막는다. */
static int
reject_graph_declaration_in_module(lua_State *L, const char *api)
{
	struct qstar_lua_context *ctx;

	ctx = get_context(L);
	if (ctx && ctx->module_depth > 0)
		return luaL_error(L,
		    "qstar: %s is forbidden inside .qsm module; modules must return a helper table and cannot declare project/config/target/stage/import_file",
		    api);
	return 0;
}

/** path가 suffix로 끝나는지 검사한다. */
static int
path_has_suffix(const char *path, const char *suffix)
{
	size_t npath, nsuffix;

	npath = strlen(path ? path : "");
	nsuffix = strlen(suffix ? suffix : "");
	return npath >= nsuffix && strcmp(path + npath - nsuffix, suffix) == 0;
}

/** 문자열 list에 path가 이미 들어 있는지 검사한다. */
static int
string_list_contains(const struct qstar_string_list *list, const char *path)
{
	size_t i;

	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], path) == 0)
			return 1;
	}
	return 0;
}

/** stack처럼 쓰는 문자열 list의 마지막 항목을 제거한다. */
static void
string_list_pop(struct qstar_string_list *list)
{
	if (!list || list->len == 0)
		return;
	list->len--;
	free(list->items[list->len]);
	list->items[list->len] = NULL;
}

/** import stack과 새 입력을 사람이 읽을 수 있는 circular chain으로 만든다. */
static void
format_import_chain(const struct qstar_lua_context *ctx, const char *next,
    char *dst, size_t dstlen)
{
	size_t i, used, n;

	if (!dstlen)
		return;
	used = 0;
	dst[0] = '\0';
	if (!ctx || ctx->import_stack.len == 0) {
		snprintf(dst, dstlen, "%s", next ? next : "<unknown>");
		return;
	}
	for (i = 0; i < ctx->import_stack.len; i++) {
		n = (size_t)snprintf(dst + used, dstlen - used, "%s%s",
		    i ? " -> " : "", ctx->import_stack.items[i]);
		if (n >= dstlen - used)
			goto truncated;
		used += n;
	}
	n = (size_t)snprintf(dst + used, dstlen - used, " -> %s",
	    next ? next : "<unknown>");
	if (n >= dstlen - used)
		goto truncated;
	return;

truncated:
	if (dstlen > 4)
		snprintf(dst + dstlen - 4, 4, "...");
}

static const char *
check_string_field(lua_State *L, int idx, const char *field)
{
	const char *s;

	lua_getfield(L, idx, field);
	s = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return s;
}

static int
check_int_field(lua_State *L, int idx, const char *field, int fallback)
{
	int value;

	lua_getfield(L, idx, field);
	value = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : fallback;
	lua_pop(L, 1);
	return value;
}

static int
check_bool_field(lua_State *L, int idx, const char *field, int fallback)
{
	int value;

	lua_getfield(L, idx, field);
	value = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : fallback;
	lua_pop(L, 1);
	return value;
}

static const char *
qstar_table_kind(lua_State *L, int idx)
{
	const char *kind;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (!lua_istable(L, idx))
		return NULL;
	lua_getfield(L, idx, "__qstar_kind");
	kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return kind;
}

/** status description 문자열이 progress line에 안전한 한 줄인지 검증한다. */
static int
validate_status_description(struct qstar_graph *graph, const char *text, size_t len)
{
	size_t i;

	if (!text || len == 0)
		return qstar_set_error(graph, "qstar: qstar.status description must not be empty");
	if (len > QSTAR_STATUS_DESCRIPTION_MAX)
		return qstar_set_error(graph,
		    "qstar: qstar.status description must be <= %d bytes",
		    QSTAR_STATUS_DESCRIPTION_MAX);
	for (i = 0; i < len; i++) {
		if (text[i] == '\n' || text[i] == '\r')
			return qstar_set_error(graph,
			    "qstar: qstar.status description must be one line");
	}
	return 0;
}

/** qstar.status("...") 객체에서 validated description 문자열을 읽는다. */
static int
read_status_description_field(lua_State *L, int table_index, struct qstar_graph *graph,
    char **dst)
{
	const char *kind, *text;
	size_t len;
	char *copy;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	lua_getfield(L, table_index, "description");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: field 'description' must be qstar.status(\"...\")");
	}
	kind = qstar_table_kind(L, -1);
	if (!kind || strcmp(kind, "status") != 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: field 'description' must be qstar.status(\"...\")");
	}
	lua_getfield(L, -1, "text");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: field 'description' must be qstar.status(\"...\")");
	}
	text = lua_tolstring(L, -1, &len);
	if (validate_status_description(graph, text, len) < 0) {
		lua_pop(L, 2);
		return -1;
	}
	copy = qstar_strdup(text);
	if (!copy) {
		lua_pop(L, 2);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	free(*dst);
	*dst = copy;
	lua_pop(L, 2);
	return 0;
}

/** qstar.output(path, metadata)가 넘긴 output path table에서 path를 읽는다. */
static const char *
output_path_table_path(lua_State *L, int idx)
{
	const char *path;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (!lua_istable(L, idx))
		return NULL;
	if (!qstar_table_kind(L, idx) ||
	    strcmp(qstar_table_kind(L, idx), "output_path") != 0)
		return NULL;
	lua_getfield(L, idx, "path");
	path = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return path;
}

static int
source_unit_suffix_matches(const struct qstar_language_unit_schema *unit,
    const char *path)
{
	size_t i;

	if (!unit || !path)
		return 0;
	for (i = 0; i < unit->suffixes.len; i++) {
		if (path_has_suffix(path, unit->suffixes.items[i]))
			return 1;
	}
	return 0;
}

static int
read_source_unit_table(lua_State *L, int idx, struct qstar_graph *graph,
    const char *field, const char **path, const char **provider,
    const char **unit_name, const struct qstar_language_unit_schema **unit)
{
	const struct qstar_language_provider *lang;
	const char *kind;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	*path = NULL;
	*provider = NULL;
	*unit_name = NULL;
	*unit = NULL;
	kind = qstar_table_kind(L, idx);
	if (!kind || strcmp(kind, "source_unit") != 0)
		return qstar_set_error(graph, "qstar: field '%s' contains non-string item",
		    field);
	lua_getfield(L, idx, "path");
	*path = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	lua_getfield(L, idx, "language");
	*provider = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	lua_getfield(L, idx, "unit");
	*unit_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	if (!*path || !*provider || !*unit_name)
		return qstar_set_error(graph,
		    "qstar: field '%s' contains malformed qstar.source token", field);
	lang = qstar_graph_find_language_provider(graph, *provider);
	if (!lang)
		return qstar_set_error(graph,
		    "qstar: source token uses inactive language provider '%s'",
		    *provider);
	*unit = qstar_language_provider_find_unit(lang, *unit_name);
	if (!*unit)
		return qstar_set_error(graph,
		    "qstar: source token uses unknown unit '%s.%s'",
		    *provider, *unit_name);
	if (!source_unit_suffix_matches(*unit, *path))
		return qstar_set_error(graph,
		    "qstar: source '%s' does not match suffixes for unit '%s.%s'",
		    *path, *provider, *unit_name);
	return 0;
}

static int
find_raw_provider_source_unit(struct qstar_graph *graph, const char *path,
    const char **provider_name, const char **unit_name,
    const struct qstar_language_unit_schema **unit)
{
	const struct qstar_language_provider *provider, *matched_provider;
	const struct qstar_language_unit_schema *candidate, *matched_unit;
	const struct qstar_source_info *builtin;
	size_t i, j;

	*provider_name = NULL;
	*unit_name = NULL;
	*unit = NULL;
	matched_provider = NULL;
	matched_unit = NULL;
	builtin = qstar_source_kind_lookup_path(path);
	for (i = 0; i < graph->language_provider_len; i++) {
		provider = &graph->language_providers[i];
		for (j = 0; j < provider->unit_len; j++) {
			candidate = &provider->units[j];
			if (!candidate->emits || strcmp(candidate->emits, "object") != 0 ||
			    !source_unit_suffix_matches(candidate, path))
				continue;
			if (matched_provider) {
				return qstar_set_error(graph,
				    "qstar: source '%s' matches multiple provider source units (%s.%s and %s.%s); use an explicit provider helper such as %s.%s(\"%s\")",
				    path,
				    matched_provider->namespace ? matched_provider->namespace :
				    "<unknown>",
				    matched_unit->name ? matched_unit->name : "<unknown>",
				    provider->namespace ? provider->namespace : "<unknown>",
				    candidate->name ? candidate->name : "<unknown>",
				    provider->namespace ? provider->namespace : "<provider>",
				    candidate->name ? candidate->name : "object", path);
			}
			matched_provider = provider;
			matched_unit = candidate;
		}
	}
	if (!matched_provider)
		return 0;
	if (builtin) {
		return qstar_set_error(graph,
		    "qstar: source '%s' matches both a built-in source suffix and provider source unit %s.%s; use an explicit provider helper such as %s.%s(\"%s\")",
		    path,
		    matched_provider->namespace ? matched_provider->namespace : "<unknown>",
		    matched_unit->name ? matched_unit->name : "<unknown>",
		    matched_provider->namespace ? matched_provider->namespace : "<provider>",
		    matched_unit->name ? matched_unit->name : "object", path);
	}
	*provider_name = matched_provider->namespace;
	*unit_name = matched_unit->name;
	*unit = matched_unit;
	return 1;
}

/** QStar Lua helper가 넘긴 placeholder table을 argv token 문자열로 변환한다. */
static int
format_placeholder_token(lua_State *L, int idx, char *dst, size_t dstlen)
{
	const char *kind, *label;
	lua_Integer index;

	kind = qstar_table_kind(L, idx);
	if (!kind)
		return -1;
	if (strcmp(kind, "input") == 0 || strcmp(kind, "output") == 0) {
		lua_getfield(L, idx, "index");
		if (!lua_isinteger(L, -1)) {
			lua_pop(L, 1);
			return -1;
		}
		index = lua_tointeger(L, -1);
		lua_pop(L, 1);
		return snprintf(dst, dstlen, "<qstar-%s:%lld>", kind,
		    (long long)index) < (int)dstlen ? 0 : -1;
	}
	if (strcmp(kind, "target_file") == 0) {
		const char *artifact;

		lua_getfield(L, idx, "label");
		label = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!label) {
			lua_pop(L, 1);
			return -1;
		}
		lua_getfield(L, idx, "artifact");
		artifact = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (snprintf(dst, dstlen, artifact && *artifact ?
		    "<qstar-target-file:%s#%s>" : "<qstar-target-file:%s>",
		    label, artifact && *artifact ? artifact : "") >= (int)dstlen) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 2);
		return 0;
	}
	if (strcmp(kind, "stage_dir") == 0) {
		lua_getfield(L, idx, "label");
		label = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!label) {
			lua_pop(L, 1);
			return -1;
		}
		if (snprintf(dst, dstlen, "<qstar-stage-dir:%s>", label) >=
		    (int)dstlen) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
		return 0;
	}
	return -1;
}

/** qstar.cli table을 내부 argv token list로 읽는다. */
static int
read_cli_command(lua_State *L, int idx, struct qstar_string_list *list,
    struct qstar_graph *graph, const char *field)
{
	size_t i, n;
	const char *kind, *s;
	char token[QSTAR_PATH_MAX];

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (lua_isnil(L, idx))
		return 0;
	if (!lua_istable(L, idx))
		return qstar_set_error(graph, "qstar: field '%s' must be qstar.cli { ... }", field);
	kind = qstar_table_kind(L, idx);
	if (!kind || strcmp(kind, "cli") != 0)
		return qstar_set_error(graph, "qstar: field '%s' must be qstar.cli { ... }", field);
	n = lua_rawlen(L, idx);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		if (lua_type(L, -1) == LUA_TSTRING) {
			s = lua_tostring(L, -1);
		} else if (lua_istable(L, -1) &&
		    format_placeholder_token(L, -1, token, sizeof(token)) == 0) {
			s = token;
		} else {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: field '%s' contains unsupported argv item", field);
		}
		if (qstar_string_list_push(list, s) < 0) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
push_provider_source_item(lua_State *L, int source_token, struct qstar_target *target,
    struct qstar_graph *graph, const char *path, const char *provider,
    const char *unit_name, const struct qstar_language_unit_schema *unit)
{
	struct qstar_provider_action_template action;
	size_t source_index;
	int rc;

	memset(&action, 0, sizeof(action));
	source_index = target->sources.len;
	if (qstar_string_list_push(&target->sources, path) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	if (lower_provider_source_unit(L, source_token, target, graph, source_index, path,
	    provider, unit_name, unit, &action) < 0) {
		qstar_string_list_free(&action.argv);
		qstar_string_list_free(&action.env);
		qstar_string_list_free(&action.inputs);
		qstar_string_list_free(&action.outputs);
		free(action.depfile);
		return -1;
	}
	rc = qstar_target_add_provider_source_unit(graph, target, source_index, path,
	    provider, unit_name, unit->emits, unit->lower, &action);
	qstar_string_list_free(&action.argv);
	qstar_string_list_free(&action.env);
	qstar_string_list_free(&action.inputs);
	qstar_string_list_free(&action.outputs);
	free(action.depfile);
	if (rc < 0)
		return -1;
	return 0;
}

static int
push_source_unit_item(lua_State *L, int idx, struct qstar_target *target,
    struct qstar_graph *graph, const char *field)
{
	const struct qstar_language_unit_schema *unit;
	const char *path, *provider, *unit_name;

	if (read_source_unit_table(L, idx, graph, field, &path, &provider,
	    &unit_name, &unit) < 0)
		return -1;
	return push_provider_source_item(L, idx, target, graph, path, provider,
	    unit_name, unit);
}

static int
push_source_string_item(lua_State *L, const char *path, struct qstar_target *target,
    struct qstar_graph *graph)
{
	const struct qstar_language_unit_schema *unit;
	const char *provider, *unit_name;
	int rc;

	rc = find_raw_provider_source_unit(graph, path, &provider, &unit_name, &unit);
	if (rc < 0)
		return -1;
	if (rc > 0)
		return push_provider_source_item(L, 0, target, graph, path, provider,
		    unit_name, unit);
	return qstar_string_list_push(&target->sources, path) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

static int
read_sources_field(lua_State *L, int table, const char *field,
    struct qstar_target *target, struct qstar_graph *graph)
{
	size_t n, i;
	const char *s, *kind;

	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s' must be a list", field);
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			s = lua_tostring(L, -1);
			if (push_source_string_item(L, s, target, graph) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		} else if (lua_istable(L, -1)) {
			kind = qstar_table_kind(L, -1);
			if (kind && strcmp(kind, "output_path") == 0) {
				s = output_path_table_path(L, -1);
				if (!s || qstar_string_list_push(&target->sources, s) < 0) {
					lua_pop(L, 2);
					return qstar_set_error(graph, "qstar: out of memory");
				}
			} else if (kind && strcmp(kind, "source_unit") == 0) {
				if (push_source_unit_item(L, -1, target, graph, field) < 0) {
					lua_pop(L, 2);
					return -1;
				}
			} else {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: field '%s' contains non-string item", field);
			}
		} else {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: field '%s' contains non-string item", field);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
push_table_strings(lua_State *L, int idx, struct qstar_string_list *list, struct qstar_graph *graph,
    const char *field, int canonicalize, const char *fragment_dir)
{
	size_t n, i;
	const char *s, *kind;
	char label[QSTAR_PATH_MAX];

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (lua_isnil(L, idx))
		return 0;
	if (!lua_istable(L, idx))
		return qstar_set_error(graph, "qstar: field '%s' must be a list", field);
	n = lua_rawlen(L, idx);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			s = lua_tostring(L, -1);
			if (canonicalize) {
				if (qstar_label_canonicalize(s, fragment_dir, label, sizeof(label)) < 0) {
					lua_pop(L, 1);
					return qstar_set_error(graph, "qstar: invalid label '%s'", s);
				}
				s = label;
			}
			if (qstar_string_list_push(list, s) < 0) {
				lua_pop(L, 1);
				return qstar_set_error(graph, "qstar: out of memory");
			}
		} else if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "__qstar_kind");
			kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
			lua_pop(L, 1);
			if (kind && strcmp(kind, "output_path") == 0) {
				s = output_path_table_path(L, -1);
				if (!s || qstar_string_list_push(list, s) < 0) {
					lua_pop(L, 1);
					return qstar_set_error(graph, "qstar: out of memory");
				}
			} else {
				lua_pop(L, 1);
				return qstar_set_error(graph, "qstar: field '%s' contains non-string item", field);
			}
		} else {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: field '%s' contains non-string item", field);
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
read_list_field(lua_State *L, int table, const char *field, struct qstar_string_list *list,
    struct qstar_graph *graph, int canonicalize, const char *fragment_dir)
{
	int rc;

	lua_getfield(L, table, field);
	rc = push_table_strings(L, -1, list, graph, field, canonicalize, fragment_dir);
	lua_pop(L, 1);
	return rc;
}

/** string 또는 qstar.target_file(...) 항목을 받는 list field를 읽는다. */
static int
read_target_file_list_field(lua_State *L, int table, const char *field,
    struct qstar_string_list *list, struct qstar_graph *graph)
{
	const char *kind, *s;
	char token[QSTAR_PATH_MAX];
	size_t i, n;

	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s' must be a list", field);
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			s = lua_tostring(L, -1);
		} else if (lua_istable(L, -1)) {
			kind = qstar_table_kind(L, -1);
			if (!kind || strcmp(kind, "target_file") != 0 ||
			    format_placeholder_token(L, -1, token, sizeof(token)) < 0) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: field '%s' contains non-string or target_file item",
				    field);
			}
			s = token;
		} else {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: field '%s' contains non-string or target_file item",
			    field);
		}
		if (!s || !*s) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: field '%s' contains empty path",
			    field);
		}
		if (qstar_string_list_push(list, s) < 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

/** run_target inputs field는 plain file, target artifact, stage layout token을 받는다. */
static int
read_run_inputs_field(lua_State *L, int table, struct qstar_target *target,
    struct qstar_graph *graph)
{
	const char *kind, *s;
	char token[QSTAR_PATH_MAX];
	size_t i, n;

	lua_getfield(L, table, "inputs");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: run_target inputs must be a list");
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			s = lua_tostring(L, -1);
		} else if (lua_istable(L, -1)) {
			kind = qstar_table_kind(L, -1);
			if (!kind ||
			    (strcmp(kind, "target_file") != 0 &&
			    strcmp(kind, "stage_dir") != 0) ||
			    format_placeholder_token(L, -1, token, sizeof(token)) < 0) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: run_target inputs contains non-string, target_file, or stage_dir item");
			}
			s = token;
		} else {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target inputs contains non-string, target_file, or stage_dir item");
		}
		if (!s || !*s) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target inputs contains empty path");
		}
		if (qstar_string_list_push(&target->run_inputs, s) < 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

/** qstar.step.run inputs field는 run_target.inputs와 같은 token set을 받는다. */
static int
read_command_step_inputs_field(lua_State *L, int table,
    struct qstar_command_step *step, struct qstar_graph *graph)
{
	const char *kind, *s;
	char token[QSTAR_PATH_MAX];
	size_t i, n;

	lua_getfield(L, table, "inputs");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: qstar.step.run inputs must be a list");
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			s = lua_tostring(L, -1);
		} else if (lua_istable(L, -1)) {
			kind = qstar_table_kind(L, -1);
			if (!kind ||
			    (strcmp(kind, "target_file") != 0 &&
			    strcmp(kind, "stage_dir") != 0) ||
			    format_placeholder_token(L, -1, token, sizeof(token)) < 0) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: qstar.step.run inputs contains non-string, target_file, or stage_dir item");
			}
			s = token;
		} else {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run inputs contains non-string, target_file, or stage_dir item");
		}
		if (!s || !*s) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run inputs contains empty path");
		}
		if (qstar_string_list_push(&step->inputs, s) < 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int append_list(struct qstar_graph *graph, struct qstar_string_list *dst,
    const struct qstar_string_list *src);
static int qstar_lua_array_table(lua_State *L, int idx);

static int
read_command_field(lua_State *L, int table, const char *field, struct qstar_string_list *list,
    struct qstar_graph *graph)
{
	int rc;

	lua_getfield(L, table, field);
	rc = read_cli_command(L, -1, list, graph, field);
	lua_pop(L, 1);
	return rc;
}

static int
legacy_field_present(lua_State *L, int table, const char *field)
{
	int present;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	present = !lua_isnil(L, -1);
	lua_pop(L, 1);
	return present;
}

/** Lua table field를 QStar-owned string slot으로 교체한다. */
static int
replace_lua_string(char **slot, const char *value, struct qstar_graph *graph)
{
	char *copy;

	copy = qstar_strdup(value ? value : "");
	if (!copy)
		return qstar_set_error(graph, "qstar: out of memory");
	free(*slot);
	*slot = copy;
	return 0;
}

/** string 또는 bool Lua field를 QStar-owned string slot으로 읽는다. */
static int
read_string_or_bool_field(lua_State *L, int table, const char *field, char **slot,
    struct qstar_graph *graph)
{
	const char *value;
	int type;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	type = lua_type(L, -1);
	if (type == LUA_TNIL) {
		lua_pop(L, 1);
		return 0;
	}
	if (type == LUA_TBOOLEAN) {
		value = lua_toboolean(L, -1) ? "true" : "false";
	} else if (type == LUA_TSTRING) {
		value = lua_tostring(L, -1);
	} else {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: field '%s' must be a string or boolean", field);
	}
	if (replace_lua_string(slot, value, graph) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

/** label scalar field를 현재 fragment 기준 canonical label로 읽는다. */
static int
read_label_scalar_field(lua_State *L, int table, const char *field, char **slot,
    struct qstar_graph *graph, const char *fragment_dir, int *present)
{
	const char *raw;
	char label[QSTAR_PATH_MAX];
	int type;

	if (present)
		*present = 0;
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	type = lua_type(L, -1);
	if (type == LUA_TNIL) {
		lua_pop(L, 1);
		return 0;
	}
	if (type != LUA_TSTRING) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s' must be a label string",
		    field);
	}
	raw = lua_tostring(L, -1);
	if (qstar_label_canonicalize(raw, fragment_dir, label, sizeof(label)) < 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: invalid %s label '%s'", field, raw);
	}
	if (replace_lua_string(slot, label, graph) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	if (present)
		*present = 1;
	lua_pop(L, 1);
	return 0;
}

/** lang.<namespace>.<field>가 명시되었는지 검사해 config scalar merge flag를 만든다. */
static int
nested_lang_field_present(lua_State *L, int table, const char *lang_name, const char *field)
{
	int present;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "lang");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	lua_getfield(L, -1, lang_name);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return 0;
	}
	lua_getfield(L, -1, field);
	present = !lua_isnil(L, -1);
	lua_pop(L, 3);
	return present;
}

/** target artifact_name field가 filename basename으로 안전한지 검사한다. */
static int
valid_target_artifact_name(const char *s)
{
	const unsigned char *p;

	if (!s || !*s)
		return 1;
	for (p = (const unsigned char *)s; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '+'))
			return 0;
	}
	return 1;
}

/** output artifact metadata token이 deterministic identity에 안전한지 확인한다. */
static int
valid_output_metadata_token(const char *s)
{
	const unsigned char *p;

	if (!s || !*s)
		return 1;
	for (p = (const unsigned char *)s; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.' ||
		    *p == ':' || *p == '+' || *p == '=' || *p == 'x'))
			return 0;
	}
	return 1;
}

/** qstar.output metadata table에 알 수 없는 field와 비문자열 값을 검사한다. */
static int
validate_output_metadata_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || (strcmp(key, "group") != 0 &&
		    strcmp(key, "output_group") != 0 && strcmp(key, "format") != 0)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: unknown qstar.output metadata field '%s'",
			    key ? key : "<non-string>");
		}
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: qstar.output metadata field '%s' must be a string",
			    key);
		}
		lua_pop(L, 1);
	}
	return 0;
}

/** qstar.output format metadata가 core bridge surface에 속하는지 검사한다. */
static int
valid_output_format(const char *format)
{
	return !format || !*format || strcmp(format, "object") == 0;
}

/** generated output path와 metadata를 genrule parallel lists에 추가한다. */
static int
push_genrule_output(lua_State *L, int idx, struct qstar_genrule *genrule,
    struct qstar_graph *graph, const char *field)
{
	const char *path, *group, *format;
	const char *kind;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	group = "";
	format = "";
	if (lua_isstring(L, idx)) {
		path = lua_tostring(L, idx);
	} else if (lua_istable(L, idx)) {
		kind = qstar_table_kind(L, idx);
		if (!kind || strcmp(kind, "output_path") != 0)
			return qstar_set_error(graph,
			    "qstar: field '%s' contains non-output item", field);
		path = output_path_table_path(L, idx);
		lua_getfield(L, idx, "group");
		group = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		if (!*group) {
			lua_getfield(L, idx, "output_group");
			group = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
			lua_pop(L, 1);
		}
		lua_getfield(L, idx, "format");
		format = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
	} else {
		return qstar_set_error(graph,
		    "qstar: field '%s' contains non-string item", field);
	}
	if (!path || !*path)
		return qstar_set_error(graph, "qstar: generated output path is empty");
	if (!valid_output_metadata_token(group) || !valid_output_metadata_token(format))
		return qstar_set_error(graph,
		    "qstar: generated output metadata for '%s' contains unsupported characters",
		    path);
	if (!valid_output_format(format))
		return qstar_set_error(graph,
		    "qstar: qstar.output format '%s' is not supported; only format = \"object\" is supported",
		    format);
	if (qstar_string_list_push(&genrule->outputs, path) < 0 ||
	    qstar_string_list_push(&genrule->output_groups, group) < 0 ||
	    qstar_string_list_push(&genrule->output_formats, format) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** custom_target outputs field를 qstar.output metadata까지 포함해 읽는다. */
static int
read_outputs_field(lua_State *L, int table, struct qstar_genrule *genrule,
    struct qstar_graph *graph)
{
	size_t i, n;
	int rc;

	lua_getfield(L, table, "outputs");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'outputs' must be a list");
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		rc = push_genrule_output(L, -1, genrule, graph, "outputs");
		lua_pop(L, 1);
		if (rc < 0) {
			lua_pop(L, 1);
			return -1;
		}
	}
	lua_pop(L, 1);
	return 0;
}

/** generated action input item을 파일 path 또는 target artifact edge로 추가한다. */
static int
push_genrule_input(lua_State *L, int idx, struct qstar_genrule *genrule,
    struct qstar_graph *graph, const char *field)
{
	const char *kind, *path;
	char token[QSTAR_PATH_MAX];

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (lua_isstring(L, idx))
		path = lua_tostring(L, idx);
	else if (lua_istable(L, idx)) {
		kind = qstar_table_kind(L, idx);
		if (!kind || strcmp(kind, "target_file") != 0 ||
		    format_placeholder_token(L, idx, token, sizeof(token)) < 0)
			return qstar_set_error(graph,
			    "qstar: field '%s' contains non-string or target_file item",
			    field);
		path = token;
	} else {
		return qstar_set_error(graph,
		    "qstar: field '%s' contains non-string or target_file item", field);
	}
	if (!path || !*path)
		return qstar_set_error(graph, "qstar: generated input path is empty");
	if (qstar_string_list_push(&genrule->inputs, path) < 0)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

/** custom_target inputs field를 qstar.target_file edge까지 포함해 읽는다. */
static int
read_genrule_inputs_field(lua_State *L, int table, struct qstar_genrule *genrule,
    struct qstar_graph *graph)
{
	size_t i, n;
	int rc;

	lua_getfield(L, table, "inputs");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'inputs' must be a list");
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		rc = push_genrule_input(L, -1, genrule, graph, "inputs");
		lua_pop(L, 1);
		if (rc < 0) {
			lua_pop(L, 1);
			return -1;
		}
	}
	lua_pop(L, 1);
	return 0;
}

static int
validate_transform_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"input", "output", "command", "description", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: unknown transform field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
read_transform_input_field(lua_State *L, int table, struct qstar_genrule *genrule,
    struct qstar_graph *graph)
{
	int rc;

	lua_getfield(L, table, "input");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: transform '%s' requires input",
		    genrule->label);
	}
	rc = push_genrule_input(L, -1, genrule, graph, "input");
	lua_pop(L, 1);
	return rc;
}

static int
read_transform_output_field(lua_State *L, int table, struct qstar_genrule *genrule,
    struct qstar_graph *graph)
{
	int rc;

	lua_getfield(L, table, "output");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: transform '%s' requires output",
		    genrule->label);
	}
	rc = push_genrule_output(L, -1, genrule, graph, "output");
	lua_pop(L, 1);
	return rc;
}

static int
parse_placeholder_index(const char *s, const char *prefix, long *index)
{
	const char *p;
	char *end;

	if (strncmp(s, prefix, strlen(prefix)) != 0)
		return 0;
	p = s + strlen(prefix);
	*index = strtol(p, &end, 10);
	return end && strcmp(end, ">") == 0 ? 1 : -1;
}

static int
resolve_cli_placeholders(struct qstar_graph *graph, struct qstar_string_list *command,
    const struct qstar_string_list *inputs, const struct qstar_string_list *outputs,
    const char *field)
{
	struct qstar_string_list resolved;
	const char *item;
	long index;
	size_t i;
	int rc;

	memset(&resolved, 0, sizeof(resolved));
	for (i = 0; i < command->len; i++) {
		item = command->items[i];
		rc = parse_placeholder_index(item, "<qstar-input:", &index);
		if (rc == 1) {
			if (index < 0 || (size_t)index >= inputs->len) {
				qstar_string_list_free(&resolved);
				return qstar_set_error(graph,
				    "qstar: %s references missing qstar.input(%ld)", field,
				    index);
			}
			item = inputs->items[index];
		} else if (rc < 0) {
			qstar_string_list_free(&resolved);
			return qstar_set_error(graph, "qstar: malformed input placeholder");
		} else {
			rc = parse_placeholder_index(item, "<qstar-output:", &index);
			if (rc == 1) {
				if (index < 0 || (size_t)index >= outputs->len) {
					qstar_string_list_free(&resolved);
					return qstar_set_error(graph,
					    "qstar: %s references missing qstar.output(%ld)",
					    field, index);
				}
				item = outputs->items[index];
			} else if (rc < 0) {
				qstar_string_list_free(&resolved);
				return qstar_set_error(graph, "qstar: malformed output placeholder");
			}
		}
		if (qstar_string_list_push(&resolved, item) < 0) {
			qstar_string_list_free(&resolved);
			return qstar_set_error(graph, "qstar: out of memory");
		}
	}
	qstar_string_list_free(command);
	*command = resolved;
	return 0;
}

static int
finish_generated_command(lua_State *L, int table, struct qstar_genrule *genrule,
    struct qstar_graph *graph, const char *api, const char *placeholder_field)
{
	size_t i;

	if (read_status_description_field(L, table, graph,
	    &genrule->description) < 0 ||
	    read_command_field(L, table, "command", &genrule->command, graph) < 0 ||
	    resolve_cli_placeholders(graph, &genrule->command, &genrule->inputs,
	    &genrule->outputs, placeholder_field) < 0)
		return -1;
	if (genrule->command.len == 0)
		return qstar_set_error(graph,
		    "qstar: %s '%s' requires command = qstar.cli { ... }",
		    api, genrule->label);
	free(genrule->tool);
	genrule->tool = qstar_strdup(genrule->command.items[0]);
	if (!genrule->tool)
		return qstar_set_error(graph, "qstar: out of memory");
	for (i = 1; i < genrule->command.len; i++) {
		if (qstar_string_list_push(&genrule->args,
		    genrule->command.items[i]) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

/** group target이 artifact/action field를 받지 않도록 DSL boundary를 고정한다. */
static int
reject_group_action_fields(lua_State *L, int table, struct qstar_graph *graph,
    const char *label)
{
	static const char *fields[] = {
		"sources",
		"lang",
		"configs",
		"libs",
		"lib_dirs",
		"link",
		"link_options",
		"link_inputs",
		"toolset",
		"toolchain",
		"stdlib",
		"artifact_name",
		"inputs",
		"command",
		"timeout",
		"expect",
	};
	size_t i;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		lua_getfield(L, table, fields[i]);
		if (!lua_isnil(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: group target '%s' cannot set '%s'; group targets have deps only, no command, no sources, and no artifact; use deps/private_deps/visibility only",
			    label, fields[i]);
		}
		lua_pop(L, 1);
	}
	return 0;
}

/** inputs field는 run_target 전용이다. */
static int
reject_non_run_inputs_field(lua_State *L, int table, struct qstar_graph *graph,
    const struct qstar_target *target)
{
	if (strcmp(target->kind, "run_target") == 0 ||
	    strcmp(target->kind, "group") == 0)
		return 0;
	if (!legacy_field_present(L, table, "inputs"))
		return 0;
	return qstar_set_error(graph,
	    "qstar: target '%s' field 'inputs' is only valid on run_target; use sources, link_inputs, or deps for build artifacts",
	    target->label);
}

static int
push_define_option(struct qstar_graph *graph, struct qstar_string_list *list, const char *def)
{
	char buf[QSTAR_PATH_MAX];

	if (snprintf(buf, sizeof(buf), "-D%s", def) >= (int)sizeof(buf))
		return qstar_set_error(graph, "qstar: lang define '%s' is too long", def);
	return qstar_string_list_push(list, buf) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

/** lang.<name>.defines list를 -D compiler option으로 변환해 읽는다. */
static int
read_lang_defines(lua_State *L, int lang, const char *lang_name, struct qstar_string_list *list,
    struct qstar_graph *graph)
{
	size_t i, n;

	lua_getfield(L, lang, "defines");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.%s.defines must be a list",
		    lang_name);
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: lang.%s.defines contains non-string item",
			    lang_name);
		}
		if (push_define_option(graph, list, lua_tostring(L, -1)) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
append_lang_include_self(struct qstar_graph *graph, struct qstar_string_list *compile_dirs,
    const struct qstar_string_list *extra)
{
	return append_list(graph, compile_dirs, extra);
}

static int
string_in_set(const char *s, const char *const *items)
{
	size_t i;

	for (i = 0; items[i]; i++) {
		if (strcmp(s, items[i]) == 0)
			return 1;
	}
	return 0;
}

static int
validate_lang_fields(lua_State *L, int table, const char *lang_name,
    const char *const *allowed, struct qstar_graph *graph)
{
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: unknown field lang.%s.%s",
			    lang_name, key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static const char *
canonical_provider_option_type(const char *type)
{
	if (!type)
		return NULL;
	if (strcmp(type, "boolean") == 0)
		return "bool";
	if (strcmp(type, "string") == 0)
		return "string";
	if (strcmp(type, "bool") == 0)
		return "bool";
	if (strcmp(type, "list") == 0)
		return "list";
	if (strcmp(type, "enum") == 0)
		return "enum";
	return NULL;
}

static int
string_list_contains_value(const struct qstar_string_list *list, const char *value)
{
	size_t i;

	if (!list || !value)
		return 0;
	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], value) == 0)
			return 1;
	}
	return 0;
}

static int
lua_string_list_contains_value(lua_State *L, int table, const char *value)
{
	const char *item;
	size_t i, n;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !value)
		return 0;
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		item = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
		if (item && strcmp(item, value) == 0) {
			lua_pop(L, 1);
			return 1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_exact_string_list_value(lua_State *L, int idx, struct qstar_graph *graph,
    const char *owner)
{
	const char *item;
	size_t i, n;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (!lua_istable(L, idx) || !qstar_lua_array_table(L, idx))
		return qstar_set_error(graph, "qstar: %s must be a list of strings",
		    owner);
	n = lua_rawlen(L, idx);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		item = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
		if (!item) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s must be a list of strings", owner);
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
read_exact_string_list_value(lua_State *L, int idx, struct qstar_string_list *list,
    struct qstar_graph *graph, const char *owner)
{
	size_t i, n;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (validate_exact_string_list_value(L, idx, graph, owner) < 0)
		return -1;
	n = lua_rawlen(L, idx);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		if (qstar_string_list_push(list, lua_tostring(L, -1)) < 0) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_provider_option_value(lua_State *L, int idx, struct qstar_graph *graph,
    const char *owner, const char *type, const struct qstar_string_list *values)
{
	const char *value;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (strcmp(type, "string") == 0) {
		if (lua_type(L, idx) != LUA_TSTRING)
			return qstar_set_error(graph, "qstar: %s must be a string",
			    owner);
		return 0;
	}
	if (strcmp(type, "bool") == 0) {
		if (!lua_isboolean(L, idx))
			return qstar_set_error(graph, "qstar: %s must be a boolean",
			    owner);
		return 0;
	}
	if (strcmp(type, "list") == 0)
		return validate_exact_string_list_value(L, idx, graph, owner);
	if (strcmp(type, "enum") == 0) {
		if (lua_type(L, idx) != LUA_TSTRING)
			return qstar_set_error(graph, "qstar: %s must be an enum string",
			    owner);
		value = lua_tostring(L, idx);
		if (!string_list_contains_value(values, value))
			return qstar_set_error(graph,
			    "qstar: %s has unsupported enum value '%s'", owner, value);
		return 0;
	}
	return qstar_set_error(graph, "qstar: %s has unsupported provider option type '%s'",
	    owner, type ? type : "<none>");
}

static int
read_lang_c(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"public_headers", "private_headers",
		"include_dirs", "public_include_dirs", "private_include_dirs",
		"system_include_dirs", "compile_options", "defines", NULL
	};
	int rc;

	lua_getfield(L, lang, "c");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.c must be a table");
	}
	rc = validate_lang_fields(L, -1, "c", allowed, graph);
	if (rc == 0)
		rc = read_list_field(L, -1, "public_headers", &target->public_headers, graph, 0,
	    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "private_headers", &target->private_headers, graph, 0,
		    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "include_dirs", &target->include_dirs, graph, 0,
	    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "public_include_dirs", &target->public_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "private_include_dirs", &target->private_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "system_include_dirs", &target->system_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "compile_options", &target->cflags, graph, 0,
		    target->fragment_dir);
	if (rc == 0)
		rc = read_lang_defines(L, -1, "c", &target->cflags, graph);
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->private_include_dirs);
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->public_include_dirs);
	lua_pop(L, 1);
	return rc;
}

static int
read_lang_cxx(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"public_headers", "private_headers", "modules",
		"standard", "include_dirs", "public_include_dirs", "private_include_dirs",
		"system_include_dirs", "compile_options", "defines", NULL
	};
	const char *standard;
	int rc;

	lua_getfield(L, lang, "cxx");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.cxx must be a table");
	}
	rc = validate_lang_fields(L, -1, "cxx", allowed, graph);
	if (rc == 0)
		rc = read_list_field(L, -1, "public_headers", &target->public_headers, graph, 0,
	    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "private_headers", &target->private_headers, graph, 0,
		    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "include_dirs", &target->include_dirs, graph, 0,
	    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "public_include_dirs", &target->public_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "private_include_dirs", &target->private_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "system_include_dirs", &target->system_include_dirs,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "compile_options", &target->cxxflags, graph, 0,
		    target->fragment_dir);
	if (rc == 0)
		rc = read_lang_defines(L, -1, "cxx", &target->cxxflags, graph);
	standard = check_string_field(L, -1, "standard");
	if (standard) {
		free(target->cxx_standard);
		target->cxx_standard = qstar_strdup(standard);
		if (!target->cxx_standard)
			rc = qstar_set_error(graph, "qstar: out of memory");
	}
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->private_include_dirs);
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->public_include_dirs);
	if (rc == 0) {
		lua_getfield(L, -1, "modules");
		if (!lua_isnil(L, -1)) {
			if (!lua_istable(L, -1)) {
				lua_pop(L, 2);
				return qstar_set_error(graph, "qstar: lang.cxx.modules must be a table");
			}
			target->cxx_modules_present = 1;
			lua_getfield(L, -1, "enabled");
			if (!lua_isnil(L, -1))
				target->cxx_modules_enabled = lua_toboolean(L, -1) ? 1 : 0;
			lua_pop(L, 1);
			if (target->cxx_modules_enabled) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: C++ modules are not supported; set lang.cxx.modules.enabled = false");
			}
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return rc;
}

static int
read_lang_asm(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"include_dirs", "compile_options", "preprocess", NULL
	};
	int rc;

	lua_getfield(L, lang, "asm");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.asm must be a table");
	}
	rc = validate_lang_fields(L, -1, "asm", allowed, graph);
	if (rc == 0)
		rc = read_list_field(L, -1, "include_dirs", &target->asm_include_dirs, graph, 0,
	    target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "compile_options", &target->asm_compile_options,
		    graph, 0, target->fragment_dir);
	lua_getfield(L, -1, "preprocess");
	if (!lua_isnil(L, -1))
		target->asm_preprocess = lua_toboolean(L, -1) ? 1 : 0;
	lua_pop(L, 1);
	lua_pop(L, 1);
	return rc;
}

static int
read_provider_lang_options(lua_State *L, int table, struct qstar_graph *graph,
    struct qstar_target *target, const struct qstar_language_provider *provider)
{
	const struct qstar_language_option_schema *schema;
	const char *key, *value;
	struct qstar_string_list list;
	char owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		memset(&list, 0, sizeof(list));
		key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
		schema = qstar_language_provider_find_option(provider, key);
		if (!key || !schema) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: unknown field lang.%s.%s",
			    provider->namespace, key ? key : "<non-string>");
		}
		snprintf(owner, sizeof(owner), "lang.%s.%s", provider->namespace, key);
		if (validate_provider_option_value(L, -1, graph, owner, schema->type,
		    &schema->values) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		value = "";
		if (strcmp(schema->type, "bool") == 0) {
			value = lua_toboolean(L, -1) ? "true" : "false";
		} else if (strcmp(schema->type, "list") == 0) {
			if (read_exact_string_list_value(L, -1, &list, graph, owner) < 0) {
				lua_pop(L, 2);
				qstar_string_list_free(&list);
				return -1;
			}
		} else {
			value = lua_tostring(L, -1);
		}
		if (qstar_target_set_provider_option(graph, target,
		    provider->namespace, key, schema->type, value, &list) < 0) {
			lua_pop(L, 2);
			qstar_string_list_free(&list);
			return -1;
		}
		qstar_string_list_free(&list);
		lua_pop(L, 1);
	}
	return 0;
}

static int
append_list(struct qstar_graph *graph, struct qstar_string_list *dst,
    const struct qstar_string_list *src)
{
	size_t i;

	for (i = 0; i < src->len; i++) {
		if (qstar_string_list_push(dst, src->items[i]) < 0)
			return qstar_set_error(graph, "qstar: out of memory");
	}
	return 0;
}

static int
read_lang_options(lua_State *L, int table, struct qstar_target *target,
    struct qstar_graph *graph)
{
	const char *key;
	int rc;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "lang");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'lang' must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !qstar_graph_language_provider_is_available(graph, key)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: unknown language namespace lang.%s",
			    key ? key : "<non-string>");
		}
		if (!lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: lang.%s must be a table",
			    key);
		}
		if (!qstar_language_provider_is_preloaded(key)) {
			const struct qstar_language_provider *provider;
			provider = qstar_graph_find_language_provider(graph, key);
			if (!provider || read_provider_lang_options(L, -1, graph,
			    target, provider) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		}
		lua_pop(L, 1);
	}
	rc = read_lang_c(L, -1, target, graph);
	if (rc == 0)
		rc = read_lang_cxx(L, -1, target, graph);
	if (rc == 0)
		rc = read_lang_asm(L, -1, target, graph);
	lua_pop(L, 1);
	return rc;
}

static int
validate_link_fields(lua_State *L, int table, const char *owner, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"frameworks", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "link");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s.link' must be a table",
		    owner);
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: unknown %s.link field '%s'",
			    owner, key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
read_link_options(lua_State *L, int table, struct qstar_target *target,
    struct qstar_graph *graph, const char *owner)
{
	int rc;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "link");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	rc = validate_link_fields(L, table, owner, graph);
	if (rc == 0)
		rc = read_list_field(L, -1, "frameworks", &target->frameworks, graph, 0,
		    target->fragment_dir);
	lua_pop(L, 1);
	return rc;
}

/** qstar.toolset table에서 허용되는 top-level field만 검사한다. */
static int
validate_toolset_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"tools", "response_files", "response_style", "path_tools",
		"allow_absolute_tools", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: unknown toolset field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
toolset_core_role(const char *role)
{
	return role && (strcmp(role, "archive") == 0 || strcmp(role, "link") == 0);
}

static int
toolset_removed_direct_role(const char *role)
{
	return role && (strcmp(role, "c") == 0 || strcmp(role, "cxx") == 0 ||
	    strcmp(role, "asm") == 0);
}

static int
valid_tool_role_component(const char *s)
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
copy_source_unit_options(lua_State *L, int metadata, int options,
    struct qstar_graph *graph, const struct qstar_language_provider *provider)
{
	const struct qstar_language_option_schema *schema;
	const char *key;
	char owner[192];

	metadata = qstar_lua_abs_index(L, metadata);
	options = qstar_lua_abs_index(L, options);
	lua_pushnil(L);
	while (lua_next(L, metadata) != 0) {
		key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
		if (key && (strcmp(key, "language") == 0 ||
		    strcmp(key, "provider") == 0 || strcmp(key, "unit") == 0)) {
			lua_pop(L, 1);
			continue;
		}
		schema = qstar_language_provider_find_option(provider, key);
		if (!key || !schema) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: unknown source option %s.%s",
			    provider->namespace, key ? key : "<non-string>");
		}
		snprintf(owner, sizeof(owner), "source.%s.%s", provider->namespace, key);
		if (validate_provider_option_value(L, -1, graph, owner, schema->type,
		    &schema->values) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_settable(L, options);
		lua_pop(L, 1);
	}
	return 0;
}

static int
qstar_lua_source(lua_State *L)
{
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	const struct qstar_language_provider *provider;
	const struct qstar_language_unit_schema *unit_schema;
	const char *path, *language, *unit;

	ctx = get_context(L);
	graph = ctx->graph;
	path = luaL_checkstring(L, 1);
	if (!qstar_path_is_package_relative(path))
		return luaL_error(L,
		    "qstar: source path '%s' must be package-relative (%s)",
		    path, qstar_path_package_relative_reason(path));
	if (!lua_istable(L, 2))
		return luaL_error(L,
		    "qstar: qstar.source metadata must be a table");
	language = check_string_field(L, 2, "language");
	if (!language)
		language = check_string_field(L, 2, "provider");
	unit = check_string_field(L, 2, "unit");
	if (!valid_tool_role_component(language) || !valid_tool_role_component(unit))
		return luaL_error(L,
		    "qstar: qstar.source metadata requires language and unit names");
	provider = qstar_graph_find_language_provider(graph, language);
	if (!provider)
		return luaL_error(L,
		    "qstar: source language '%s' is not active; use qstar.use_language(\"%s\") first",
		    language, language);
	unit_schema = qstar_language_provider_find_unit(provider, unit);
	if (!unit_schema)
		return luaL_error(L,
		    "qstar: language provider '%s' has no source unit '%s'",
		    language, unit);
	if (strcmp(unit_schema->emits, "object") != 0)
		return luaL_error(L,
		    "qstar: language provider source unit '%s.%s' does not emit objects",
		    language, unit);
	if (!source_unit_suffix_matches(unit_schema, path))
		return luaL_error(L,
		    "qstar: source '%s' does not match suffixes for unit '%s.%s'",
		    path, language, unit);
	lua_newtable(L);
	lua_pushstring(L, "source_unit");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, path);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, language);
	lua_setfield(L, -2, "language");
	lua_pushstring(L, language);
	lua_setfield(L, -2, "provider");
	lua_pushstring(L, unit);
	lua_setfield(L, -2, "unit");
	lua_newtable(L);
	if (copy_source_unit_options(L, 2, -1, graph, provider) < 0)
		return luaL_error(L, "%s", graph->error);
	lua_setfield(L, -2, "options");
	return 1;
}

static int
valid_tool_role_name(const char *s)
{
	char component[64];
	const char *p, *dot;
	size_t n;

	if (!s || !*s)
		return 0;
	p = s;
	while (*p) {
		dot = strchr(p, '.');
		n = dot ? (size_t)(dot - p) : strlen(p);
		if (n == 0 || n >= sizeof(component))
			return 0;
		memcpy(component, p, n);
		component[n] = '\0';
		if (!valid_tool_role_component(component))
			return 0;
		if (!dot)
			break;
		p = dot + 1;
	}
	return 1;
}

static void
push_provider_string_list(lua_State *L, const struct qstar_string_list *list)
{
	size_t i;

	lua_newtable(L);
	for (i = 0; list && i < list->len; i++) {
		lua_pushstring(L, list->items[i]);
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
}

static int
push_provider_option_lua_value(lua_State *L, const char *type, const char *value,
    const struct qstar_string_list *list)
{
	if (strcmp(type, "bool") == 0) {
		lua_pushboolean(L, value && strcmp(value, "true") == 0);
		return 1;
	}
	if (strcmp(type, "list") == 0) {
		push_provider_string_list(L, list);
		return 1;
	}
	lua_pushstring(L, value ? value : "");
	return 1;
}

static int
provider_ctx_tool(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;
	const char *raw, *role;
	char resolved[128], token[160];

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	raw = luaL_checkstring(L, 1);
	if (strchr(raw, '.')) {
		role = raw;
	} else {
		if (snprintf(resolved, sizeof(resolved), "%s.%s",
		    ctx->provider->namespace, raw) >= (int)sizeof(resolved))
			return luaL_error(L, "qstar: provider tool role is too long");
		role = resolved;
	}
	if (!valid_tool_role_name(role))
		return luaL_error(L, "qstar: invalid provider tool role '%s'", role);
	if (snprintf(token, sizeof(token), "<qstar-tool:%s>", role) >=
	    (int)sizeof(token))
		return luaL_error(L, "qstar: provider tool token is too long");
	lua_pushstring(L, token);
	return 1;
}

static int
provider_ctx_input(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;
	const char *name;

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	name = luaL_checkstring(L, 1);
	if (strcmp(name, "source") == 0) {
		if (!ctx->source)
			return luaL_error(L,
			    "qstar: provider input 'source' is not available for final actions");
		lua_pushstring(L, ctx->source);
		return 1;
	}
	if (strcmp(name, "sources") == 0) {
		push_provider_string_list(L, ctx->sources);
		return 1;
	}
	return luaL_error(L, "qstar: unknown provider input '%s'", name);
}

static int
provider_ctx_output(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;
	const char *name;

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	name = luaL_checkstring(L, 1);
	if (strcmp(name, "artifact") == 0) {
		if (!ctx->artifact)
			return luaL_error(L,
			    "qstar: provider output 'artifact' is not available for source actions");
		lua_pushstring(L, ctx->artifact);
		return 1;
	}
	if (strcmp(name, "object") == 0) {
		if (!ctx->object)
			return luaL_error(L,
			    "qstar: provider output 'object' is not available for final actions");
		lua_pushstring(L, ctx->object);
		return 1;
	}
	if (strcmp(name, "depfile") == 0) {
		if (!ctx->depfile)
			return luaL_error(L,
			    "qstar: provider output 'depfile' is not available");
		lua_pushstring(L, ctx->depfile);
		return 1;
	}
	return luaL_error(L, "qstar: unknown provider output '%s'", name);
}

static int
provider_ctx_kind(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	lua_pushstring(L, ctx->final_kind ? ctx->final_kind : "");
	return 1;
}

static int
provider_cache_name_ok(const char *name)
{
	if (!name || !*name || !qstar_path_is_package_relative(name))
		return 0;
	if (name[0] == '.' || name[strlen(name) - 1] == '.')
		return 0;
	return 1;
}

static int
provider_ctx_cache(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;
	const char *name;
	char path[QSTAR_PATH_MAX];

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	name = luaL_checkstring(L, 1);
	if (!provider_cache_name_ok(name))
		return luaL_error(L,
		    "qstar: provider cache name must be package-relative");
	if (qstar_path_join(ctx->cache_base, name, path, sizeof(path)) < 0)
		return luaL_error(L, "qstar: provider cache path is too long");
	lua_pushstring(L, path);
	return 1;
}

static int
provider_ctx_option(lua_State *L)
{
	struct qstar_provider_lowering_ctx *ctx;
	const struct qstar_language_option_schema *schema;
	const struct qstar_provider_option_value *value;
	const char *name;

	ctx = lua_touserdata(L, lua_upvalueindex(1));
	name = luaL_checkstring(L, 1);
	schema = qstar_language_provider_find_option(ctx->provider, name);
	if (!schema)
		return luaL_error(L, "qstar: unknown provider option '%s.%s'",
		    ctx->provider->namespace, name);
	if (ctx->source_options_ref != LUA_NOREF) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->source_options_ref);
		lua_getfield(L, -1, name);
		if (!lua_isnil(L, -1)) {
			lua_remove(L, -2);
			return 1;
		}
		lua_pop(L, 2);
	}
	value = qstar_target_provider_option(ctx->target, ctx->provider->namespace,
	    name);
	if (value)
		return push_provider_option_lua_value(L, value->type, value->value,
		    &value->list);
	if (!schema->has_default) {
		lua_pushnil(L);
		return 1;
	}
	return push_provider_option_lua_value(L, schema->type, schema->default_value,
	    &schema->default_list);
}

static void
push_provider_lowering_context(lua_State *L, struct qstar_provider_lowering_ctx *ctx)
{
	lua_newtable(L);
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_tool, 1);
	lua_setfield(L, -2, "tool");
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_input, 1);
	lua_setfield(L, -2, "input");
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_output, 1);
	lua_setfield(L, -2, "output");
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_cache, 1);
	lua_setfield(L, -2, "cache");
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_option, 1);
	lua_setfield(L, -2, "option");
	lua_pushlightuserdata(L, ctx);
	lua_pushcclosure(L, provider_ctx_kind, 1);
	lua_setfield(L, -2, "kind");
}

static int
read_provider_string_list_result(lua_State *L, int table,
    struct qstar_string_list *dst, struct qstar_graph *graph, const char *field,
    const char *fallback)
{
	char owner[128];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return fallback ? qstar_string_list_push(dst, fallback) : 0;
	}
	snprintf(owner, sizeof(owner), "provider action %s", field);
	if (read_exact_string_list_value(L, -1, dst, graph, owner) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
read_provider_command_result(lua_State *L, int table,
    struct qstar_provider_action_template *action, struct qstar_graph *graph)
{
	const char *kind, *item;
	size_t i, n;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "command");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider lowering result must include command = qstar.argv() or a string list");
	}
	kind = qstar_table_kind(L, -1);
	if (kind && strcmp(kind, "argv") != 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider lowering command must be qstar.argv() or a string list");
	}
	n = lua_rawlen(L, -1);
	if (n == 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider lowering command must not be empty");
	}
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		item = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
		if (!item) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: provider lowering command contains non-string argv item");
		}
		if (qstar_string_list_push(&action->argv, item) < 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
provider_env_assignment_ok(const char *item)
{
	const unsigned char *p;
	const char *eq;

	if (!item || !*item)
		return 0;
	eq = strchr(item, '=');
	if (!eq || eq == item)
		return 0;
	p = (const unsigned char *)item;
	if (!isalpha(*p) && *p != '_')
		return 0;
	for (; (const char *)p < eq; p++) {
		if (!isalnum(*p) && *p != '_')
			return 0;
	}
	return 1;
}

static int
validate_provider_env_result(struct qstar_graph *graph,
    const struct qstar_string_list *env)
{
	size_t i;

	for (i = 0; env && i < env->len; i++) {
		if (!provider_env_assignment_ok(env->items[i]))
			return qstar_set_error(graph,
			    "qstar: provider lowering env[%zu] must be NAME=value",
			    i);
	}
	return 0;
}

static int
provider_outputs_contain_path(const struct qstar_provider_action_template *action,
    const char *path)
{
	size_t i;

	for (i = 0; i < action->outputs.len; i++) {
		if (strcmp(action->outputs.items[i], path) == 0)
			return 1;
	}
	return 0;
}

static int
read_provider_lowering_result(lua_State *L, int result,
    struct qstar_provider_action_template *action, struct qstar_graph *graph,
    const struct qstar_language_unit_schema *unit, const char *source,
    const char *required_output, const char *required_output_name,
    const char *depfile)
{
	const char *explicit_depfile;

	result = qstar_lua_abs_index(L, result);
	if (!lua_istable(L, result))
		return qstar_set_error(graph,
		    "qstar: provider lowering must return an action table");
	if (read_provider_command_result(L, result, action, graph) < 0 ||
	    read_provider_string_list_result(L, result, &action->env, graph,
	    "env", NULL) < 0 ||
	    read_provider_string_list_result(L, result, &action->inputs, graph,
	    "inputs", source) < 0 ||
	    read_provider_string_list_result(L, result, &action->outputs, graph,
	    "outputs", required_output) < 0)
		return -1;
	if (validate_provider_env_result(graph, &action->env) < 0)
		return -1;
	if (!provider_outputs_contain_path(action, required_output))
		return qstar_set_error(graph,
		    "qstar: provider lowering outputs must include ctx.output(\"%s\")",
		    required_output_name);
	lua_getfield(L, result, "depfile");
	explicit_depfile = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	if (!lua_isnil(L, -1) && !explicit_depfile) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider lowering depfile must be a string");
	}
	action->wants_depfile = explicit_depfile ||
	    (unit && unit->deps && strcmp(unit->deps, "make") == 0);
	action->depfile = qstar_strdup(explicit_depfile ? explicit_depfile :
	    action->wants_depfile ? depfile : "");
	lua_pop(L, 1);
	if (!action->depfile)
		return qstar_set_error(graph, "qstar: out of memory");
	return 0;
}

static int
lower_provider_source_unit(lua_State *L, int source_token,
    struct qstar_target *target, struct qstar_graph *graph, size_t source_index,
    const char *path, const char *provider_name, const char *unit_name,
    const struct qstar_language_unit_schema *unit,
    struct qstar_provider_action_template *action)
{
	struct qstar_provider_lowering_ctx lowering;
	const struct qstar_language_provider *provider;
	char object[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], object_dir[QSTAR_PATH_MAX];
	const char *message;
	int rc;

	(void)unit_name;
	provider = qstar_graph_find_language_provider(graph, provider_name);
	if (!provider)
		return qstar_set_error(graph,
		    "qstar: source token uses inactive language provider '%s'",
		    provider_name);
	if (qstar_graph_object_output_path(graph, target, source_index, object,
	    sizeof(object)) < 0 ||
	    qstar_graph_depfile_output_path(graph, target, source_index, depfile,
	    sizeof(depfile)) < 0)
		return qstar_set_error(graph, "qstar: provider source output path too long");
	if (qstar_dirname(object, object_dir, sizeof(object_dir)) < 0)
		return qstar_set_error(graph, "qstar: provider cache path too long");
	memset(&lowering, 0, sizeof(lowering));
	lowering.graph = graph;
	lowering.target = target;
	lowering.provider = provider;
	lowering.unit = unit;
	lowering.source = path;
	lowering.object = object;
	lowering.depfile = depfile;
	if (qstar_path_join(object_dir, "cache", lowering.cache_base,
	    sizeof(lowering.cache_base)) < 0)
		return qstar_set_error(graph, "qstar: provider cache path too long");
	lowering.source_options_ref = LUA_NOREF;
	if (source_token != 0) {
		source_token = qstar_lua_abs_index(L, source_token);
		lua_getfield(L, source_token, "options");
		if (lua_istable(L, -1))
			lowering.source_options_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		else
			lua_pop(L, 1);
	}
	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.provider.implementations");
	if (!lua_istable(L, -1)) {
		if (lowering.source_options_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, lowering.source_options_ref);
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: provider implementation registry is empty");
	}
	lua_getfield(L, -1, provider_name);
	lua_remove(L, -2);
	if (!lua_istable(L, -1)) {
		if (lowering.source_options_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, lowering.source_options_ref);
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider implementation for '%s' is not loaded",
		    provider_name);
	}
	lua_getfield(L, -1, unit->lower);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		if (lowering.source_options_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, lowering.source_options_ref);
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider '%s' implementation has no lowering function '%s'",
		    provider_name, unit->lower);
	}
	push_provider_lowering_context(L, &lowering);
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		message = lua_tostring(L, -1);
		if (lowering.source_options_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, lowering.source_options_ref);
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: provider lowering failed: %s",
		    message ? message : "<unknown>");
	}
	rc = read_provider_lowering_result(L, -1, action, graph, unit, path, object,
	    "object", depfile);
	lua_pop(L, 1);
	if (lowering.source_options_ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, lowering.source_options_ref);
	return rc;
}

static const char *
provider_final_kind_for_target(const char *kind)
{
	if (!kind)
		return NULL;
	if (strcmp(kind, "exe") == 0 || strcmp(kind, "test") == 0)
		return "executable";
	if (strcmp(kind, "staticlib") == 0)
		return "staticlib";
	if (strcmp(kind, "sharedlib") == 0)
		return "sharedlib";
	return NULL;
}

static int
target_has_native_final_inputs(const struct qstar_target *target)
{
	return target->deps.len || target->private_deps.len || target->libs.len ||
	    target->lib_dirs.len || target->frameworks.len || target->link_options.len ||
	    target->link_inputs.len;
}

static const struct qstar_language_final_schema *
target_provider_final_schema(struct qstar_graph *graph,
    const struct qstar_target *target, const struct qstar_language_provider **provider_out,
    const char **kind_out)
{
	const struct qstar_provider_source_unit *unit;
	const struct qstar_language_provider *provider;
	const struct qstar_language_final_schema *final;
	const char *provider_name, *kind;
	size_t i;

	if (!target || target->sources.len == 0 || target_has_native_final_inputs(target))
		return NULL;
	kind = provider_final_kind_for_target(target->kind);
	if (!kind)
		return NULL;
	provider_name = NULL;
	for (i = 0; i < target->sources.len; i++) {
		unit = qstar_target_provider_source_unit(target, i);
		if (!unit)
			return NULL;
		if (!provider_name) {
			provider_name = unit->provider;
		} else if (strcmp(provider_name, unit->provider) != 0) {
			return NULL;
		}
	}
	if (!provider_name || qstar_language_provider_is_preloaded(provider_name))
		return NULL;
	provider = qstar_graph_find_language_provider(graph, provider_name);
	if (!provider)
		return NULL;
	final = qstar_language_provider_find_final(provider, kind);
	if (!final)
		return NULL;
	if (provider_out)
		*provider_out = provider;
	if (kind_out)
		*kind_out = kind;
	return final;
}

static int
lower_provider_final_action(lua_State *L, struct qstar_target *target,
    struct qstar_graph *graph)
{
	struct qstar_provider_lowering_ctx lowering;
	struct qstar_provider_action_template action;
	const struct qstar_language_provider *provider;
	const struct qstar_language_final_schema *final;
	const char *kind, *message;
	char artifact[QSTAR_PATH_MAX], depfile[QSTAR_PATH_MAX], artifact_dir[QSTAR_PATH_MAX];
	int rc;

	final = target_provider_final_schema(graph, target, &provider, &kind);
	if (!final)
		return 0;
	if (qstar_graph_artifact_output_path(graph, target, artifact,
	    sizeof(artifact)) < 0)
		return qstar_set_error(graph, "qstar: provider final artifact path too long");
	if (snprintf(depfile, sizeof(depfile), "%s.d", artifact) >=
	    (int)sizeof(depfile))
		return qstar_set_error(graph, "qstar: provider final depfile path too long");
	if (qstar_dirname(artifact, artifact_dir, sizeof(artifact_dir)) < 0)
		return qstar_set_error(graph, "qstar: provider final cache path too long");
	memset(&lowering, 0, sizeof(lowering));
	lowering.graph = graph;
	lowering.target = target;
	lowering.provider = provider;
	lowering.final_kind = kind;
	lowering.sources = &target->sources;
	lowering.artifact = artifact;
	lowering.depfile = depfile;
	lowering.source_options_ref = LUA_NOREF;
	if (qstar_path_join(artifact_dir, "cache", lowering.cache_base,
	    sizeof(lowering.cache_base)) < 0)
		return qstar_set_error(graph, "qstar: provider final cache path too long");
	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.provider.implementations");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: provider implementation registry is empty");
	}
	lua_getfield(L, -1, provider->namespace);
	lua_remove(L, -2);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider implementation for '%s' is not loaded",
		    provider->namespace);
	}
	lua_getfield(L, -1, final->lower);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: provider '%s' implementation has no final lowering function '%s'",
		    provider->namespace, final->lower);
	}
	push_provider_lowering_context(L, &lowering);
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		message = lua_tostring(L, -1);
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: provider final lowering failed: %s",
		    message ? message : "<unknown>");
	}
	memset(&action, 0, sizeof(action));
	rc = read_provider_lowering_result(L, -1, &action, graph, NULL, NULL, artifact,
	    "artifact", depfile);
	lua_pop(L, 1);
	if (rc == 0)
		rc = qstar_target_set_provider_final_action(graph, target,
		    provider->namespace, kind, final->lower, &action);
	qstar_string_list_free(&action.argv);
	qstar_string_list_free(&action.env);
	qstar_string_list_free(&action.inputs);
	qstar_string_list_free(&action.outputs);
	free(action.depfile);
	return rc;
}

static int
read_toolset_role_command(lua_State *L, int idx, struct qstar_toolset *toolset,
    struct qstar_graph *graph, const char *role, const char *field)
{
	struct qstar_string_list *list;

	if (!valid_tool_role_name(role))
		return qstar_set_error(graph, "qstar: invalid toolset tool role '%s'",
		    role ? role : "<non-string>");
	list = qstar_toolset_add_role(graph, toolset, role);
	if (!list)
		return -1;
	return read_cli_command(L, idx, list, graph, field);
}

static int
read_toolset_provider_tools(lua_State *L, int table, struct qstar_toolset *toolset,
    struct qstar_graph *graph, const char *namespace, size_t *count)
{
	const char *subrole, *kind;
	char role[128], field[128];

	if (!valid_tool_role_component(namespace))
		return qstar_set_error(graph, "qstar: invalid toolset provider namespace '%s'",
		    namespace ? namespace : "<non-string>");
	if (toolset_core_role(namespace))
		return qstar_set_error(graph,
		    "qstar: tools.%s is a core role and must be qstar.cli { ... }",
		    namespace);
	if (!qstar_graph_language_provider_is_available(graph, namespace))
		return qstar_set_error(graph,
		    "qstar: unknown toolset provider namespace tools.%s; activate a language provider with qstar.use_language(\"%s\")",
		    namespace, namespace);
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return qstar_set_error(graph,
		    "qstar: tools.%s must be a provider tool table", namespace);
	kind = qstar_table_kind(L, table);
	if (kind) {
		if (strcmp(kind, "cli") == 0 && toolset_removed_direct_role(namespace))
			return qstar_set_error(graph,
			    "qstar: tools.%s direct compiler role is removed; use tools.%s = { compiler = qstar.cli { ... } }",
			    namespace, namespace);
		return qstar_set_error(graph,
		    "qstar: tools.%s must be a provider tool table, not qstar.%s",
		    namespace, kind);
	}
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		subrole = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(subrole)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: invalid provider tool role tools.%s.%s",
			    namespace, subrole ? subrole : "<non-string>");
		}
		if (snprintf(role, sizeof(role), "%s.%s", namespace, subrole) >=
		    (int)sizeof(role) ||
		    snprintf(field, sizeof(field), "tools.%s.%s", namespace, subrole) >=
		    (int)sizeof(field)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: toolset tool role is too long");
		}
		if (read_toolset_role_command(L, -1, toolset, graph, role, field) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		(*count)++;
		lua_pop(L, 1);
	}
	return 0;
}

/** qstar.toolset tools table을 core role과 provider namespace role map으로 읽는다. */
static int
read_toolset_tools(lua_State *L, int table, struct qstar_toolset *toolset,
    struct qstar_graph *graph)
{
	const char *role, *kind;
	size_t count;
	char field[64];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "tools");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: toolset '%s' requires tools table",
		    toolset->label);
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'tools' must be a table");
	}
	count = 0;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		role = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!role) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: toolset tools keys must be strings");
		}
		kind = qstar_table_kind(L, -1);
		if (toolset_core_role(role)) {
			if (snprintf(field, sizeof(field), "tools.%s", role) >=
			    (int)sizeof(field)) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: toolset tool role is too long");
			}
			if (read_toolset_role_command(L, -1, toolset, graph, role,
			    field) < 0) {
				lua_pop(L, 2);
				return -1;
			}
			count++;
		} else if (kind && strcmp(kind, "cli") == 0) {
			if (toolset_removed_direct_role(role)) {
				lua_pop(L, 2);
				return qstar_set_error(graph,
				    "qstar: tools.%s direct compiler role is removed; use tools.%s = { compiler = qstar.cli { ... } }",
				    role, role);
			}
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: unknown direct tool role '%s'; use archive/link or a provider namespace table",
			    role);
		} else {
			if (read_toolset_provider_tools(L, -1, toolset, graph, role,
			    &count) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	if (count == 0)
		return qstar_set_error(graph,
		    "qstar: toolset '%s' must declare at least one tool role",
		    toolset->label);
	qstar_toolset_sort_roles(toolset);
	return 0;
}

/** Lua qstar.toolset 선언을 Graph IR toolset declaration으로 낮춘다. */
static int
add_toolset(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_toolset *toolset;
	struct qstar_graph *graph;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.toolset") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: toolset name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid toolset name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >=
		    (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label,
		    sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid toolset name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	toolset = qstar_graph_add_toolset(graph, label, name, fragment_dir, origin_file,
	    origin_line);
	if (!toolset)
		return luaL_error(L, "%s", graph->error);
	if (validate_toolset_fields(L, table_index, graph) < 0 ||
	    read_toolset_tools(L, table_index, toolset, graph) < 0 ||
	    read_string_or_bool_field(L, table_index, "response_files",
	    &toolset->response_files, graph) < 0 ||
	    read_string_or_bool_field(L, table_index, "response_style",
	    &toolset->response_style, graph) < 0 ||
	    read_string_or_bool_field(L, table_index, "allow_absolute_tools",
	    &toolset->allow_absolute_tools, graph) < 0 ||
	    read_list_field(L, table_index, "path_tools", &toolset->path_tools, graph,
	    0, toolset->fragment_dir) < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

/** qstar.toolset "name" { ... } 형태의 후행 table call을 처리한다. */
static int
qstar_lua_toolset_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_toolset(L, name, 1, fragment_dir);
}

/** qstar.toolset API entry point다. */
static int
qstar_lua_toolset(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.toolset") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_toolset_finish, 2);
		return 1;
	}
	return add_toolset(L, name, 2, ctx->current_dir);
}

/** qstar.config table에서 config primitive가 받을 수 있는 top-level field만 허용한다. */
static int
validate_config_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"lang", "libs", "lib_dirs", "link", "link_options", "link_inputs",
		"toolset", "artifact_name", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: unknown config field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

/** Lua qstar.config 선언을 reusable Graph IR config로 낮춘다. */
static int
add_config(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_config *config;
	struct qstar_graph *graph;
	const char *artifact_name;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line, has_toolset;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.config") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: config name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid config name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >=
		    (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label,
		    sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid config name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	config = qstar_graph_add_config(graph, label, name, fragment_dir, origin_file,
	    origin_line);
	if (!config)
		return luaL_error(L, "%s", graph->error);
	if (validate_config_fields(L, table_index, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "libs", &config->options.libs, graph, 0,
	    config->options.fragment_dir) < 0 ||
	    read_list_field(L, table_index, "lib_dirs", &config->options.lib_dirs, graph,
	    0, config->options.fragment_dir) < 0 ||
	    read_link_options(L, table_index, &config->options, graph, "config") < 0 ||
	    read_list_field(L, table_index, "link_options", &config->options.link_options,
	    graph, 0, config->options.fragment_dir) < 0 ||
	    read_target_file_list_field(L, table_index, "link_inputs",
	    &config->options.link_inputs, graph) < 0 ||
	    read_lang_options(L, table_index, &config->options, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	config->has_cxx_standard = nested_lang_field_present(L, table_index, "cxx",
	    "standard");
	config->has_asm_preprocess = nested_lang_field_present(L, table_index, "asm",
	    "preprocess");
	config->has_cxx_modules = nested_lang_field_present(L, table_index, "cxx",
	    "modules");
	artifact_name = check_string_field(L, table_index, "artifact_name");
	if (read_label_scalar_field(L, table_index, "toolset", &config->options.toolset,
	    graph, config->fragment_dir, &has_toolset) < 0)
		return luaL_error(L, "%s", graph->error);
	if (has_toolset)
		config->has_toolset = 1;
	if (artifact_name) {
		if (!valid_target_artifact_name(artifact_name))
			return luaL_error(L,
			    "qstar: artifact_name '%s' must be a filename, not a path",
			    artifact_name);
		config->has_artifact_name = 1;
		if (replace_lua_string(&config->options.artifact_name, artifact_name,
		    graph) < 0)
			return luaL_error(L, "%s", graph->error);
	}
	return 0;
}

/** qstar.config "name" { ... } 형태의 후행 table call을 처리한다. */
static int
qstar_lua_config_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_config(L, name, 1, fragment_dir);
}

/** qstar.config API entry point다. */
static int
qstar_lua_config(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.config") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_config_finish, 2);
		return 1;
	}
	return add_config(L, name, 2, ctx->current_dir);
}

static int
validate_target_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"kind", "configs", "sources", "deps", "public_deps", "private_deps",
		"visibility", "libs", "lib_dirs", "link", "link_options",
		"link_inputs", "lang", "toolset",
		"artifact_name", "inputs", "command", "description", "timeout", "expect", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: unknown target field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

/** run_target expect table은 contains와 선택적 file만 허용한다. */
static int
read_run_expect_field(lua_State *L, int table, struct qstar_target *target,
    struct qstar_graph *graph)
{
	static const char *const allowed[] = { "contains", "file", NULL };
	const char *key, *contains, *file;
	size_t len, i;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "expect");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: run_target expect must be a table with contains and optional file");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: unknown run_target expect field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	lua_getfield(L, -1, "contains");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: run_target expect.contains must be a string");
	}
	contains = lua_tolstring(L, -1, &len);
	if (!contains || len == 0) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: run_target expect.contains must not be empty");
	}
	for (i = 0; i < len; i++) {
		if (contains[i] == '\n' || contains[i] == '\r') {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target expect.contains must be one line");
		}
	}
	if (replace_lua_string(&target->run_expect_contains, contains, graph) < 0) {
		lua_pop(L, 2);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "file");
	if (!lua_isnil(L, -1)) {
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target expect.file must be a package-relative string");
		}
		file = lua_tostring(L, -1);
		if (!file || !*file) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target expect.file must not be empty");
		}
		if (!qstar_path_is_package_relative(file)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: run_target expect.file must be package-relative");
		}
		if (replace_lua_string(&target->run_expect_file, file, graph) < 0) {
			lua_pop(L, 2);
			return -1;
		}
	}
	lua_pop(L, 2);
	return 0;
}

static int
add_target(lua_State *L, const char *name, int table_index, const char *default_kind,
    const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_target *target;
	struct qstar_graph *graph;
	const char *kind, *artifact_name;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "target declaration") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	kind = check_string_field(L, table_index, "kind");
	if (default_kind && strcmp(default_kind, "group") == 0 && kind && *kind &&
	    strcmp(kind, "group") != 0)
		return luaL_error(L, "qstar: qstar.group target '%s' cannot override kind",
		    name);
	if (!kind || !*kind)
		kind = default_kind && *default_kind ? default_kind : "target";
	if (!name[0])
		return luaL_error(L, "qstar: target name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid target name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >= (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid target name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	target = qstar_graph_add_target(graph, label, name, kind, fragment_dir, origin_file,
	    origin_line);
	if (!target)
		return luaL_error(L, "%s", graph->error);
	if (validate_target_fields(L, table_index, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (strcmp(target->kind, "group") == 0 &&
	    reject_group_action_fields(L, table_index, graph, target->label) < 0)
		return luaL_error(L, "%s", graph->error);
	if (reject_non_run_inputs_field(L, table_index, graph, target) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "configs", &target->configs, graph, 1,
	    target->fragment_dir) < 0 ||
	    qstar_graph_apply_target_configs(graph, target) < 0 ||
	    read_lang_options(L, table_index, target, graph) < 0 ||
	    read_sources_field(L, table_index, "sources", target, graph) < 0 ||
	    read_list_field(L, table_index, "deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "public_deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "private_deps", &target->private_deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "visibility", &target->visibility, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "libs", &target->libs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "lib_dirs", &target->lib_dirs, graph, 0, target->fragment_dir) < 0 ||
	    read_link_options(L, table_index, target, graph, "target") < 0 ||
	    read_list_field(L, table_index, "link_options", &target->link_options, graph, 0, target->fragment_dir) < 0 ||
	    read_target_file_list_field(L, table_index, "link_inputs",
	    &target->link_inputs, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	artifact_name = check_string_field(L, table_index, "artifact_name");
	if (read_label_scalar_field(L, table_index, "toolset", &target->toolset, graph,
	    target->fragment_dir, NULL) < 0)
		return luaL_error(L, "%s", graph->error);
	if (artifact_name) {
		if (!valid_target_artifact_name(artifact_name))
			return luaL_error(L,
			    "qstar: artifact_name '%s' must be a filename, not a path",
			    artifact_name);
		free(target->artifact_name);
		target->artifact_name = qstar_strdup(artifact_name);
	}
	if (lower_provider_final_action(L, target, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (strcmp(target->kind, "run_target") == 0) {
		struct qstar_string_list empty;

		memset(&empty, 0, sizeof(empty));
		if (read_status_description_field(L, table_index, graph,
		    &target->description) < 0 ||
		    read_run_inputs_field(L, table_index, target, graph) < 0 ||
		    read_command_field(L, table_index, "command", &target->run_command,
		    graph) < 0 ||
		    resolve_cli_placeholders(graph, &target->run_command,
		    &target->run_inputs, &empty, "run_target command") < 0)
			return luaL_error(L, "%s", graph->error);
		if (target->run_command.len == 0)
			return luaL_error(L,
			    "qstar: run_target '%s' requires command = qstar.cli { ... }",
			    target->label);
		target->run_timeout_sec = check_int_field(L, table_index, "timeout", 0);
			if (target->run_timeout_sec < 0)
				return luaL_error(L, "qstar: run_target '%s' timeout must be >= 0",
				    target->label);
			if (read_run_expect_field(L, table_index, target, graph) < 0)
				return luaL_error(L, "%s", graph->error);
		}
	if (!target->toolchain || !target->toolset || !target->stdlib_policy ||
	    !target->artifact_name || !target->cxx_standard ||
	    !target->run_expect_contains || !target->run_expect_file)
		return luaL_error(L, "qstar: out of memory");
	return 0;
}

static int
qstar_lua_target_finish(lua_State *L)
{
	const char *name, *kind, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	kind = lua_tostring(L, lua_upvalueindex(2));
	fragment_dir = lua_tostring(L, lua_upvalueindex(3));
	return add_target(L, name, 1, kind, fragment_dir);
}

static int
qstar_lua_target(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name, *default_kind;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "target declaration") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	default_kind = lua_tostring(L, lua_upvalueindex(1));
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, default_kind ? default_kind : "");
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_target_finish, 3);
		return 1;
	}
	return add_target(L, name, 2, default_kind, ctx->current_dir);
}

static int
add_genrule(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_genrule *genrule;
	struct qstar_graph *graph;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.custom_target") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: generated action name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid generated action name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >= (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid generated action name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	genrule = qstar_graph_add_genrule(graph, label, name, fragment_dir, origin_file,
	    origin_line);
	if (!genrule)
		return luaL_error(L, "%s", graph->error);
	if (legacy_field_present(L, table_index, "tool") ||
	    legacy_field_present(L, table_index, "args"))
		return luaL_error(L,
		    "qstar: custom_target uses command = qstar.cli { ... }; tool/args are removed");
	if (read_genrule_inputs_field(L, table_index, genrule, graph) < 0 ||
	    read_outputs_field(L, table_index, genrule, graph) < 0 ||
	    finish_generated_command(L, table_index, genrule, graph,
	    "custom_target", "custom_target command") < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

static int
add_transform(lua_State *L, const char *name, int table_index,
    const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_genrule *genrule;
	struct qstar_graph *graph;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.transform") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: transform name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label,
		    sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid transform name '%s'",
			    name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >=
		    (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label,
		    sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid transform name '%s'",
			    name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	genrule = qstar_graph_add_genrule(graph, label, name, fragment_dir,
	    origin_file, origin_line);
	if (!genrule)
		return luaL_error(L, "%s", graph->error);
	if (validate_transform_fields(L, table_index, graph) < 0 ||
	    read_transform_input_field(L, table_index, genrule, graph) < 0 ||
	    read_transform_output_field(L, table_index, genrule, graph) < 0 ||
	    finish_generated_command(L, table_index, genrule, graph,
	    "transform", "transform command") < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

/** qstar.configure_file 선언을 generated header action으로 graph에 추가한다. */
static int
add_config_header(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_genrule *genrule;
	struct qstar_graph *graph;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	const char *output;
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.configure_file") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: config header name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid config header name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >= (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid config header name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	genrule = qstar_graph_add_genrule(graph, label, name, fragment_dir, origin_file,
	    origin_line);
	if (!genrule)
		return luaL_error(L, "%s", graph->error);
	genrule->config_header = 1;
	free(genrule->tool);
	genrule->tool = qstar_strdup("<qstar-config-header>");
	if (!genrule->tool)
		return luaL_error(L, "qstar: out of memory");
	output = check_string_field(L, table_index, "output");
	if (!output || !*output)
		return luaL_error(L, "qstar: config header '%s' requires output", name);
	if (qstar_string_list_push(&genrule->outputs, output) < 0)
		return luaL_error(L, "qstar: out of memory");
	if (read_status_description_field(L, table_index, graph,
	    &genrule->description) < 0 ||
	    read_list_field(L, table_index, "defines", &genrule->args, graph, 0,
	    genrule->fragment_dir) < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

static int
qstar_lua_genrule_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_genrule(L, name, 1, fragment_dir);
}

static int
qstar_lua_genrule(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.custom_target") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_genrule_finish, 2);
		return 1;
	}
	return add_genrule(L, name, 2, ctx->current_dir);
}

static int
qstar_lua_transform_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_transform(L, name, 1, fragment_dir);
}

static int
qstar_lua_transform(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.transform") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_transform_finish, 2);
		return 1;
	}
	return add_transform(L, name, 2, ctx->current_dir);
}

static int
qstar_lua_config_header_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_config_header(L, name, 1, fragment_dir);
}

static int
qstar_lua_config_header(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.configure_file") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_config_header_finish, 2);
		return 1;
	}
	return add_config_header(L, name, 2, ctx->current_dir);
}

/** qstar.stage_file(src, dst) table에서 source token 문자열을 만든다. */
static int
stage_file_src_token(lua_State *L, int idx, char *dst, size_t dstlen,
    struct qstar_graph *graph)
{
	const char *s, *kind;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (lua_isstring(L, idx)) {
		s = lua_tostring(L, idx);
		return snprintf(dst, dstlen, "%s", s) < (int)dstlen ? 0 :
		    qstar_set_error(graph, "qstar: stage_file source is too long");
	}
	if (lua_istable(L, idx)) {
		kind = qstar_table_kind(L, idx);
		if (kind && strcmp(kind, "target_file") == 0 &&
		    format_placeholder_token(L, idx, dst, dstlen) == 0)
			return 0;
	}
	return qstar_set_error(graph,
	    "qstar: qstar.stage_file source must be a string or qstar.target_file(...)");
}

/** qstar.stage files field를 stage_file edge 목록으로 읽는다. */
static int
read_stage_files_field(lua_State *L, int table, struct qstar_stage *stage,
    struct qstar_graph *graph)
{
	char src[QSTAR_PATH_MAX];
	const char *dst, *kind;
	size_t i, n;

	lua_getfield(L, table, "files");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: stage '%s' requires files = { ... }",
		    stage->label);
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: stage '%s' files contains non-stage_file item",
			    stage->label);
		}
		kind = qstar_table_kind(L, -1);
		if (!kind || strcmp(kind, "stage_file") != 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: stage '%s' files contains non-stage_file item",
			    stage->label);
		}
		lua_getfield(L, -1, "src");
		if (stage_file_src_token(L, -1, src, sizeof(src), graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		dst = check_string_field(L, -1, "dst");
		if (!dst || !*dst) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: stage '%s' stage_file destination is empty",
			    stage->label);
		}
		if (qstar_string_list_push(&stage->srcs, src) < 0 ||
		    qstar_string_list_push(&stage->dsts, dst) < 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

/** qstar.stage 선언을 copy-only stage rule로 graph에 추가한다. */
static int
add_stage(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	struct qstar_stage *stage;
	const char *root;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.stage") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	if (!name[0])
		return luaL_error(L, "qstar: stage name is empty");
	if (name[0] == ':' || name[0] == '/' || name[0] == '@') {
		if (qstar_label_canonicalize(name, ctx->current_dir, label, sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid stage name '%s'", name);
	} else {
		if (snprintf(rawlabel, sizeof(rawlabel), ":%s", name) >=
		    (int)sizeof(rawlabel) ||
		    qstar_label_canonicalize(rawlabel, ctx->current_dir, label,
		    sizeof(label)) < 0)
			return luaL_error(L, "qstar: invalid stage name '%s'", name);
	}
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	stage = qstar_graph_add_stage(graph, label, name, fragment_dir, origin_file,
	    origin_line);
	if (!stage)
		return luaL_error(L, "%s", graph->error);
	root = check_string_field(L, table_index, "root");
	if (root) {
		free(stage->root);
		stage->root = qstar_strdup(root);
		if (!stage->root)
			return luaL_error(L, "qstar: out of memory");
	}
	if (read_status_description_field(L, table_index, graph, &stage->description) < 0 ||
	    read_stage_files_field(L, table_index, stage, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

static int
qstar_lua_stage_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_stage(L, name, 1, fragment_dir);
}

static int
qstar_lua_stage(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.stage") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_stage_finish, 2);
		return 1;
	}
	return add_stage(L, name, 2, ctx->current_dir);
}

static int
qstar_lua_stage_file(lua_State *L)
{
	struct qstar_lua_context *ctx;
	char src[QSTAR_PATH_MAX];
	const char *dst;

	ctx = get_context(L);
	if (stage_file_src_token(L, 1, src, sizeof(src), ctx->graph) < 0)
		return luaL_error(L, "%s", ctx->graph->error);
	dst = luaL_checkstring(L, 2);
	lua_newtable(L);
	lua_pushstring(L, "stage_file");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, src);
	lua_setfield(L, -2, "src");
	lua_pushstring(L, dst);
	lua_setfield(L, -2, "dst");
	return 1;
}

/** qstar.target_family 선언을 lint grouping primitive로 graph에 추가한다. */
static int
add_target_family(lua_State *L, const char *name, int table_index,
    const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	struct qstar_target_family *family;
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.target_family") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	family = qstar_graph_add_target_family(graph, name, fragment_dir, origin_file,
	    origin_line);
	if (!family)
		return luaL_error(L, "%s", graph->error);
	family->allow_shared_sources = check_bool_field(L, table_index,
	    "allow_shared_sources", 0);
	if (read_list_field(L, table_index, "variants", &family->variants, graph, 0,
	    fragment_dir) < 0 ||
	    read_list_field(L, table_index, "targets", &family->targets, graph, 1,
	    fragment_dir) < 0)
		return luaL_error(L, "%s", graph->error);
	if (family->variants.len == 0 && family->targets.len == 0)
		return luaL_error(L,
		    "qstar: target_family '%s' requires variants or targets", name);
	return 0;
}

static int
qstar_lua_target_family_finish(lua_State *L)
{
	const char *name, *fragment_dir;

	name = lua_tostring(L, lua_upvalueindex(1));
	fragment_dir = lua_tostring(L, lua_upvalueindex(2));
	return add_target_family(L, name, 1, fragment_dir);
}

static int
qstar_lua_target_family(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.target_family") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_target_family_finish, 2);
		return 1;
	}
	return add_target_family(L, name, 2, ctx->current_dir);
}

/** Lua stack index를 push/pop에 흔들리지 않는 absolute index로 바꾼다. */
static int
qstar_lua_abs_index(lua_State *L, int idx)
{
	if (idx < 0 && idx > LUA_REGISTRYINDEX)
		return lua_gettop(L) + idx + 1;
	return idx;
}

/** QStar metadata table이 아닌 일반 Lua table인지 검사한다. */
static int
qstar_lua_plain_table(lua_State *L, int idx)
{
	idx = qstar_lua_abs_index(L, idx);
	return lua_istable(L, idx) && !qstar_table_kind(L, idx);
}

/** table이 1..n integer key만 가진 list 형태인지 검사한다. */
static int
qstar_lua_array_table(lua_State *L, int idx)
{
	size_t n;
	lua_Integer key;

	idx = qstar_lua_abs_index(L, idx);
	if (!qstar_lua_plain_table(L, idx))
		return 0;
	n = lua_rawlen(L, idx);
	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		if (!lua_isinteger(L, -2)) {
			lua_pop(L, 2);
			return 0;
		}
		key = lua_tointeger(L, -2);
		if (key < 1 || (size_t)key > n) {
			lua_pop(L, 2);
			return 0;
		}
		lua_pop(L, 1);
	}
	return 1;
}

/** Lua value를 새 table에 안전하게 복사하기 위한 재귀 copy primitive다. */
static void
qstar_lua_push_value_copy(lua_State *L, int idx, int depth)
{
	int src, dst;

	if (depth > 64)
		luaL_error(L, "qstar: helper table is nested too deeply");
	src = qstar_lua_abs_index(L, idx);
	if (!lua_istable(L, src)) {
		lua_pushvalue(L, src);
		return;
	}
	lua_newtable(L);
	dst = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, src) != 0) {
		qstar_lua_push_value_copy(L, -2, depth + 1);
		qstar_lua_push_value_copy(L, -2, depth + 1);
		lua_rawset(L, dst);
		lua_pop(L, 1);
	}
}

/** list table 끝에 value copy 하나를 추가한다. */
static void
qstar_lua_append_value_copy(lua_State *L, int dst, int value, size_t *next,
    int depth)
{
	dst = qstar_lua_abs_index(L, dst);
	value = qstar_lua_abs_index(L, value);
	qstar_lua_push_value_copy(L, value, depth + 1);
	lua_rawseti(L, dst, (lua_Integer)(*next));
	(*next)++;
}

/** src list의 array part를 dst list 뒤에 copy해서 붙인다. */
static void
qstar_lua_append_list_copy(lua_State *L, int dst, int src, size_t *next,
    int depth)
{
	size_t i, n;

	dst = qstar_lua_abs_index(L, dst);
	src = qstar_lua_abs_index(L, src);
	n = lua_rawlen(L, src);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, src, (lua_Integer)i);
		qstar_lua_append_value_copy(L, dst, -1, next, depth + 1);
		lua_pop(L, 1);
	}
}

/** dst table에 src table을 deep merge한다. list field는 append semantics를 쓴다. */
static void
qstar_lua_merge_into(lua_State *L, int dst, int src, int depth)
{
	int key_idx, value_idx, existing_idx;
	size_t next;

	if (depth > 64)
		luaL_error(L, "qstar: helper table is nested too deeply");
	dst = qstar_lua_abs_index(L, dst);
	src = qstar_lua_abs_index(L, src);
	lua_pushnil(L);
	while (lua_next(L, src) != 0) {
		key_idx = lua_gettop(L) - 1;
		value_idx = lua_gettop(L);
		if (qstar_lua_plain_table(L, value_idx)) {
			lua_pushvalue(L, key_idx);
			lua_gettable(L, dst);
			existing_idx = lua_gettop(L);
			if (qstar_lua_plain_table(L, existing_idx)) {
				if (qstar_lua_array_table(L, existing_idx) &&
				    qstar_lua_array_table(L, value_idx)) {
					next = lua_rawlen(L, existing_idx) + 1;
					qstar_lua_append_list_copy(L, existing_idx,
					    value_idx, &next, depth + 1);
				} else {
					qstar_lua_merge_into(L, existing_idx,
					    value_idx, depth + 1);
				}
				lua_pop(L, 1);
			} else {
				lua_pop(L, 1);
				qstar_lua_push_value_copy(L, key_idx, depth + 1);
				qstar_lua_push_value_copy(L, value_idx, depth + 1);
				lua_settable(L, dst);
			}
		} else {
			qstar_lua_push_value_copy(L, key_idx, depth + 1);
			qstar_lua_push_value_copy(L, value_idx, depth + 1);
			lua_settable(L, dst);
		}
		lua_pop(L, 1);
	}
}

/** qstar.copy(table): authoring helper table을 deep copy해서 반환한다. */
static int
qstar_lua_copy(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	qstar_lua_push_value_copy(L, 1, 0);
	return 1;
}

/** qstar.append(list, ...): list를 mutate하지 않고 값을 뒤에 붙인 새 list를 만든다. */
static int
qstar_lua_append(lua_State *L)
{
	int top, i;
	size_t next;

	luaL_checktype(L, 1, LUA_TTABLE);
	if (!qstar_lua_array_table(L, 1))
		return luaL_error(L, "qstar: qstar.append first argument must be a list");
	lua_newtable(L);
	next = 1;
	qstar_lua_append_list_copy(L, -1, 1, &next, 0);
	top = lua_gettop(L) - 1;
	for (i = 2; i <= top; i++) {
		if (qstar_lua_array_table(L, i))
			qstar_lua_append_list_copy(L, -1, i, &next, 0);
		else
			qstar_lua_append_value_copy(L, -1, i, &next, 0);
	}
	return 1;
}

/** qstar.merge(...): plain table들을 새 table로 deep merge해서 반환한다. */
static int
qstar_lua_merge(lua_State *L)
{
	int top, i;
	size_t next;

	top = lua_gettop(L);
	lua_newtable(L);
	for (i = 1; i <= top; i++) {
		luaL_checktype(L, i, LUA_TTABLE);
		if (!qstar_lua_plain_table(L, i))
			return luaL_error(L,
			    "qstar: qstar.merge arguments must be plain tables");
		if (qstar_lua_array_table(L, -1) && qstar_lua_array_table(L, i)) {
			next = lua_rawlen(L, -1) + 1;
			qstar_lua_append_list_copy(L, -1, i, &next, 0);
		} else {
			qstar_lua_merge_into(L, -1, i, 0);
		}
	}
	return 1;
}

/** qstar.extend(base, ...): base table을 deep merge로 갱신하고 base를 반환한다. */
static int
qstar_lua_extend(lua_State *L)
{
	int top, i;
	size_t next;

	luaL_checktype(L, 1, LUA_TTABLE);
	if (!qstar_lua_plain_table(L, 1))
		return luaL_error(L, "qstar: qstar.extend base must be a plain table");
	top = lua_gettop(L);
	for (i = 2; i <= top; i++) {
		luaL_checktype(L, i, LUA_TTABLE);
		if (!qstar_lua_plain_table(L, i))
			return luaL_error(L,
			    "qstar: qstar.extend overlays must be plain tables");
		if (qstar_lua_array_table(L, 1) && qstar_lua_array_table(L, i)) {
			next = lua_rawlen(L, 1) + 1;
			qstar_lua_append_list_copy(L, 1, i, &next, 0);
		} else {
			qstar_lua_merge_into(L, 1, i, 0);
		}
	}
	lua_pushvalue(L, 1);
	return 1;
}

static int
qstar_lua_required_string_field(lua_State *L, int table, struct qstar_graph *graph,
    const char *owner, const char *field, char *dst, size_t dstlen)
{
	const char *value;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: %s.%s must be a string",
		    owner, field);
	}
	value = lua_tostring(L, -1);
	if (!value || !*value) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: %s.%s must not be empty",
		    owner, field);
	}
	if (dst && snprintf(dst, dstlen, "%s", value) >= (int)dstlen) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: %s.%s is too long", owner,
		    field);
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_list_of_strings(lua_State *L, int table, struct qstar_graph *graph,
    const char *owner, const char *field, int require_nonempty, int require_dot_prefix)
{
	const char *s;
	size_t i, n;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return qstar_set_error(graph, "qstar: %s.%s must be a list",
		    owner, field);
	n = lua_rawlen(L, table);
	if (require_nonempty && n == 0)
		return qstar_set_error(graph, "qstar: %s.%s must not be empty",
		    owner, field);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		s = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!s || !*s || (require_dot_prefix && s[0] != '.')) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s.%s contains invalid string item", owner,
			    field);
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
qstar_lua_validate_provider_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"api", "id", "version", "namespace", "implementation",
		"tools", "units", "finals", "options", "exports", "scaffold",
		"__qstar_kind", NULL
	};
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: unknown language provider manifest field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
qstar_lua_validate_provider_tools(lua_State *L, int table, struct qstar_graph *graph,
    const char *namespace)
{
	static const char *const allowed[] = { "role", "required", NULL };
	const char *name, *key, *role;
	char expected_prefix[128], owner[160];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "tools");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider tools must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(name) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider tools contains invalid tool '%s'",
			    name ? name : "<non-string>");
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || !string_in_set(key, allowed)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: unknown language provider tool field tools.%s.%s",
				    name, key ? key : "<non-string>");
			}
			lua_pop(L, 1);
		}
		snprintf(owner, sizeof(owner), "language provider tools.%s", name);
		if (qstar_lua_required_string_field(L, -1, graph, owner, "role",
		    NULL, 0) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "role");
		role = lua_tostring(L, -1);
		if (!valid_tool_role_name(role) ||
		    snprintf(expected_prefix, sizeof(expected_prefix), "%s.",
		    namespace) >= (int)sizeof(expected_prefix) ||
		    strncmp(role, expected_prefix, strlen(expected_prefix)) != 0) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider tools.%s.role must start with '%s.'",
			    name, namespace);
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "required");
		if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider tools.%s.required must be boolean",
			    name);
		}
		lua_pop(L, 1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_provider_units(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"suffixes", "emits", "lower", "deps", NULL
	};
	const char *name, *key, *emits;
	char owner[160];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "units");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider units must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(name) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider units contains invalid unit '%s'",
			    name ? name : "<non-string>");
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || !string_in_set(key, allowed)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: unknown language provider unit field units.%s.%s",
				    name, key ? key : "<non-string>");
			}
			lua_pop(L, 1);
		}
		snprintf(owner, sizeof(owner), "language provider units.%s", name);
		lua_getfield(L, -1, "suffixes");
		if (lua_isnil(L, -1) || qstar_lua_validate_list_of_strings(L, -1,
		    graph, owner, "suffixes", 1, 1) < 0) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider units.%s.suffixes must be a non-empty list",
			    name);
		}
		lua_pop(L, 1);
		if (qstar_lua_required_string_field(L, -1, graph, owner, "emits",
		    NULL, 0) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "emits");
		emits = lua_tostring(L, -1);
		if (strcmp(emits, "object") != 0) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider units.%s.emits only supports 'object'",
			    name);
		}
		lua_pop(L, 1);
		if (qstar_lua_required_string_field(L, -1, graph, owner, "lower",
		    NULL, 0) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "deps");
		if (!lua_isnil(L, -1) && !lua_isstring(L, -1)) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider units.%s.deps must be a string",
			    name);
		}
		lua_pop(L, 1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_provider_finals(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed_fields[] = { "lower", NULL };
	static const char *const allowed_kinds[] = {
		"executable", "staticlib", "sharedlib", NULL
	};
	const char *name, *key;
	char owner[160];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "finals");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider finals must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!name || !string_in_set(name, allowed_kinds) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider finals contains invalid final '%s'",
			    name ? name : "<non-string>");
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || !string_in_set(key, allowed_fields)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: unknown language provider final field finals.%s.%s",
				    name, key ? key : "<non-string>");
			}
			lua_pop(L, 1);
		}
		snprintf(owner, sizeof(owner), "language provider finals.%s", name);
		if (qstar_lua_required_string_field(L, -1, graph, owner, "lower",
		    NULL, 0) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_provider_options(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = { "type", "values", "default", NULL };
	const char *name, *key, *type_name, *type_canonical, *default_value;
	int option_idx;
	char owner[160];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "options");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider options must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(name) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider options contains invalid option '%s'",
			    name ? name : "<non-string>");
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || !string_in_set(key, allowed)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: unknown language provider option field options.%s.%s",
				    name, key ? key : "<non-string>");
			}
			lua_pop(L, 1);
		}
		snprintf(owner, sizeof(owner), "language provider options.%s", name);
		if (qstar_lua_required_string_field(L, -1, graph, owner, "type",
		    NULL, 0) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "type");
		type_name = lua_tostring(L, -1);
		type_canonical = canonical_provider_option_type(type_name);
		if (!type_canonical) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider options.%s.type is unsupported",
			    name);
		}
		lua_pop(L, 1);
		option_idx = lua_gettop(L);
		if (strcmp(type_canonical, "enum") == 0) {
			lua_getfield(L, -1, "values");
			if (lua_isnil(L, -1) ||
			    validate_exact_string_list_value(L, -1, graph,
			    "language provider enum values") < 0 ||
			    lua_rawlen(L, -1) == 0) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: language provider options.%s.values must be a non-empty string list",
				    name);
			}
			lua_pop(L, 1);
		}
		lua_getfield(L, option_idx, "default");
		if (!lua_isnil(L, -1)) {
			if (strcmp(type_canonical, "string") == 0) {
				if (lua_type(L, -1) != LUA_TSTRING) {
					lua_pop(L, 3);
					return qstar_set_error(graph,
					    "qstar: language provider options.%s.default must be a string",
					    name);
				}
			} else if (strcmp(type_canonical, "bool") == 0) {
				if (!lua_isboolean(L, -1)) {
					lua_pop(L, 3);
					return qstar_set_error(graph,
					    "qstar: language provider options.%s.default must be a boolean",
					    name);
				}
			} else if (strcmp(type_canonical, "list") == 0) {
				if (validate_exact_string_list_value(L, -1, graph,
				    "language provider option default") < 0) {
					lua_pop(L, 3);
					return qstar_set_error(graph,
					    "qstar: language provider options.%s.default must be a string list",
					    name);
				}
			} else if (strcmp(type_canonical, "enum") == 0) {
				default_value = lua_type(L, -1) == LUA_TSTRING ?
				    lua_tostring(L, -1) : NULL;
				lua_getfield(L, option_idx, "values");
				if (!default_value ||
				    !lua_string_list_contains_value(L, -1, default_value)) {
					lua_pop(L, 4);
					return qstar_set_error(graph,
					    "qstar: language provider options.%s.default must be one of values",
					    name);
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_table_fields(lua_State *L, int table, struct qstar_graph *graph,
    const char *owner, const char *const *allowed)
{
	const char *key;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: unknown %s field '%s'",
			    owner, key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
qstar_lua_table_has_key(lua_State *L, int table, const char *key)
{
	int has;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!key || !*key)
		return 0;
	lua_getfield(L, table, key);
	has = !lua_isnil(L, -1);
	lua_pop(L, 1);
	return has;
}

static int
scaffold_template_var_allowed(const char *name, size_t len)
{
	static const char *const allowed[] = {
		"project_name", "project_ident", "shape", "namespace",
		"target_name", "source_ext", NULL
	};
	size_t i;

	for (i = 0; allowed[i]; i++) {
		if (strlen(allowed[i]) == len && strncmp(name, allowed[i], len) == 0)
			return 1;
	}
	return 0;
}

static int
validate_scaffold_template_string(struct qstar_graph *graph, const char *owner,
    const char *value)
{
	const char *p, *end;

	if (!value)
		return qstar_set_error(graph, "qstar: %s must be a string", owner);
	for (p = value; *p; p++) {
		if (*p == '`')
			return qstar_set_error(graph,
			    "qstar: %s contains shell command substitution", owner);
		if (*p != '$')
			continue;
		if (p[1] != '{')
			return qstar_set_error(graph,
			    "qstar: %s contains unsupported template syntax", owner);
		end = strchr(p + 2, '}');
		if (!end || end == p + 2 ||
		    !scaffold_template_var_allowed(p + 2, (size_t)(end - (p + 2))))
			return qstar_set_error(graph,
			    "qstar: %s contains unsupported template variable", owner);
		p = end;
	}
	return 0;
}

static int
validate_scaffold_path(struct qstar_graph *graph, const char *owner,
    const char *path)
{
	size_t n;

	if (!path || !*path)
		return qstar_set_error(graph, "qstar: %s path must not be empty", owner);
	if (!qstar_path_is_package_relative(path))
		return qstar_set_error(graph,
		    "qstar: %s path '%s' must be package-relative (%s)", owner,
		    path, qstar_path_package_relative_reason(path));
	n = strlen(path);
	if (n > 0 && path[n - 1] == '/')
		return qstar_set_error(graph,
		    "qstar: %s path '%s' must not end with '/'", owner, path);
	return validate_scaffold_template_string(graph, owner, path);
}

static int
validate_scaffold_string_list(lua_State *L, int idx, struct qstar_graph *graph,
    const char *owner, int as_path)
{
	const char *item;
	size_t i, n;
	char item_owner[192];

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	if (!lua_istable(L, idx) || !qstar_lua_array_table(L, idx))
		return qstar_set_error(graph, "qstar: %s must be a list of strings",
		    owner);
	n = lua_rawlen(L, idx);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		item = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
		if (!item || !*item) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s must be a list of non-empty strings", owner);
		}
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (as_path) {
			if (validate_scaffold_path(graph, item_owner, item) < 0) {
				lua_pop(L, 1);
				return -1;
			}
		} else if (validate_scaffold_template_string(graph, item_owner,
		    item) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_option_value(lua_State *L, int manifest, int value,
    struct qstar_graph *graph, const char *option_name, const char *owner)
{
	const char *type, *canonical, *enum_value;

	if (manifest < 0)
		manifest = lua_gettop(L) + manifest + 1;
	if (value < 0)
		value = lua_gettop(L) + value + 1;
	lua_getfield(L, manifest, "options");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: %s references undeclared provider option '%s'", owner,
		    option_name ? option_name : "<non-string>");
	}
	lua_getfield(L, -1, option_name);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: %s references undeclared provider option '%s'", owner,
		    option_name ? option_name : "<non-string>");
	}
	lua_getfield(L, -1, "type");
	type = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
	canonical = canonical_provider_option_type(type);
	if (!canonical) {
		lua_pop(L, 3);
		return qstar_set_error(graph,
		    "qstar: %s references provider option '%s' with unsupported type",
		    owner, option_name ? option_name : "<non-string>");
	}
	if (strcmp(canonical, "string") == 0) {
		if (lua_type(L, value) != LUA_TSTRING) {
			lua_pop(L, 3);
			return qstar_set_error(graph, "qstar: %s must be a string",
			    owner);
		}
	} else if (strcmp(canonical, "bool") == 0) {
		if (!lua_isboolean(L, value)) {
			lua_pop(L, 3);
			return qstar_set_error(graph, "qstar: %s must be a boolean",
			    owner);
		}
	} else if (strcmp(canonical, "list") == 0) {
		if (validate_exact_string_list_value(L, value, graph, owner) < 0) {
			lua_pop(L, 3);
			return -1;
		}
	} else if (strcmp(canonical, "enum") == 0) {
		enum_value = lua_type(L, value) == LUA_TSTRING ?
		    lua_tostring(L, value) : NULL;
		lua_getfield(L, -2, "values");
		if (!enum_value || !lua_string_list_contains_value(L, -1,
		    enum_value)) {
			lua_pop(L, 4);
			return qstar_set_error(graph,
			    "qstar: %s has unsupported enum value '%s'", owner,
			    enum_value ? enum_value : "<non-string>");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 3);
	return 0;
}

static int
validate_scaffold_options_table(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner)
{
	const char *name;
	char item_owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return qstar_set_error(graph, "qstar: %s must be a table", owner);
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(name)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: %s contains invalid option '%s'", owner,
			    name ? name : "<non-string>");
		}
		snprintf(item_owner, sizeof(item_owner), "%s.%s", owner, name);
		if (validate_scaffold_option_value(L, manifest, -1, graph, name,
		    item_owner) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_tool_argv(lua_State *L, int table, struct qstar_graph *graph,
    const char *owner)
{
	const char *arg;
	size_t i, n;
	char item_owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !qstar_lua_array_table(L, table) ||
	    lua_rawlen(L, table) == 0)
		return qstar_set_error(graph,
		    "qstar: %s must be a non-empty list of argv strings", owner);
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		arg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
		if (!arg || !*arg) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s must be a non-empty list of argv strings", owner);
		}
		if (strstr(arg, "://") || strstr(arg, "$(") || strchr(arg, '`')) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s contains network or shell syntax", owner);
		}
		if (fs_path_is_absolute(arg)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s must not contain absolute paths", owner);
		}
		if (strchr(arg, '/') || strchr(arg, '\\') || strchr(arg, ':')) {
			if (!qstar_path_is_package_relative(arg)) {
				lua_pop(L, 1);
				return qstar_set_error(graph,
				    "qstar: %s path '%s' must be package-relative (%s)",
				    owner, arg, qstar_path_package_relative_reason(arg));
			}
		}
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (validate_scaffold_template_string(graph, item_owner, arg) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_sources(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner)
{
	static const char *const allowed_source[] = {
		"helper", "path", "options", NULL
	};
	const char *path, *helper;
	size_t i, n;
	char item_owner[192], path_owner[224], option_owner[224];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !qstar_lua_array_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a list", owner);
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (lua_type(L, -1) == LUA_TSTRING) {
			path = lua_tostring(L, -1);
			if (validate_scaffold_path(graph, item_owner, path) < 0) {
				lua_pop(L, 1);
				return -1;
			}
			lua_pop(L, 1);
			continue;
		}
		if (!qstar_lua_plain_table(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: %s must be a string path or provider helper table",
			    item_owner);
		}
		if (qstar_lua_validate_table_fields(L, -1, graph, item_owner,
		    allowed_source) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		if (qstar_lua_required_string_field(L, -1, graph, item_owner,
		    "helper", NULL, 0) < 0 ||
		    qstar_lua_required_string_field(L, -1, graph, item_owner,
		    "path", NULL, 0) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_getfield(L, -1, "helper");
		helper = lua_tostring(L, -1);
		lua_getfield(L, manifest, "exports");
		if (!valid_tool_role_component(helper) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: %s.helper references missing provider export '%s'",
			    item_owner, helper ? helper : "<non-string>");
		}
		lua_getfield(L, -1, helper);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: %s.helper references missing provider export '%s'",
			    item_owner, helper ? helper : "<non-string>");
		}
		lua_pop(L, 3);
		lua_getfield(L, -1, "path");
		path = lua_tostring(L, -1);
		snprintf(path_owner, sizeof(path_owner), "%s.path", item_owner);
		if (validate_scaffold_path(graph, path_owner, path) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "options");
		if (!lua_isnil(L, -1)) {
			snprintf(option_owner, sizeof(option_owner), "%s.options",
			    item_owner);
			if (validate_scaffold_options_table(L, manifest, -1, graph,
			    option_owner) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		}
		lua_pop(L, 1);
		lua_pop(L, 1);
	}
	return 0;
}

static int validate_scaffold_target(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace,
    int require_kind);

static int
validate_scaffold_group(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace)
{
	static const char *const allowed_group[] = { "name", "deps", NULL };
	const char *name;
	(void)manifest;
	(void)namespace;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!qstar_lua_plain_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a table", owner);
	if (qstar_lua_validate_table_fields(L, table, graph, owner, allowed_group) < 0)
		return -1;
	if (qstar_lua_required_string_field(L, table, graph, owner, "name", NULL,
	    0) < 0)
		return -1;
	lua_getfield(L, table, "name");
	name = lua_tostring(L, -1);
	if (validate_scaffold_template_string(graph, owner, name) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "deps");
	if (!lua_isnil(L, -1) && validate_scaffold_string_list(L, -1, graph,
	    "language provider scaffold group deps", 0) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
validate_scaffold_lang_table(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace)
{
	const char *name;
	char lang_owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return qstar_set_error(graph, "qstar: %s must be a table", owner);
	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!valid_tool_role_component(name) || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: %s contains invalid language namespace '%s'",
			    owner, name ? name : "<non-string>");
		}
		if (strcmp(name, namespace) == 0) {
			snprintf(lang_owner, sizeof(lang_owner), "%s.%s", owner, name);
			if (validate_scaffold_options_table(L, manifest, -1, graph,
			    lang_owner) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_target(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace,
    int require_kind)
{
	static const char *const allowed_target[] = {
		"kind", "name", "sources", "deps", "lang", NULL
	};
	static const char *const allowed_kinds[] = {
		"executable", "staticlib", "sharedlib", "test", "run_target",
		"group", NULL
	};
	const char *kind, *name;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!qstar_lua_plain_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a table", owner);
	if (qstar_lua_validate_table_fields(L, table, graph, owner,
	    allowed_target) < 0)
		return -1;
	lua_getfield(L, table, "kind");
	kind = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
	if (require_kind && !kind) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: %s.kind must be a string",
		    owner);
	}
	if (kind && !string_in_set(kind, allowed_kinds)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: %s.kind has unsupported target kind '%s'", owner, kind);
	}
	lua_pop(L, 1);
	if (qstar_lua_required_string_field(L, table, graph, owner, "name", NULL,
	    0) < 0)
		return -1;
	lua_getfield(L, table, "name");
	name = lua_tostring(L, -1);
	if (validate_scaffold_template_string(graph, owner, name) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "sources");
	if (!lua_isnil(L, -1) && validate_scaffold_sources(L, manifest, -1,
	    graph, "language provider scaffold target sources") < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "deps");
	if (!lua_isnil(L, -1) && validate_scaffold_string_list(L, -1, graph,
	    "language provider scaffold target deps", 0) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "lang");
	if (!lua_isnil(L, -1) && validate_scaffold_lang_table(L, manifest, -1,
	    graph, "language provider scaffold target lang", namespace) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
validate_scaffold_files(lua_State *L, int table, struct qstar_graph *graph,
    const char *owner)
{
	static const char *const allowed_file[] = { "path", "body", "executable", NULL };
	const char *path, *body;
	size_t i, n;
	char item_owner[192], field_owner[224];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !qstar_lua_array_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a list", owner);
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (!qstar_lua_plain_table(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: %s must be a table",
			    item_owner);
		}
		if (qstar_lua_validate_table_fields(L, -1, graph, item_owner,
		    allowed_file) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		if (qstar_lua_required_string_field(L, -1, graph, item_owner,
		    "path", NULL, 0) < 0 ||
		    qstar_lua_required_string_field(L, -1, graph, item_owner,
		    "body", NULL, 0) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_getfield(L, -1, "path");
		path = lua_tostring(L, -1);
		snprintf(field_owner, sizeof(field_owner), "%s.path", item_owner);
		if (validate_scaffold_path(graph, field_owner, path) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "body");
		body = lua_tostring(L, -1);
		snprintf(field_owner, sizeof(field_owner), "%s.body", item_owner);
		if (validate_scaffold_template_string(graph, field_owner,
		    body) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "executable");
		if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: %s.executable must be boolean", item_owner);
		}
		lua_pop(L, 1);
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_targets_list(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace)
{
	size_t i, n;
	char item_owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !qstar_lua_array_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a list", owner);
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (validate_scaffold_target(L, manifest, -1, graph, item_owner,
		    namespace, 1) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int validate_scaffold_shape(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace);

static int
validate_scaffold_fragments(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace)
{
	static const char *const allowed_fragment[] = {
		"path", "directories", "files", "targets", "target", "group", NULL
	};
	const char *path;
	size_t i, n;
	char item_owner[192], path_owner[224];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table) || !qstar_lua_array_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a list", owner);
	n = lua_rawlen(L, table);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, table, (lua_Integer)i);
		snprintf(item_owner, sizeof(item_owner), "%s[%zu]", owner, i);
		if (!qstar_lua_plain_table(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: %s must be a table",
			    item_owner);
		}
		if (qstar_lua_validate_table_fields(L, -1, graph, item_owner,
		    allowed_fragment) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		if (qstar_lua_required_string_field(L, -1, graph, item_owner,
		    "path", NULL, 0) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_getfield(L, -1, "path");
		path = lua_tostring(L, -1);
		snprintf(path_owner, sizeof(path_owner), "%s.path", item_owner);
		if (validate_scaffold_path(graph, path_owner, path) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		if (!path_has_suffix(path, ".qst")) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: %s.path must end with .qst", item_owner);
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "directories");
		if (!lua_isnil(L, -1) && validate_scaffold_string_list(L, -1, graph,
		    "language provider scaffold fragment directories", 1) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "files");
		if (!lua_isnil(L, -1) && validate_scaffold_files(L, -1, graph,
		    "language provider scaffold fragment files") < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "targets");
		if (!lua_isnil(L, -1) && validate_scaffold_targets_list(L, manifest,
		    -1, graph, "language provider scaffold fragment targets",
		    namespace) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "target");
		if (!lua_isnil(L, -1) && validate_scaffold_target(L, manifest, -1,
		    graph, "language provider scaffold fragment target", namespace,
		    1) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "group");
		if (!lua_isnil(L, -1) && validate_scaffold_group(L, manifest, -1,
		    graph, "language provider scaffold fragment group", namespace) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
		if (validate_scaffold_template_string(graph, item_owner, path) < 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int
validate_scaffold_shape(lua_State *L, int manifest, int table,
    struct qstar_graph *graph, const char *owner, const char *namespace)
{
	static const char *const allowed_shape[] = {
		"directories", "files", "targets", "target", "fragments", "group", NULL
	};

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!qstar_lua_plain_table(L, table))
		return qstar_set_error(graph, "qstar: %s must be a table", owner);
	if (qstar_lua_validate_table_fields(L, table, graph, owner, allowed_shape) < 0)
		return -1;
	lua_getfield(L, table, "directories");
	if (!lua_isnil(L, -1) && validate_scaffold_string_list(L, -1, graph,
	    "language provider scaffold directories", 1) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "files");
	if (!lua_isnil(L, -1) && validate_scaffold_files(L, -1, graph,
	    "language provider scaffold files") < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "targets");
	if (!lua_isnil(L, -1) && validate_scaffold_targets_list(L, manifest, -1,
	    graph, "language provider scaffold targets", namespace) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "target");
	if (!lua_isnil(L, -1) && validate_scaffold_target(L, manifest, -1, graph,
	    "language provider scaffold target", namespace, 1) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "fragments");
	if (!lua_isnil(L, -1) && validate_scaffold_fragments(L, manifest, -1,
	    graph, "language provider scaffold fragments", namespace) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, table, "group");
	if (!lua_isnil(L, -1) && validate_scaffold_group(L, manifest, -1, graph,
	    "language provider scaffold group", namespace) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_provider_scaffold(lua_State *L, int table,
    struct qstar_graph *graph, const char *namespace)
{
	static const char *const allowed_root[] = {
		"api", "tools", "options", "shapes", NULL
	};
	static const char *const allowed_shapes[] = {
		"app", "lib", "tool", "empty", "workspace", NULL
	};
	const char *api, *name;
	size_t count;
	char owner[192];

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "scaffold");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!qstar_lua_plain_table(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider scaffold must be a table");
	}
	if (qstar_lua_validate_table_fields(L, -1, graph,
	    "language provider scaffold", allowed_root) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	if (qstar_lua_required_string_field(L, -1, graph,
	    "language provider scaffold", "api", NULL, 0) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	lua_getfield(L, -1, "api");
	api = lua_tostring(L, -1);
	if (strcmp(api, "qstar.scaffold/1") != 0) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: language provider scaffold.api must be \"qstar.scaffold/1\"");
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "tools");
	if (!lua_isnil(L, -1)) {
		if (!qstar_lua_plain_table(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider scaffold.tools must be a table");
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!valid_tool_role_component(name)) {
				lua_pop(L, 4);
				return qstar_set_error(graph,
				    "qstar: language provider scaffold.tools contains invalid tool '%s'",
				    name ? name : "<non-string>");
			}
			lua_getfield(L, table, "tools");
			if (!lua_istable(L, -1) ||
			    !qstar_lua_table_has_key(L, -1, name)) {
				lua_pop(L, 4);
				return qstar_set_error(graph,
				    "qstar: language provider scaffold.tools.%s references undeclared provider tool",
				    name);
			}
			lua_pop(L, 1);
			snprintf(owner, sizeof(owner),
			    "language provider scaffold.tools.%s", name);
			if (validate_scaffold_tool_argv(L, -1, graph, owner) < 0) {
				lua_pop(L, 3);
				return -1;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "options");
	if (!lua_isnil(L, -1) && validate_scaffold_options_table(L, table, -1,
	    graph, "language provider scaffold.options") < 0) {
		lua_pop(L, 2);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "shapes");
	if (!qstar_lua_plain_table(L, -1)) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: language provider scaffold.shapes must be a table");
	}
	count = 0;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!name || !string_in_set(name, allowed_shapes)) {
			lua_pop(L, 4);
			return qstar_set_error(graph,
			    "qstar: language provider scaffold.shapes contains unsupported shape '%s'",
			    name ? name : "<non-string>");
		}
		snprintf(owner, sizeof(owner), "language provider scaffold.shapes.%s",
		    name);
		if (validate_scaffold_shape(L, table, -1, graph, owner,
		    namespace) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		count++;
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	if (count == 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider scaffold.shapes must not be empty");
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_validate_provider_exports(lua_State *L, int table, struct qstar_graph *graph)
{
	const char *name, *value;
	size_t count;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "exports");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: language provider exports must be a table");
	}
	count = 0;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		value = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!valid_tool_role_component(name) || !valid_tool_role_component(value)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: language provider exports must map names to implementation fields");
		}
		count++;
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	if (count == 0)
		return qstar_set_error(graph,
		    "qstar: language provider exports must not be empty");
	return 0;
}

static int
validate_language_provider_manifest_table(lua_State *L, int table,
    struct qstar_graph *graph, char *api, size_t api_len, char *id, size_t id_len,
    char *namespace, size_t namespace_len, char *version, size_t version_len,
    char *implementation, size_t implementation_len, int require_kind)
{
	const char *kind;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	if (!lua_istable(L, table))
		return qstar_set_error(graph,
		    "qstar: provider manifest must be qstar.language_provider { ... }");
	kind = qstar_table_kind(L, table);
	if (kind && strcmp(kind, "language_provider") != 0)
		return qstar_set_error(graph,
		    "qstar: provider manifest must return qstar.language_provider { ... }");
	if (require_kind && !kind)
		return qstar_set_error(graph,
		    "qstar: provider manifest must return qstar.language_provider { ... }");
	if (qstar_lua_validate_provider_fields(L, table, graph) < 0 ||
	    qstar_lua_required_string_field(L, table, graph, "language provider",
	    "api", api, api_len) < 0 ||
	    qstar_lua_required_string_field(L, table, graph, "language provider",
	    "id", id, id_len) < 0 ||
	    qstar_lua_required_string_field(L, table, graph, "language provider",
	    "version", version, version_len) < 0 ||
	    qstar_lua_required_string_field(L, table, graph, "language provider",
	    "namespace", namespace, namespace_len) < 0 ||
	    qstar_lua_required_string_field(L, table, graph, "language provider",
	    "implementation", implementation, implementation_len) < 0)
		return -1;
	if (strcmp(api, "qstar.lang/1") != 0)
		return qstar_set_error(graph,
		    "qstar: language provider api must be \"qstar.lang/1\"");
	if (!valid_tool_role_component(id))
		return qstar_set_error(graph,
		    "qstar: invalid language provider id '%s'", id);
	if (!valid_tool_role_component(namespace))
		return qstar_set_error(graph,
		    "qstar: invalid language provider namespace '%s'", namespace);
	if (!qstar_path_is_package_relative(implementation) ||
	    !path_has_suffix(implementation, ".lua"))
		return qstar_set_error(graph,
		    "qstar: language provider implementation must be a relative .lua path");
	if (qstar_lua_validate_provider_tools(L, table, graph, namespace) < 0 ||
	    qstar_lua_validate_provider_units(L, table, graph) < 0 ||
	    qstar_lua_validate_provider_finals(L, table, graph) < 0 ||
	    qstar_lua_validate_provider_options(L, table, graph) < 0 ||
	    qstar_lua_validate_provider_scaffold(L, table, graph, namespace) < 0 ||
	    qstar_lua_validate_provider_exports(L, table, graph) < 0)
		return -1;
	return 0;
}

/** provider manifest 전용 author API다. */
static int
qstar_lua_language_provider(lua_State *L)
{
	struct qstar_lua_context *ctx;
	char api[64], id[128], namespace[128], version[128], implementation[QSTAR_PATH_MAX];

	ctx = get_context(L);
	if (!ctx || ctx->provider_stack.len == 0)
		return luaL_error(L,
		    "qstar: qstar.language_provider is only valid inside provider manifests");
	luaL_checktype(L, 1, LUA_TTABLE);
	if (validate_language_provider_manifest_table(L, 1, ctx->graph, api,
	    sizeof(api), id, sizeof(id), namespace, sizeof(namespace), version,
	    sizeof(version), implementation, sizeof(implementation), 0) < 0)
		return luaL_error(L, "%s", ctx->graph->error);
	lua_pushstring(L, "language_provider");
	lua_setfield(L, 1, "__qstar_kind");
	lua_pushvalue(L, 1);
	return 1;
}

static int
qstar_lua_provider_tools(lua_State *L)
{
	const char *namespace;

	namespace = luaL_checkstring(L, 1);
	if (!valid_tool_role_component(namespace))
		return luaL_error(L, "qstar: invalid provider namespace '%s'",
		    namespace);
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_pushvalue(L, 2);
	return 1;
}

static int
qstar_lua_language_options(lua_State *L)
{
	const char *namespace;

	namespace = luaL_checkstring(L, 1);
	if (!valid_tool_role_component(namespace))
		return luaL_error(L, "qstar: invalid language namespace '%s'",
		    namespace);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_newtable(L);
		return 1;
	}
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_pushvalue(L, 2);
	return 1;
}

/** qstar.join("a", "b") 형태의 package path string을 만든다. */
static int
qstar_lua_join_path(lua_State *L)
{
	luaL_Buffer buf;
	const char *s;
	size_t len, start, end;
	int i, top, wrote;

	top = lua_gettop(L);
	if (top == 0)
		return luaL_error(L, "qstar: qstar.join requires path segments");
	luaL_buffinit(L, &buf);
	wrote = 0;
	for (i = 1; i <= top; i++) {
		s = luaL_checklstring(L, i, &len);
		start = 0;
		end = len;
		if (wrote) {
			while (start < end && s[start] == '/')
				start++;
		}
		while (end > start && s[end - 1] == '/')
			end--;
		if (end == start)
			continue;
		if (wrote)
			luaL_addchar(&buf, '/');
		luaL_addlstring(&buf, s + start, end - start);
		wrote = 1;
	}
	luaL_pushresult(&buf);
	return 1;
}

static int
qstar_lua_join(lua_State *L)
{
	if (lua_gettop(L) == 1 && lua_istable(L, 1))
		return luaL_error(L,
		    "qstar: qstar.join table form was removed; use qstar.append for lists or qstar.join(\"a\", \"b\") for paths");
	return qstar_lua_join_path(L);
}

/** 문자열에 glob wildcard가 들어 있는지 검사한다. */
static int
has_glob_magic(const char *s)
{
	return s && (strchr(s, '*') || strchr(s, '?'));
}

/** 간단한 wildcard pattern을 path에 맞춘다. */
static int
wildcard_match(const char *pattern, const char *text)
{
	if (!*pattern)
		return !*text;
	if (*pattern == '*') {
		while (pattern[1] == '*')
			pattern++;
		if (wildcard_match(pattern + 1, text))
			return 1;
		return *text && wildcard_match(pattern, text + 1);
	}
	if (*pattern == '?')
		return *text && *text != '/' && wildcard_match(pattern + 1, text + 1);
	return *pattern == *text && wildcard_match(pattern + 1, text + 1);
}

static int
string_cmp(const void *a, const void *b)
{
	const char *const *sa = a;
	const char *const *sb = b;

	return strcmp(*sa, *sb);
}

/** glob pattern을 directory와 basename pattern으로 나눈다. */
static void
split_glob_pattern(const char *pattern, char *dir, size_t dirlen, char *base, size_t baselen)
{
	const char *first, *slash;
	size_t n;

	first = strpbrk(pattern, "*?");
	slash = first ? first : pattern + strlen(pattern);
	while (slash > pattern && slash[-1] != '/')
		slash--;
	if (slash == pattern) {
		snprintf(dir, dirlen, ".");
		snprintf(base, baselen, "%s", pattern);
		return;
	}
	n = (size_t)(slash - pattern - 1);
	if (n + 1 > dirlen)
		n = dirlen - 1;
	memcpy(dir, pattern, n);
	dir[n] = '\0';
	snprintf(base, baselen, "%s", slash);
}

/** package root 아래 단일 directory glob을 deterministic하게 확장한다. */
static int
expand_glob(struct qstar_lua_context *ctx, const char *pattern, struct qstar_string_list *out)
{
	char dir[QSTAR_PATH_MAX], base[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	struct qstar_string_list matches;
	DIR *d;
	struct dirent *ent;
	size_t i;

	memset(&matches, 0, sizeof(matches));
	split_glob_pattern(pattern, dir, sizeof(dir), base, sizeof(base));
	if (qstar_path_join(ctx->root_dir, dir, full, sizeof(full)) < 0)
		return qstar_set_error(ctx->graph, "qstar: file glob '%s' is too long", pattern);
	d = opendir(full);
	if (!d)
		return qstar_set_error(ctx->graph, "qstar: file glob '%s' matched no files", pattern);
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (!wildcard_match(base, ent->d_name))
			continue;
		if (strcmp(dir, ".") == 0)
			snprintf(rel, sizeof(rel), "%s", ent->d_name);
		else
			snprintf(rel, sizeof(rel), "%s/%s", dir, ent->d_name);
		if (qstar_string_list_push(&matches, rel) < 0) {
			closedir(d);
			qstar_string_list_free(&matches);
			return qstar_set_error(ctx->graph, "qstar: out of memory");
		}
	}
	closedir(d);
	if (matches.len == 0) {
		qstar_string_list_free(&matches);
		return qstar_set_error(ctx->graph, "qstar: file glob '%s' matched no files", pattern);
	}
	qsort(matches.items, matches.len, sizeof(matches.items[0]), string_cmp);
	for (i = 0; i < matches.len; i++) {
		if (qstar_string_list_push(out, matches.items[i]) < 0) {
			qstar_string_list_free(&matches);
			return qstar_set_error(ctx->graph, "qstar: out of memory");
		}
	}
	qstar_string_list_free(&matches);
	return 0;
}

/** exclude pattern list에 path가 걸리는지 검사한다. */
static int
excluded_by_patterns(const char *path, const struct qstar_string_list *excludes)
{
	size_t i;

	for (i = 0; i < excludes->len; i++) {
		if (has_glob_magic(excludes->items[i])) {
			if (wildcard_match(excludes->items[i], path))
				return 1;
		} else if (strcmp(excludes->items[i], path) == 0) {
			return 1;
		}
	}
	return 0;
}

/** qstar.files table을 실제 package-root 파일 목록으로 확장한다. */
static int
qstar_lua_files(lua_State *L)
{
	struct qstar_lua_context *ctx;
	struct qstar_string_list files, excludes;
	size_t i, n, out_index;
	const char *path;

	ctx = get_context(L);
	memset(&files, 0, sizeof(files));
	memset(&excludes, 0, sizeof(excludes));
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "exclude");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return luaL_error(L, "qstar: qstar.files exclude must be a list");
		}
		n = lua_rawlen(L, -1);
		for (i = 1; i <= n; i++) {
			lua_rawgeti(L, -1, (lua_Integer)i);
			if (!lua_isstring(L, -1)) {
				lua_pop(L, 2);
				qstar_string_list_free(&files);
				qstar_string_list_free(&excludes);
				return luaL_error(L, "qstar: qstar.files exclude contains non-string item");
			}
			if (has_glob_magic(lua_tostring(L, -1)))
				ctx->graph->uses_file_globs = 1;
			if (qstar_string_list_push(&excludes, lua_tostring(L, -1)) < 0) {
				lua_pop(L, 2);
				qstar_string_list_free(&files);
				qstar_string_list_free(&excludes);
				return luaL_error(L, "qstar: out of memory");
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	n = lua_rawlen(L, 1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i);
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 1);
			qstar_string_list_free(&files);
			qstar_string_list_free(&excludes);
			return luaL_error(L, "qstar: qstar.files contains non-string item");
		}
		path = lua_tostring(L, -1);
		if (!qstar_path_is_package_relative(path)) {
			lua_pop(L, 1);
			qstar_string_list_free(&files);
			qstar_string_list_free(&excludes);
			return luaL_error(L, "qstar: qstar.files path '%s' must be package-relative", path);
		}
		if (has_glob_magic(path)) {
			ctx->graph->uses_file_globs = 1;
			if (expand_glob(ctx, path, &files) < 0) {
				lua_pop(L, 1);
				qstar_string_list_free(&files);
				qstar_string_list_free(&excludes);
				return luaL_error(L, "%s", ctx->graph->error);
			}
		} else if (qstar_string_list_push(&files, path) < 0) {
			lua_pop(L, 1);
			qstar_string_list_free(&files);
			qstar_string_list_free(&excludes);
			return luaL_error(L, "qstar: out of memory");
		}
		lua_pop(L, 1);
	}
	lua_newtable(L);
	out_index = 1;
	for (i = 0; i < files.len; i++) {
		if (excluded_by_patterns(files.items[i], &excludes))
			continue;
		lua_pushstring(L, files.items[i]);
		lua_rawseti(L, -2, (lua_Integer)out_index++);
	}
	qstar_string_list_free(&files);
	qstar_string_list_free(&excludes);
	return 1;
}

static int
qstar_lua_output(lua_State *L)
{
	struct qstar_lua_context *ctx;
	lua_Integer index;
	const char *path, *group, *format;

	if (lua_isinteger(L, 1)) {
		index = lua_tointeger(L, 1);
		if (index < 0)
			return luaL_error(L, "qstar: qstar.output index must be >= 0");
		lua_newtable(L);
		lua_pushstring(L, "output");
		lua_setfield(L, -2, "__qstar_kind");
		lua_pushinteger(L, index);
		lua_setfield(L, -2, "index");
		return 1;
	}
	path = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, path);
		return 1;
	}
	ctx = get_context(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	if (validate_output_metadata_fields(L, 2, ctx->graph) < 0)
		return luaL_error(L, "%s", ctx->graph->error);
	group = check_string_field(L, 2, "group");
	if (!group || !*group)
		group = check_string_field(L, 2, "output_group");
	format = check_string_field(L, 2, "format");
	if ((group && !valid_output_metadata_token(group)) ||
	    (format && !valid_output_metadata_token(format)))
		return luaL_error(L,
		    "qstar: generated output metadata for '%s' contains unsupported characters",
		    path);
	if (!valid_output_format(format))
		return luaL_error(L,
		    "qstar: qstar.output format '%s' is not supported; only format = \"object\" is supported",
		    format);
	lua_newtable(L);
	lua_pushstring(L, "output_path");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, path);
	lua_setfield(L, -2, "path");
	if (group) {
		lua_pushstring(L, group);
		lua_setfield(L, -2, "group");
	}
	if (format) {
		lua_pushstring(L, format);
		lua_setfield(L, -2, "format");
	}
	return 1;
}

static int
valid_artifact_selector(const char *value)
{
	const unsigned char *p;

	if (!value || !*value)
		return 0;
	for (p = (const unsigned char *)value; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-'))
			return 0;
	}
	return 1;
}

static int
qstar_lua_input(lua_State *L)
{
	lua_Integer index;

	index = luaL_checkinteger(L, 1);
	if (index < 0)
		return luaL_error(L, "qstar: qstar.input index must be >= 0");
	lua_newtable(L);
	lua_pushstring(L, "input");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushinteger(L, index);
	lua_setfield(L, -2, "index");
	return 1;
}

static int
qstar_lua_target_file(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *raw, *artifact;
	char label[QSTAR_PATH_MAX];

	ctx = get_context(L);
	raw = luaL_checkstring(L, 1);
	if (qstar_label_canonicalize(raw, ctx->current_dir, label, sizeof(label)) < 0)
		return luaL_error(L, "qstar: invalid target_file label '%s'", raw);
	artifact = NULL;
	if (!lua_isnoneornil(L, 2)) {
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_getfield(L, 2, "artifact");
		if (!lua_isstring(L, -1))
			return luaL_error(L,
			    "qstar: qstar.target_file selector field 'artifact' must be a string");
		artifact = lua_tostring(L, -1);
		if (!valid_artifact_selector(artifact))
			return luaL_error(L,
			    "qstar: qstar.target_file selector artifact '%s' is invalid",
			    artifact);
		lua_pop(L, 1);
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			const char *key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || strcmp(key, "artifact") != 0)
				return luaL_error(L,
				    "qstar: unknown qstar.target_file selector field '%s'",
				    key ? key : "<non-string>");
			lua_pop(L, 1);
		}
	}
	lua_newtable(L);
	lua_pushstring(L, "target_file");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	if (artifact && *artifact) {
		lua_pushstring(L, artifact);
		lua_setfield(L, -2, "artifact");
	}
	return 1;
}

static int
qstar_lua_stage_dir(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *raw;
	char label[QSTAR_PATH_MAX];

	ctx = get_context(L);
	raw = luaL_checkstring(L, 1);
	if (qstar_label_canonicalize(raw, ctx->current_dir, label, sizeof(label)) < 0)
		return luaL_error(L, "qstar: invalid stage_dir label '%s'", raw);
	if (!lua_isnoneornil(L, 2))
		return luaL_error(L, "qstar: qstar.stage_dir takes exactly one label");
	lua_newtable(L);
	lua_pushstring(L, "stage_dir");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	return 1;
}

static int
valid_command_option_ref_name(const char *name)
{
	const unsigned char *p;

	if (!name || !*name)
		return 0;
	if (!(isalpha((unsigned char)name[0]) || name[0] == '_'))
		return 0;
	for (p = (const unsigned char *)name + 1; *p; p++) {
		if (!(isalnum(*p) || *p == '_' || *p == '-'))
			return 0;
	}
	return 1;
}

static const char *
command_param_table_name(lua_State *L, int idx)
{
	const char *kind, *name;

	kind = qstar_table_kind(L, idx);
	if (!kind || strcmp(kind, "command_param") != 0)
		return NULL;
	lua_getfield(L, idx, "name");
	name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	lua_pop(L, 1);
	return name;
}

static int
format_command_param_token(lua_State *L, int idx, char *dst, size_t dstlen)
{
	const char *name;

	name = command_param_table_name(L, idx);
	if (!name)
		return -1;
	return snprintf(dst, dstlen, "<qstar-command-param:%s>", name) <
	    (int)dstlen ? 0 : -1;
}

static int
format_command_arg_if_token(const char *name, const char *arg, char *dst,
    size_t dstlen)
{
	size_t arg_len;

	arg_len = strlen(arg ? arg : "");
	return snprintf(dst, dstlen, "<qstar-command-arg-if:%s:%zu:%s>", name,
	    arg_len, arg ? arg : "") < (int)dstlen ? 0 : -1;
}

static int
push_cli_item(lua_State *L, int out_idx, int item_idx)
{
	const char *kind, *name, *arg;
	char token[QSTAR_PATH_MAX];
	size_t dst, i, n;

	if (item_idx < 0)
		item_idx = lua_gettop(L) + item_idx + 1;
	if (lua_isstring(L, item_idx)) {
		dst = lua_rawlen(L, out_idx);
		lua_pushvalue(L, item_idx);
		lua_rawseti(L, out_idx, (lua_Integer)dst + 1);
		return 0;
	}
	if (!lua_istable(L, item_idx))
		return -1;
	kind = qstar_table_kind(L, item_idx);
	if (kind && strcmp(kind, "command_param") == 0) {
		if (format_command_param_token(L, item_idx, token, sizeof(token)) < 0)
			return -1;
		dst = lua_rawlen(L, out_idx);
		lua_pushstring(L, token);
		lua_rawseti(L, out_idx, (lua_Integer)dst + 1);
		return 0;
	}
	if (kind && strcmp(kind, "command_arg_if") == 0) {
		lua_getfield(L, item_idx, "name");
		name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		lua_getfield(L, item_idx, "arg");
		arg = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!name || !arg ||
		    format_command_arg_if_token(name, arg, token, sizeof(token)) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		dst = lua_rawlen(L, out_idx);
		lua_pushstring(L, token);
		lua_rawseti(L, out_idx, (lua_Integer)dst + 1);
		lua_pop(L, 2);
		return 0;
	}
	if (kind && strcmp(kind, "command_args_if") == 0) {
		lua_getfield(L, item_idx, "name");
		name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		lua_getfield(L, item_idx, "args");
		if (!name || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return -1;
		}
		n = lua_rawlen(L, -1);
		for (i = 1; i <= n; i++) {
			lua_rawgeti(L, -1, (lua_Integer)i);
			arg = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
			if (!arg ||
			    format_command_arg_if_token(name, arg, token,
			    sizeof(token)) < 0) {
				lua_pop(L, 3);
				return -1;
			}
			dst = lua_rawlen(L, out_idx);
			lua_pushstring(L, token);
			lua_rawseti(L, out_idx, (lua_Integer)dst + 1);
			lua_pop(L, 1);
		}
		lua_pop(L, 2);
		return 0;
	}
	if (format_placeholder_token(L, item_idx, token, sizeof(token)) == 0) {
		dst = lua_rawlen(L, out_idx);
		lua_pushstring(L, token);
		lua_rawseti(L, out_idx, (lua_Integer)dst + 1);
		return 0;
	}
	return -1;
}

static int
qstar_lua_cli(lua_State *L)
{
	size_t i, n;
	int out_idx;

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	out_idx = lua_gettop(L);
	lua_pushstring(L, "cli");
	lua_setfield(L, -2, "__qstar_kind");
	n = lua_rawlen(L, 1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i);
		if (push_cli_item(L, out_idx, -1) < 0) {
			lua_pop(L, 2);
			return luaL_error(L, "qstar: qstar.cli contains unsupported argv item");
		}
		lua_pop(L, 1);
	}
	return 1;
}

static int
qstar_lua_argv_add(lua_State *L)
{
	size_t n;

	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_type(L, 2) != LUA_TSTRING)
		return luaL_error(L, "qstar: argv:add expects a string");
	n = lua_rawlen(L, 1);
	lua_pushvalue(L, 2);
	lua_rawseti(L, 1, (lua_Integer)n + 1);
	return 0;
}

static int
qstar_lua_argv_add_all(lua_State *L)
{
	size_t i, n, dst;

	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_isnil(L, 2))
		return 0;
	luaL_checktype(L, 2, LUA_TTABLE);
	dst = lua_rawlen(L, 1);
	n = lua_rawlen(L, 2);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 2, (lua_Integer)i);
		if (lua_type(L, -1) != LUA_TSTRING) {
			lua_pop(L, 1);
			return luaL_error(L,
			    "qstar: argv:add_all expects a list of strings");
		}
		lua_rawseti(L, 1, (lua_Integer)(++dst));
	}
	return 0;
}

static int
qstar_lua_argv(lua_State *L)
{
	lua_newtable(L);
	lua_pushstring(L, "argv");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushcfunction(L, qstar_lua_argv_add);
	lua_setfield(L, -2, "add");
	lua_pushcfunction(L, qstar_lua_argv_add_all);
	lua_setfield(L, -2, "add_all");
	return 1;
}

/** qstar.status("...")를 description field 전용 validated helper table로 만든다. */
static int
qstar_lua_status(lua_State *L)
{
	struct qstar_graph fake_graph;
	const char *text;
	size_t len;

	if (lua_gettop(L) != 1)
		return luaL_error(L, "qstar: qstar.status expects exactly one string");
	text = luaL_checklstring(L, 1, &len);
	memset(&fake_graph, 0, sizeof(fake_graph));
	if (validate_status_description(&fake_graph, text, len) < 0)
		return luaL_error(L, "%s", fake_graph.error);
	lua_newtable(L);
	lua_pushstring(L, "status");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushlstring(L, text, len);
	lua_setfield(L, -2, "text");
	return 1;
}

static int
reject_project_command_location(lua_State *L)
{
	struct qstar_lua_context *ctx;

	ctx = get_context(L);
	if (ctx && ctx->module_depth > 0)
		return luaL_error(L,
		    "qstar: qstar.command is forbidden inside .qsm module; project commands must be declared only in root qstar.lua");
	if (ctx && ctx->current_dir[0] != '\0')
		return luaL_error(L,
		    "qstar: qstar.command is only allowed in root qstar.lua");
	return 0;
}

static int
read_optional_string_field(lua_State *L, int table, const char *field,
    char **slot, struct qstar_graph *graph)
{
	const char *value;

	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s' must be a string",
		    field);
	}
	value = lua_tostring(L, -1);
	lua_pop(L, 1);
	return replace_lua_string(slot, value, graph);
}

static int
read_command_option_description(lua_State *L, int table,
    struct qstar_command_option *option, struct qstar_graph *graph)
{
	const char *kind, *text;
	char *copy;

	lua_getfield(L, table, "description");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (lua_isstring(L, -1)) {
		text = lua_tostring(L, -1);
		copy = qstar_strdup(text);
		if (!copy) {
			lua_pop(L, 1);
			return qstar_set_error(graph, "qstar: out of memory");
		}
		free(option->description);
		option->description = copy;
		lua_pop(L, 1);
		return 0;
	}
	if (lua_istable(L, -1)) {
		kind = qstar_table_kind(L, -1);
		if (kind && strcmp(kind, "status") == 0) {
			lua_getfield(L, -1, "text");
			text = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
			copy = qstar_strdup(text ? text : "");
			if (!copy) {
				lua_pop(L, 2);
				return qstar_set_error(graph, "qstar: out of memory");
			}
			free(option->description);
			option->description = copy;
			lua_pop(L, 2);
			return 0;
		}
	}
	lua_pop(L, 1);
	return qstar_set_error(graph,
	    "qstar: command option description must be a string or qstar.status(\"...\")");
}

static int
read_command_option_bool_field(lua_State *L, int table, const char *field,
    int *slot, struct qstar_graph *graph)
{
	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_isboolean(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: command option field '%s' must be boolean",
		    field);
	}
	*slot = lua_toboolean(L, -1) ? 1 : 0;
	lua_pop(L, 1);
	return 0;
}

static int
read_command_option_default(lua_State *L, int table,
    struct qstar_command_option *option, struct qstar_graph *graph)
{
	const char *value;
	char buf[64];

	lua_getfield(L, table, "default");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (strcmp(option->type, "bool") == 0) {
		if (!lua_isboolean(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: bool command option default must be boolean");
		}
		value = lua_toboolean(L, -1) ? "true" : "false";
	} else if (strcmp(option->type, "int") == 0) {
		if (!lua_isinteger(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: int command option default must be integer");
		}
		snprintf(buf, sizeof(buf), "%lld", (long long)lua_tointeger(L, -1));
		value = buf;
	} else if (strcmp(option->type, "list") == 0) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: list command option default is not supported");
	} else {
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: command option default must be a string");
		}
		value = lua_tostring(L, -1);
	}
	if (replace_lua_string(&option->default_value, value, graph) < 0) {
		lua_pop(L, 1);
		return -1;
	}
	option->has_default = 1;
	lua_pop(L, 1);
	return 0;
}

/** qstar.step.run expect table은 run_target.expect와 같은 contains/file 계약을 쓴다. */
static int
read_command_step_expect_field(lua_State *L, int table,
    struct qstar_command_step *step, struct qstar_graph *graph)
{
	static const char *const allowed[] = { "contains", "file", NULL };
	const char *key, *contains, *file;
	size_t len, i;

	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, "expect");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph,
		    "qstar: qstar.step.run expect must be a table with contains and optional file");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: unknown qstar.step.run expect field '%s'",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	lua_getfield(L, -1, "contains");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: qstar.step.run expect.contains must be a string");
	}
	contains = lua_tolstring(L, -1, &len);
	if (!contains || len == 0) {
		lua_pop(L, 2);
		return qstar_set_error(graph,
		    "qstar: qstar.step.run expect.contains must not be empty");
	}
	for (i = 0; i < len; i++) {
		if (contains[i] == '\n' || contains[i] == '\r') {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run expect.contains must be one line");
		}
	}
	if (replace_lua_string(&step->run_expect_contains, contains, graph) < 0) {
		lua_pop(L, 2);
		return -1;
	}
	lua_pop(L, 1);
	lua_getfield(L, -1, "file");
	if (!lua_isnil(L, -1)) {
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run expect.file must be a package-relative string");
		}
		file = lua_tostring(L, -1);
		if (!file || !*file) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run expect.file must not be empty");
		}
		if (!qstar_path_is_package_relative(file)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: qstar.step.run expect.file must be package-relative");
		}
		if (replace_lua_string(&step->run_expect_file, file, graph) < 0) {
			lua_pop(L, 2);
			return -1;
		}
	}
	lua_pop(L, 2);
	return 0;
}

static int
read_command_options_field(lua_State *L, int table,
    struct qstar_project_command *command, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"__qstar_kind", "type", "description", "required", "default",
		"choices", NULL
	};
	struct qstar_command_option *option;
	const char *name, *kind, *type;

	lua_getfield(L, table, "options");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'options' must be a table");
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!name || !lua_istable(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: command options must map option names to qstar.param.* schemas");
		}
		kind = qstar_table_kind(L, -1);
		if (!kind || strcmp(kind, "command_option") != 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: command option '%s' must be qstar.param.* { ... }",
			    name);
		}
		if (qstar_lua_validate_table_fields(L, -1, graph,
		    "command option", allowed) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "type");
		type = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!type) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: command option '%s' is missing type", name);
		}
		option = qstar_project_command_add_option(graph, command, name, type);
		lua_pop(L, 1);
		if (!option) {
			lua_pop(L, 2);
			return -1;
		}
		if (read_command_option_description(L, -1, option, graph) < 0 ||
		    read_command_option_bool_field(L, -1, "required",
		    &option->required, graph) < 0 ||
		    read_command_option_default(L, -1, option, graph) < 0 ||
		    read_list_field(L, -1, "choices", &option->choices, graph, 0,
		    command->name) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
read_command_steps_field(lua_State *L, int table,
    struct qstar_project_command *command, struct qstar_graph *graph)
{
	struct qstar_command_step *step;
	struct qstar_string_list empty;
	const char *kind, *label, *called, *root, *when, *workdir, *export_to;
	size_t i, n;

	memset(&empty, 0, sizeof(empty));
	lua_getfield(L, table, "steps");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'steps' must be a list");
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (!lua_istable(L, -1) ||
		    !(kind = qstar_table_kind(L, -1)) ||
		    strcmp(kind, "command_step") != 0) {
			lua_pop(L, 2);
			return qstar_set_error(graph,
			    "qstar: command steps must contain qstar.step.* values");
		}
		lua_getfield(L, -1, "kind");
		kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!kind) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: command step is missing kind");
		}
		step = qstar_project_command_add_step(graph, command, kind);
		lua_pop(L, 1);
		if (!step) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "label");
		label = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (label && replace_lua_string(&step->label, label, graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "command");
		called = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (called && replace_lua_string(&step->command, called, graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "root");
		root = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (root && replace_lua_string(&step->stage_root, root, graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "dry_run");
		if (!lua_isnil(L, -1)) {
			if (!lua_isboolean(L, -1)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: command step field 'dry_run' must be boolean");
			}
			step->stage_dry_run = lua_toboolean(L, -1) ? 1 : 0;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "timeout");
		if (!lua_isnil(L, -1)) {
			if (!lua_isinteger(L, -1)) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: qstar.step.run field 'timeout' must be integer");
			}
			step->timeout_sec = (int)lua_tointeger(L, -1);
			if (step->timeout_sec < 0) {
				lua_pop(L, 3);
				return qstar_set_error(graph,
				    "qstar: qstar.step.run timeout must be >= 0");
			}
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "when");
		when = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (when && replace_lua_string(&step->when, when, graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, -1, "working_dir");
		workdir = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (workdir && replace_lua_string(&step->working_dir, workdir,
		    graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		if (read_status_description_field(L, -1, graph,
		    &step->description) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		if (read_list_field(L, -1, "env", &step->env, graph, 0,
		    command->name) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		lua_getfield(L, -1, "to");
		export_to = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (export_to && replace_lua_string(&step->export_to, export_to,
		    graph) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
		if (read_command_step_inputs_field(L, -1, step, graph) < 0) {
			lua_pop(L, 2);
			return -1;
		}
		if (strcmp(kind, "run") == 0) {
			if (read_command_field(L, -1, "command", &step->run_command,
			    graph) < 0 ||
			    resolve_cli_placeholders(graph, &step->run_command,
			    &step->inputs, &empty,
			    "project command run step command") < 0 ||
			    read_command_step_expect_field(L, -1, step, graph) < 0) {
				lua_pop(L, 2);
				return -1;
			}
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
add_project_command(lua_State *L, const char *name, int table_index)
{
	static const char *const allowed[] = {
		"description", "options", "env", "working_dir", "steps",
		"is_default", "hidden", "aliases", NULL
	};
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	struct qstar_project_command *command;
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_project_command_location(L) != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	command = qstar_graph_add_project_command(graph, name, origin_file, origin_line);
	if (!command)
		return luaL_error(L, "%s", graph->error);
	if (qstar_lua_validate_table_fields(L, table_index, graph, "qstar.command",
	    allowed) < 0 ||
	    read_status_description_field(L, table_index, graph,
	    &command->description) < 0 ||
	    read_list_field(L, table_index, "env", &command->env, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table_index, "aliases", &command->aliases, graph, 0,
	    "") < 0 ||
	    read_optional_string_field(L, table_index, "working_dir",
	    &command->working_dir, graph) < 0 ||
	    read_command_options_field(L, table_index, command, graph) < 0 ||
	    read_command_steps_field(L, table_index, command, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	command->is_default = check_bool_field(L, table_index, "is_default", 0);
	command->hidden = check_bool_field(L, table_index, "hidden", 0);
	return 0;
}

static int
qstar_lua_command_finish(lua_State *L)
{
	const char *name;

	name = lua_tostring(L, lua_upvalueindex(1));
	return add_project_command(L, name, 1);
}

static int
qstar_lua_command(lua_State *L)
{
	const char *name;

	if (reject_project_command_location(L) != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushcclosure(L, qstar_lua_command_finish, 1);
		return 1;
	}
	return add_project_command(L, name, 2);
}

static void
push_command_step_table(lua_State *L, const char *kind)
{
	lua_newtable(L);
	lua_pushstring(L, "command_step");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, kind);
	lua_setfield(L, -2, "kind");
}

static int
set_command_step_when_from_table(lua_State *L, int step_idx, int opts_idx,
    const char *api, int allow_stage_opts)
{
	const char *key, *name;

	if (step_idx < 0)
		step_idx = lua_gettop(L) + step_idx + 1;
	if (opts_idx < 0)
		opts_idx = lua_gettop(L) + opts_idx + 1;
	lua_pushnil(L);
	while (lua_next(L, opts_idx) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || (strcmp(key, "when") != 0 &&
		    (!allow_stage_opts || (strcmp(key, "root") != 0 &&
		    strcmp(key, "dry_run") != 0))))
			return luaL_error(L, "qstar: unknown %s option '%s'", api,
			    key ? key : "<non-string>");
		lua_pop(L, 1);
	}
	lua_getfield(L, opts_idx, "when");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1) ||
		    !(name = command_param_table_name(L, -1)))
			return luaL_error(L,
			    "qstar: %s when must be qstar.param(\"name\")", api);
		lua_pushstring(L, name);
		lua_setfield(L, step_idx, "when");
	}
	lua_pop(L, 1);
	return 0;
}

static int
qstar_lua_step_label(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *kind, *raw;
	char label[QSTAR_PATH_MAX];
	char api_name[64];
	int out_idx, has_opts;

	ctx = get_context(L);
	kind = lua_tostring(L, lua_upvalueindex(1));
	raw = luaL_checkstring(L, 1);
	has_opts = !lua_isnoneornil(L, 2);
	if (strcmp(raw, "//...") == 0) {
		snprintf(label, sizeof(label), "%s", raw);
	} else if (qstar_label_canonicalize(raw, ctx->current_dir, label,
	    sizeof(label)) < 0) {
		return luaL_error(L, "qstar: invalid qstar.step.%s label '%s'",
		    kind, raw);
	}
	push_command_step_table(L, kind);
	out_idx = lua_gettop(L);
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	if (has_opts) {
		luaL_checktype(L, 2, LUA_TTABLE);
		snprintf(api_name, sizeof(api_name), "qstar.step.%s", kind);
		if (set_command_step_when_from_table(L, out_idx, 2,
		    api_name, 0) != 0)
			return 1;
	}
	return 1;
}

static int
qstar_lua_step_stage(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *raw, *root, *key;
	char label[QSTAR_PATH_MAX];
	int out_idx, has_opts;

	ctx = get_context(L);
	raw = luaL_checkstring(L, 1);
	has_opts = !lua_isnoneornil(L, 2);
	if (qstar_label_canonicalize(raw, ctx->current_dir, label, sizeof(label)) < 0)
		return luaL_error(L, "qstar: invalid qstar.step.stage label '%s'",
		    raw);
	push_command_step_table(L, "stage");
	out_idx = lua_gettop(L);
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	if (has_opts) {
		luaL_checktype(L, 2, LUA_TTABLE);
		if (set_command_step_when_from_table(L, out_idx, 2,
		    "qstar.step.stage", 1) != 0)
			return 1;
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
			if (!key || (strcmp(key, "root") != 0 &&
			    strcmp(key, "dry_run") != 0 &&
			    strcmp(key, "when") != 0))
				return luaL_error(L,
				    "qstar: unknown qstar.step.stage option '%s'",
				    key ? key : "<non-string>");
			lua_pop(L, 1);
		}
		lua_getfield(L, 2, "root");
		if (!lua_isnil(L, -1)) {
			if (!lua_isstring(L, -1))
				return luaL_error(L,
				    "qstar: qstar.step.stage root must be a string");
			root = lua_tostring(L, -1);
			lua_pushstring(L, root);
			lua_setfield(L, out_idx, "root");
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "dry_run");
		if (!lua_isnil(L, -1)) {
			if (!lua_isboolean(L, -1))
				return luaL_error(L,
				    "qstar: qstar.step.stage dry_run must be boolean");
			lua_pushboolean(L, lua_toboolean(L, -1));
			lua_setfield(L, out_idx, "dry_run");
		}
		lua_pop(L, 1);
	}
	return 1;
}

static int
qstar_lua_step_export_stage(lua_State *L)
{
	static const char *const allowed[] = { "to", "dry_run", "when", NULL };
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	const char *raw, *to, *name;
	char label[QSTAR_PATH_MAX], token[QSTAR_PATH_MAX];
	int out_idx;

	ctx = get_context(L);
	graph = ctx->graph;
	raw = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	if (qstar_label_canonicalize(raw, ctx->current_dir, label, sizeof(label)) < 0)
		return luaL_error(L,
		    "qstar: invalid qstar.step.export_stage label '%s'", raw);
	if (qstar_lua_validate_table_fields(L, 2, graph, "qstar.step.export_stage",
	    allowed) < 0)
		return luaL_error(L, "%s", graph->error);
	push_command_step_table(L, "export_stage");
	out_idx = lua_gettop(L);
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	lua_getfield(L, 2, "to");
	if (lua_isnil(L, -1))
		return luaL_error(L, "qstar: qstar.step.export_stage requires to");
	if (lua_isstring(L, -1)) {
		to = lua_tostring(L, -1);
		lua_pushstring(L, to);
		lua_setfield(L, out_idx, "to");
	} else if (lua_istable(L, -1) && command_param_table_name(L, -1) &&
	    format_command_param_token(L, -1, token, sizeof(token)) == 0) {
		lua_pushstring(L, token);
		lua_setfield(L, out_idx, "to");
	} else {
		return luaL_error(L,
		    "qstar: qstar.step.export_stage to must be a string or qstar.param(\"name\")");
	}
	lua_pop(L, 1);
	lua_getfield(L, 2, "dry_run");
	if (!lua_isnil(L, -1)) {
		if (!lua_isboolean(L, -1))
			return luaL_error(L,
			    "qstar: qstar.step.export_stage dry_run must be boolean");
		lua_pushboolean(L, lua_toboolean(L, -1));
		lua_setfield(L, out_idx, "dry_run");
	}
	lua_pop(L, 1);
	lua_getfield(L, 2, "when");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1) ||
		    !(name = command_param_table_name(L, -1)))
			return luaL_error(L,
			    "qstar: qstar.step.export_stage when must be qstar.param(\"name\")");
		lua_pushstring(L, name);
		lua_setfield(L, out_idx, "when");
	}
	lua_pop(L, 1);
	return 1;
}

static int
qstar_lua_step_call(lua_State *L)
{
	const char *name;
	int out_idx, has_opts;

	name = luaL_checkstring(L, 1);
	has_opts = !lua_isnoneornil(L, 2);
	push_command_step_table(L, "call");
	out_idx = lua_gettop(L);
	lua_pushstring(L, name);
	lua_setfield(L, -2, "command");
	if (has_opts) {
		luaL_checktype(L, 2, LUA_TTABLE);
		if (set_command_step_when_from_table(L, out_idx, 2,
		    "qstar.step.call", 0) != 0)
			return 1;
	}
	return 1;
}

static int
qstar_lua_step_run(lua_State *L)
{
	static const char *const allowed[] = {
		"command", "inputs", "env", "working_dir", "description",
		"timeout", "expect", "when", NULL
	};
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	const char *name;
	int out_idx;

	ctx = get_context(L);
	graph = ctx->graph;
	luaL_checktype(L, 1, LUA_TTABLE);
	if (qstar_lua_validate_table_fields(L, 1, graph, "qstar.step.run",
	    allowed) < 0)
		return luaL_error(L, "%s", graph->error);
	push_command_step_table(L, "run");
	out_idx = lua_gettop(L);
	lua_getfield(L, 1, "command");
	if (lua_isnil(L, -1))
		return luaL_error(L, "qstar: qstar.step.run requires command");
	if (!lua_istable(L, -1) || !qstar_table_kind(L, -1) ||
	    strcmp(qstar_table_kind(L, -1), "cli") != 0)
		return luaL_error(L,
		    "qstar: qstar.step.run command must be qstar.cli { ... }");
	lua_setfield(L, out_idx, "command");
	lua_getfield(L, 1, "env");
	if (!lua_isnil(L, -1))
		lua_setfield(L, out_idx, "env");
	else
		lua_pop(L, 1);
	lua_getfield(L, 1, "inputs");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1))
			return luaL_error(L,
			    "qstar: qstar.step.run inputs must be a list");
		lua_setfield(L, out_idx, "inputs");
	} else {
		lua_pop(L, 1);
	}
	lua_getfield(L, 1, "working_dir");
	if (!lua_isnil(L, -1)) {
		if (!lua_isstring(L, -1))
			return luaL_error(L,
			    "qstar: qstar.step.run working_dir must be a string");
		lua_setfield(L, out_idx, "working_dir");
	} else {
		lua_pop(L, 1);
	}
	lua_getfield(L, 1, "description");
	if (!lua_isnil(L, -1))
		lua_setfield(L, out_idx, "description");
	else
		lua_pop(L, 1);
	lua_getfield(L, 1, "timeout");
	if (!lua_isnil(L, -1)) {
		if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0)
			return luaL_error(L,
			    "qstar: qstar.step.run timeout must be >= 0");
		lua_setfield(L, out_idx, "timeout");
	} else {
		lua_pop(L, 1);
	}
	lua_getfield(L, 1, "expect");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1))
			return luaL_error(L,
			    "qstar: qstar.step.run expect must be a table");
		lua_setfield(L, out_idx, "expect");
	} else {
		lua_pop(L, 1);
	}
	lua_getfield(L, 1, "when");
	if (!lua_isnil(L, -1)) {
		if (!lua_istable(L, -1) ||
		    !(name = command_param_table_name(L, -1)))
			return luaL_error(L,
			    "qstar: qstar.step.run when must be qstar.param(\"name\")");
		lua_pushstring(L, name);
		lua_setfield(L, out_idx, "when");
	}
	lua_pop(L, 1);
	return 1;
}

static int
qstar_lua_param_ref(lua_State *L)
{
	const char *name;
	int name_idx;

	name_idx = lua_istable(L, 1) ? 2 : 1;
	name = luaL_checkstring(L, name_idx);
	if (!valid_command_option_ref_name(name))
		return luaL_error(L, "qstar: invalid command option reference '%s'",
		    name);
	lua_newtable(L);
	lua_pushstring(L, "command_param");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, name);
	lua_setfield(L, -2, "name");
	return 1;
}

static int
qstar_lua_arg_if(lua_State *L)
{
	const char *name, *arg;

	luaL_checktype(L, 1, LUA_TTABLE);
	name = command_param_table_name(L, 1);
	if (!name)
		return luaL_error(L,
		    "qstar: qstar.arg_if condition must be qstar.param(\"name\")");
	arg = luaL_checkstring(L, 2);
	lua_newtable(L);
	lua_pushstring(L, "command_arg_if");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, name);
	lua_setfield(L, -2, "name");
	lua_pushstring(L, arg);
	lua_setfield(L, -2, "arg");
	return 1;
}

static int
qstar_lua_args_if(lua_State *L)
{
	const char *name;
	size_t i, n;

	luaL_checktype(L, 1, LUA_TTABLE);
	name = command_param_table_name(L, 1);
	if (!name)
		return luaL_error(L,
		    "qstar: qstar.args_if condition must be qstar.param(\"name\")");
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_newtable(L);
	lua_pushstring(L, "command_args_if");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, name);
	lua_setfield(L, -2, "name");
	lua_newtable(L);
	n = lua_rawlen(L, 2);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 2, (lua_Integer)i);
		if (!lua_isstring(L, -1))
			return luaL_error(L,
			    "qstar: qstar.args_if args must be a list of strings");
		lua_rawseti(L, -2, (lua_Integer)i);
	}
	lua_setfield(L, -2, "args");
	return 1;
}

static int
qstar_lua_param_type(lua_State *L)
{
	const char *type;
	int src, dst;

	type = lua_tostring(L, lua_upvalueindex(1));
	if (!lua_isnoneornil(L, 1))
		luaL_checktype(L, 1, LUA_TTABLE);
	src = lua_isnoneornil(L, 1) ? 0 : 1;
	lua_newtable(L);
	dst = lua_gettop(L);
	if (src) {
		lua_pushnil(L);
		while (lua_next(L, src) != 0) {
			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);
			lua_settable(L, dst);
			lua_pop(L, 1);
		}
	}
	lua_pushstring(L, "command_option");
	lua_setfield(L, dst, "__qstar_kind");
	lua_pushstring(L, type);
	lua_setfield(L, dst, "type");
	return 1;
}

/** qstar.project metadata를 graph에 등록하고 v1 root contract를 검증한다. */
static int
qstar_lua_project(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *name, *version, *root, *build_dir, *generated_dir, *compile_commands;
	int idx;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.project") != 0)
		return 1;
	idx = lua_gettop(L) >= 2 && lua_istable(L, 2) ? 2 : 1;
	luaL_checktype(L, idx, LUA_TTABLE);
	name = check_string_field(L, idx, "name");
	version = check_string_field(L, idx, "version");
	root = check_string_field(L, idx, "root");
	build_dir = check_string_field(L, idx, "build_dir");
	generated_dir = check_string_field(L, idx, "generated_dir");
	compile_commands = check_string_field(L, idx, "compile_commands");
	if (qstar_graph_set_project(ctx->graph, name, version, root, build_dir,
	    generated_dir, compile_commands) < 0)
		return luaL_error(L, "%s", ctx->graph->error);
	return 0;
}

static int eval_fragment(lua_State *L, struct qstar_lua_context *ctx, const char *file,
    const char *fragment_dir);
static int eval_module(lua_State *L, struct qstar_lua_context *ctx, const char *file,
    const char *module_dir, const char *rel);
static int eval_provider_implementation(lua_State *L, struct qstar_lua_context *ctx,
    const char *file, const char *provider_dir, const char *rel);

/** filesystem path를 package-relative authoring input path로 변환한다. */
static int
package_relative_input(struct qstar_lua_context *ctx, const char *file, char *rel,
    size_t rellen)
{
	const char *p;
	size_t n;

	p = file;
	n = strlen(ctx->root_dir);
	if (n > 0 && strcmp(ctx->root_dir, ".") != 0 &&
	    strncmp(file, ctx->root_dir, n) == 0 && file[n] == '/')
		p = file + n + 1;
	else if (strncmp(file, "./", 2) == 0)
		p = file + 2;
	return snprintf(rel, rellen, "%s", p) < (int)rellen ? 0 : -1;
}

/** 평가한 authoring input을 graph와 import stack에 기록한다. */
static int
enter_authoring_input(struct qstar_lua_context *ctx, const char *rel)
{
	if (qstar_string_list_push(&ctx->graph->evaluated_fragments, rel) < 0)
		return -1;
	if (qstar_string_list_push(&ctx->import_stack, rel) < 0)
		return -1;
	return 0;
}

/** 현재 authoring input 평가 stack에서 빠져나온다. */
static void
leave_authoring_input(struct qstar_lua_context *ctx)
{
	string_list_pop(&ctx->import_stack);
}

/** package-root 기준 import path와 실제 path를 만든다. */
static int
resolve_import_path(struct qstar_lua_context *ctx, const char *raw, char *rel,
    size_t rellen, char *full, size_t fulllen)
{
	if (!qstar_path_is_package_relative(raw))
		return -1;
	if (snprintf(rel, rellen, "%s", raw) >= (int)rellen)
		return -1;
	return qstar_path_join(ctx->root_dir, rel, full, fulllen);
}

static void
push_module_export_cache(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.module.exports");
	if (lua_istable(L, -1))
		return;
	lua_pop(L, 1);
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_setfield(L, LUA_REGISTRYINDEX, "qstar.module.exports");
}

static int
push_cached_module_export(lua_State *L, const char *rel)
{
	push_module_export_cache(L);
	lua_getfield(L, -1, rel);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 2);
		return 0;
	}
	lua_remove(L, -2);
	return 1;
}

static void
cache_module_export(lua_State *L, const char *rel, int value)
{
	value = qstar_lua_abs_index(L, value);
	push_module_export_cache(L);
	lua_pushvalue(L, value);
	lua_setfield(L, -2, rel);
	lua_pop(L, 1);
}

static int
readonly_module_pairs(lua_State *L)
{
	lua_getglobal(L, "next");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushnil(L);
	return 3;
}

static int
readonly_module_len(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)lua_rawlen(L, lua_upvalueindex(1)));
	return 1;
}

static void
push_readonly_module_export(lua_State *L, int table)
{
	table = qstar_lua_abs_index(L, table);
	lua_newtable(L);
	lua_newtable(L);
	lua_pushvalue(L, table);
	lua_setfield(L, -2, "__index");
	lua_pushstring(L, "module exports");
	lua_pushcclosure(L, readonly_table_assignment_forbidden, 1);
	lua_setfield(L, -2, "__newindex");
	lua_pushvalue(L, table);
	lua_pushcclosure(L, readonly_module_pairs, 1);
	lua_setfield(L, -2, "__pairs");
	lua_pushvalue(L, table);
	lua_pushcclosure(L, readonly_module_len, 1);
	lua_setfield(L, -2, "__len");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);
}

/** folder path의 basename component를 반환한다. */
static const char *
path_basename_component(const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int
filesystem_file_exists(const char *path)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
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
standard_provider_manifest_from_base(const char *base, const char *id,
    char *dir, size_t dir_len, char *manifest, size_t manifest_len)
{
	char candidate_dir[QSTAR_PATH_MAX], candidate_manifest[QSTAR_PATH_MAX];

	if (!base || !*base)
		return 0;
	if (qstar_path_join(base, id, candidate_dir, sizeof(candidate_dir)) < 0 ||
	    snprintf(candidate_manifest, sizeof(candidate_manifest), "%s/%s.qsm",
	    candidate_dir, id) >= (int)sizeof(candidate_manifest))
		return -1;
	if (!filesystem_file_exists(candidate_manifest))
		return 0;
	if (copy_existing_path(candidate_dir, dir, dir_len) < 0 ||
	    copy_existing_path(candidate_manifest, manifest,
	    manifest_len) < 0)
		return -1;
	return 1;
}

static int
resolve_standard_language_provider_path(const char *id, char *dir, size_t dir_len,
    char *manifest, size_t manifest_len)
{
	const char *env_dir;
	char exe[QSTAR_PATH_MAX], bin_dir[QSTAR_PATH_MAX], prefix[QSTAR_PATH_MAX];
	char source_build[QSTAR_PATH_MAX], source_root[QSTAR_PATH_MAX];
	char base[QSTAR_PATH_MAX];
	int rc;

	env_dir = getenv("QSTAR_PROVIDER_DIR");
	rc = standard_provider_manifest_from_base(env_dir, id, dir, dir_len, manifest,
	    manifest_len);
	if (rc != 0)
		return rc;
	if (current_executable_path(exe, sizeof(exe)) < 0 ||
	    qstar_dirname(exe, bin_dir, sizeof(bin_dir)) < 0)
		return 0;
	if (qstar_dirname(bin_dir, prefix, sizeof(prefix)) == 0 &&
	    qstar_path_join(prefix, "share/qstar/languages", base, sizeof(base)) == 0) {
		rc = standard_provider_manifest_from_base(base, id, dir, dir_len,
		    manifest, manifest_len);
		if (rc != 0)
			return rc;
	}
	if (qstar_dirname(bin_dir, source_build, sizeof(source_build)) == 0 &&
	    qstar_dirname(source_build, source_root, sizeof(source_root)) == 0 &&
	    qstar_path_join(source_root, "qstar/languages", base, sizeof(base)) == 0) {
		rc = standard_provider_manifest_from_base(base, id, dir, dir_len,
		    manifest, manifest_len);
		if (rc != 0)
			return rc;
	}
	return 0;
}

/** qstar.import_file("path.qst")로 명시적 graph fragment를 평가한다. */
static int
qstar_lua_import_file(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *raw;
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], fragment_dir[QSTAR_PATH_MAX];
	char chain[512];
	FILE *f;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.import_file") != 0)
		return 1;
	raw = luaL_checkstring(L, 1);
	if (!path_has_suffix(raw, ".qst"))
		return luaL_error(L, "qstar: import_file path '%s' must end with .qst",
		    raw);
	if (resolve_import_path(ctx, raw, rel, sizeof(rel), full, sizeof(full)) < 0)
		return luaL_error(L,
		    "qstar: import_file path '%s' must be package-relative", raw);
	if (string_list_contains(&ctx->import_stack, rel)) {
		format_import_chain(ctx, rel, chain, sizeof(chain));
		return luaL_error(L, "qstar: circular import chain: %s", chain);
	}
	if (string_list_contains(&ctx->graph->evaluated_fragments, rel))
		return luaL_error(L, "qstar: duplicate import '%s'", rel);
	f = fopen(full, "r");
	if (!f)
		return luaL_error(L,
		    "qstar: import_file '%s' not found; expected package-relative .qst file '%s'",
		    raw, rel);
	fclose(f);
	if (qstar_dirname(rel, fragment_dir, sizeof(fragment_dir)) < 0)
		return luaL_error(L, "qstar: import_file path '%s' is too long", rel);
	if (strcmp(fragment_dir, ".") == 0)
		fragment_dir[0] = '\0';
	if (eval_fragment(L, ctx, full, fragment_dir) < 0)
		return lua_error(L);
	return 0;
}

/** qstar.import_module("folder")를 folder/basename.qsm module table로 평가한다. */
static int
qstar_lua_import_module(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *raw, *base;
	char rel_dir[QSTAR_PATH_MAX];
	char rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX], expected_folder[QSTAR_PATH_MAX];
	char chain[512];
	size_t n;
	FILE *f;

	ctx = get_context(L);
	raw = luaL_checkstring(L, 1);
	if (path_has_suffix(raw, ".qsm") || path_has_suffix(raw, ".qst") ||
	    strcmp(raw, "qstar.lua") == 0) {
		snprintf(expected_folder, sizeof(expected_folder), "%s", raw);
		if (path_has_suffix(expected_folder, ".qsm") ||
		    path_has_suffix(expected_folder, ".qst")) {
			char *slash, *dot;
			slash = strrchr(expected_folder, '/');
			dot = strrchr(expected_folder, '.');
			if (dot)
				*dot = '\0';
			if (slash && slash[1] && strcmp(slash + 1,
			    path_basename_component(expected_folder)) == 0)
				*slash = '\0';
		}
		return luaL_error(L,
		    "qstar: import_module expects a folder path, not file '%s'; use qstar.import_module(\"%s\") and keep the entry file at <folder>/<name>.qsm",
		    raw, expected_folder);
	}
	n = strlen(raw);
	if (n == 0 || raw[n - 1] == '/')
		return luaL_error(L,
		    "qstar: import_module expects a normalized folder path, not '%s'",
		    raw);
	if (resolve_import_path(ctx, raw, rel_dir, sizeof(rel_dir), full,
	    sizeof(full)) < 0)
		return luaL_error(L,
		    "qstar: import_module path '%s' must be package-relative", raw);
	base = path_basename_component(rel_dir);
	if (!base || !*base)
		return luaL_error(L,
		    "qstar: import_module path '%s' has no module basename", raw);
	if (snprintf(rel, sizeof(rel), "%s/%s.qsm", rel_dir, base) >=
	    (int)sizeof(rel) ||
	    qstar_path_join(ctx->root_dir, rel, full, sizeof(full)) < 0)
		return luaL_error(L, "qstar: import_module path '%s' is too long",
		    raw);
	if (string_list_contains(&ctx->import_stack, rel)) {
		format_import_chain(ctx, rel, chain, sizeof(chain));
		return luaL_error(L, "qstar: circular import chain: %s", chain);
	}
	if (push_cached_module_export(L, rel))
		return 1;
	if (string_list_contains(&ctx->graph->evaluated_fragments, rel))
		return luaL_error(L,
		    "qstar: import_module '%s' was already evaluated as a graph input",
		    rel);
	f = fopen(full, "r");
	if (!f)
		return luaL_error(L,
		    "qstar: import_module '%s' not found; expected module entry '%s'",
		    raw, rel);
	fclose(f);
	if (eval_module(L, ctx, full, rel_dir, rel) < 0)
		return lua_error(L);
	push_readonly_module_export(L, -1);
	cache_module_export(L, rel, -1);
	lua_remove(L, -2);
	return 1;
}

/** qstar.use_language("zig") 또는 qstar.use_language("qstar/languages/zig")를 manifest로 해석한다. */
static int
resolve_language_provider_path(struct qstar_lua_context *ctx, const char *raw,
    char *rel_dir, size_t rel_dir_len, char *rel, size_t rel_len, char *full,
    size_t full_len)
{
	const char *base;
	char standard_dir[QSTAR_PATH_MAX];
	char standard_manifest[QSTAR_PATH_MAX];
	size_t n;
	int is_id, standard_rc;

	if (!raw || !*raw)
		return qstar_set_error(ctx->graph, "qstar: use_language id is empty");
	if (path_has_suffix(raw, ".qsm") || path_has_suffix(raw, ".qst") ||
	    strcmp(raw, "qstar.lua") == 0)
		return qstar_set_error(ctx->graph,
		    "qstar: use_language expects a provider id or folder path, not file '%s'",
		    raw);
	n = strlen(raw);
	if (raw[n - 1] == '/')
		return qstar_set_error(ctx->graph,
		    "qstar: use_language expects a normalized provider path, not '%s'",
		    raw);
	is_id = strchr(raw, '/') == NULL;
	if (strchr(raw, '/')) {
		if (!qstar_path_is_package_relative(raw))
			return qstar_set_error(ctx->graph,
			    "qstar: use_language path '%s' must be package-relative", raw);
		if (snprintf(rel_dir, rel_dir_len, "%s", raw) >= (int)rel_dir_len)
			return qstar_set_error(ctx->graph,
			    "qstar: use_language path '%s' is too long", raw);
	} else {
		if (!valid_tool_role_component(raw))
			return qstar_set_error(ctx->graph,
			    "qstar: invalid language provider id '%s'", raw);
		if (snprintf(standard_dir, sizeof(standard_dir), "qstar/languages/%s",
		    raw) >= (int)sizeof(standard_dir) ||
		    snprintf(rel_dir, rel_dir_len, "%s", standard_dir) >=
		    (int)rel_dir_len)
			return qstar_set_error(ctx->graph,
			    "qstar: use_language id '%s' is too long", raw);
	}
	base = path_basename_component(rel_dir);
	if (!base || !*base)
		return qstar_set_error(ctx->graph,
		    "qstar: use_language path '%s' has no provider basename", raw);
	if (snprintf(rel, rel_len, "%s/%s.qsm", rel_dir, base) >= (int)rel_len ||
	    qstar_path_join(ctx->root_dir, rel, full, full_len) < 0)
		return qstar_set_error(ctx->graph,
		    "qstar: use_language path '%s' is too long", raw);
	if (filesystem_file_exists(full))
		return 0;
	if (is_id) {
		standard_rc = resolve_standard_language_provider_path(raw, standard_dir,
		    sizeof(standard_dir), standard_manifest, sizeof(standard_manifest));
		if (standard_rc < 0)
			return qstar_set_error(ctx->graph,
			    "qstar: standard language provider path for '%s' is too long",
			    raw);
		if (standard_rc > 0) {
			if (snprintf(rel_dir, rel_dir_len, "%s", standard_dir) >=
			    (int)rel_dir_len ||
			    snprintf(rel, rel_len, "%s", standard_manifest) >=
			    (int)rel_len ||
			    snprintf(full, full_len, "%s", standard_manifest) >=
			    (int)full_len)
				return qstar_set_error(ctx->graph,
				    "qstar: standard language provider path for '%s' is too long",
				    raw);
			return 0;
		}
	}
	return 0;
}

static int
read_language_provider_manifest(lua_State *L, int table, struct qstar_graph *graph,
    char *api, size_t api_len, char *id, size_t id_len, char *namespace,
    size_t namespace_len, char *version, size_t version_len, char *implementation,
    size_t implementation_len)
{
	return validate_language_provider_manifest_table(L, table, graph, api,
	    api_len, id, id_len, namespace, namespace_len, version, version_len,
	    implementation, implementation_len, 1);
}

static int
build_language_provider_exports(lua_State *L, int manifest, int implementation,
    struct qstar_graph *graph, const char *id)
{
	const char *export_name, *implementation_name;
	int exports, result;

	manifest = qstar_lua_abs_index(L, manifest);
	implementation = qstar_lua_abs_index(L, implementation);
	lua_getfield(L, manifest, "exports");
	exports = lua_gettop(L);
	lua_newtable(L);
	result = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, exports) != 0) {
		export_name = lua_tostring(L, -2);
		implementation_name = lua_tostring(L, -1);
		lua_getfield(L, implementation, implementation_name);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 4);
			return qstar_set_error(graph,
			    "qstar: language provider '%s' export '%s' references missing implementation field '%s'",
			    id, export_name ? export_name : "<non-string>",
			    implementation_name ? implementation_name : "<non-string>");
		}
		qstar_lua_push_value_copy(L, -1, 0);
		lua_setfield(L, result, export_name);
		lua_pop(L, 2);
	}
	lua_remove(L, exports);
	return 0;
}

static int
add_language_provider_option_schemas(lua_State *L, int manifest,
    struct qstar_graph *graph, struct qstar_language_provider *provider)
{
	struct qstar_string_list values, default_list;
	const char *name, *type, *canonical, *default_value;
	int has_default, option_idx;

	manifest = qstar_lua_abs_index(L, manifest);
	lua_getfield(L, manifest, "options");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		memset(&values, 0, sizeof(values));
		memset(&default_list, 0, sizeof(default_list));
		name = lua_tostring(L, -2);
		option_idx = lua_gettop(L);
		lua_getfield(L, option_idx, "type");
		type = lua_tostring(L, -1);
		canonical = canonical_provider_option_type(type);
		lua_pop(L, 1);
		if (!canonical) {
			lua_pop(L, 3);
			return qstar_set_error(graph,
			    "qstar: language provider options.%s.type is unsupported",
			    name ? name : "<non-string>");
		}
		if (strcmp(canonical, "enum") == 0) {
			lua_getfield(L, option_idx, "values");
			if (read_exact_string_list_value(L, -1, &values, graph,
			    "language provider enum values") < 0) {
				lua_pop(L, 4);
				qstar_string_list_free(&values);
				return -1;
			}
			lua_pop(L, 1);
		}
		has_default = 0;
		default_value = "";
		lua_getfield(L, option_idx, "default");
		if (!lua_isnil(L, -1)) {
			has_default = 1;
			if (strcmp(canonical, "bool") == 0) {
				default_value = lua_toboolean(L, -1) ? "true" : "false";
			} else if (strcmp(canonical, "list") == 0) {
				if (read_exact_string_list_value(L, -1, &default_list, graph,
				    "language provider option default") < 0) {
					lua_pop(L, 4);
					qstar_string_list_free(&values);
					qstar_string_list_free(&default_list);
					return -1;
				}
			} else {
				default_value = lua_tostring(L, -1);
			}
		}
		lua_pop(L, 1);
		if (qstar_language_provider_add_option_schema(graph, provider, name,
		    canonical, &values, has_default, default_value, &default_list) < 0) {
			lua_pop(L, 3);
			qstar_string_list_free(&values);
			qstar_string_list_free(&default_list);
			return -1;
		}
		qstar_string_list_free(&values);
		qstar_string_list_free(&default_list);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
add_language_provider_unit_schemas(lua_State *L, int manifest,
    struct qstar_graph *graph, struct qstar_language_provider *provider)
{
	struct qstar_string_list suffixes;
	const char *name, *emits, *lower, *deps;
	int unit_idx;

	manifest = qstar_lua_abs_index(L, manifest);
	lua_getfield(L, manifest, "units");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		memset(&suffixes, 0, sizeof(suffixes));
		name = lua_tostring(L, -2);
		unit_idx = lua_gettop(L);
		lua_getfield(L, unit_idx, "suffixes");
		if (read_exact_string_list_value(L, -1, &suffixes, graph,
		    "language provider unit suffixes") < 0) {
			lua_pop(L, 4);
			qstar_string_list_free(&suffixes);
			return -1;
		}
		lua_pop(L, 1);
		lua_getfield(L, unit_idx, "emits");
		emits = lua_tostring(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, unit_idx, "lower");
		lower = lua_tostring(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, unit_idx, "deps");
		deps = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		if (qstar_language_provider_add_unit_schema(graph, provider, name,
		    &suffixes, emits, lower, deps) < 0) {
			lua_pop(L, 3);
			qstar_string_list_free(&suffixes);
			return -1;
		}
		qstar_string_list_free(&suffixes);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

static int
add_language_provider_final_schemas(lua_State *L, int manifest,
    struct qstar_graph *graph, struct qstar_language_provider *provider)
{
	const char *name, *lower;
	int final_idx;

	manifest = qstar_lua_abs_index(L, manifest);
	lua_getfield(L, manifest, "finals");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		name = lua_tostring(L, -2);
		final_idx = lua_gettop(L);
		lua_getfield(L, final_idx, "lower");
		lower = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (qstar_language_provider_add_final_schema(graph, provider, name,
		    lower) < 0) {
			lua_pop(L, 3);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return 0;
}

/** qstar.use_language("id"): provider manifest를 읽고 lang.<namespace>를 활성화한다. */
static int
qstar_lua_use_language(lua_State *L)
{
	struct qstar_lua_context *ctx;
	struct qstar_graph *graph;
	struct qstar_language_provider *provider;
	const char *raw;
	char rel_dir[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX], full[QSTAR_PATH_MAX];
	char impl_name[QSTAR_PATH_MAX], impl_rel[QSTAR_PATH_MAX], impl_full[QSTAR_PATH_MAX];
	char chain[512];
	char api[64], id[128], namespace[128], version[128];
	int manifest_idx, implementation_idx;
	FILE *f;

	ctx = get_context(L);
	graph = ctx->graph;
	if (ctx->module_depth > 0 && ctx->provider_stack.len == 0)
		return luaL_error(L,
		    "qstar: qstar.use_language is forbidden inside ordinary .qsm module; activate providers from qstar.lua or from another provider manifest");
	raw = luaL_checkstring(L, 1);
	if (resolve_language_provider_path(ctx, raw, rel_dir, sizeof(rel_dir), rel,
	    sizeof(rel), full, sizeof(full)) < 0)
		return luaL_error(L, "%s", graph->error);
	if (string_list_contains(&ctx->import_stack, rel) ||
	    string_list_contains(&ctx->provider_stack, rel)) {
		format_import_chain(ctx, rel, chain, sizeof(chain));
		return luaL_error(L,
		    "qstar: circular language provider activation: %s", chain);
	}
	if (qstar_graph_find_language_provider_manifest(graph, rel)) {
		lua_getfield(L, LUA_REGISTRYINDEX, "qstar.provider.exports");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, rel);
			if (lua_istable(L, -1)) {
				lua_remove(L, -2);
				return 1;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		return luaL_error(L,
		    "qstar: duplicate language provider '%s' has no cached exports",
		    rel);
	}
	f = fopen(full, "r");
	if (!f)
		return luaL_error(L,
		    "qstar: language provider '%s' not found; expected provider manifest '%s'",
		    raw, rel);
	fclose(f);
	if (qstar_string_list_push(&ctx->provider_stack, rel) < 0)
		return luaL_error(L, "qstar: out of memory");
	if (eval_module(L, ctx, full, rel_dir, rel) < 0) {
		string_list_pop(&ctx->provider_stack);
		return lua_error(L);
	}
	string_list_pop(&ctx->provider_stack);
	manifest_idx = lua_gettop(L);
	if (read_language_provider_manifest(L, manifest_idx, graph, api, sizeof(api),
	    id, sizeof(id), namespace, sizeof(namespace), version, sizeof(version),
	    impl_name, sizeof(impl_name)) < 0) {
		lua_pop(L, 1);
		return luaL_error(L, "%s", graph->error);
	}
	if (qstar_path_join(rel_dir, impl_name, impl_rel, sizeof(impl_rel)) < 0 ||
	    path_join_root_or_absolute(ctx->root_dir, impl_rel, impl_full,
	    sizeof(impl_full)) < 0) {
		lua_pop(L, 1);
		return luaL_error(L,
		    "qstar: language provider '%s' implementation path is too long",
		    id);
	}
	f = fopen(impl_full, "r");
	if (!f) {
		lua_pop(L, 1);
		return luaL_error(L,
		    "qstar: language provider '%s' implementation not found; expected provider implementation '%s'",
		    id, impl_rel);
	}
	fclose(f);
	if (eval_provider_implementation(L, ctx, impl_full, rel_dir, impl_rel) < 0) {
		lua_remove(L, manifest_idx);
		return lua_error(L);
	}
	implementation_idx = lua_gettop(L);
	if (build_language_provider_exports(L, manifest_idx, implementation_idx,
	    graph, id) < 0) {
		lua_pop(L, 2);
		return luaL_error(L, "%s", graph->error);
	}
	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.provider.implementations");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, "qstar.provider.implementations");
	}
	lua_pushvalue(L, implementation_idx);
	lua_setfield(L, -2, namespace);
	lua_pop(L, 1);
	provider = qstar_graph_add_language_provider(graph, api, id, namespace, version,
	    rel_dir, rel, impl_rel);
	if (!provider) {
		lua_pop(L, 3);
		return luaL_error(L, "%s", graph->error);
	}
	if (add_language_provider_option_schemas(L, manifest_idx, graph, provider) < 0) {
		lua_pop(L, 3);
		return luaL_error(L, "%s", graph->error);
	}
	if (add_language_provider_unit_schemas(L, manifest_idx, graph, provider) < 0) {
		lua_pop(L, 3);
		return luaL_error(L, "%s", graph->error);
	}
	if (add_language_provider_final_schemas(L, manifest_idx, graph, provider) < 0) {
		lua_pop(L, 3);
		return luaL_error(L, "%s", graph->error);
	}
	lua_getfield(L, LUA_REGISTRYINDEX, "qstar.provider.exports");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, "qstar.provider.exports");
	}
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, rel);
	lua_pop(L, 1);
	lua_remove(L, implementation_idx);
	lua_remove(L, manifest_idx);
	return 1;
}

static int
qstar_lua_subdir(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *dir, *base;
	char path[QSTAR_PATH_MAX], candidate[QSTAR_PATH_MAX];
	char full_dir[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	char chain[512];
	int origin_line;
	FILE *f;

	ctx = get_context(L);
	if (reject_graph_declaration_in_module(L, "qstar.subdir") != 0)
		return 1;
	dir = luaL_checkstring(L, 1);
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	if (!qstar_path_is_package_relative(dir))
		return luaL_error(L, "qstar: subdir path '%s' must be package-relative", dir);
	if (ctx->current_dir[0]) {
		if (qstar_path_join(ctx->current_dir, dir, full_dir, sizeof(full_dir)) < 0)
			return luaL_error(L, "qstar: subdir path too long");
	} else {
		snprintf(full_dir, sizeof(full_dir), "%s", dir);
	}
	base = strrchr(full_dir, '/');
	base = base ? base + 1 : full_dir;
	snprintf(path, sizeof(path), "%s/%s.qst", full_dir, base);
	if (qstar_path_join(ctx->root_dir, path, candidate, sizeof(candidate)) < 0)
		return luaL_error(L, "qstar: subdir path too long");
	f = fopen(candidate, "r");
	if (f) {
		fclose(f);
		if (string_list_contains(&ctx->import_stack, path)) {
			format_import_chain(ctx, path, chain, sizeof(chain));
			return luaL_error(L, "qstar: circular import chain: %s", chain);
		}
		if (string_list_contains(&ctx->graph->evaluated_fragments, path))
			return luaL_error(L, "qstar: duplicate import '%s'", path);
		if (eval_fragment(L, ctx, candidate, full_dir) < 0)
			return lua_error(L);
		return 0;
	}
	if (qstar_graph_add_lint(ctx->graph, "QSTAR002", "error", origin_file,
	    origin_line, "subdir", "<none>",
	    "missing fragment: subdir '%s' requires '%s'", full_dir, path) < 0)
		return luaL_error(L, "qstar: out of memory");
	return luaL_error(L, "qstar: subdir '%s' has no qstar fragment '%s'",
	    full_dir, path);
}

static int
forbidden(lua_State *L)
{
	const char *name;

	name = lua_tostring(L, lua_upvalueindex(1));
	return luaL_error(L, "qstar: forbidden Lua API '%s'", name ? name : "unknown");
}

static int
global_assignment_forbidden(lua_State *L)
{
	const char *name;

	name = lua_isstring(L, 2) ? lua_tostring(L, 2) : "<non-string>";
	return luaL_error(L, "qstar: global assignment is not allowed: %s", name);
}

static int
readonly_table_assignment_forbidden(lua_State *L)
{
	const char *table, *key;

	table = lua_tostring(L, lua_upvalueindex(1));
	key = lua_isstring(L, 2) ? lua_tostring(L, 2) : "<non-string>";
	return luaL_error(L, "qstar: %s is read-only: %s", table, key);
}

static void
set_forbidden_function(lua_State *L, const char *name)
{
	lua_pushstring(L, name);
	lua_pushcclosure(L, forbidden, 1);
	lua_setglobal(L, name);
}

static void
set_forbidden_table_function(lua_State *L, const char *table, const char *field)
{
	char name[128];

	lua_getglobal(L, table);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, table);
	}
	snprintf(name, sizeof(name), "%s.%s", table, field);
	lua_pushstring(L, name);
	lua_pushcclosure(L, forbidden, 1);
	lua_setfield(L, -2, field);
	lua_pop(L, 1);
}

static void
open_sandbox(lua_State *L)
{
	luaL_requiref(L, "_G", luaopen_base, 1);
	lua_pop(L, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	lua_pop(L, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	lua_pop(L, 1);
	set_forbidden_function(L, "dofile");
	set_forbidden_function(L, "loadfile");
	set_forbidden_function(L, "load");
	set_forbidden_function(L, "require");
	set_forbidden_function(L, "collectgarbage");
	set_forbidden_function(L, "rawget");
	set_forbidden_function(L, "rawset");
	set_forbidden_function(L, "setmetatable");
	set_forbidden_table_function(L, "io", "open");
	set_forbidden_table_function(L, "io", "popen");
	set_forbidden_table_function(L, "os", "execute");
	set_forbidden_table_function(L, "os", "time");
	set_forbidden_table_function(L, "os", "clock");
	set_forbidden_table_function(L, "os", "getenv");
	set_forbidden_table_function(L, "debug", "getinfo");
	set_forbidden_table_function(L, "package", "loadlib");
}

static void
push_authoring_env(lua_State *L)
{
	lua_newtable(L);
	lua_newtable(L);
	lua_pushglobaltable(L);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, global_assignment_forbidden);
	lua_setfield(L, -2, "__newindex");
	lua_setmetatable(L, -2);
}

static void
copy_global_to_table(lua_State *L, int table, const char *name)
{
	table = qstar_lua_abs_index(L, table);
	lua_getglobal(L, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_setfield(L, table, name);
}

static void
set_table_cfunction(lua_State *L, int table, const char *name, lua_CFunction fn)
{
	table = qstar_lua_abs_index(L, table);
	lua_pushcfunction(L, fn);
	lua_setfield(L, table, name);
}

static void set_readonly_host_table(lua_State *L);

static void
push_provider_env(lua_State *L)
{
	int env, qstar;
	static const char *const globals[] = {
		"assert", "error", "ipairs", "next", "pairs", "pcall", "select",
		"tonumber", "tostring", "type", "xpcall", "table", "string", NULL
	};
	size_t i;

	lua_newtable(L);
	env = lua_gettop(L);
	for (i = 0; globals[i]; i++)
		copy_global_to_table(L, env, globals[i]);
	lua_pushvalue(L, env);
	lua_setfield(L, env, "_G");
	lua_newtable(L);
	qstar = lua_gettop(L);
	set_table_cfunction(L, qstar, "provider_tools", qstar_lua_provider_tools);
	set_table_cfunction(L, qstar, "language_options", qstar_lua_language_options);
	set_table_cfunction(L, qstar, "source", qstar_lua_source);
	set_table_cfunction(L, qstar, "argv", qstar_lua_argv);
	set_table_cfunction(L, qstar, "join", qstar_lua_join);
	set_table_cfunction(L, qstar, "copy", qstar_lua_copy);
	set_table_cfunction(L, qstar, "append", qstar_lua_append);
	set_table_cfunction(L, qstar, "merge", qstar_lua_merge);
	set_table_cfunction(L, qstar, "extend", qstar_lua_extend);
	set_readonly_host_table(L);
	lua_setfield(L, env, "qstar");
	lua_newtable(L);
	lua_pushcfunction(L, global_assignment_forbidden);
	lua_setfield(L, -2, "__newindex");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, env);
}

static void
set_global_string(lua_State *L, const char *name, const char *value)
{
	lua_pushstring(L, value ? value : "");
	lua_setglobal(L, name);
}

static void
set_global_integer(lua_State *L, const char *name, lua_Integer value)
{
	lua_pushinteger(L, value);
	lua_setglobal(L, name);
}

static void
set_table_string(lua_State *L, int table, const char *name, const char *value)
{
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_pushstring(L, value ? value : "");
	lua_setfield(L, table, name);
}

static void
register_global_constants(lua_State *L, const struct qstar_lua_context *ctx)
{
	const struct qstar_graph *graph;
	const char *target;

	graph = ctx->graph;
	target = graph->build_context.target && *graph->build_context.target ? graph->build_context.target : "host";
	set_global_string(L, "QSTAR_VERSION", QSTAR_VERSION);
	set_global_integer(L, "QSTAR_VERSION_MAJOR", QSTAR_VERSION_MAJOR);
	set_global_integer(L, "QSTAR_VERSION_MINOR", QSTAR_VERSION_MINOR);
	set_global_integer(L, "QSTAR_VERSION_PATCH", QSTAR_VERSION_PATCH);
	set_global_string(L, "QSTAR_HOST_OS", qstar_host_os());
	set_global_string(L, "QSTAR_HOST_ARCH", qstar_host_arch());
	set_global_string(L, "QSTAR_PACKAGE_ROOT", ctx->root_dir);
	set_global_string(L, "QSTAR_PROJECT_ROOT", ctx->root_dir);
	set_global_string(L, "QSTAR_TARGET", target);
}

static void
set_readonly_host_table(lua_State *L)
{
	lua_newtable(L);
	lua_newtable(L);
	set_table_string(L, -1, "os", qstar_host_os());
	set_table_string(L, -1, "arch", qstar_host_arch());
	lua_newtable(L);
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, "__index");
	lua_pushstring(L, "qstar.host");
	lua_pushcclosure(L, readonly_table_assignment_forbidden, 1);
	lua_setfield(L, -2, "__newindex");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -3);
	lua_pop(L, 1);
	lua_setfield(L, -2, "host");
}

static void
register_qstar(lua_State *L, struct qstar_lua_context *ctx)
{
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "qstar.context");
	lua_newtable(L);
	lua_pushstring(L, QSTAR_VERSION);
	lua_setfield(L, -2, "version");
	set_readonly_host_table(L);
	lua_newtable(L);
	set_table_string(L, -1, "root", ctx->root_dir);
	lua_newtable(L);
	lua_pushcfunction(L, qstar_lua_project);
	lua_setfield(L, -2, "__call");
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "project");
	lua_pushcfunction(L, qstar_lua_toolset);
	lua_setfield(L, -2, "toolset");
	lua_pushcfunction(L, qstar_lua_config);
	lua_setfield(L, -2, "config");
	lua_pushstring(L, "target");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "target");
	lua_pushstring(L, "exe");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "executable");
	lua_pushstring(L, "run_target");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "run_target");
	lua_pushstring(L, "group");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "group");
	lua_pushstring(L, "staticlib");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "staticlib");
	lua_pushstring(L, "sharedlib");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "sharedlib");
	lua_pushstring(L, "test");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "test");
	lua_pushcfunction(L, qstar_lua_genrule);
	lua_setfield(L, -2, "custom_target");
	lua_pushcfunction(L, qstar_lua_transform);
	lua_setfield(L, -2, "transform");
	lua_pushcfunction(L, qstar_lua_config_header);
	lua_setfield(L, -2, "configure_file");
	lua_pushcfunction(L, qstar_lua_stage);
	lua_setfield(L, -2, "stage");
	lua_pushcfunction(L, qstar_lua_target_family);
	lua_setfield(L, -2, "target_family");
	lua_pushcfunction(L, qstar_lua_output);
	lua_setfield(L, -2, "output");
	lua_pushcfunction(L, qstar_lua_input);
	lua_setfield(L, -2, "input");
	lua_pushcfunction(L, qstar_lua_source);
	lua_setfield(L, -2, "source");
	lua_pushcfunction(L, qstar_lua_target_file);
	lua_setfield(L, -2, "target_file");
	lua_pushcfunction(L, qstar_lua_stage_dir);
	lua_setfield(L, -2, "stage_dir");
	lua_pushcfunction(L, qstar_lua_stage_file);
	lua_setfield(L, -2, "stage_file");
	lua_pushcfunction(L, qstar_lua_cli);
	lua_setfield(L, -2, "cli");
	lua_pushcfunction(L, qstar_lua_status);
	lua_setfield(L, -2, "status");
	lua_pushcfunction(L, qstar_lua_command);
	lua_setfield(L, -2, "command");
	lua_newtable(L);
	lua_pushstring(L, "build");
	lua_pushcclosure(L, qstar_lua_step_label, 1);
	lua_setfield(L, -2, "build");
	lua_pushstring(L, "test");
	lua_pushcclosure(L, qstar_lua_step_label, 1);
	lua_setfield(L, -2, "test");
	lua_pushcfunction(L, qstar_lua_step_stage);
	lua_setfield(L, -2, "stage");
	lua_pushcfunction(L, qstar_lua_step_export_stage);
	lua_setfield(L, -2, "export_stage");
	lua_pushstring(L, "check");
	lua_pushcclosure(L, qstar_lua_step_label, 1);
	lua_setfield(L, -2, "check");
	lua_pushstring(L, "lint");
	lua_pushcclosure(L, qstar_lua_step_label, 1);
	lua_setfield(L, -2, "lint");
	lua_pushcfunction(L, qstar_lua_step_call);
	lua_setfield(L, -2, "call");
	lua_pushcfunction(L, qstar_lua_step_run);
	lua_setfield(L, -2, "run");
	lua_setfield(L, -2, "step");
	lua_pushcfunction(L, qstar_lua_arg_if);
	lua_setfield(L, -2, "arg_if");
	lua_pushcfunction(L, qstar_lua_args_if);
	lua_setfield(L, -2, "args_if");
	lua_newtable(L);
	lua_pushstring(L, "string");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "string");
	lua_pushstring(L, "path");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, "bool");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "bool");
	lua_pushstring(L, "int");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "int");
	lua_pushstring(L, "enum");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "enum");
	lua_pushstring(L, "list");
	lua_pushcclosure(L, qstar_lua_param_type, 1);
	lua_setfield(L, -2, "list");
	lua_newtable(L);
	lua_pushcfunction(L, qstar_lua_param_ref);
	lua_setfield(L, -2, "__call");
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "param");
	lua_pushcfunction(L, qstar_lua_files);
	lua_setfield(L, -2, "files");
	lua_pushcfunction(L, qstar_lua_join);
	lua_setfield(L, -2, "join");
	lua_pushcfunction(L, qstar_lua_copy);
	lua_setfield(L, -2, "copy");
	lua_pushcfunction(L, qstar_lua_append);
	lua_setfield(L, -2, "append");
	lua_pushcfunction(L, qstar_lua_merge);
	lua_setfield(L, -2, "merge");
	lua_pushcfunction(L, qstar_lua_extend);
	lua_setfield(L, -2, "extend");
	lua_pushcfunction(L, qstar_lua_import_file);
	lua_setfield(L, -2, "import_file");
	lua_pushcfunction(L, qstar_lua_import_module);
	lua_setfield(L, -2, "import_module");
	lua_pushcfunction(L, qstar_lua_use_language);
	lua_setfield(L, -2, "use_language");
	lua_pushcfunction(L, qstar_lua_language_provider);
	lua_setfield(L, -2, "language_provider");
	lua_pushcfunction(L, qstar_lua_subdir);
	lua_setfield(L, -2, "subdir");
	lua_setglobal(L, "qstar");
}

static int
eval_fragment(lua_State *L, struct qstar_lua_context *ctx, const char *file, const char *fragment_dir)
{
	char old[QSTAR_PATH_MAX], rel[QSTAR_PATH_MAX];
	int rc;

	snprintf(old, sizeof(old), "%s", ctx->current_dir);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", fragment_dir ? fragment_dir : "");
	if (package_relative_input(ctx, file, rel, sizeof(rel)) < 0) {
		lua_pushstring(L, "qstar: authoring input path is too long");
		return -1;
	}
	if (enter_authoring_input(ctx, rel) < 0) {
		lua_pushstring(L, "qstar: out of memory");
		return -1;
	}
	if (luaL_loadfilex(L, file, "t") != LUA_OK) {
		leave_authoring_input(ctx);
		snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
		return -1;
	}
	push_authoring_env(L);
	if (!lua_setupvalue(L, -2, 1))
		lua_pop(L, 1);
	rc = lua_pcall(L, 0, 0, 0);
	leave_authoring_input(ctx);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
	return rc == LUA_OK ? 0 : -1;
}

static int
eval_module(lua_State *L, struct qstar_lua_context *ctx, const char *file,
    const char *module_dir, const char *rel)
{
	char old[QSTAR_PATH_MAX];
	int rc;

	snprintf(old, sizeof(old), "%s", ctx->current_dir);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", module_dir ? module_dir : "");
	if (enter_authoring_input(ctx, rel) < 0) {
		lua_pushstring(L, "qstar: out of memory");
		return -1;
	}
	if (luaL_loadfilex(L, file, "t") != LUA_OK) {
		leave_authoring_input(ctx);
		snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
		return -1;
	}
	push_authoring_env(L);
	if (!lua_setupvalue(L, -2, 1))
		lua_pop(L, 1);
	ctx->module_depth++;
	rc = lua_pcall(L, 0, 1, 0);
	ctx->module_depth--;
	leave_authoring_input(ctx);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
	if (rc != LUA_OK)
		return -1;
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_pushfstring(L, "qstar: module '%s' must return a table", rel);
		return -1;
	}
	return 0;
}

static int
eval_provider_implementation(lua_State *L, struct qstar_lua_context *ctx,
    const char *file, const char *provider_dir, const char *rel)
{
	char old[QSTAR_PATH_MAX];
	int rc;

	snprintf(old, sizeof(old), "%s", ctx->current_dir);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s",
	    provider_dir ? provider_dir : "");
	if (enter_authoring_input(ctx, rel) < 0) {
		lua_pushstring(L, "qstar: out of memory");
		return -1;
	}
	if (luaL_loadfilex(L, file, "t") != LUA_OK) {
		leave_authoring_input(ctx);
		snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
		return -1;
	}
	push_provider_env(L);
	if (!lua_setupvalue(L, -2, 1))
		lua_pop(L, 1);
	rc = lua_pcall(L, 0, 1, 0);
	leave_authoring_input(ctx);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
	if (rc != LUA_OK)
		return -1;
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_pushfstring(L, "qstar: provider implementation '%s' must return a table",
		    rel);
		return -1;
	}
	return 0;
}

/** 일반 파일이 존재하는지 확인한다. */
static int
regular_file_exists(const char *path)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

/** child directory를 root 기준 package fragment path로 변환한다. */
static int
workspace_fragment(const char *root, const char *child, char *dst, size_t dstlen)
{
	size_t n;

	if (strcmp(root, child) == 0 || strcmp(child, ".") == 0) {
		if (dstlen)
			dst[0] = '\0';
		return 0;
	}
	if (strcmp(root, ".") == 0)
		return snprintf(dst, dstlen, "%s", child) < (int)dstlen ? 0 : -1;
	n = strlen(root);
	if (strncmp(child, root, n) == 0 && child[n] == '/')
		return snprintf(dst, dstlen, "%s", child + n + 1) < (int)dstlen ? 0 : -1;
	if (strcmp(root, "/") == 0 && child[0] == '/')
		return snprintf(dst, dstlen, "%s", child + 1) < (int)dstlen ? 0 : -1;
	return -1;
}

/** qstar authoring path가 제거된 .qs suffix를 쓰는지 검사한다. */
static int
removed_qs_suffix(const char *path)
{
	size_t n;

	n = strlen(path);
	return n >= 3 && strcmp(path + n - 3, ".qs") == 0;
}

/** qstar authoring path에서 위로 올라가며 가장 가까운 qstar.lua root를 찾는다. */
static int
discover_workspace_root(const char *file_dir, char *root, size_t rootlen,
    char *fragment, size_t fragmentlen)
{
	char cur[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];
	char qstar_lua[QSTAR_PATH_MAX];

	snprintf(cur, sizeof(cur), "%s", file_dir && *file_dir ? file_dir : ".");
	for (;;) {
		if (qstar_path_join(cur, "qstar.lua", qstar_lua, sizeof(qstar_lua)) == 0 &&
		    regular_file_exists(qstar_lua)) {
			if (snprintf(root, rootlen, "%s", cur) >= (int)rootlen ||
			    workspace_fragment(cur, file_dir, fragment, fragmentlen) < 0)
				return -1;
			return 0;
		}
		if (strcmp(cur, ".") == 0 || strcmp(cur, "/") == 0)
			break;
		if (qstar_dirname(cur, parent, sizeof(parent)) < 0 ||
		    strcmp(parent, cur) == 0)
			break;
		snprintf(cur, sizeof(cur), "%s", parent);
	}
	(void)root;
	(void)rootlen;
	(void)fragment;
	(void)fragmentlen;
	return -1;
}

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int
qstar_lua_eval_file(struct qstar_graph *graph, const char *file)
{
	struct qstar_lua_context ctx;
	lua_State *L;
	char file_dir[QSTAR_PATH_MAX], initial_fragment[QSTAR_PATH_MAX];
	const char *base;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.graph = graph;
	if (removed_qs_suffix(file))
		return qstar_set_error(graph, "qstar: .qs fragments were removed; use .qst");
	if (qstar_dirname(file, file_dir, sizeof(file_dir)) < 0 ||
	    discover_workspace_root(file_dir, ctx.root_dir, sizeof(ctx.root_dir),
	    initial_fragment, sizeof(initial_fragment)) < 0)
		return qstar_set_error(graph,
		    "qstar: could not find qstar.lua package root for '%s'", file);
	if (qstar_graph_set_package_root(graph, ctx.root_dir) < 0)
		return -1;
	base = strrchr(file, '/');
	base = base ? base + 1 : file;
	if (initial_fragment[0] == '\0' && strcmp(base, "qstar.lua") != 0) {
		return qstar_set_error_origin(graph, file, 1, "file", "<none>",
		    "qstar: root entry must be qstar.lua");
	}
	L = luaL_newstate();
	if (!L)
		return qstar_set_error(graph, "qstar: could not create Lua state");
	open_sandbox(L);
	register_global_constants(L, &ctx);
	register_qstar(L, &ctx);
	rc = eval_fragment(L, &ctx, file, initial_fragment);
	if (rc < 0)
		qstar_set_error(graph, "%s", lua_tostring(L, -1));
	lua_close(L);
	qstar_string_list_free(&ctx.import_stack);
	qstar_string_list_free(&ctx.provider_stack);
	return rc;
}
