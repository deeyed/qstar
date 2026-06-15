#include "internal.h"

#include <string.h>

/** host target이 현재 build host에서 Windows 계열인지 반환한다. */
static int
host_target_is_windows(void)
{
#ifdef _WIN32
	return 1;
#else
	return 0;
#endif
}

/** host target이 현재 build host에서 Darwin/macOS 계열인지 반환한다. */
static int
host_target_is_darwin(void)
{
#ifdef __APPLE__
	return 1;
#else
	return 0;
#endif
}

/** host target이 현재 build host에서 Linux 계열인지 반환한다. */
static int
host_target_is_linux(void)
{
#if defined(__linux__)
	return 1;
#else
	return 0;
#endif
}

/** target triple에서 Windows 계열 여부를 보수적으로 판정한다. */
int
qstar_toolchain_target_is_windows(const char *target)
{
	if (!target || !*target || strcmp(target, "host") == 0 ||
	    strcmp(target, "default") == 0)
		return host_target_is_windows();
	return target && (strstr(target, "windows") || strstr(target, "mingw") ||
	    strstr(target, "msvc"));
}

/** target triple에서 Darwin/macOS 계열 여부를 보수적으로 판정한다. */
int
qstar_toolchain_target_is_darwin(const char *target)
{
	if (!target || !*target || strcmp(target, "host") == 0 ||
	    strcmp(target, "default") == 0)
		return host_target_is_darwin();
	return strstr(target, "apple") || strstr(target, "darwin") ||
	    strstr(target, "macos");
}

/** target triple에서 Linux 계열 여부를 보수적으로 판정한다. */
int
qstar_toolchain_target_is_linux(const char *target)
{
	if (!target || !*target || strcmp(target, "host") == 0 ||
	    strcmp(target, "default") == 0)
		return host_target_is_linux();
	return strstr(target, "linux") || strstr(target, "musl");
}

/** 이번 sharedlib backend가 지원하는 target triple인지 확인한다. */
int
qstar_toolchain_target_supports_sharedlib(const char *target)
{
	return qstar_toolchain_target_is_darwin(target) ||
	    qstar_toolchain_target_is_linux(target);
}

/** target triple에서 MSVC command-line 계열 여부를 판정한다. */
static int
target_is_msvc(const char *target)
{
	return target && strstr(target, "msvc");
}

/** response_files 값을 boolean capability로 낮춘다. */
static int
response_files_enabled(const char *value, int default_value)
{
	if (!value || !*value || strcmp(value, "auto") == 0)
		return default_value;
	if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0 ||
	    strcmp(value, "0") == 0)
		return 0;
	return 1;
}

/** target/build context 입력을 합쳐 toolchain v1을 결정한다. */
int
qstar_resolve_toolchain(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_resolved_toolchain *resolved)
{
	const struct qstar_toolset *toolset;
	const struct qstar_string_list *role;
	const char *name, *stdlib_policy, *triple;

	memset(resolved, 0, sizeof(*resolved));
	name = target->toolchain && strcmp(target->toolchain, "host") != 0 ?
	    target->toolchain : graph->build_context.toolchain;
	if (!name || !*name)
		name = target->toolchain && *target->toolchain ? target->toolchain : "host";
	stdlib_policy = target->stdlib_policy && strcmp(target->stdlib_policy, "system") != 0 ?
	    target->stdlib_policy : graph->build_context.stdlib_policy;
	if (!stdlib_policy || !*stdlib_policy)
		stdlib_policy = target->stdlib_policy && *target->stdlib_policy ?
		    target->stdlib_policy : "system";
	triple = graph->build_context.target && *graph->build_context.target ?
	    graph->build_context.target : "host";
	snprintf(resolved->name, sizeof(resolved->name), "%s", name);
	snprintf(resolved->target, sizeof(resolved->target), "%s", triple);
	snprintf(resolved->stdlib_policy, sizeof(resolved->stdlib_policy), "%s",
	    stdlib_policy);
	snprintf(resolved->resolver, sizeof(resolved->resolver), "builtin-v1");
	if (strcmp(name, "host") == 0 || strcmp(name, "default") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "cc");
		snprintf(resolved->cxx, sizeof(resolved->cxx), "c++");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "cc");
	} else if (strcmp(name, "clang") == 0) {
		snprintf(resolved->cc, sizeof(resolved->cc), "clang");
		snprintf(resolved->cxx, sizeof(resolved->cxx), "clang++");
		snprintf(resolved->ar, sizeof(resolved->ar), "ar");
		snprintf(resolved->linker, sizeof(resolved->linker), "clang");
	} else {
		return qstar_set_error(graph, "qstar: unknown toolchain context '%s'", name);
	}
	snprintf(resolved->asm_, sizeof(resolved->asm_), "%s", resolved->cc);
	if (graph->build_context.cc && *graph->build_context.cc)
		snprintf(resolved->cc, sizeof(resolved->cc), "%s", graph->build_context.cc);
	if (graph->build_context.cxx && *graph->build_context.cxx)
		snprintf(resolved->cxx, sizeof(resolved->cxx), "%s", graph->build_context.cxx);
	if (graph->build_context.ar && *graph->build_context.ar)
		snprintf(resolved->ar, sizeof(resolved->ar), "%s", graph->build_context.ar);
	if (graph->build_context.linker && *graph->build_context.linker)
		snprintf(resolved->linker, sizeof(resolved->linker), "%s",
		    graph->build_context.linker);
	snprintf(resolved->asm_, sizeof(resolved->asm_), "%s", resolved->cc);
	if (graph->build_context.sysroot && *graph->build_context.sysroot)
		snprintf(resolved->sysroot, sizeof(resolved->sysroot), "%s",
		    graph->build_context.sysroot);
	if (graph->build_context.resource_dir && *graph->build_context.resource_dir)
		snprintf(resolved->resource_dir, sizeof(resolved->resource_dir), "%s",
		    graph->build_context.resource_dir);
	resolved->response_files =
	    response_files_enabled(graph->build_context.response_files, 1);
	if (graph->build_context.response_style && *graph->build_context.response_style)
		snprintf(resolved->response_style, sizeof(resolved->response_style), "%s",
		    graph->build_context.response_style);
	else if (target_is_msvc(resolved->target))
		snprintf(resolved->response_style, sizeof(resolved->response_style), "msvc");
	else if (qstar_toolchain_target_is_windows(resolved->target))
		snprintf(resolved->response_style, sizeof(resolved->response_style),
		    "windows");
	else
		snprintf(resolved->response_style, sizeof(resolved->response_style), "posix");
	snprintf(resolved->resolver, sizeof(resolved->resolver), "builtin-toolchain-v1");
	toolset = target && target->toolset && *target->toolset ?
	    qstar_graph_find_toolset(graph, target->toolset) : NULL;
	if (toolset) {
		snprintf(resolved->toolset, sizeof(resolved->toolset), "%s", toolset->label);
		role = qstar_toolset_role_argv(toolset, "c");
		if (role)
			snprintf(resolved->cc, sizeof(resolved->cc), "%s", role->items[0]);
		role = qstar_toolset_role_argv(toolset, "cxx");
		if (role)
			snprintf(resolved->cxx, sizeof(resolved->cxx), "%s", role->items[0]);
		role = qstar_toolset_role_argv(toolset, "asm");
		if (role)
			snprintf(resolved->asm_, sizeof(resolved->asm_), "%s", role->items[0]);
		else
			snprintf(resolved->asm_, sizeof(resolved->asm_), "%s", resolved->cc);
		role = qstar_toolset_role_argv(toolset, "archive");
		if (role)
			snprintf(resolved->ar, sizeof(resolved->ar), "%s", role->items[0]);
		role = qstar_toolset_role_argv(toolset, "link");
		if (role)
			snprintf(resolved->linker, sizeof(resolved->linker), "%s",
			    role->items[0]);
		resolved->response_files = response_files_enabled(
		    toolset->response_files, resolved->response_files);
		if (toolset->response_style && *toolset->response_style &&
		    strcmp(toolset->response_style, "auto") != 0)
			snprintf(resolved->response_style, sizeof(resolved->response_style),
			    "%s", toolset->response_style);
		snprintf(resolved->resolver, sizeof(resolved->resolver), "toolset-schema-v1");
	}
	return 0;
}
