#ifndef QSTAR_QSTAR_H
#define QSTAR_QSTAR_H

#include <stddef.h>
#include <stdio.h>

#ifndef QSTAR_PATH_MAX
#define QSTAR_PATH_MAX 4096
#endif

#define QSTAR_VERSION "0.4.0-beta.1"
#define QSTAR_VERSION_MAJOR 0
#define QSTAR_VERSION_MINOR 4
#define QSTAR_VERSION_PATCH 0

struct qstar_string_list {
	char **items;
	size_t len;
	size_t cap;
};

struct qstar_modules {
	int present;
	char *root;
	struct qstar_string_list include;
	struct qstar_string_list exclude;
};

struct qstar_target {
	char *label;
	char *name;
	char *kind;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	struct qstar_modules modules;
	struct qstar_string_list configs;
	struct qstar_string_list sources;
	struct qstar_string_list public_headers;
	struct qstar_string_list private_headers;
	struct qstar_string_list include_dirs;
	struct qstar_string_list public_include_dirs;
	struct qstar_string_list private_include_dirs;
	struct qstar_string_list interface_include_dirs;
	struct qstar_string_list system_include_dirs;
	struct qstar_string_list deps;
	struct qstar_string_list private_deps;
	struct qstar_string_list visibility;
	struct qstar_string_list libs;
	struct qstar_string_list lib_dirs;
	struct qstar_string_list frameworks;
	struct qstar_string_list link_options;
	struct qstar_string_list defsyms;
	struct qstar_string_list cflags;
	struct qstar_string_list cxxflags;
	struct qstar_string_list asm_include_dirs;
	struct qstar_string_list asm_compile_options;
	struct qstar_string_list cale_compile_options;
	struct qstar_string_list run_command;
	char *artifact_name;
	char *cxx_standard;
	char *cale_profile;
	char *linker_script;
	char *run_marker;
	char *run_marker_log;
	int run_timeout_sec;
	int asm_preprocess;
	int cxx_modules_present;
	int cxx_modules_enabled;
	char *toolchain;
	char *stdlib_policy;
};

struct qstar_config {
	char *label;
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	struct qstar_target options;
	int has_artifact_name;
	int has_cxx_standard;
	int has_cale_profile;
	int has_linker_script;
	int has_asm_preprocess;
	int has_cxx_modules;
	int has_toolchain;
	int has_stdlib_policy;
};

struct qstar_genrule {
	char *label;
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	char *tool;
	int config_header;
	struct qstar_string_list inputs;
	struct qstar_string_list outputs;
	struct qstar_string_list output_groups;
	struct qstar_string_list output_formats;
	struct qstar_string_list output_addresses;
	struct qstar_string_list output_layouts;
	struct qstar_string_list args;
	struct qstar_string_list command;
};

struct qstar_stage {
	char *label;
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	char *root;
	struct qstar_string_list srcs;
	struct qstar_string_list dsts;
};

struct qstar_target_family {
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	int allow_shared_sources;
	struct qstar_string_list variants;
	struct qstar_string_list targets;
};

struct qstar_project {
	int present;
	char *name;
	char *version;
	char *root;
	char *build_dir;
	char *generated_dir;
	char *compile_commands;
};

struct qstar_package_alias {
	char *alias;
	char *root;
};

struct qstar_profile_input {
	char *name;
	char *target;
	char *toolchain;
	char *stdlib_policy;
	char *freestanding;
	char *arch;
	char *cpu;
	char *abi;
	char *cc;
	char *cxx;
	char *cale;
	char *ar;
	char *linker;
	char *sysroot;
	char *resource_dir;
	char *response_files;
	char *response_style;
	char *linker_script;
	struct qstar_string_list artifact_names;
	char *allow_absolute_tools;
	struct qstar_string_list compile_options;
	struct qstar_string_list include_dirs;
	struct qstar_string_list lib_dirs;
	struct qstar_string_list link_options;
	struct qstar_string_list defsyms;
	struct qstar_string_list path_tools;
	struct qstar_string_list tool_overrides;
};

struct qstar_profile_decl {
	char *name;
	char *extends;
	char *origin_file;
	int origin_line;
	struct qstar_profile_input input;
};

struct qstar_lint_diagnostic {
	char *code;
	char *severity;
	char *file;
	char *field;
	char *label;
	char *message;
	int line;
};

struct qstar_source_info {
	const char *path;
	const char *language;
	const char *tool_role;
	const char *provider;
	const char *output_group;
	int compile_input;
	int header_input;
};

struct qstar_target_rule_info {
	const char *kind;
	const char *provider;
	const char *final_action;
	const char *output_group;
	const char *artifact_prefix;
	const char *artifact_suffix;
	int executable_artifact;
	int installable_artifact;
	int local_executor_supported;
};

struct qstar_graph {
	struct qstar_target *targets;
	size_t len;
	size_t cap;
	struct qstar_config *configs;
	size_t config_len;
	size_t config_cap;
	char *package_root;
	struct qstar_package_alias *packages;
	size_t package_len;
	size_t package_cap;
	struct qstar_genrule *genrules;
	size_t genrule_len;
	size_t genrule_cap;
	struct qstar_stage *stages;
	size_t stage_len;
	size_t stage_cap;
	struct qstar_target_family *families;
	size_t family_len;
	size_t family_cap;
	struct qstar_lint_diagnostic *lint_diagnostics;
	size_t lint_len;
	size_t lint_cap;
	struct qstar_string_list evaluated_fragments;
	struct qstar_project project;
	char *generator;
	char *requested_generator;
	char *build_dir_override;
	struct qstar_profile_input profile;
	struct qstar_profile_decl *profile_decls;
	size_t profile_decl_len;
	size_t profile_decl_cap;
	char error[512];
	char error_file[QSTAR_PATH_MAX];
	char error_field[64];
	char error_label[QSTAR_PATH_MAX];
	int error_line;
};

struct qstar_build_options {
	int explain_cache;
	int jobs;
	int schedule_trace;
	int verbose;
	int quiet;
	int progress_mode;
	int color_mode;
};

enum {
	QSTAR_PROGRESS_AUTO = 0,
	QSTAR_PROGRESS_PLAIN = 1,
	QSTAR_PROGRESS_OFF = 2
};

enum {
	QSTAR_COLOR_AUTO = 0,
	QSTAR_COLOR_ALWAYS = 1,
	QSTAR_COLOR_NEVER = 2
};

struct qstar_install_options {
	const char *prefix;
	int dry_run;
};

struct qstar_stage_options {
	const char *root;
	int dry_run;
};

/** QStar graph 저장소를 빈 상태로 초기화한다. */
void qstar_graph_init(struct qstar_graph *graph);

/** QStar graph가 소유한 모든 동적 메모리를 해제한다. */
void qstar_graph_free(struct qstar_graph *graph);

/** QStar package root를 graph에 기록한다. */
int qstar_graph_set_package_root(struct qstar_graph *graph, const char *root);

/** qstar.project metadata를 graph에 기록한다. */
int qstar_graph_set_project(struct qstar_graph *graph, const char *name,
    const char *version, const char *root, const char *build_dir,
    const char *generated_dir, const char *compile_commands);

/** CLI generator/build directory override를 graph effective option으로 기록한다. */
int qstar_graph_set_cli_overrides(struct qstar_graph *graph, const char *generator,
    const char *build_dir);

/** QStar project의 effective build directory를 반환한다. */
const char *qstar_graph_build_dir(const struct qstar_graph *graph);

/** QStar project의 effective generated output directory를 반환한다. */
const char *qstar_graph_generated_dir(const struct qstar_graph *graph);

/** path가 현재 project의 generated output root 아래 있는지 검사한다. */
int qstar_graph_path_is_generated(const struct qstar_graph *graph, const char *path);

/** QStar project의 effective generator를 반환한다. */
const char *qstar_graph_generator(const struct qstar_graph *graph);

/** CLI가 요청한 generator 값을 반환한다. */
const char *qstar_graph_requested_generator(const struct qstar_graph *graph);

/** QStar project의 effective compile_commands policy를 반환한다. */
const char *qstar_graph_compile_commands_policy(const struct qstar_graph *graph);

/** QStar graph에 새 target을 추가하고 중복 label을 stable error로 막는다. */
struct qstar_target *qstar_graph_add_target(struct qstar_graph *graph, const char *label,
    const char *name, const char *kind, const char *fragment_dir, const char *origin_file,
    int origin_line);

/** QStar reusable config declaration을 graph에 추가한다. */
struct qstar_config *qstar_graph_add_config(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir, const char *origin_file, int origin_line);

/** QStar target에 선언된 configs list를 target option field로 병합한다. */
int qstar_graph_apply_target_configs(struct qstar_graph *graph, struct qstar_target *target);

/** QStar graph에 generated action skeleton을 추가하고 중복 label을 막는다. */
struct qstar_genrule *qstar_graph_add_genrule(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir, const char *origin_file, int origin_line);

/** QStar graph에 copy-only staging rule을 추가하고 중복 label을 막는다. */
struct qstar_stage *qstar_graph_add_stage(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir, const char *origin_file, int origin_line);

/** QStar target family lint grouping primitive를 추가한다. */
struct qstar_target_family *qstar_graph_add_target_family(struct qstar_graph *graph,
    const char *name, const char *fragment_dir, const char *origin_file, int origin_line);

/** QStar package alias를 추가하고 중복 alias를 stable error로 막는다. */
int qstar_graph_add_package_alias(struct qstar_graph *graph, const char *alias, const char *root);

/** QStar explain profile 입력을 graph에 기록한다. */
int qstar_graph_set_profile_input(struct qstar_graph *graph, const char *name,
    const char *target, const char *toolchain, const char *stdlib_policy);

/** qstar.profile DSL 선언을 graph에 저장한다. */
int qstar_graph_add_profile_decl(struct qstar_graph *graph, const char *name,
    const char *extends, const char *origin_file, int origin_line,
    const struct qstar_profile_input *input);

/** 선택된 qstar.profile 선언과 extends chain을 active profile에 적용한다. */
int qstar_graph_apply_selected_profile(struct qstar_graph *graph);

/** QStar package alias map에서 alias를 찾는다. */
const struct qstar_package_alias *qstar_graph_find_package_alias(const struct qstar_graph *graph,
    const char *alias);

/** QStar in-DSL profile schema 입력을 검증한다. */
int qstar_graph_validate_profile(struct qstar_graph *graph);

/** QStar header file graph policy를 검증한다. */
int qstar_graph_validate_headers(struct qstar_graph *graph);

/** QStar source path와 language classification을 검증한다. */
int qstar_graph_validate_sources(struct qstar_graph *graph);

/** QStar generated output edge skeleton을 검증한다. */
int qstar_graph_validate_generated_outputs(struct qstar_graph *graph);

/** QStar workspace/package ownership과 visibility boundary를 검증한다. */
int qstar_graph_validate_packages(struct qstar_graph *graph);

/** QStar authoring input file이 package root 아래 실제로 존재하는지 검증한다. */
int qstar_graph_validate_file_inputs(struct qstar_graph *graph);

/** generated output path를 생산하는 action skeleton을 찾는다. */
const struct qstar_genrule *qstar_graph_find_output_owner(const struct qstar_graph *graph,
    const char *path);

/** QStar source path를 language/tool role로 분류한다. */
int qstar_source_classify(const char *path, struct qstar_source_info *info);

/** QStar source kind registry에서 path suffix에 맞는 항목을 찾는다. */
const struct qstar_source_info *qstar_source_kind_lookup_path(const char *path);

/** QStar target rule registry에서 target kind에 맞는 rule을 찾는다. */
const struct qstar_target_rule_info *qstar_target_rule_lookup(const char *kind);

/** QStar target의 header file plan skeleton을 출력한다. */
void qstar_target_dump_header_files(const struct qstar_target *target, FILE *out);

/** QStar target의 source discovery skeleton을 출력한다. */
void qstar_target_dump_source_discovery(const struct qstar_target *target, FILE *out);

/** QStar label을 현재 fragment 기준 canonical label로 정규화한다. */
int qstar_label_canonicalize(const char *label, const char *fragment_dir, char *dst, size_t dstlen);

/** QStar Graph IR를 deterministic explain text로 출력한다. */
int qstar_graph_dump(const struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target 목록을 deterministic text로 출력한다. */
int qstar_graph_list_targets(const struct qstar_graph *graph, FILE *out);

/** QStar target/generated action 목록을 machine-readable JSON으로 출력한다. */
int qstar_graph_list_targets_json(const struct qstar_graph *graph, FILE *out);

/** QStar target 하나를 authoring query text로 출력한다. */
int qstar_graph_query(const struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target closure와 non-executing command plan을 deterministic text로 출력한다. */
int qstar_graph_explain_plan(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar target closure를 실제 실행 없이 dry-run command stream으로 출력한다. */
int qstar_graph_dry_run(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar local executor로 제한된 build action을 실행한다. */
int qstar_graph_build(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar local executor에 cache/job/trace option을 적용해 build action을 실행한다. */
int qstar_graph_build_with_options(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out);

/** QStar graph를 Ninja build file과 compile database로 lower한다. */
int qstar_graph_emit_ninja(struct qstar_graph *graph, const char *label, FILE *out);

/** Ninja backend로 build.ninja를 emit한 뒤 requested target을 빌드한다. */
int qstar_graph_build_ninja(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out);

/** Ninja backend로 test artifact를 build한 뒤 제한된 runner로 실행한다. */
int qstar_graph_test_ninja(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar action cache 기준으로 rebuild 이유를 설명한다. */
int qstar_graph_why_rebuild(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar local build output을 전체 또는 target 단위로 지운다. */
int qstar_graph_clean(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar test target을 build한 뒤 제한된 runner로 실행한다. */
int qstar_graph_test(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar installable artifact와 public header를 prefix 아래 배치한다. */
int qstar_graph_install(struct qstar_graph *graph, const char *label,
    const struct qstar_install_options *options, FILE *out);

/** QStar boot/package staging rule을 package-local root 아래 copy-only로 실행한다. */
int qstar_graph_stage(struct qstar_graph *graph, const char *label,
    const struct qstar_stage_options *options, FILE *out);

/** QStar action log path 목록을 target 기준으로 출력한다. */
int qstar_graph_log(struct qstar_graph *graph, const char *label, FILE *out);

/** 마지막 실패 replay 파일을 출력한다. */
int qstar_graph_last_failure(struct qstar_graph *graph, FILE *out);

/** action id에 대응하는 deterministic action log를 출력한다. */
int qstar_graph_action_log(struct qstar_graph *graph, const char *action_id, FILE *out);

/** action id에 대응하는 shell replay command를 출력한다. */
int qstar_graph_replay_action(struct qstar_graph *graph, const char *action_id, FILE *out);

/** QStar authoring check 결과를 deterministic text로 출력한다. */
int qstar_graph_check(struct qstar_graph *graph, const char *label, FILE *out);

/** QStar lint diagnostic을 text 또는 LSP-ready JSON으로 출력한다. */
int qstar_graph_lint(struct qstar_graph *graph, const char *label, const char *format, FILE *out);

/** QStar lint diagnostic을 color 정책과 함께 출력한다. */
int qstar_graph_lint_with_color(struct qstar_graph *graph, const char *label,
    const char *format, int color_mode, FILE *out);

/** QStar 전체 package doctor 결과를 deterministic text로 출력한다. */
int qstar_graph_doctor(struct qstar_graph *graph, FILE *out);

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int qstar_lua_eval_file(struct qstar_graph *graph, const char *file);

#endif
