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
	return qstar_platform_is_darwin(platform) || qstar_platform_is_linux(platform) ||
	    qstar_platform_is_windows(platform);
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

	memset(resolved, 0, sizeof(*resolved));
	snprintf(resolved->platform, sizeof(resolved->platform), "%s",
	    qstar_graph_platform(graph));
	snprintf(resolved->cc, sizeof(resolved->cc), "cc");
	snprintf(resolved->cxx, sizeof(resolved->cxx), "c++");
	snprintf(resolved->ar, sizeof(resolved->ar), "ar");
	snprintf(resolved->linker, sizeof(resolved->linker), "cc");
	snprintf(resolved->asm_, sizeof(resolved->asm_), "%s", resolved->cc);
	resolved->response_files = 1;
	if (qstar_platform_is_windows(resolved->platform))
		snprintf(resolved->response_style, sizeof(resolved->response_style),
		    "windows");
	else
		snprintf(resolved->response_style, sizeof(resolved->response_style), "posix");
	snprintf(resolved->resolver, sizeof(resolved->resolver), "builtin-tools-v1");
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

int
qstar_resolve_toolset_context(struct qstar_graph *graph,
    const char *toolset_label, struct qstar_resolved_toolchain *resolved)
{
	struct qstar_target owner;

	if (!toolset_label || !*toolset_label)
		return qstar_set_error(graph,
		    "qstar: explicit toolset context requires a toolset label");
	memset(&owner, 0, sizeof(owner));
	owner.toolset = (char *)toolset_label;
	return qstar_resolve_toolchain(graph, &owner, resolved);
}

/** list가 exact command tool capability를 선언했는지 확인한다. */
static int
tool_list_contains(const struct qstar_string_list *list, const char *tool)
{
	size_t i;

	if (!list || !tool || !*tool)
		return 0;
	for (i = 0; i < list->len; i++) {
		if (strcmp(list->items[i], tool) == 0)
			return 1;
	}
	return 0;
}

/**
 * custom/run command는 compiler/archive/link role이 아니다.
 *
 * 따라서 toolset의 일반 response policy만으로 @file을 주입하지 않고, command
 * tool이 response_file_tools에 명시된 경우에만 그 policy를 활성화한다.
 */
int
qstar_resolve_command_materialization_context(struct qstar_graph *graph,
    const char *toolset_label, const char *tool,
    struct qstar_resolved_toolchain *resolved)
{
	const struct qstar_toolset *toolset;

	if (qstar_resolve_toolset_context(graph, toolset_label, resolved) < 0)
		return -1;
	toolset = qstar_graph_find_toolset(graph, toolset_label);
	if (!toolset || !tool_list_contains(&toolset->response_file_tools, tool))
		resolved->response_files = 0;
	return 0;
}
