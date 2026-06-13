#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
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

/** source language를 progress description용 짧은 이름으로 변환한다. */
static const char *
description_compile_language(const struct qstar_source_info *source)
{
	if (!source)
		return "";
	if (strcmp(source->language, "c") == 0)
		return "C";
	if (strcmp(source->language, "cxx") == 0 ||
	    strcmp(source->language, "cxx-module") == 0)
		return "CXX";
	if (strcmp(source->language, "asm") == 0 ||
	    strcmp(source->language, "asm-cpp") == 0)
		return "ASM";
	if (strcmp(source->language, "cale") == 0)
		return "Cale";
	return "";
}

/** target source list에 C++ 계열 입력이 있는지 확인한다. */
static int
description_target_uses_cxx(const struct qstar_target *target)
{
	struct qstar_source_info source;
	size_t i;

	if (!target)
		return 0;
	for (i = 0; i < target->sources.len; i++) {
		if (qstar_source_classify(target->sources.items[i], &source) == 0 &&
		    (strcmp(source.language, "cxx") == 0 ||
		    strcmp(source.language, "cxx-module") == 0))
			return 1;
	}
	return 0;
}

/** printf style 결과를 QStar style 성공/실패로 변환한다. */
static int
description_format(char *dst, size_t dstlen, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!dst || dstlen == 0)
		return -1;
	va_start(ap, fmt);
	n = vsnprintf(dst, dstlen, fmt, ap);
	va_end(ap);
	return n >= 0 && (size_t)n < dstlen ? 0 : -1;
}

/** compile action의 사용자-facing description을 만든다. */
int
qstar_action_description_compile(const struct qstar_target *target,
    const struct qstar_source_info *source, const char *output, char *dst, size_t dstlen)
{
	const char *language;

	(void)target;
	language = description_compile_language(source);
	if (language && *language)
		return description_format(dst, dstlen, "Building %s object %s",
		    language, output ? output : "<object>");
	return description_format(dst, dstlen, "Building object %s",
	    output ? output : "<object>");
}

/** final archive/link action의 사용자-facing description을 만든다. */
int
qstar_action_description_final(const struct qstar_target *target, const char *action,
    const char *artifact, char *dst, size_t dstlen)
{
	const char *language;
	const char *noun;

	language = description_target_uses_cxx(target) ? "CXX" : "C";
	if (action && strcmp(action, "archive") == 0)
		noun = "static library";
	else if (action && strcmp(action, "link-shared") == 0)
		noun = "shared library";
	else if (action && strcmp(action, "compile-objects") == 0)
		return description_format(dst, dstlen, "Collecting objects %s",
		    artifact ? artifact : "<artifact>");
	else
		noun = "executable";
	return description_format(dst, dstlen, "Linking %s %s %s",
	    language, noun, artifact ? artifact : "<artifact>");
}

/** generated action의 사용자-facing description을 만든다. */
int
qstar_action_description_generate(const struct qstar_genrule *genrule, char *dst,
    size_t dstlen)
{
	const char *output;

	output = genrule && genrule->outputs.len > 0 ? genrule->outputs.items[0] :
	    genrule && genrule->label ? genrule->label : "<generated>";
	if (genrule && genrule->config_header)
		return description_format(dst, dstlen, "Configuring %s", output);
	return description_format(dst, dstlen, "Generating %s", output);
}

/** run_target action의 사용자-facing description을 만든다. */
int
qstar_action_description_run(const struct qstar_target *target, char *dst, size_t dstlen)
{
	return description_format(dst, dstlen, "Running %s",
	    target && target->label ? target->label : "<run-target>");
}

/** stage action의 사용자-facing description을 만든다. */
int
qstar_action_description_stage(const struct qstar_stage *stage, char *dst, size_t dstlen)
{
	return description_format(dst, dstlen, "Staging %s",
	    stage && stage->label ? stage->label : "<stage>");
}

/** install action의 사용자-facing description을 만든다. */
int
qstar_action_description_install(const char *artifact, char *dst, size_t dstlen)
{
	return description_format(dst, dstlen, "Installing %s",
	    artifact && *artifact ? artifact : "<artifact>");
}
