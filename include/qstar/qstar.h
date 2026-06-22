#ifndef QSTAR_QSTAR_H
#define QSTAR_QSTAR_H

#include <stddef.h>
#include <stdio.h>

#ifndef QSTAR_PATH_MAX
#define QSTAR_PATH_MAX 4096
#endif

#define QSTAR_VERSION "0.7.1-beta"
#define QSTAR_VERSION_MAJOR 0
#define QSTAR_VERSION_MINOR 7
#define QSTAR_VERSION_PATCH 1

struct qstar_string_list {
	char **items;
	size_t len;
	size_t cap;
};

struct qstar_provider_option_value {
	char *provider;
	char *name;
	char *type;
	char *value;
	struct qstar_string_list list;
};

struct qstar_provider_action_template {
	struct qstar_string_list argv;
	struct qstar_string_list env;
	struct qstar_string_list inputs;
	struct qstar_string_list outputs;
	char *depfile;
	int wants_depfile;
};

struct qstar_modules {
	int present;
	char *root;
	struct qstar_string_list include;
	struct qstar_string_list exclude;
};

struct qstar_provider_source_unit {
	size_t source_index;
	char *path;
	char *provider;
	char *unit;
	char *emits;
	char *lower;
	char *toolset_role;
	struct qstar_provider_action_template action;
};

struct qstar_provider_final_action {
	int present;
	char *provider;
	char *kind;
	char *lower;
	struct qstar_provider_action_template action;
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
	struct qstar_provider_source_unit *provider_sources;
	size_t provider_source_len;
	size_t provider_source_cap;
	struct qstar_provider_final_action provider_final;
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
	struct qstar_string_list link_inputs;
	struct qstar_string_list cflags;
	struct qstar_string_list cxxflags;
	struct qstar_string_list asm_include_dirs;
	struct qstar_string_list asm_compile_options;
	struct qstar_provider_option_value *provider_options;
	size_t provider_option_len;
	size_t provider_option_cap;
	struct qstar_string_list run_inputs;
	struct qstar_string_list run_command;
	char *description;
	char *artifact_name;
	char *cxx_standard;
	char *run_expect_contains;
	char *run_expect_file;
	int run_timeout_sec;
	int asm_preprocess;
	int cxx_modules_present;
	int cxx_modules_enabled;
	char *toolset;
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
	int has_asm_preprocess;
	int has_cxx_modules;
	int has_toolset;
	int has_toolchain;
	int has_stdlib_policy;
};

struct qstar_tool_role {
	char *role;
	struct qstar_string_list argv;
};

struct qstar_toolset {
	char *label;
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	struct qstar_tool_role *roles;
	size_t role_len;
	size_t role_cap;
	struct qstar_string_list path_tools;
	char *response_files;
	char *response_style;
	char *allow_absolute_tools;
};

struct qstar_language_option_schema {
	char *name;
	char *type;
	int has_default;
	char *default_value;
	struct qstar_string_list default_list;
	struct qstar_string_list values;
};

struct qstar_language_unit_schema {
	char *name;
	struct qstar_string_list suffixes;
	char *emits;
	char *lower;
	char *deps;
};

struct qstar_language_final_schema {
	char *kind;
	char *lower;
};

struct qstar_language_provider {
	char *api;
	char *id;
	char *namespace;
	char *version;
	char *dir;
	char *manifest;
	char *implementation;
	struct qstar_language_option_schema *options;
	size_t option_len;
	size_t option_cap;
	struct qstar_language_unit_schema *units;
	size_t unit_len;
	size_t unit_cap;
	struct qstar_language_final_schema *finals;
	size_t final_len;
	size_t final_cap;
};

struct qstar_genrule {
	char *label;
	char *name;
	char *fragment_dir;
	char *origin_file;
	int origin_line;
	char *tool;
	char *description;
	int config_header;
	struct qstar_string_list inputs;
	struct qstar_string_list outputs;
	struct qstar_string_list output_groups;
	struct qstar_string_list output_formats;
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
	char *description;
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

struct qstar_command_option {
	char *name;
	char *type;
	char *description;
	char *default_value;
	int required;
	int has_default;
	struct qstar_string_list choices;
};

struct qstar_command_step {
	char *kind;
	char *label;
	char *command;
	char *stage_root;
	char *when;
	char *working_dir;
	char *description;
	char *run_expect_contains;
	char *run_expect_file;
	char *export_to;
	struct qstar_string_list run_command;
	struct qstar_string_list inputs;
	struct qstar_string_list env;
	int stage_dry_run;
	int timeout_sec;
};

struct qstar_project_command {
	char *name;
	char *origin_file;
	int origin_line;
	char *description;
	char *working_dir;
	struct qstar_string_list env;
	struct qstar_string_list aliases;
	struct qstar_command_option *options;
	size_t option_len;
	size_t option_cap;
	struct qstar_command_step *steps;
	size_t step_len;
	size_t step_cap;
	int is_default;
	int hidden;
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

struct qstar_build_context {
	char *name;
	char *target;
	char *platform;
	char *toolchain;
	char *stdlib_policy;
	char *cc;
	char *cxx;
	char *ar;
	char *linker;
	char *sysroot;
	char *resource_dir;
	char *response_files;
	char *response_style;
	struct qstar_string_list artifact_names;
	char *allow_absolute_tools;
	struct qstar_string_list compile_options;
	struct qstar_string_list include_dirs;
	struct qstar_string_list lib_dirs;
	struct qstar_string_list link_options;
	struct qstar_string_list path_tools;
	struct qstar_string_list tool_overrides;
};

struct qstar_cached_action {
	char *id;
	char *kind;
	char *target_label;
	char *description;
	char *depfile;
	char *source_path;
	size_t source_index;
	int wants_depfile;
	struct qstar_string_list argv;
	struct qstar_string_list env;
	struct qstar_string_list outputs;
	struct qstar_string_list inputs;
	struct qstar_string_list depfile_inputs;
};

struct qstar_target_artifact {
	char id[64];
	char role[64];
	char path[QSTAR_PATH_MAX];
	char install_dir[32];
	int primary;
	int installable;
};

struct qstar_target_artifact_map {
	struct qstar_target_artifact items[4];
	size_t len;
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

struct qstar_language_provider_info {
	const char *namespace;
	const char *display_name;
	const char *compiler_role;
	int preloaded;
};

struct qstar_source_info {
	const char *path;
	const char *language;
	const char *tool_role;
	const char *provider;
	const char *provider_role;
	const char *toolset_role;
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
	struct qstar_toolset *toolsets;
	size_t toolset_len;
	size_t toolset_cap;
	struct qstar_language_provider *language_providers;
	size_t language_provider_len;
	size_t language_provider_cap;
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
	struct qstar_project_command *commands;
	size_t command_len;
	size_t command_cap;
	struct qstar_lint_diagnostic *lint_diagnostics;
	size_t lint_len;
	size_t lint_cap;
	struct qstar_string_list evaluated_fragments;
	struct qstar_project project;
	char *generator;
	char *requested_generator;
	char *build_dir_override;
	struct qstar_build_context build_context;
	struct qstar_cached_action *cached_actions;
	size_t cached_action_len;
	size_t cached_action_cap;
	int cached_action_plan_loaded;
	char error[512];
	char error_file[QSTAR_PATH_MAX];
	char error_field[64];
	char error_label[QSTAR_PATH_MAX];
	int error_line;
	int uses_file_globs;
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

struct qstar_stage_options {
	const char *root;
	int dry_run;
	int dependencies_ready;
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

/** QStar toolset declaration을 graph에 추가한다. */
struct qstar_toolset *qstar_graph_add_toolset(struct qstar_graph *graph, const char *label,
    const char *name, const char *fragment_dir, const char *origin_file, int origin_line);

/** Project-local language provider activation을 graph registry에 추가한다. */
struct qstar_language_provider *qstar_graph_add_language_provider(struct qstar_graph *graph,
    const char *api, const char *id, const char *namespace, const char *version,
    const char *dir, const char *manifest, const char *implementation);

/** Activated language provider에 option schema를 추가한다. */
int qstar_language_provider_add_option_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *name, const char *type,
    const struct qstar_string_list *values, int has_default, const char *default_value,
    const struct qstar_string_list *default_list);

/** Activated language provider에 source unit schema를 추가한다. */
int qstar_language_provider_add_unit_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *name,
    const struct qstar_string_list *suffixes, const char *emits, const char *lower,
    const char *deps);

/** Activated language provider에 final artifact schema를 추가한다. */
int qstar_language_provider_add_final_schema(struct qstar_graph *graph,
    struct qstar_language_provider *provider, const char *kind, const char *lower);

/** QStar target source list entry에 provider source unit metadata를 붙인다. */
int qstar_target_add_provider_source_unit(struct qstar_graph *graph,
    struct qstar_target *target, size_t source_index, const char *path,
    const char *provider, const char *unit, const char *emits, const char *lower,
    const struct qstar_provider_action_template *action);

/** QStar target에 provider-owned final artifact action metadata를 붙인다. */
int qstar_target_set_provider_final_action(struct qstar_graph *graph,
    struct qstar_target *target, const char *provider, const char *kind,
    const char *lower, const struct qstar_provider_action_template *action);

int qstar_target_set_provider_option(struct qstar_graph *graph,
    struct qstar_target *target, const char *provider, const char *name,
    const char *type, const char *value, const struct qstar_string_list *list);

const struct qstar_provider_option_value *qstar_target_provider_option(
    const struct qstar_target *target, const char *provider, const char *name);

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

/** Root project command primitive를 추가한다. */
struct qstar_project_command *qstar_graph_add_project_command(struct qstar_graph *graph,
    const char *name, const char *origin_file, int origin_line);

/** Project command에 typed option schema를 추가한다. */
struct qstar_command_option *qstar_project_command_add_option(struct qstar_graph *graph,
    struct qstar_project_command *command, const char *name, const char *type);

/** Project command에 ordered step을 추가한다. */
struct qstar_command_step *qstar_project_command_add_step(struct qstar_graph *graph,
    struct qstar_project_command *command, const char *kind);

/** QStar package alias를 추가하고 중복 alias를 stable error로 막는다. */
int qstar_graph_add_package_alias(struct qstar_graph *graph, const char *alias, const char *root);

/** CLI/internal target/toolchain override를 graph의 임시 build context에 기록한다. */
int qstar_graph_set_build_context_input(struct qstar_graph *graph, const char *name,
    const char *target, const char *toolchain, const char *stdlib_policy);
int qstar_graph_set_platform_context(struct qstar_graph *graph, const char *platform);
const char *qstar_graph_platform(const struct qstar_graph *graph);

/** QStar package alias map에서 alias를 찾는다. */
const struct qstar_package_alias *qstar_graph_find_package_alias(const struct qstar_graph *graph,
    const char *alias);

/** CLI/internal target/toolchain override와 build context를 검증한다. */
int qstar_graph_validate_build_context(struct qstar_graph *graph);

/** QStar toolset 참조가 선언된 toolset label을 가리키는지 검증한다. */
int qstar_graph_validate_toolsets(struct qstar_graph *graph);

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

/** Built-in/preloaded language provider namespace metadata를 조회한다. */
const struct qstar_language_provider_info *qstar_language_provider_lookup(
    const char *namespace);

/** language provider namespace가 public lang table에서 preloaded되어 있는지 확인한다. */
int qstar_language_provider_is_preloaded(const char *namespace);

/** graph-local language provider namespace가 qstar.use_language로 활성화됐는지 확인한다. */
const struct qstar_language_provider *qstar_graph_find_language_provider(
    const struct qstar_graph *graph, const char *namespace);

/** qstar.use_language manifest path가 이미 활성화됐는지 확인한다. */
const struct qstar_language_provider *qstar_graph_find_language_provider_manifest(
    const struct qstar_graph *graph, const char *manifest);

/** Activated language provider에서 option schema를 찾는다. */
const struct qstar_language_option_schema *qstar_language_provider_find_option(
    const struct qstar_language_provider *provider, const char *name);

/** Activated language provider에서 source unit schema를 찾는다. */
const struct qstar_language_unit_schema *qstar_language_provider_find_unit(
    const struct qstar_language_provider *provider, const char *name);

/** Activated language provider에서 final artifact schema를 찾는다. */
const struct qstar_language_final_schema *qstar_language_provider_find_final(
    const struct qstar_language_provider *provider, const char *kind);

/** public lang table에서 preloaded 또는 graph-local activated namespace인지 확인한다. */
int qstar_graph_language_provider_is_available(const struct qstar_graph *graph,
    const char *namespace);

/** target이 provider-owned final artifact action을 갖는지 확인한다. */
int qstar_target_has_provider_final_action(const struct qstar_target *target);

/** target source index에 붙은 provider source unit metadata를 찾는다. */
const struct qstar_provider_source_unit *qstar_target_provider_source_unit(
    const struct qstar_target *target, size_t source_index);

/** target source index를 built-in suffix 또는 provider source unit 기준으로 분류한다. */
int qstar_target_source_classify(const struct qstar_target *target, size_t source_index,
    struct qstar_source_info *info);

/** source kind가 compile action을 요구하는지 확인한다. */
int qstar_source_requires_compile(const struct qstar_source_info *source);

/** source kind가 이미 만들어진 object artifact인지 확인한다. */
int qstar_source_is_link_object(const struct qstar_source_info *source);

/** source kind가 assembler provider에 속하는지 확인한다. */
int qstar_source_is_asm(const struct qstar_source_info *source);

/** source kind가 ASM preprocess mode를 사용해야 하는지 확인한다. */
int qstar_source_uses_asm_preprocessor(const struct qstar_target *target,
    const struct qstar_source_info *source);

/** source kind가 unsupported C++ module provider source인지 확인한다. */
int qstar_source_is_cxx_module(const struct qstar_source_info *source);

/** target source list에 특정 compile provider namespace가 있는지 확인한다. */
int qstar_target_has_compile_provider(const struct qstar_target *target,
    const char *provider);

/** source kind의 provider toolset role name을 반환한다. */
const char *qstar_source_toolset_role(const struct qstar_source_info *source);

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

/** QStar root project command 목록을 deterministic text로 출력한다. */
int qstar_graph_list_project_commands(const struct qstar_graph *graph, FILE *out);

/** QStar root project command 목록을 machine-readable JSON으로 출력한다. */
int qstar_graph_list_project_commands_json(const struct qstar_graph *graph, FILE *out);

/** QStar root project command declarations를 검증한다. */
int qstar_graph_validate_project_commands(struct qstar_graph *graph);

/** QStar project command name 또는 alias를 찾는다. */
const struct qstar_project_command *qstar_graph_find_project_command(
    const struct qstar_graph *graph, const char *name);

/** is_default command를 찾는다. */
const struct qstar_project_command *qstar_graph_default_project_command(
    const struct qstar_graph *graph);

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

/** Root project command를 실행한다. */
int qstar_graph_run_project_command(struct qstar_graph *graph, const char *name,
    int argc, char **argv, const struct qstar_build_options *options, FILE *out);

/** QStar 전체 package doctor 결과를 deterministic text로 출력한다. */
int qstar_graph_doctor(struct qstar_graph *graph, FILE *out);

/** qstar.lua 파일을 sandboxed Lua runtime으로 평가해 Graph IR를 만든다. */
int qstar_lua_eval_file(struct qstar_graph *graph, const char *file);

#endif
