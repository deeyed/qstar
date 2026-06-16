#include "internal.h"

#include <string.h>

/** host platform이 현재 build host에서 Windows 계열인지 반환한다. */
static int
host_platform_is_windows(void)
{
#ifdef _WIN32
	return 1;
#else
	return 0;
#endif
}

/** host platform이 현재 build host에서 Darwin/macOS 계열인지 반환한다. */
static int
host_platform_is_darwin(void)
{
#ifdef __APPLE__
	return 1;
#else
	return 0;
#endif
}

/** host platform이 현재 build host에서 Linux 계열인지 반환한다. */
static int
host_platform_is_linux(void)
{
#if defined(__linux__)
	return 1;
#else
	return 0;
#endif
}

/** host platform context를 반환한다. */
const char *
qstar_host_platform(void)
{
	if (host_platform_is_windows())
		return "windows";
	if (host_platform_is_darwin())
		return "darwin";
	if (host_platform_is_linux())
		return "linux";
	return "generic";
}

/** platform context가 Windows 계열인지 확인한다. */
int
qstar_platform_is_windows(const char *platform)
{
	if (!platform || !*platform || strcmp(platform, "host") == 0 ||
	    strcmp(platform, "default") == 0)
		return host_platform_is_windows();
	return strcmp(platform, "windows") == 0;
}

/** platform context가 Darwin/macOS 계열인지 확인한다. */
int
qstar_platform_is_darwin(const char *platform)
{
	if (!platform || !*platform || strcmp(platform, "host") == 0 ||
	    strcmp(platform, "default") == 0)
		return host_platform_is_darwin();
	return strcmp(platform, "darwin") == 0;
}

/** platform context가 Linux 계열인지 확인한다. */
int
qstar_platform_is_linux(const char *platform)
{
	if (!platform || !*platform || strcmp(platform, "host") == 0 ||
	    strcmp(platform, "default") == 0)
		return host_platform_is_linux();
	return strcmp(platform, "linux") == 0;
}

/** 이번 sharedlib backend가 지원하는 platform context인지 확인한다. */
int
qstar_platform_supports_sharedlib(const char *platform)
{
	return qstar_platform_is_darwin(platform) || qstar_platform_is_linux(platform);
}

static int
tool_name_uses_msvc_link_style(const char *tool)
{
	if (!tool)
		return 0;
	return strstr(tool, "clang-cl") || strstr(tool, "cl.exe") ||
	    strstr(tool, "lld-link") || strstr(tool, "link.exe");
}

static const char *
resolved_link_style(const struct qstar_resolved_toolchain *resolved)
{
	if (resolved->response_style[0] &&
	    strcmp(resolved->response_style, "msvc") == 0)
		return "msvc";
	if (tool_name_uses_msvc_link_style(resolved->cc) ||
	    tool_name_uses_msvc_link_style(resolved->cxx) ||
	    tool_name_uses_msvc_link_style(resolved->linker))
		return "msvc";
	return "posix";
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

const char *
qstar_resolved_toolchain_provider_tool(const struct qstar_resolved_toolchain *resolved,
    const char *provider, const char *provider_role)
{
	if (!resolved || !provider || !provider_role ||
	    strcmp(provider_role, "compiler") != 0)
		return NULL;
	if (strcmp(provider, "c") == 0)
		return resolved->cc;
	if (strcmp(provider, "cxx") == 0)
		return resolved->cxx;
	if (strcmp(provider, "asm") == 0)
		return resolved->asm_;
	return NULL;
}

static void
apply_builtin_provider_tool(const struct qstar_toolset *toolset, const char *provider_name,
    char *dst, size_t dstlen)
{
	const struct qstar_language_provider_info *provider;
	const struct qstar_string_list *role;
	char role_name[128];

	provider = qstar_language_provider_lookup(provider_name);
	if (!provider || !provider->compiler_role || !provider->compiler_role[0])
		return;
	if (snprintf(role_name, sizeof(role_name), "%s.%s", provider->namespace,
	    provider->compiler_role) >= (int)sizeof(role_name))
		return;
	role = qstar_toolset_role_argv(toolset, role_name);
	if (role)
		snprintf(dst, dstlen, "%s", role->items[0]);
}

/** target/build context 입력을 tool role metadata로 결정한다. */
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
	snprintf(resolved->platform, sizeof(resolved->platform), "%s",
	    qstar_graph_platform(graph));
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
	resolved->response_files =
	    response_files_enabled(graph->build_context.response_files, 1);
	if (graph->build_context.response_style && *graph->build_context.response_style)
		snprintf(resolved->response_style, sizeof(resolved->response_style), "%s",
		    graph->build_context.response_style);
	else if (qstar_platform_is_windows(resolved->platform))
		snprintf(resolved->response_style, sizeof(resolved->response_style),
		    "windows");
	else
		snprintf(resolved->response_style, sizeof(resolved->response_style), "posix");
	snprintf(resolved->resolver, sizeof(resolved->resolver), "builtin-toolchain-v1");
	toolset = target && target->toolset && *target->toolset ?
	    qstar_graph_find_toolset(graph, target->toolset) : NULL;
	if (toolset) {
		snprintf(resolved->toolset, sizeof(resolved->toolset), "%s", toolset->label);
		apply_builtin_provider_tool(toolset, "c", resolved->cc,
		    sizeof(resolved->cc));
		apply_builtin_provider_tool(toolset, "cxx", resolved->cxx,
		    sizeof(resolved->cxx));
		apply_builtin_provider_tool(toolset, "asm", resolved->asm_,
		    sizeof(resolved->asm_));
		if (!qstar_toolset_role_argv(toolset, "asm.compiler"))
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
	snprintf(resolved->link_style, sizeof(resolved->link_style), "%s",
	    resolved_link_style(resolved));
	return 0;
}
