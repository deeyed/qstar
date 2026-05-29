#ifndef QSTAR_INTERNAL_H
#define QSTAR_INTERNAL_H

#include "qstar/qstar.h"

#include <stddef.h>

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

#endif
