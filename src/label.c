#include "internal.h"

#include <stdio.h>
#include <string.h>

static int
valid_name_char(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static int
valid_path_char(int c)
{
	return valid_name_char(c) || c == '/';
}

static int
valid_span(const char *s, int path)
{
	if (!s || !*s)
		return 0;
	for (; *s; s++) {
		if (!(path ? valid_path_char((unsigned char)*s) : valid_name_char((unsigned char)*s)))
			return 0;
	}
	return 1;
}

/** QStar label을 현재 fragment 기준 canonical label로 정규화한다. */
int
qstar_label_canonicalize(const char *label, const char *fragment_dir, char *dst, size_t dstlen)
{
	const char *colon, *rest, *path, *name;
	char tmp[QSTAR_PATH_MAX];
	size_t pathlen;

	if (!label || !*label)
		return -1;
	if (label[0] == ':') {
		name = label + 1;
		if (!valid_span(name, 0))
			return -1;
		if (fragment_dir && *fragment_dir)
			return snprintf(dst, dstlen, "//%s:%s", fragment_dir, name) < (int)dstlen ? 0 : -1;
		return snprintf(dst, dstlen, "//:%s", name) < (int)dstlen ? 0 : -1;
	}
	if (label[0] == '@') {
		rest = strstr(label, "//");
		if (!rest || rest == label + 1)
			return -1;
		memcpy(tmp, label + 1, (size_t)(rest - (label + 1)));
		tmp[rest - (label + 1)] = '\0';
		if (!valid_span(tmp, 0))
			return -1;
		rest += 2;
	} else if (label[0] == '/' && label[1] == '/') {
		rest = label + 2;
	} else {
		return -1;
	}
	colon = strrchr(rest, ':');
	if (!colon)
		return -1;
	pathlen = (size_t)(colon - rest);
	name = colon + 1;
	if (!valid_span(name, 0))
		return -1;
	if (pathlen > 0) {
		if (pathlen >= sizeof(tmp))
			return -1;
		memcpy(tmp, rest, pathlen);
		tmp[pathlen] = '\0';
		if (!valid_span(tmp, 1))
			return -1;
		path = tmp;
	} else {
		path = "";
	}
	(void)path;
	if (snprintf(dst, dstlen, "%s", label) >= (int)dstlen)
		return -1;
	return 0;
}

/** external canonical label에서 package alias 부분을 추출한다. */
int
qstar_label_package_alias(const char *label, char *dst, size_t dstlen)
{
	const char *rest;
	size_t n;

	if (!label || label[0] != '@')
		return -1;
	rest = strstr(label, "//");
	if (!rest)
		return -1;
	n = (size_t)(rest - label);
	if (n == 0 || n + 1 > dstlen)
		return -1;
	memcpy(dst, label, n);
	dst[n] = '\0';
	return 0;
}
