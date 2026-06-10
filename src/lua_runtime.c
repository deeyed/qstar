#include "internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qstar_lua_context {
	struct qstar_graph *graph;
	char root_dir[QSTAR_PATH_MAX];
	char current_dir[QSTAR_PATH_MAX];
};

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

static int
push_define_option(struct qstar_graph *graph, struct qstar_string_list *list, const char *def)
{
	char buf[QSTAR_PATH_MAX];

	if (snprintf(buf, sizeof(buf), "-D%s", def) >= (int)sizeof(buf))
		return qstar_set_error(graph, "qstar: lang define '%s' is too long", def);
	return qstar_string_list_push(list, buf) < 0 ?
	    qstar_set_error(graph, "qstar: out of memory") : 0;
}

static int
read_lang_defines(lua_State *L, int lang, const char *name, struct qstar_string_list *list,
    struct qstar_graph *graph)
{
	size_t i, n;

	lua_getfield(L, lang, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: lang.%s.defines must be a list", name);
	}
	n = lua_rawlen(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 2);
			return qstar_set_error(graph, "qstar: lang.%s.defines contains non-string item",
			    name);
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
read_lang_c(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
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
	lua_pop(L, 1);
	return rc;
}

static int
read_lang_asm(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
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
read_lang_cale(lua_State *L, int lang, struct qstar_target *target, struct qstar_graph *graph)
{
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
	rc = read_list_field(L, -1, "hcl_include_dirs", &target->cale_hcl_include_dirs,
	    graph, 0, target->fragment_dir);
	if (rc == 0)
		rc = read_list_field(L, -1, "compile_options", &target->cale_compile_options,
		    graph, 0, target->fragment_dir);
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
read_modules(lua_State *L, int table, struct qstar_target *target, struct qstar_graph *graph)
{
	const char *root;
	int rc;

	lua_getfield(L, table, "modules");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return qstar_set_error(graph, "qstar: field 'modules' must be a table");
	}
	target->modules.present = 1;
	root = check_string_field(L, -1, "root");
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
add_target(lua_State *L, const char *name, int table_index, const char *default_kind,
    const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_target *target;
	struct qstar_graph *graph;
	const char *kind, *toolchain, *stdlib_policy;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;

	if (table_index < 0)
		table_index = lua_gettop(L) + table_index + 1;
	ctx = get_context(L);
	graph = ctx->graph;
	luaL_checktype(L, table_index, LUA_TTABLE);
	kind = check_string_field(L, table_index, "kind");
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
	    "top-level include_dirs is not allowed; use lang.c.include_dirs or lang.cxx.include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "public_include_dirs",
	    "top-level public_include_dirs is not allowed; use lang.c.public_include_dirs or lang.cxx.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "private_include_dirs",
	    "top-level private_include_dirs is not allowed; use lang.c.private_include_dirs or lang.cxx.private_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "system_include_dirs",
	    "top-level system_include_dirs is not allowed; use lang.c.system_include_dirs or lang.cxx.system_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "interface_include_dirs",
	    "top-level interface_include_dirs is not allowed; use lang.c.public_include_dirs or lang.cxx.public_include_dirs") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cflags",
	    "top-level cflags is not allowed; use lang.c.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxxflags",
	    "top-level cxxflags is not allowed; use lang.cxx.compile_options") < 0 ||
	    reject_top_level_field(L, table_index, graph, "cxx_standard",
	    "top-level cxx_standard is not allowed; use lang.cxx.standard") < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_modules(L, table_index, target, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "sources", &target->sources, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "public_headers", &target->public_headers, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "private_headers", &target->private_headers, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "public_deps", &target->deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "private_deps", &target->private_deps, graph, 1, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "visibility", &target->visibility, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "libs", &target->libs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "lib_dirs", &target->lib_dirs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "frameworks", &target->frameworks, graph, 0, target->fragment_dir) < 0 ||
	    read_lang_options(L, table_index, target, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	toolchain = check_string_field(L, table_index, "toolchain");
	stdlib_policy = check_string_field(L, table_index, "stdlib");
	if (toolchain) {
		free(target->toolchain);
		target->toolchain = qstar_strdup(toolchain);
	}
	if (stdlib_policy) {
		free(target->stdlib_policy);
		target->stdlib_policy = qstar_strdup(stdlib_policy);
	}
	if (!target->toolchain || !target->stdlib_policy || !target->cxx_standard)
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
	tool = check_string_field(L, table_index, "tool");
	if (tool) {
		free(genrule->tool);
		genrule->tool = qstar_strdup(tool);
	}
	if (read_list_field(L, table_index, "inputs", &genrule->inputs, graph, 0,
	    genrule->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "outputs", &genrule->outputs, graph, 0,
	    genrule->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "args", &genrule->args, graph, 0,
	    genrule->fragment_dir) < 0)
		return luaL_error(L, "%s", graph->error);
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
	if (read_list_field(L, table_index, "defines", &genrule->args, graph, 0,
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
	name = luaL_checkstring(L, 1);
	if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
		lua_pushstring(L, name);
		lua_pushstring(L, ctx->current_dir);
		lua_pushcclosure(L, qstar_lua_config_header_finish, 2);
		return 1;
	}
	return add_config_header(L, name, 2, ctx->current_dir);
}

static int
qstar_lua_identity_table(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushvalue(L, 1);
	return 1;
}

static int
qstar_lua_join(lua_State *L)
{
	size_t n, i, j, m;

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	n = lua_rawlen(L, 1);
	m = 1;
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, (lua_Integer)i);
		if (lua_istable(L, -1)) {
			for (j = 1; j <= lua_rawlen(L, -1); j++) {
				lua_rawgeti(L, -1, (lua_Integer)j);
				lua_rawseti(L, -3, (lua_Integer)m++);
			}
			lua_pop(L, 1);
		} else {
			lua_rawseti(L, -2, (lua_Integer)m++);
		}
	}
	return 1;
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
	lua_pushstring(L, luaL_checkstring(L, 1));
	return 1;
}

static int
qstar_lua_select(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *target, *key, *selected;
	int matched, has_default;

	ctx = get_context(L);
	target = ctx->graph->profile.target && *ctx->graph->profile.target ?
	    ctx->graph->profile.target : "host";
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
		    (strcmp(key, "arch=x86_64") == 0 && (strstr(target, "x86_64") || strstr(target, "amd64"))) ||
		    (strcmp(key, "arch=aarch64") == 0 && (strstr(target, "aarch64") || strstr(target, "arm64"))) ||
		    (strcmp(key, "arch=riscv64") == 0 && (strstr(target, "riscv64") || strstr(target, "rv64"))))) {
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

static int eval_fragment(lua_State *L, struct qstar_lua_context *ctx, const char *file,
    const char *fragment_dir);

/** 평가한 authoring fragment를 package-relative path로 graph에 기록한다. */
static int
remember_fragment(struct qstar_lua_context *ctx, const char *file)
{
	const char *rel;
	size_t n;

	rel = file;
	n = strlen(ctx->root_dir);
	if (n > 0 && strcmp(ctx->root_dir, ".") != 0 &&
	    strncmp(file, ctx->root_dir, n) == 0 && file[n] == '/')
		rel = file + n + 1;
	else if (strncmp(file, "./", 2) == 0)
		rel = file + 2;
	return qstar_string_list_push(&ctx->graph->evaluated_fragments, rel);
}

static int
qstar_lua_subdir(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *dir, *base;
	char path[QSTAR_PATH_MAX], candidate[QSTAR_PATH_MAX], fragment[QSTAR_PATH_MAX];
	char full_dir[QSTAR_PATH_MAX];
	char origin_file[QSTAR_PATH_MAX];
	int origin_line;
	FILE *f;

	ctx = get_context(L);
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
	snprintf(path, sizeof(path), "%s/%s.qs", full_dir, base);
	if (qstar_path_join(ctx->root_dir, path, candidate, sizeof(candidate)) < 0)
		return luaL_error(L, "qstar: subdir path too long");
	f = fopen(candidate, "r");
	if (f) {
		fclose(f);
		if (eval_fragment(L, ctx, candidate, full_dir) < 0)
			return lua_error(L);
		return 0;
	}
	if (qstar_path_join(full_dir, "qstar.qs", fragment, sizeof(fragment)) < 0 ||
	    qstar_path_join(ctx->root_dir, fragment, candidate, sizeof(candidate)) < 0)
		return luaL_error(L, "qstar: subdir path too long");
	f = fopen(candidate, "r");
	if (f) {
		fclose(f);
		if (qstar_graph_add_lint(ctx->graph, "QSTAR003", "warning",
		    origin_file, origin_line, "subdir", "<none>",
		    "deprecated-fragment-name: subdir '%s' used '%s'; use '%s'",
		    full_dir, fragment, path) < 0)
			return luaL_error(L, "qstar: out of memory");
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
	lua_pushcfunction(L, qstar_lua_identity_table);
	lua_setfield(L, -2, "modules");
	lua_pushcfunction(L, qstar_lua_files);
	lua_setfield(L, -2, "files");
	lua_pushcfunction(L, qstar_lua_join);
	lua_setfield(L, -2, "join");
	lua_pushcfunction(L, qstar_lua_select);
	lua_setfield(L, -2, "select");
	lua_pushcfunction(L, qstar_lua_incompatible);
	lua_setfield(L, -2, "incompatible");
	lua_pushcfunction(L, qstar_lua_subdir);
	lua_setfield(L, -2, "subdir");
	lua_setglobal(L, "qstar");
}

static int
eval_fragment(lua_State *L, struct qstar_lua_context *ctx, const char *file, const char *fragment_dir)
{
	char old[QSTAR_PATH_MAX];

	snprintf(old, sizeof(old), "%s", ctx->current_dir);
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", fragment_dir ? fragment_dir : "");
	if (remember_fragment(ctx, file) < 0)
		return -1;
	if (luaL_loadfilex(L, file, "t") != LUA_OK)
		return -1;
	if (lua_pcall(L, 0, 0, 0) != LUA_OK)
		return -1;
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
	return 0;
}

/** directory에 qstar.workspace marker가 있는지 확인한다. */
static int
workspace_marker_exists(const char *dir)
{
	char path[QSTAR_PATH_MAX];
	FILE *f;

	if (qstar_path_join(dir, "qstar.workspace", path, sizeof(path)) < 0)
		return 0;
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

/** qstar.lua 위치에서 위로 올라가며 qstar.workspace root를 찾는다. */
static int
discover_workspace_root(const char *file_dir, char *root, size_t rootlen,
    char *fragment, size_t fragmentlen)
{
	char cur[QSTAR_PATH_MAX], parent[QSTAR_PATH_MAX];

	snprintf(cur, sizeof(cur), "%s", file_dir && *file_dir ? file_dir : ".");
	for (;;) {
		if (workspace_marker_exists(cur)) {
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
	if (snprintf(root, rootlen, "%s", file_dir && *file_dir ? file_dir : ".") >=
	    (int)rootlen)
		return -1;
	if (fragmentlen)
		fragment[0] = '\0';
	return 0;
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
	if (qstar_dirname(file, file_dir, sizeof(file_dir)) < 0 ||
	    discover_workspace_root(file_dir, ctx.root_dir, sizeof(ctx.root_dir),
	    initial_fragment, sizeof(initial_fragment)) < 0)
		return qstar_set_error(graph, "qstar: qstar file path too long");
	if (qstar_graph_set_package_root(graph, ctx.root_dir) < 0)
		return -1;
	base = strrchr(file, '/');
	base = base ? base + 1 : file;
	if (initial_fragment[0] == '\0' && strcmp(base, "qstar.lua") != 0) {
		if (qstar_graph_add_lint(graph, "QSTAR001", "error", file, 1,
		    "file", "<none>", "root entry must be qstar.lua") < 0)
			return -1;
	}
	L = luaL_newstate();
	if (!L)
		return qstar_set_error(graph, "qstar: could not create Lua state");
	open_sandbox(L);
	register_qstar(L, &ctx);
	rc = eval_fragment(L, &ctx, file, initial_fragment);
	if (rc < 0)
		qstar_set_error(graph, "%s", lua_tostring(L, -1));
	lua_close(L);
	return rc;
}
