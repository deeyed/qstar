#include "internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct qstar_lua_context {
	struct qstar_graph *graph;
	char root_dir[QSTAR_PATH_MAX];
	char current_dir[QSTAR_PATH_MAX];
};

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
	target = qstar_graph_add_target(graph, label, name, kind, fragment_dir);
	if (!target)
		return luaL_error(L, "%s", graph->error);
	if (read_modules(L, table_index, target, graph) < 0)
		return luaL_error(L, "%s", graph->error);
	if (read_list_field(L, table_index, "sources", &target->sources, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "public_headers", &target->public_headers, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "private_headers", &target->private_headers, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "include_dirs", &target->include_dirs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "system_include_dirs", &target->system_include_dirs, graph, 0, target->fragment_dir) < 0 ||
	    read_list_field(L, table_index, "deps", &target->deps, graph, 1, target->fragment_dir) < 0)
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
	if (!target->toolchain || !target->stdlib_policy)
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
add_genrule(lua_State *L, const char *name, int table_index, const char *fragment_dir)
{
	struct qstar_lua_context *ctx;
	struct qstar_genrule *genrule;
	struct qstar_graph *graph;
	const char *tool;
	char label[QSTAR_PATH_MAX], rawlabel[QSTAR_PATH_MAX];

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
	genrule = qstar_graph_add_genrule(graph, label, name, fragment_dir);
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

static int
qstar_lua_output(lua_State *L)
{
	lua_pushstring(L, luaL_checkstring(L, 1));
	return 1;
}

static int
qstar_lua_select(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushvalue(L, 1);
	lua_pushstring(L, "select");
	lua_setfield(L, -2, "__qstar_kind");
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

static int
qstar_lua_subdir(lua_State *L)
{
	struct qstar_lua_context *ctx;
	const char *dir, *base;
	char path[QSTAR_PATH_MAX], candidate[QSTAR_PATH_MAX], fragment[QSTAR_PATH_MAX];
	const char *names[3];
	size_t i;
	FILE *f;

	ctx = get_context(L);
	dir = luaL_checkstring(L, 1);
	if (!qstar_path_is_package_relative(dir))
		return luaL_error(L, "qstar: subdir path '%s' must be package-relative", dir);
	base = strrchr(dir, '/');
	base = base ? base + 1 : dir;
	snprintf(path, sizeof(path), "%s/%s.qs", dir, base);
	names[0] = path;
	names[1] = "qstar.lua";
	names[2] = "qstar.qs";
	for (i = 0; i < 3; i++) {
		if (i == 0) {
			if (qstar_path_join(ctx->root_dir, names[i], candidate, sizeof(candidate)) < 0)
				return luaL_error(L, "qstar: subdir path too long");
		} else {
			if (qstar_path_join(dir, names[i], fragment, sizeof(fragment)) < 0 ||
			    qstar_path_join(ctx->root_dir, fragment, candidate, sizeof(candidate)) < 0)
				return luaL_error(L, "qstar: subdir path too long");
		}
		f = fopen(candidate, "r");
		if (f) {
			fclose(f);
			if (eval_fragment(L, ctx, candidate, dir) < 0)
				return lua_error(L);
			return 0;
		}
	}
	return luaL_error(L, "qstar: subdir '%s' has no qstar fragment", dir);
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
	lua_setfield(L, -2, "exe");
	lua_pushstring(L, "staticlib");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "staticlib");
	lua_pushstring(L, "sharedlib");
	lua_pushcclosure(L, qstar_lua_target, 1);
	lua_setfield(L, -2, "sharedlib");
	lua_pushcfunction(L, qstar_lua_genrule);
	lua_setfield(L, -2, "genrule");
	lua_pushcfunction(L, qstar_lua_output);
	lua_setfield(L, -2, "output");
	lua_pushcfunction(L, qstar_lua_identity_table);
	lua_setfield(L, -2, "modules");
	lua_pushcfunction(L, qstar_lua_identity_table);
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
	if (luaL_loadfilex(L, file, "t") != LUA_OK)
		return -1;
	if (lua_pcall(L, 0, 0, 0) != LUA_OK)
		return -1;
	snprintf(ctx->current_dir, sizeof(ctx->current_dir), "%s", old);
	return 0;
}

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int
qstar_lua_eval_file(struct qstar_graph *graph, const char *file)
{
	struct qstar_lua_context ctx;
	lua_State *L;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.graph = graph;
	if (qstar_dirname(file, ctx.root_dir, sizeof(ctx.root_dir)) < 0)
		return qstar_set_error(graph, "qstar: qstar file path too long");
	if (qstar_graph_set_package_root(graph, ctx.root_dir) < 0)
		return -1;
	L = luaL_newstate();
	if (!L)
		return qstar_set_error(graph, "qstar: could not create Lua state");
	open_sandbox(L);
	register_qstar(L, &ctx);
	rc = eval_fragment(L, &ctx, file, "");
	if (rc < 0)
		qstar_set_error(graph, "%s", lua_tostring(L, -1));
	lua_close(L);
	return rc;
}
