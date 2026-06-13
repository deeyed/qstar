#include "internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qstar_lua_context {
	struct qstar_graph *graph;
	char root_dir[QSTAR_PATH_MAX];
	char current_dir[QSTAR_PATH_MAX];
	struct qstar_string_list import_stack;
	int module_depth;
};

#define QSTAR_STATUS_DESCRIPTION_MAX 240

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
		    "qstar: %s is forbidden inside .qsm module; modules must return a helper table and cannot declare project/profile/config/target/stage/import_file",
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
		lua_getfield(L, idx, "label");
		label = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
		if (!label) {
			lua_pop(L, 1);
			return -1;
		}
		if (snprintf(dst, dstlen, "<qstar-target-file:%s>", label) >=
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
			if (kind && strcmp(kind, "select") == 0) {
				if (qstar_string_list_push(list, "<select>") < 0) {
					lua_pop(L, 1);
					return qstar_set_error(graph, "qstar: out of memory");
				}
			} else if (kind && strcmp(kind, "output_path") == 0) {
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

static int append_list(struct qstar_graph *graph, struct qstar_string_list *dst,
    const struct qstar_string_list *src);

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
		    strcmp(key, "output_group") != 0 && strcmp(key, "format") != 0 &&
		    strcmp(key, "address") != 0 && strcmp(key, "layout") != 0)) {
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

/** generated output path와 metadata를 genrule parallel lists에 추가한다. */
static int
push_genrule_output(lua_State *L, int idx, struct qstar_genrule *genrule,
    struct qstar_graph *graph)
{
	const char *path, *group, *format, *address, *layout;
	const char *kind;

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	group = "";
	format = "";
	address = "";
	layout = "";
	if (lua_isstring(L, idx)) {
		path = lua_tostring(L, idx);
	} else if (lua_istable(L, idx)) {
		kind = qstar_table_kind(L, idx);
		if (!kind || strcmp(kind, "output_path") != 0)
			return qstar_set_error(graph,
			    "qstar: field 'outputs' contains non-output item");
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
		lua_getfield(L, idx, "address");
		address = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		lua_getfield(L, idx, "layout");
		layout = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
	} else {
		return qstar_set_error(graph, "qstar: field 'outputs' contains non-string item");
	}
	if (!path || !*path)
		return qstar_set_error(graph, "qstar: generated output path is empty");
	if (!valid_output_metadata_token(group) || !valid_output_metadata_token(format) ||
	    !valid_output_metadata_token(address) || !valid_output_metadata_token(layout))
		return qstar_set_error(graph,
		    "qstar: generated output metadata for '%s' contains unsupported characters",
		    path);
	if (qstar_string_list_push(&genrule->outputs, path) < 0 ||
	    qstar_string_list_push(&genrule->output_groups, group) < 0 ||
	    qstar_string_list_push(&genrule->output_formats, format) < 0 ||
	    qstar_string_list_push(&genrule->output_addresses, address) < 0 ||
	    qstar_string_list_push(&genrule->output_layouts, layout) < 0)
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
		rc = push_genrule_output(L, -1, genrule, graph);
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
    struct qstar_graph *graph)
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
			    "qstar: field 'inputs' contains non-string or target_file item");
		path = token;
	} else {
		return qstar_set_error(graph,
		    "qstar: field 'inputs' contains non-string or target_file item");
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
		rc = push_genrule_input(L, -1, genrule, graph);
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
reject_top_level_field(lua_State *L, int table, struct qstar_graph *graph,
    const char *field, const char *message)
{
	if (table < 0)
		table = lua_gettop(L) + table + 1;
	lua_getfield(L, table, field);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "%s", message);
	}
	lua_pop(L, 1);
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
		"frameworks",
		"link_options",
		"defsyms",
		"toolchain",
		"stdlib",
		"artifact_name",
		"linker_script",
		"command",
		"timeout",
		"marker",
		"marker_log",
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
read_modules_field(lua_State *L, int table, const char *field,
    struct qstar_target *target, struct qstar_graph *graph)
{
	const char *root;
	int rc;

	lua_getfield(L, table, field);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field '%s' must be a table", field);
	}
	target->modules.present = 1;
	root = check_string_field(L, -1, "root");
	free(target->modules.root);
	target->modules.root = qstar_strdup(root ? root : "");
	if (!target->modules.root) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: out of memory");
	}
	rc = read_list_field(L, -1, "include", &target->modules.include, graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "exclude", &target->modules.exclude, graph, 0, target->fragment_dir);
	lua_pop(L, 1);
	return rc;
}

static int
read_lang_cale(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"public_headers", "private_headers", "include_dirs", "public_include_dirs",
		"private_include_dirs", "profile", "compile_options", "modules", NULL
	};
	const char *profile;
	int rc;

	lua_getfield(L, lang, "cale");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.cale must be a table");
	}
	rc = validate_lang_fields(L, -1, "cale", allowed, graph);
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
		rc = read_list_field(L, -1, "compile_options", &target->cale_compile_options,
		    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->private_include_dirs);
	if (rc == 0)
		rc = append_lang_include_self(graph, &target->include_dirs,
		    &target->public_include_dirs);
	if (rc == 0)
		rc = read_modules_field(L, -1, "modules", target, graph);
	profile = check_string_field(L, -1, "profile");
	if (profile) {
		free(target->cale_profile);
		target->cale_profile = qstar_strdup(profile);
		if (!target->cale_profile)
			rc = qstar_set_error(graph, "qstar: out of memory");
	}
	lua_pop(L, 1);
	return rc;
}

static int
read_lang_options(lua_State *L, int table, struct qstar_target *target,
    struct qstar_graph *graph)
{
	static const char *const allowed_langs[] = {
		"c", "cxx", "asm", "cale", NULL
	};
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
		if (!key || !string_in_set(key, allowed_langs)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: unknown language namespace lang.%s",
			    key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	rc = read_lang_c(L, -1, target, graph);
	if (rc == 0)
		rc = read_lang_cxx(L, -1, target, graph);
	if (rc == 0)
		rc = read_lang_asm(L, -1, target, graph);
	if (rc == 0)
		rc = read_lang_cale(L, -1, target, graph);
	lua_pop(L, 1);
	return rc;
}

/** qstar.config table에서 config primitive가 받을 수 있는 top-level field만 허용한다. */
static int
validate_config_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"lang", "libs", "lib_dirs", "frameworks", "link_options", "defsyms",
		"toolchain", "stdlib", "artifact_name", "linker_script", NULL
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
	const char *artifact_name, *linker_script, *toolchain, *stdlib_policy;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

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
	if (reject_top_level_field(L, table_index, graph, "include_dirs",
	    "top-level include_dirs is not allowed; move it under lang.c.include_dirs, lang.cxx.include_dirs, lang.asm.include_dirs, or lang.cale.include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "public_include_dirs",
	    "top-level public_include_dirs is not allowed; move it under lang.c.public_include_dirs, lang.cxx.public_include_dirs, or lang.cale.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "private_include_dirs",
	    "top-level private_include_dirs is not allowed; move it under lang.c.private_include_dirs, lang.cxx.private_include_dirs, or lang.cale.private_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "system_include_dirs",
	    "top-level system_include_dirs is not allowed; move it under lang.c.system_include_dirs or lang.cxx.system_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "interface_include_dirs",
	    "top-level interface_include_dirs is not allowed; move it under lang.c.public_include_dirs or lang.cxx.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cflags",
	    "top-level cflags is not allowed; move it under lang.c.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxxflags",
	    "top-level cxxflags is not allowed; move it under lang.cxx.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxx_standard",
	    "top-level cxx_standard is not allowed; move it to lang.cxx.standard") < 0 ||
	    reject_top_level_field(L, table_index, graph, "public_headers",
	    "top-level public_headers is not allowed; move it under lang.c.public_headers, lang.cxx.public_headers, or lang.cale.public_headers") < 0 ||
	    reject_top_level_field(L, table_index, graph, "private_headers",
	    "top-level private_headers is not allowed; move it under lang.c.private_headers, lang.cxx.private_headers, or lang.cale.private_headers") < 0 ||
	    reject_top_level_field(L, table_index, graph, "modules",
	    "top-level modules is not allowed; move it under lang.cale.modules or lang.cxx.modules") < 0 ||
	    reject_top_level_field(L, table_index, graph, "hcl_include_dirs",
	    "hcl_include_dirs is removed; use lang.cale.public_include_dirs or lang.cale.private_include_dirs") < 0)
		return luaL_error(L, "%s", graph->error);
	if (validate_config_fields(L, table_index, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "libs", &config->options.libs, graph, 0,
	    config->options.fragment_dir) < 0 ||
	    read_list_field(L, table_index, "lib_dirs", &config->options.lib_dirs, graph,
	    0, config->options.fragment_dir) < 0 ||
	    read_list_field(L, table_index, "frameworks", &config->options.frameworks,
	    graph, 0, config->options.fragment_dir) < 0 ||
	    read_list_field(L, table_index, "link_options", &config->options.link_options,
	    graph, 0, config->options.fragment_dir) < 0 ||
	    read_list_field(L, table_index, "defsyms", &config->options.defsyms, graph,
	    0, config->options.fragment_dir) < 0 ||
	    read_lang_options(L, table_index, &config->options, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	config->has_cxx_standard = nested_lang_field_present(L, table_index, "cxx",
	    "standard");
	config->has_asm_preprocess = nested_lang_field_present(L, table_index, "asm",
	    "preprocess");
	config->has_cxx_modules = nested_lang_field_present(L, table_index, "cxx",
	    "modules");
	config->has_cale_profile = nested_lang_field_present(L, table_index, "cale",
	    "profile");
	artifact_name = check_string_field(L, table_index, "artifact_name");
	linker_script = check_string_field(L, table_index, "linker_script");
	toolchain = check_string_field(L, table_index, "toolchain");
	stdlib_policy = check_string_field(L, table_index, "stdlib");
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
	if (linker_script) {
		config->has_linker_script = 1;
		if (replace_lua_string(&config->options.linker_script, linker_script,
		    graph) < 0)
			return luaL_error(L, "%s", graph->error);
	}
	if (toolchain) {
		config->has_toolchain = 1;
		if (replace_lua_string(&config->options.toolchain, toolchain, graph) < 0)
			return luaL_error(L, "%s", graph->error);
	}
	if (stdlib_policy) {
		config->has_stdlib_policy = 1;
		if (replace_lua_string(&config->options.stdlib_policy, stdlib_policy,
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
add_target(lua_State *L, const char *name, int table_index, const char *default_kind,
    const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_target *target;
	struct qstar_graph *graph;
	const char *kind, *toolchain, *stdlib_policy, *artifact_name;
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
	if (reject_top_level_field(L, table_index, graph, "include_dirs",
	    "top-level include_dirs is not allowed; move it under lang.c.include_dirs, lang.cxx.include_dirs, lang.asm.include_dirs, or lang.cale.include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "public_include_dirs",
	    "top-level public_include_dirs is not allowed; move it under lang.c.public_include_dirs, lang.cxx.public_include_dirs, or lang.cale.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "private_include_dirs",
	    "top-level private_include_dirs is not allowed; move it under lang.c.private_include_dirs, lang.cxx.private_include_dirs, or lang.cale.private_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "system_include_dirs",
	    "top-level system_include_dirs is not allowed; move it under lang.c.system_include_dirs or lang.cxx.system_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "interface_include_dirs",
	    "top-level interface_include_dirs is not allowed; move it under lang.c.public_include_dirs or lang.cxx.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cflags",
	    "top-level cflags is not allowed; move it under lang.c.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxxflags",
	    "top-level cxxflags is not allowed; move it under lang.cxx.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxx_standard",
	    "top-level cxx_standard is not allowed; move it to lang.cxx.standard") < 0 ||
	    reject_top_level_field(L, table_index, graph, "public_headers",
	    "top-level public_headers is not allowed; move it under lang.c.public_headers, lang.cxx.public_headers, or lang.cale.public_headers") < 0 ||
	    reject_top_level_field(L, table_index, graph, "private_headers",
	    "top-level private_headers is not allowed; move it under lang.c.private_headers, lang.cxx.private_headers, or lang.cale.private_headers") < 0 ||
	    reject_top_level_field(L, table_index, graph, "modules",
	    "top-level modules is not allowed; move it under lang.cale.modules or lang.cxx.modules") < 0 ||
	    reject_top_level_field(L, table_index, graph, "hcl_include_dirs",
	    "hcl_include_dirs is removed; use lang.cale.public_include_dirs or lang.cale.private_include_dirs") < 0)
		return luaL_error(L, "%s", graph->error);
	if (strcmp(target->kind, "group") == 0 &&
	    reject_group_action_fields(L, table_index, graph, target->label) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "configs", &target->configs, graph, 1,
	    target->fragment_dir) < 0 ||
	    qstar_graph_apply_target_configs(graph, target) < 0 ||
	    read_list_field(L, table_index, "sources", &target->sources, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "public_deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "private_deps", &target->private_deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "visibility", &target->visibility, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "libs", &target->libs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "lib_dirs", &target->lib_dirs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "frameworks", &target->frameworks, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "link_options", &target->link_options, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "defsyms", &target->defsyms, graph, 0, target->fragment_dir) < 0 ||
	    read_lang_options(L, table_index, target, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	toolchain = check_string_field(L, table_index, "toolchain");
	stdlib_policy = check_string_field(L, table_index, "stdlib");
	artifact_name = check_string_field(L, table_index, "artifact_name");
	if (artifact_name) {
		if (!valid_target_artifact_name(artifact_name))
			return luaL_error(L,
			    "qstar: artifact_name '%s' must be a filename, not a path",
			    artifact_name);
		free(target->artifact_name);
		target->artifact_name = qstar_strdup(artifact_name);
	}
	if (check_string_field(L, table_index, "linker_script")) {
		free(target->linker_script);
		target->linker_script = qstar_strdup(check_string_field(L, table_index,
		    "linker_script"));
	}
	if (toolchain) {
		free(target->toolchain);
		target->toolchain = qstar_strdup(toolchain);
	}
	if (stdlib_policy) {
		free(target->stdlib_policy);
		target->stdlib_policy = qstar_strdup(stdlib_policy);
	}
	if (strcmp(target->kind, "run_target") == 0) {
		struct qstar_string_list empty;

		memset(&empty, 0, sizeof(empty));
		if (read_status_description_field(L, table_index, graph,
		    &target->description) < 0 ||
		    read_command_field(L, table_index, "command", &target->run_command,
		    graph) < 0 ||
		    resolve_cli_placeholders(graph, &target->run_command, &empty, &empty,
		    "run_target command") < 0)
			return luaL_error(L, "%s", graph->error);
		if (target->run_command.len == 0)
			return luaL_error(L,
			    "qstar: run_target '%s' requires command = qstar.cli { ... }",
			    target->label);
		target->run_timeout_sec = check_int_field(L, table_index, "timeout", 0);
		if (target->run_timeout_sec < 0)
			return luaL_error(L, "qstar: run_target '%s' timeout must be >= 0",
			    target->label);
		free(target->run_marker);
		target->run_marker = qstar_strdup(check_string_field(L, table_index, "marker"));
		free(target->run_marker_log);
		target->run_marker_log = qstar_strdup(check_string_field(L, table_index,
		    "marker_log"));
		if (target->run_marker_log && *target->run_marker_log &&
		    !qstar_path_is_package_relative(target->run_marker_log))
			return luaL_error(L,
			    "qstar: run_target '%s' marker_log must be package-relative",
			    target->label);
		if (!target->run_marker || !target->run_marker_log)
			return luaL_error(L, "qstar: out of memory");
	}
	if (!target->toolchain || !target->stdlib_policy || !target->artifact_name ||
	    !target->cxx_standard || !target->linker_script || !target->run_marker ||
	    !target->run_marker_log)
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
qstar_lua_removed_api(lua_State *L)
{
	const char *message;

	message = lua_tostring(L, lua_upvalueindex(1));
	return luaL_error(L, "%s", message ? message : "qstar: removed API");
}

static int
add_genrule(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_genrule *genrule;
	struct qstar_graph *graph;
	const char *tool;
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
	tool = check_string_field(L, table_index, "tool");
	if (tool) {
		free(genrule->tool);
		genrule->tool = qstar_strdup(tool);
	}
	if (read_status_description_field(L, table_index, graph,
	    &genrule->description) < 0 ||
	    read_genrule_inputs_field(L, table_index, genrule, graph) < 0 ||
	    read_outputs_field(L, table_index, genrule, graph) < 0 ||
	    read_command_field(L, table_index, "command", &genrule->command, graph) < 0 ||
	    resolve_cli_placeholders(graph, &genrule->command, &genrule->inputs,
	    &genrule->outputs, "custom_target command") < 0)
		return luaL_error(L, "%s", graph->error);
	if (genrule->command.len == 0)
		return luaL_error(L,
		    "qstar: custom_target '%s' requires command = qstar.cli { ... }",
		    genrule->label);
	free(genrule->tool);
	genrule->tool = qstar_strdup(genrule->command.items[0]);
	if (!genrule->tool)
		return luaL_error(L, "qstar: out of memory");
	for (size_t i = 1; i < genrule->command.len; i++) {
		if (qstar_string_list_push(&genrule->args, genrule->command.items[i]) < 0)
			return luaL_error(L, "qstar: out of memory");
	}
	if (!genrule->tool)
		return luaL_error(L, "qstar: out of memory");
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

static int
qstar_lua_identity_table(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushvalue(L, 1);
	return 1;
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

/** qstar.join { ... }의 legacy list flatten 형태를 유지한다. */
static int
qstar_lua_join_list(lua_State *L)
{
	size_t n, i, m;

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	n = lua_rawlen(L, 1);
	m = 1;
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i);
		if (qstar_lua_array_table(L, -1))
			qstar_lua_append_list_copy(L, -2, -1, &m, 0);
		else
			qstar_lua_append_value_copy(L, -2, -1, &m, 0);
		lua_pop(L, 1);
	}
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
		return qstar_lua_join_list(L);
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
	const char *path, *group, *format, *address, *layout;

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
	address = check_string_field(L, 2, "address");
	layout = check_string_field(L, 2, "layout");
	if ((group && !valid_output_metadata_token(group)) ||
	    (format && !valid_output_metadata_token(format)) ||
	    (address && !valid_output_metadata_token(address)) ||
	    (layout && !valid_output_metadata_token(layout)))
		return luaL_error(L,
		    "qstar: generated output metadata for '%s' contains unsupported characters",
		    path);
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
	if (address) {
		lua_pushstring(L, address);
		lua_setfield(L, -2, "address");
	}
	if (layout) {
		lua_pushstring(L, layout);
		lua_setfield(L, -2, "layout");
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
	const char *raw;
	char label[QSTAR_PATH_MAX];

	ctx = get_context(L);
	raw = luaL_checkstring(L, 1);
	if (qstar_label_canonicalize(raw, ctx->current_dir, label, sizeof(label)) < 0)
		return luaL_error(L, "qstar: invalid target_file label '%s'", raw);
	lua_newtable(L);
	lua_pushstring(L, "target_file");
	lua_setfield(L, -2, "__qstar_kind");
	lua_pushstring(L, label);
	lua_setfield(L, -2, "label");
	return 1;
}

static int
qstar_lua_cli(lua_State *L)
{
	size_t i, n;
	char token[QSTAR_PATH_MAX];

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	lua_pushstring(L, "cli");
	lua_setfield(L, -2, "__qstar_kind");
	n = lua_rawlen(L, 1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i);
		if (lua_isstring(L, -1)) {
			lua_pushvalue(L, -1);
		} else if (lua_istable(L, -1) &&
		    format_placeholder_token(L, -1, token, sizeof(token)) == 0) {
			lua_pushstring(L, token);
		} else {
			lua_pop(L, 2);
			return luaL_error(L, "qstar: qstar.cli contains unsupported argv item");
		}
		lua_rawseti(L, -3, (lua_Integer)i);
		lua_pop(L, 1);
	}
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
qstar_lua_select(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *arch, *target, *key, *selected;
	int matched, has_default;

	ctx = get_context(L);
	target = ctx->graph->profile.target && *ctx->graph->profile.target ?
	    ctx->graph->profile.target : "host";
	arch = ctx->graph->profile.arch && *ctx->graph->profile.arch ?
	    ctx->graph->profile.arch : target;
	luaL_checktype(L, 1, LUA_TTABLE);
	matched = 0;
	selected = NULL;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (key && ((strcmp(key, "os=macos") == 0 &&
		    (strstr(target, "apple") || strstr(target, "macos") || strstr(target, "darwin"))) ||
		    (strcmp(key, "os=linux") == 0 && strstr(target, "linux")) ||
		    (strcmp(key, "os=windows") == 0 && (strstr(target, "windows") || strstr(target, "win"))) ||
		    (strcmp(key, "arch=x86_64") == 0 && (strstr(arch, "x86_64") || strstr(arch, "amd64"))) ||
		    (strcmp(key, "arch=aarch64") == 0 && (strstr(arch, "aarch64") || strstr(arch, "arm64"))) ||
		    (strcmp(key, "arch=riscv64") == 0 && (strstr(arch, "riscv64") || strstr(arch, "rv64"))))) {
			matched = 1;
			lua_remove(L, -2);
			break;
		}
		lua_pop(L, 1);
	}
	if (!matched) {
		lua_getfield(L, 1, "default");
		has_default = !lua_isnil(L, -1);
		if (!has_default)
			return luaL_error(L, "qstar: select has no branch for target '%s'", target);
	}
	if (lua_isstring(L, -1)) {
		selected = lua_tostring(L, -1);
		if (strncmp(selected, "incompatible(", 13) == 0)
			return luaL_error(L, "qstar: selected incompatible branch %s", selected);
	}
	return 1;
}

static int
qstar_lua_incompatible(lua_State *L)
{
	const char *reason;
	char buf[256];

	reason = luaL_checkstring(L, 1);
	snprintf(buf, sizeof(buf), "incompatible(\"%s\")", reason);
	lua_pushstring(L, buf);
	return 1;
}

/** Lua stack에서 임시로 만든 profile input을 해제한다. */
static void
free_lua_profile_input(struct qstar_profile_input *profile)
{
	free(profile->name);
	free(profile->target);
	free(profile->toolchain);
	free(profile->stdlib_policy);
	free(profile->freestanding);
	free(profile->arch);
	free(profile->cpu);
	free(profile->abi);
	free(profile->cc);
	free(profile->cxx);
	free(profile->cale);
	free(profile->ar);
	free(profile->linker);
	free(profile->sysroot);
	free(profile->resource_dir);
	free(profile->response_files);
	free(profile->response_style);
	free(profile->linker_script);
	free(profile->allow_absolute_tools);
	qstar_string_list_free(&profile->artifact_names);
	qstar_string_list_free(&profile->compile_options);
	qstar_string_list_free(&profile->include_dirs);
	qstar_string_list_free(&profile->lib_dirs);
	qstar_string_list_free(&profile->link_options);
	qstar_string_list_free(&profile->defsyms);
	qstar_string_list_free(&profile->path_tools);
	qstar_string_list_free(&profile->tool_overrides);
	memset(profile, 0, sizeof(*profile));
}

/** qstar.profile table의 scalar/string-or-bool field 하나를 읽는다. */
static int
read_profile_scalar_field(lua_State *L, int table, const char *field, char **slot,
    struct qstar_graph *graph)
{
	const char *value;
	int type;

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
		return qstar_set_error(graph, "qstar: profile field '%s' must be a string",
		    field);
	}
	free(*slot);
	*slot = qstar_strdup(value);
	lua_pop(L, 1);
	return *slot ? 0 : qstar_set_error(graph, "qstar: out of memory");
}

/** 여러 alias field 중 처음 등장한 scalar 값을 profile slot에 저장한다. */
static int
read_profile_first_scalar(lua_State *L, int table, char **slot, struct qstar_graph *graph,
    const char *a, const char *b, const char *c)
{
	if (read_profile_scalar_field(L, table, a, slot, graph) < 0)
		return -1;
	if (*slot || !b)
		return 0;
	if (read_profile_scalar_field(L, table, b, slot, graph) < 0)
		return -1;
	if (*slot || !c)
		return 0;
	return read_profile_scalar_field(L, table, c, slot, graph);
}

/** qstar.profile table을 내부 profile input 구조로 변환한다. */
static int
read_profile_table(lua_State *L, int table, struct qstar_profile_input *input,
    struct qstar_graph *graph)
{
	if (read_profile_first_scalar(L, table, &input->target, graph, "target", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->toolchain, graph, "toolchain",
	    NULL, NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->stdlib_policy, graph, "stdlib",
	    "stdlib_policy", NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->freestanding, graph,
	    "freestanding", NULL, NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->arch, graph, "arch", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->cpu, graph, "cpu", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->abi, graph, "abi", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->cc, graph, "cc", "compiler",
	    "c_compiler") < 0 ||
	    read_profile_first_scalar(L, table, &input->cxx, graph, "cxx",
	    "cxx_compiler", NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->cale, graph, "cale",
	    "cale_compiler", NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->ar, graph, "ar", "archiver",
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->linker, graph, "linker", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->sysroot, graph, "sysroot", NULL,
	    NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->resource_dir, graph,
	    "resource_dir", NULL, NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->response_files, graph,
	    "response_files", "rsp", NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->response_style, graph,
	    "response_style", "rsp_style", NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->linker_script, graph,
	    "linker_script", NULL, NULL) < 0 ||
	    read_profile_first_scalar(L, table, &input->allow_absolute_tools, graph,
	    "allow_absolute_tools", "external_absolute_tools", NULL) < 0)
		return -1;
	if (read_list_field(L, table, "compile_options", &input->compile_options, graph,
	    0, "") < 0 ||
	    read_list_field(L, table, "include_dirs", &input->include_dirs, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table, "lib_dirs", &input->lib_dirs, graph, 0, "") < 0 ||
	    read_list_field(L, table, "link_options", &input->link_options, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table, "defsyms", &input->defsyms, graph, 0, "") < 0 ||
	    read_list_field(L, table, "artifact_names", &input->artifact_names, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table, "path_tools", &input->path_tools, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table, "external_tools", &input->path_tools, graph, 0,
	    "") < 0 ||
	    read_list_field(L, table, "tool_overrides", &input->tool_overrides, graph, 0,
	    "") < 0)
		return -1;
	return 0;
}

/** qstar.profile v1에서 허용되지 않는 field를 stable diagnostic으로 막는다. */
static int
validate_profile_fields(lua_State *L, int table, struct qstar_graph *graph)
{
	static const char *const allowed[] = {
		"extends", "target", "toolchain", "stdlib", "stdlib_policy",
		"freestanding", "arch", "cpu", "abi", "cc", "compiler",
		"c_compiler", "cxx", "cxx_compiler", "cale", "cale_compiler",
		"ar", "archiver", "linker", "sysroot", "resource_dir",
		"response_files", "rsp", "response_style", "rsp_style",
		"linker_script", "allow_absolute_tools", "external_absolute_tools",
		"compile_options", "include_dirs", "lib_dirs", "link_options",
		"defsyms", "artifact_names", "path_tools", "external_tools",
		"tool_overrides", NULL
	};
	const char *key;

	lua_pushnil(L);
	while (lua_next(L, table) != 0) {
		key = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
		if (!key || !string_in_set(key, allowed)) {
			lua_pop(L, 1);
			return qstar_set_error(graph,
			    "qstar: unknown profile field '%s'", key ? key : "<non-string>");
		}
		lua_pop(L, 1);
	}
	return 0;
}

/** Lua qstar.profile 호출을 graph declaration 추가로 낮춘다. */
static int
add_profile(lua_State *L, const char *name, int table_index)
{
	struct qstar_lua_context *ctx;
	struct qstar_profile_input input;
	struct qstar_graph *graph;
	const char *extends, *selected;
	char origin_file[QSTAR_PATH_MAX];
	int origin_line, rc;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	if (reject_graph_declaration_in_module(L, "qstar.profile") != 0)
		return 1;
	luaL_checktype(L, table_index, LUA_TTABLE);
	memset(&input, 0, sizeof(input));
	current_origin(L, origin_file, sizeof(origin_file), &origin_line);
	extends = check_string_field(L, table_index, "extends");
	if (validate_profile_fields(L, table_index, graph) < 0 ||
	    read_profile_table(L, table_index, &input, graph) < 0) {
		free_lua_profile_input(&input);
		return luaL_error(L, "%s", graph->error);
	}
	rc = qstar_graph_add_profile_decl(graph, name, extends, origin_file, origin_line,
	    &input);
	free_lua_profile_input(&input);
	if (rc < 0)
		return luaL_error(L, "%s", graph->error);
	selected = graph->profile.name && *graph->profile.name ? graph->profile.name :
	    "default";
	if (strcmp(selected, name) == 0 && qstar_graph_apply_selected_profile(graph) < 0)
		return luaL_error(L, "%s", graph->error);
	return 0;
}

/** qstar.profile "name" { ... } 형태의 후행 table call을 처리한다. */
static int
qstar_lua_profile_finish(lua_State *L)
{
	const char *name;

	name = lua_tostring(L, lua_upvalueindex(1));
	return add_profile(L, name, 1);
}

/** qstar.profile "name" API entry point다. */
static int
qstar_lua_profile(lua_State *L)
{
	const char *name;

	if (reject_graph_declaration_in_module(L, "qstar.profile") != 0)
		return 1;
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushcclosure(L, qstar_lua_profile_finish, 1);
		return 1;
	}
	return add_profile(L, name, 2);
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

/** folder path의 basename component를 반환한다. */
static const char *
path_basename_component(const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
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
	if (string_list_contains(&ctx->graph->evaluated_fragments, rel))
		return luaL_error(L, "qstar: duplicate import '%s'", rel);
	f = fopen(full, "r");
	if (!f)
		return luaL_error(L,
		    "qstar: import_module '%s' not found; expected module entry '%s'",
		    raw, rel);
	fclose(f);
	if (eval_module(L, ctx, full, rel_dir, rel) < 0)
		return lua_error(L);
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
	const char *profile, *target;

	graph = ctx->graph;
	profile = graph->profile.name && *graph->profile.name ? graph->profile.name : "default";
	target = graph->profile.target && *graph->profile.target ? graph->profile.target : "host";
	set_global_string(L, "QSTAR_VERSION", QSTAR_VERSION);
	set_global_integer(L, "QSTAR_VERSION_MAJOR", QSTAR_VERSION_MAJOR);
	set_global_integer(L, "QSTAR_VERSION_MINOR", QSTAR_VERSION_MINOR);
	set_global_integer(L, "QSTAR_VERSION_PATCH", QSTAR_VERSION_PATCH);
	set_global_string(L, "QSTAR_HOST_OS", qstar_host_os());
	set_global_string(L, "QSTAR_HOST_ARCH", qstar_host_arch());
	set_global_string(L, "QSTAR_PACKAGE_ROOT", ctx->root_dir);
	set_global_string(L, "QSTAR_PROJECT_ROOT", ctx->root_dir);
	set_global_string(L, "QSTAR_PROFILE", profile);
	set_global_string(L, "QSTAR_TARGET", target);
}

static void
register_conditions(lua_State *L)
{
	lua_newtable(L);
	lua_newtable(L);
	lua_pushstring(L, "os=macos");
	lua_setfield(L, -2, "macos");
	lua_pushstring(L, "os=linux");
	lua_setfield(L, -2, "linux");
	lua_pushstring(L, "os=windows");
	lua_setfield(L, -2, "windows");
	lua_setfield(L, -2, "os");
	lua_newtable(L);
	lua_pushstring(L, "arch=x86_64");
	lua_setfield(L, -2, "x86_64");
	lua_pushstring(L, "arch=aarch64");
	lua_setfield(L, -2, "aarch64");
	lua_pushstring(L, "arch=riscv64");
	lua_setfield(L, -2, "riscv64");
	lua_setfield(L, -2, "arch");
}

static void
register_qstar(lua_State *L, struct qstar_lua_context *ctx)
{
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "qstar.context");
	register_conditions(L);
	lua_pushstring(L, QSTAR_VERSION);
	lua_setfield(L, -2, "version");
	lua_newtable(L);
	set_table_string(L, -1, "os", qstar_host_os());
	set_table_string(L, -1, "arch", qstar_host_arch());
	lua_setfield(L, -2, "host");
	lua_newtable(L);
	set_table_string(L, -1, "root", ctx->root_dir);
	lua_newtable(L);
	lua_pushcfunction(L, qstar_lua_project);
	lua_setfield(L, -2, "__call");
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "project");
	lua_pushcfunction(L, qstar_lua_profile);
	lua_setfield(L, -2, "profile");
	lua_pushcfunction(L, qstar_lua_config);
	lua_setfield(L, -2, "config");
	lua_pushstring(L, "target");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "target");
	lua_pushstring(L, "exe");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "executable");
	lua_pushstring(L, "qstar.exe removed; use qstar.executable");
	lua_pushcclosure(L, qstar_lua_removed_api, 1);
	lua_setfield(L, -2, "exe");
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
	lua_pushcfunction(L, qstar_lua_config_header);
	lua_setfield(L, -2, "configure_file");
	lua_pushcfunction(L, qstar_lua_stage);
	lua_setfield(L, -2, "stage");
	lua_pushcfunction(L, qstar_lua_target_family);
	lua_setfield(L, -2, "target_family");
	lua_pushstring(L, "qstar.genrule removed; use qstar.custom_target");
	lua_pushcclosure(L, qstar_lua_removed_api, 1);
	lua_setfield(L, -2, "genrule");
	lua_pushstring(L, "qstar.config_header removed; use qstar.configure_file");
	lua_pushcclosure(L, qstar_lua_removed_api, 1);
	lua_setfield(L, -2, "config_header");
	lua_pushstring(L, "qstar.write_config_header removed; use qstar.configure_file");
	lua_pushcclosure(L, qstar_lua_removed_api, 1);
	lua_setfield(L, -2, "write_config_header");
	lua_pushcfunction(L, qstar_lua_output);
	lua_setfield(L, -2, "output");
	lua_pushcfunction(L, qstar_lua_input);
	lua_setfield(L, -2, "input");
	lua_pushcfunction(L, qstar_lua_target_file);
	lua_setfield(L, -2, "target_file");
	lua_pushcfunction(L, qstar_lua_stage_file);
	lua_setfield(L, -2, "stage_file");
	lua_pushcfunction(L, qstar_lua_cli);
	lua_setfield(L, -2, "cli");
	lua_pushcfunction(L, qstar_lua_status);
	lua_setfield(L, -2, "status");
	lua_pushcfunction(L, qstar_lua_identity_table);
	lua_setfield(L, -2, "modules");
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
	lua_pushcfunction(L, qstar_lua_select);
	lua_setfield(L, -2, "select");
	lua_pushcfunction(L, qstar_lua_incompatible);
	lua_setfield(L, -2, "incompatible");
	lua_pushcfunction(L, qstar_lua_import_file);
	lua_setfield(L, -2, "import_file");
	lua_pushcfunction(L, qstar_lua_import_module);
	lua_setfield(L, -2, "import_module");
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
	return rc;
}
