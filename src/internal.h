#ifndef QSTAR_INTERNAL_H
#define QSTAR_INTERNAL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "qstar/qstar.h"

#include <stddef.h>
#include <stdio.h>

struct qstar_resolved_toolchain {
	char name[64];
	char target[128];
	char stdlib_policy[64];
	char cc[QSTAR_PATH_MAX];
	char cxx[QSTAR_PATH_MAX];
	char cale[QSTAR_PATH_MAX];
	char ar[QSTAR_PATH_MAX];
	char linker[QSTAR_PATH_MAX];
	char sysroot[QSTAR_PATH_MAX];
	char resource_dir[QSTAR_PATH_MAX];
	char resolver[64];
	int response_files;
	char response_style[32];
};

/** 문자열을 QStar 소유 메모리로 복사한다. */
char *qstar_strdup(const char *s);

/** 문자열 list에 새 항목을 복사해 추가한다. */
int qstar_string_list_push(struct qstar_string_list *list, const char *s);

/** 문자열 list가 소유한 모든 동적 메모리를 해제한다. */
void qstar_string_list_free(struct qstar_string_list *list);

/** Graph에 저장된 cached lowered action plan을 비운다. */
void qstar_graph_clear_cached_actions(struct qstar_graph *graph);

/** Graph의 cached lowered action plan에 새 action slot을 추가한다. */
struct qstar_cached_action *qstar_graph_add_cached_action(struct qstar_graph *graph);

/** Graph error buffer에 첫 오류만 기록한다. */
int qstar_set_error(struct qstar_graph *graph, const char *fmt, ...);

/** Graph error buffer에 origin metadata와 첫 오류를 함께 기록한다. */
int qstar_set_error_origin(struct qstar_graph *graph, const char *file, int line,
    const char *field, const char *label, const char *fmt, ...);

/** QStar lint diagnostic을 graph에 추가한다. */
int qstar_graph_add_lint(struct qstar_graph *graph, const char *code,
    const char *severity, const char *file, int line, const char *field,
    const char *label, const char *fmt, ...);

/** 현재 graph error buffer를 lint diagnostic으로 변환해 추가한다. */
int qstar_graph_add_lint_from_error(struct qstar_graph *graph);

/** 경로에서 package root로 쓸 dirname을 계산한다. */
int qstar_dirname(const char *path, char *dst, size_t dstlen);

/** 두 path 조각을 slash 기준으로 결합한다. */
int qstar_path_join(const char *a, const char *b, char *dst, size_t dstlen);

/** QStar path가 package-relative normalized path인지 검사한다. */
int qstar_path_is_package_relative(const char *path);

/** package-relative path 검증 실패 이유를 사용자-facing 문구로 반환한다. */
const char *qstar_path_package_relative_reason(const char *path);

/** external canonical label에서 package alias 부분을 추출한다. */
int qstar_label_package_alias(const char *label, char *dst, size_t dstlen);

/** canonical target label에서 local package path를 추출한다. */
int qstar_label_package_path(const char *label, char *dst, size_t dstlen);

/** target/profile 입력을 합쳐 host/clang/cale toolchain v1을 결정한다. */
int qstar_resolve_toolchain(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_resolved_toolchain *resolved);

/** target triple이 Windows 계열인지 보수적으로 판정한다. */
int qstar_toolchain_target_is_windows(const char *target);

/** target triple이 Darwin/macOS 계열인지 보수적으로 판정한다. */
int qstar_toolchain_target_is_darwin(const char *target);

/** target triple이 Linux 계열인지 보수적으로 판정한다. */
int qstar_toolchain_target_is_linux(const char *target);

/** target triple이 이번 sharedlib 구현에서 지원되는지 확인한다. */
int qstar_toolchain_target_supports_sharedlib(const char *target);

/** profile external tool policy로 custom_target 첫 argv를 실행 path로 해석한다. */
int qstar_profile_resolve_command_tool(const struct qstar_graph *graph, const char *tool,
    char *resolved, size_t resolved_len, char *mode, size_t mode_len, char *error,
    size_t error_len);

/** resolved tool mode가 package-local file input으로 action key에 들어가야 하는지 본다. */
int qstar_profile_tool_mode_is_package_input(const char *mode);

/** PATH에서 실행 tool을 찾고 발견한 절대 path를 반환한다. */
int qstar_profile_find_path_tool(const char *tool, char *dst, size_t dstlen);

/** build directory 아래 상대 path를 deterministic package-relative path로 만든다. */
int qstar_graph_build_path(const struct qstar_graph *graph, const char *subpath,
    char *dst, size_t dstlen);

/** target label을 build output directory 아래 파일명에 안전한 이름으로 바꾼다. */
void qstar_mangle_label(const char *label, char *dst, size_t dstlen);

/** compile object output path를 deterministic package-relative path로 만든다. */
int qstar_graph_object_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t index, char *dst, size_t dstlen);

/** compile depfile output path를 deterministic package-relative path로 만든다. */
int qstar_graph_depfile_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, size_t index, char *dst, size_t dstlen);

/** target artifact output path를 deterministic package-relative path로 만든다. */
int qstar_artifact_output_path(const struct qstar_target *target, char *dst, size_t dstlen);

/** profile/target artifact_name policy를 적용한 artifact output path를 만든다. */
int qstar_graph_artifact_output_path(const struct qstar_graph *graph,
    const struct qstar_target *target, char *dst, size_t dstlen);

/** generated action label로 action을 찾는다. */
const struct qstar_genrule *qstar_graph_find_genrule(const struct qstar_graph *graph,
    const char *label);

/** qstar.target_file placeholder token에서 canonical label을 추출한다. */
int qstar_target_file_token_label(const char *arg, char *label, size_t labellen);

/** stage/package rule label로 staging rule을 찾는다. */
const struct qstar_stage *qstar_graph_find_stage(const struct qstar_graph *graph,
    const char *label);

/** generated output metadata의 output group을 기본값 포함해 반환한다. */
const char *qstar_genrule_output_group(const struct qstar_genrule *genrule, size_t index);

/** generated output metadata의 format을 기본값 포함해 반환한다. */
const char *qstar_genrule_output_format(const struct qstar_genrule *genrule, size_t index);

/** generated output metadata의 address를 기본값 포함해 반환한다. */
const char *qstar_genrule_output_address(const struct qstar_genrule *genrule, size_t index);

/** generated output metadata의 layout을 기본값 포함해 반환한다. */
const char *qstar_genrule_output_layout(const struct qstar_genrule *genrule, size_t index);

/** generated output path와 format/address/layout metadata를 action identity로 만든다. */
int qstar_genrule_output_identity(const struct qstar_genrule *genrule, size_t index,
    char *dst, size_t dstlen);

/** generated output identity list를 action key material로 만든다. */
int qstar_genrule_output_identity_list(const struct qstar_genrule *genrule, char *dst,
    size_t dstlen);

/** target kind의 최종 action 이름을 rule registry 기준으로 반환한다. */
const char *qstar_target_final_action(const struct qstar_target *target);

/** target kind의 output group 이름을 rule registry 기준으로 반환한다. */
const char *qstar_target_output_group(const struct qstar_target *target);

/** target artifact가 local executor에서 실행 가능한 파일인지 확인한다. */
int qstar_target_has_executable_artifact(const struct qstar_target *target);

/** target artifact가 qstar install 대상인지 확인한다. */
int qstar_target_is_installable(const struct qstar_target *target);

/** compile action의 사용자-facing description을 만든다. */
int qstar_action_description_compile(const struct qstar_target *target,
    const struct qstar_source_info *source, const char *output, char *dst, size_t dstlen);

/** final archive/link action의 사용자-facing description을 만든다. */
int qstar_action_description_final(const struct qstar_target *target, const char *action,
    const char *artifact, char *dst, size_t dstlen);

/** generated action의 사용자-facing description을 만든다. */
int qstar_action_description_generate(const struct qstar_genrule *genrule, char *dst,
    size_t dstlen);

/** run_target action의 사용자-facing description을 만든다. */
int qstar_action_description_run(const struct qstar_target *target, char *dst, size_t dstlen);

/** stage action의 사용자-facing description을 만든다. */
int qstar_action_description_stage(const struct qstar_stage *stage, char *dst, size_t dstlen);

/** install action의 사용자-facing description을 만든다. */
int qstar_action_description_install(const char *artifact, char *dst, size_t dstlen);

/** qstar init template을 지정된 directory에 생성한다. */
int qstar_init_project(const char *template_name, const char *directory, FILE *out,
    char *error, size_t error_len);

/** qstar authoring file 하나를 simple canonical style로 format/check한다. */
int qstar_fmt_file(const char *path, int check, int stdout_mode, FILE *out);

/** stdio 기반 QStar Language Server Protocol v1 loop를 실행한다. */
int qstar_lsp_stdio(FILE *in, FILE *out);

/** QStar plan/executor 공용 target closure callback이다. */
typedef int (*qstar_target_visit_fn)(struct qstar_graph *graph, const struct qstar_target *target,
    size_t order, void *user);

/** dependency-first closure를 계산해 각 target callback을 순서대로 호출한다. */
int qstar_graph_visit_closure(struct qstar_graph *graph, const char *label,
    qstar_target_visit_fn visit, void *user);

/** Stella lowered plan cache를 읽어 Lua eval 없이 Graph IR를 복원한다. */
int qstar_stella_plan_cache_try_load(struct qstar_graph *graph, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, char *reason, size_t reason_len);

/** 검증된 Graph IR와 lowered action summary를 Stella plan cache로 저장한다. */
int qstar_stella_plan_cache_store(struct qstar_graph *graph, const char *file,
    const char *cmd, const char *label, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, char *reason, size_t reason_len);

/** 현재 Graph에서 실행 가능한 lowered action plan을 준비한다. */
int qstar_graph_prepare_lowered_action_cache(struct qstar_graph *graph, const char *label);

struct qstar_stella_state_cache;

/** Stella daemon이 유지하는 in-memory dirty/deps state cache를 생성한다. */
struct qstar_stella_state_cache *qstar_stella_state_cache_new(void);

/** Stella daemon in-memory dirty/deps state cache를 해제한다. */
void qstar_stella_state_cache_free(struct qstar_stella_state_cache *cache);

/** in-memory dirty/deps state cache를 사용해 Stella build action을 실행한다. */
int qstar_graph_build_with_state_cache(struct qstar_graph *graph, const char *label,
    const struct qstar_build_options *options, FILE *out,
    struct qstar_stella_state_cache *cache);

enum {
	QSTAR_DAEMON_NEVER = 0,
	QSTAR_DAEMON_AUTO = 1,
	QSTAR_DAEMON_ALWAYS = 2
};

/** CLI daemon mode 문자열을 experimental daemon policy로 변환한다. */
int qstar_daemon_parse_mode(const char *s, int *mode);

/** experimental persistent Stella daemon command를 실행한다. */
int qstar_daemon_command(int argc, char **argv, const char *file,
    const char *cli_build_dir, const char *cli_profile, const char *cli_target,
    const char *cli_toolchain, const char *cli_stdlib, FILE *out);

/** build request를 experimental daemon으로 보내고 응답 output을 out에 복사한다. */
int qstar_daemon_build_client(const char *socket_path, int mode, const char *file,
    const char *label, const char *cli_build_dir, const char *cli_profile,
    const char *cli_target, const char *cli_toolchain, const char *cli_stdlib,
    const struct qstar_build_options *options, FILE *out, int *build_status,
    char *error, size_t error_len);

#endif
