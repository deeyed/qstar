#include "internal.h"

#include <string.h>

struct source_rule {
	const char *suffix;
	struct qstar_source_info info;
};

static const struct source_rule source_rules[] = {
	{ ".c", { NULL, "c", "c-compiler", "c", "objects", 1, 0 } },
	{ ".cc", { NULL, "cxx", "cxx-compiler", "cxx", "objects", 1, 0 } },
	{ ".cpp", { NULL, "cxx", "cxx-compiler", "cxx", "objects", 1, 0 } },
	{ ".cxx", { NULL, "cxx", "cxx-compiler", "cxx", "objects", 1, 0 } },
	{ ".cppm", { NULL, "cxx-module", "cxx-module-scanner", "cxx", "modules", 0, 0 } },
	{ ".ixx", { NULL, "cxx-module", "cxx-module-scanner", "cxx", "modules", 0, 0 } },
	{ ".h", { NULL, "header", "header-input", "c", "headers", 0, 1 } },
	{ ".hpp", { NULL, "cxx-header", "header-input", "cxx", "headers", 0, 1 } },
	{ ".hh", { NULL, "cxx-header", "header-input", "cxx", "headers", 0, 1 } },
	{ ".cl", { NULL, "cale", "cale-compiler", "cale", "objects", 1, 0 } },
	{ ".cale", { NULL, "cale", "cale-compiler", "cale", "objects", 1, 0 } },
	{ ".s", { NULL, "asm", "assembler", "asm", "objects", 1, 0 } },
	{ ".S", { NULL, "asm-cpp", "preprocessed-assembler", "asm", "objects", 1, 0 } },
	{ ".o", { NULL, "object", "link-object", "native", "objects", 0, 0 } },
	{ ".obj", { NULL, "object", "link-object", "native", "objects", 0, 0 } },
};

static const struct qstar_target_rule_info target_rules[] = {
	{ "exe", "native", "link", "exe", "", "", 1, 1, 1 },
	{ "test", "native", "link", "exe", "", "", 1, 0, 1 },
	{ "staticlib", "native", "archive", "libs", "lib", ".a", 0, 1, 1 },
	{ "sharedlib", "native", "link-shared", "libs", "lib", ".so", 0, 0, 0 },
	{ "objectlib", "native", "compile-objects", "objects", "", "", 0, 0, 0 },
	{ "run_target", "generic", "run", "generic", "", "", 0, 0, 0 },
	{ "group", "generic", "group", "none", "", "", 0, 0, 0 },
	{ "target", "generic", "materialize", "generic", "", "", 0, 0, 0 },
};

/** path가 source kind rule의 suffix와 일치하는지 확인한다. */
static int
has_suffix(const char *path, const char *suffix)
{
	size_t npath, nsuffix;

	npath = strlen(path);
	nsuffix = strlen(suffix);
	return npath >= nsuffix && strcmp(path + npath - nsuffix, suffix) == 0;
}

/** QStar source kind registry에서 path suffix에 맞는 항목을 찾는다. */
const struct qstar_source_info *
qstar_source_kind_lookup_path(const char *path)
{
	size_t i;

	for (i = 0; i < sizeof(source_rules) / sizeof(source_rules[0]); i++) {
		if (has_suffix(path, source_rules[i].suffix))
			return &source_rules[i].info;
	}
	return NULL;
}

/** QStar source path를 language/tool role로 분류한다. */
int
qstar_source_classify(const char *path, struct qstar_source_info *info)
{
	const struct qstar_source_info *rule;

	rule = qstar_source_kind_lookup_path(path);
	if (!rule) {
		if (info) {
			info->path = path;
			info->language = "unknown";
			info->tool_role = "unsupported";
			info->provider = "unknown";
			info->output_group = "unknown";
			info->compile_input = 0;
			info->header_input = 0;
		}
		return -1;
	}
	if (info) {
		*info = *rule;
		info->path = path;
	}
	return 0;
}

/** QStar target rule registry에서 target kind에 맞는 rule을 찾는다. */
const struct qstar_target_rule_info *
qstar_target_rule_lookup(const char *kind)
{
	size_t i;

	for (i = 0; i < sizeof(target_rules) / sizeof(target_rules[0]); i++) {
		if (strcmp(target_rules[i].kind, kind) == 0)
			return &target_rules[i];
	}
	return NULL;
}

/** target kind의 최종 action 이름을 rule registry 기준으로 반환한다. */
const char *
qstar_target_final_action(const struct qstar_target *target)
{
	const struct qstar_target_rule_info *rule;

	rule = qstar_target_rule_lookup(target->kind);
	return rule ? rule->final_action : "materialize";
}

/** target kind의 output group 이름을 rule registry 기준으로 반환한다. */
const char *
qstar_target_output_group(const struct qstar_target *target)
{
	const struct qstar_target_rule_info *rule;

	rule = qstar_target_rule_lookup(target->kind);
	return rule ? rule->output_group : "generic";
}

/** target artifact가 local executor에서 실행 가능한 파일인지 확인한다. */
int
qstar_target_has_executable_artifact(const struct qstar_target *target)
{
	const struct qstar_target_rule_info *rule;

	rule = qstar_target_rule_lookup(target->kind);
	return rule ? rule->executable_artifact : 0;
}

/** target artifact가 qstar install 대상인지 확인한다. */
int
qstar_target_is_installable(const struct qstar_target *target)
{
	const struct qstar_target_rule_info *rule;

	rule = qstar_target_rule_lookup(target->kind);
	return rule ? rule->installable_artifact : 0;
}
