#ifndef QSTAR_INTERNAL_H
#define QSTAR_INTERNAL_H

#include "qstar/qstar.h"

#include <stddef.h>
#include <stdio.h>

struct qstar_resolved_toolchain {
	char name[64];
	char target[128];
	char stdlib_policy[64];
	char cc[QSTAR_PATH_MAX];
	char cale[QSTAR_PATH_MAX];
	char ar[QSTAR_PATH_MAX];
	char linker[QSTAR_PATH_MAX];
	char resolver[64];
};

/** 문자열을 QStar 소유 메모리로 복사한다. */
char *qstar_strdup(const char *s);

/** 문자열 list에 새 항목을 복사해 추가한다. */
int qstar_string_list_push(struct qstar_string_list *list, const char *s);

/** 문자열 list가 소유한 모든 동적 메모리를 해제한다. */
void qstar_string_list_free(struct qstar_string_list *list);

/** Graph error buffer에 첫 오류만 기록한다. */
int qstar_set_error(struct qstar_graph *graph, const char *fmt, ...);

/** Graph error buffer에 origin metadata와 첫 오류를 함께 기록한다. */
int qstar_set_error_origin(struct qstar_graph *graph, const char *file, int line,
    const char *field, const char *label, const char *fmt, ...);

/** 경로에서 package root로 쓸 dirname을 계산한다. */
int qstar_dirname(const char *path, char *dst, size_t dstlen);

/** 두 path 조각을 slash 기준으로 결합한다. */
int qstar_path_join(const char *a, const char *b, char *dst, size_t dstlen);

/** QStar path가 package-relative normalized path인지 검사한다. */
int qstar_path_is_package_relative(const char *path);

/** external canonical label에서 package alias 부분을 추출한다. */
int qstar_label_package_alias(const char *label, char *dst, size_t dstlen);

/** target/profile 입력을 합쳐 host/clang/cale toolchain v1을 결정한다. */
int qstar_resolve_toolchain(struct qstar_graph *graph, const struct qstar_target *target,
    struct qstar_resolved_toolchain *resolved);

/** target label을 .qstar/out 아래 파일명에 안전한 이름으로 바꾼다. */
void qstar_mangle_label(const char *label, char *dst, size_t dstlen);

/** compile object output path를 deterministic package-relative path로 만든다. */
int qstar_object_output_path(const struct qstar_target *target, size_t index, char *dst,
    size_t dstlen);

/** target artifact output path를 deterministic package-relative path로 만든다. */
int qstar_artifact_output_path(const struct qstar_target *target, char *dst, size_t dstlen);

/** target kind의 최종 action 이름을 rule registry 기준으로 반환한다. */
const char *qstar_target_final_action(const struct qstar_target *target);

/** target kind의 output group 이름을 rule registry 기준으로 반환한다. */
const char *qstar_target_output_group(const struct qstar_target *target);

/** target artifact가 local executor에서 실행 가능한 파일인지 확인한다. */
int qstar_target_has_executable_artifact(const struct qstar_target *target);

/** target artifact가 qstar install 대상인지 확인한다. */
int qstar_target_is_installable(const struct qstar_target *target);

/** qstar init template을 지정된 directory에 생성한다. */
int qstar_init_project(const char *template_name, const char *directory, FILE *out,
    char *error, size_t error_len);

/** QStar plan/executor 공용 target closure callback이다. */
typedef int (*qstar_target_visit_fn)(struct qstar_graph *graph, const struct qstar_target *target,
    size_t order, void *user);

/** dependency-first closure를 계산해 각 target callback을 순서대로 호출한다. */
int qstar_graph_visit_closure(struct qstar_graph *graph, const char *label,
    qstar_target_visit_fn visit, void *user);

#endif
