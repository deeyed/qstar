#include "internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fmt_buf {
	char *data;
	size_t len;
	size_t cap;
};

/** formatter 동적 문자열 buffer를 비운 상태로 초기화한다. */
static void
fmt_buf_init(struct fmt_buf *buf)
{
	memset(buf, 0, sizeof(*buf));
}

/** formatter 동적 문자열 buffer가 소유한 메모리를 해제한다. */
static void
fmt_buf_free(struct fmt_buf *buf)
{
	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}

/** formatter buffer에 raw byte span을 추가한다. */
static int
fmt_append_n(struct fmt_buf *buf, const char *s, size_t n)
{
	char *data;
	size_t cap;

	if (buf->len + n + 1 > buf->cap) {
		cap = buf->cap ? buf->cap * 2 : 512;
		while (cap < buf->len + n + 1)
			cap *= 2;
		data = realloc(buf->data, cap);
		if (!data)
			return -1;
		buf->data = data;
		buf->cap = cap;
	}
	memcpy(buf->data + buf->len, s, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
	return 0;
}

/** formatter buffer에 C string을 추가한다. */
static int
fmt_append(struct fmt_buf *buf, const char *s)
{
	return fmt_append_n(buf, s ? s : "", strlen(s ? s : ""));
}

/** formatter buffer에 printf 형식 문자열을 추가한다. */
static int
fmt_printf(struct fmt_buf *buf, const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	return fmt_append_n(buf, tmp, (size_t)n);
}

/** span 양끝의 ASCII whitespace를 제거한다. */
static void
trim_span(const char **start, size_t *len)
{
	const char *s;
	size_t n;

	s = *start;
	n = *len;
	while (n > 0 && isspace((unsigned char)*s)) {
		s++;
		n--;
	}
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		n--;
	*start = s;
	*len = n;
}

/** trim된 span을 새 NUL-terminated 문자열로 복사한다. */
static char *
copy_trimmed(const char *start, size_t len)
{
	char *s;

	trim_span(&start, &len);
	s = malloc(len + 1);
	if (!s)
		return NULL;
	memcpy(s, start, len);
	s[len] = '\0';
	return s;
}

/** string literal 안을 건너뛰며 closing quote 이후 위치를 반환한다. */
static const char *
skip_string(const char *p)
{
	char quote;

	quote = *p++;
	while (*p) {
		if (*p == '\\' && p[1]) {
			p += 2;
			continue;
		}
		if (*p++ == quote)
			break;
	}
	return p;
}

/** span이 keyword로 시작하고 뒤가 identifier 문자가 아닌지 확인한다. */
static int
starts_with_keyword(const char *p, const char *keyword)
{
	size_t n;

	while (*p && isspace((unsigned char)*p))
		p++;
	n = strlen(keyword);
	return strncmp(p, keyword, n) == 0 &&
	    !(isalnum((unsigned char)p[n]) || p[n] == '_');
}

/** qstar.* authoring call인지 확인한다. */
static int
is_qstar_statement(const char *s, size_t len)
{
	trim_span(&s, &len);
	return len >= 6 && strncmp(s, "qstar.", 6) == 0;
}

/** Lua block token 뒤 위치를 찾는다. */
static const char *
lua_token_end(const char *p)
{
	while (isalnum((unsigned char)*p) || *p == '_')
		p++;
	return p;
}

/** local function/for/if 같은 Lua helper block 전체 끝 위치를 찾는다. */
static const char *
find_lua_block_end(const char *p)
{
	const char *q, *tok, *end;
	int depth;

	q = p;
	depth = 0;
	while (*q) {
		if (*q == '"' || *q == '\'') {
			q = skip_string(q);
			continue;
		}
		if (*q == '-' && q[1] == '-') {
			while (*q && *q != '\n')
				q++;
			continue;
		}
		if (isalpha((unsigned char)*q) || *q == '_') {
			tok = q;
			end = lua_token_end(q);
			if ((size_t)(end - tok) == 8 && strncmp(tok, "function", 8) == 0)
				depth++;
			else if ((size_t)(end - tok) == 4 &&
			    (strncmp(tok, "then", 4) == 0 || strncmp(tok, "else", 4) == 0)) {
				if (strncmp(tok, "then", 4) == 0)
					depth++;
			} else if ((size_t)(end - tok) == 2 && strncmp(tok, "do", 2) == 0)
				depth++;
			else if ((size_t)(end - tok) == 6 && strncmp(tok, "repeat", 6) == 0)
				depth++;
			else if ((size_t)(end - tok) == 3 && strncmp(tok, "end", 3) == 0) {
				if (depth > 0)
					depth--;
				if (depth == 0) {
					q = end;
					while (*q == ' ' || *q == '\t')
						q++;
					if (*q == '\r')
						q++;
					if (*q == '\n')
						q++;
					return q;
				}
			} else if ((size_t)(end - tok) == 5 && strncmp(tok, "until", 5) == 0) {
				if (depth > 0)
					depth--;
				if (depth == 0) {
					q = end;
					while (*q == ' ' || *q == '\t')
						q++;
					if (*q == '\r')
						q++;
					if (*q == '\n')
						q++;
					return q;
				}
			}
			q = end;
			continue;
		}
		q++;
	}
	return q;
}

/** top-level statement 끝 위치를 찾는다. */
static const char *
find_statement_end(const char *p)
{
	const char *q;
	int depth, saw_block;

	if (starts_with_keyword(p, "local function") || starts_with_keyword(p, "function") ||
	    starts_with_keyword(p, "for") || starts_with_keyword(p, "while") ||
	    starts_with_keyword(p, "if") || starts_with_keyword(p, "repeat"))
		return find_lua_block_end(p);
	depth = 0;
	saw_block = 0;
	for (q = p; *q; q++) {
		if (*q == '"' || *q == '\'') {
			q = skip_string(q) - 1;
			continue;
		}
		if (*q == '{') {
			depth++;
			saw_block = 1;
		} else if (*q == '}') {
			if (depth > 0)
				depth--;
			if (saw_block && depth == 0) {
				q++;
				while (*q == ' ' || *q == '\t')
					q++;
				if (*q == ',')
					q++;
				while (*q == ' ' || *q == '\t')
					q++;
				if (*q == '\r')
					q++;
				if (*q == '\n')
					q++;
				return q;
			}
		} else if (!saw_block && (*q == '\n' || *q == '\r')) {
			while (*q == '\n' || *q == '\r')
				q++;
			return q;
		}
	}
	return q;
}

/** top-level comma 위치까지 value span을 읽는다. */
static const char *
read_value_end(const char *p, const char *end)
{
	int brace, paren, bracket;

	brace = 0;
	paren = 0;
	bracket = 0;
	while (p < end && *p) {
		if (*p == '"' || *p == '\'') {
			p = skip_string(p);
			continue;
		}
		if (*p == '{')
			brace++;
		else if (*p == '}') {
			if (brace > 0)
				brace--;
		} else if (*p == '(')
			paren++;
		else if (*p == ')') {
			if (paren > 0)
				paren--;
		} else if (*p == '[')
			bracket++;
		else if (*p == ']') {
			if (bracket > 0)
				bracket--;
		} else if (*p == ',' && brace == 0 && paren == 0 && bracket == 0)
			break;
		p++;
	}
	return p;
}

/** list item 하나를 canonical indentation으로 출력한다. */
static int
append_list_item(struct fmt_buf *out, const char *start, size_t len)
{
	char *item;
	int rc;

	item = copy_trimmed(start, len);
	if (!item)
		return -1;
	if (!*item) {
		free(item);
		return 0;
	}
	rc = fmt_printf(out, "    %s,\n", item);
	free(item);
	return rc;
}

/** list value를 multi-line canonical style로 출력한다. */
static int
append_list_value(struct fmt_buf *out, const char *field, const char *value, size_t len)
{
	const char *start, *end, *p, *item_start, *item_end;

	start = value;
	trim_span(&start, &len);
	if (len < 2 || start[0] != '{' || start[len - 1] != '}')
		return -1;
	if (fmt_printf(out, "  %s = {\n", field) < 0)
		return -1;
	p = start + 1;
	end = start + len - 1;
	while (p < end) {
		while (p < end && (isspace((unsigned char)*p) || *p == ','))
			p++;
		item_start = p;
		item_end = read_value_end(p, end);
		if (append_list_item(out, item_start, (size_t)(item_end - item_start)) < 0)
			return -1;
		p = item_end;
		if (p < end && *p == ',')
			p++;
	}
	return fmt_append(out, "  },\n");
}

/** field assignment value를 canonical style로 출력한다. */
static int
append_field(struct fmt_buf *out, const char *field, const char *value, size_t len)
{
	const char *v;
	size_t n;
	char *copy;
	int rc;

	v = value;
	n = len;
	trim_span(&v, &n);
	if (n >= 2 && v[0] == '{' && v[n - 1] == '}')
		return append_list_value(out, field, v, n);
	copy = copy_trimmed(value, len);
	if (!copy)
		return -1;
	rc = fmt_printf(out, "  %s = %s,\n", field, copy);
	free(copy);
	return rc;
}

/** qstar.* block body를 field 단위로 canonical formatting한다. */
static int
format_block_body(struct fmt_buf *out, const char *body, size_t len)
{
	const char *p, *end, *name_start, *value_start, *value_end;
	char field[128];
	size_t name_len;

	p = body;
	end = body + len;
	while (p < end) {
		while (p < end && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (p >= end)
			break;
		if (!isalpha((unsigned char)*p) && *p != '_')
			return -1;
		name_start = p;
		p++;
		while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
			p++;
		name_len = (size_t)(p - name_start);
		if (name_len == 0 || name_len >= sizeof(field))
			return -1;
		memcpy(field, name_start, name_len);
		field[name_len] = '\0';
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p >= end || *p != '=')
			return -1;
		p++;
		value_start = p;
		value_end = read_value_end(p, end);
		if (append_field(out, field, value_start, (size_t)(value_end - value_start)) < 0)
			return -1;
		p = value_end;
		if (p < end && *p == ',')
			p++;
	}
	return 0;
}

/** qstar.* statement 하나를 canonical formatting한다. */
static int
format_statement(struct fmt_buf *out, const char *start, size_t len)
{
	const char *s, *open, *close, *p;
	char *header, *line;
	int depth;

	s = start;
	trim_span(&s, &len);
	if (len == 0)
		return 0;
	if (!is_qstar_statement(s, len)) {
		line = copy_trimmed(s, len);
		if (!line)
			return -1;
		if (fmt_printf(out, "%s\n", line) < 0) {
			free(line);
			return -1;
		}
		free(line);
		return 0;
	}
	open = memchr(s, '{', len);
	if (!open) {
		line = copy_trimmed(s, len);
		if (!line)
			return -1;
		if (fmt_printf(out, "%s\n", line) < 0) {
			free(line);
			return -1;
		}
		free(line);
		return 0;
	}
	depth = 0;
	close = NULL;
	for (p = open; p < s + len; p++) {
		if (*p == '"' || *p == '\'') {
			p = skip_string(p) - 1;
			continue;
		}
		if (*p == '{')
			depth++;
		else if (*p == '}') {
			depth--;
			if (depth == 0) {
				close = p;
				break;
			}
		}
	}
	if (!close)
		return -1;
	header = copy_trimmed(s, (size_t)(open - s));
	if (!header)
		return -1;
	if (fmt_printf(out, "%s {\n", header) < 0) {
		free(header);
		return -1;
	}
	free(header);
	if (format_block_body(out, open + 1, (size_t)(close - open - 1)) < 0)
		return -1;
	return fmt_append(out, "}\n");
}

/** qstar.lua/.qst 전체 text를 simple canonical style로 변환한다. */
static int
format_text(const char *src, struct fmt_buf *out)
{
	const char *p, *stmt_end;
	int wrote;

	p = src ? src : "";
	wrote = 0;
	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		stmt_end = find_statement_end(p);
		if (wrote && fmt_append(out, "\n") < 0)
			return -1;
		if (format_statement(out, p, (size_t)(stmt_end - p)) < 0)
			return -1;
		wrote = 1;
		p = stmt_end;
	}
	if (!wrote && fmt_append(out, "") < 0)
		return -1;
	return 0;
}

/** file 전체를 메모리로 읽는다. */
static char *
read_file(const char *path)
{
	FILE *f;
	char *data;
	long n;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	data = malloc((size_t)n + 1);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		fclose(f);
		return NULL;
	}
	data[n] = '\0';
	fclose(f);
	return data;
}

/** formatted text를 path에 쓴다. */
static int
write_file(const char *path, const char *data)
{
	FILE *f;

	f = fopen(path, "wb");
	if (!f)
		return -1;
	if (fputs(data ? data : "", f) < 0) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}

/** qstar authoring file 하나를 format/check/stdout mode로 처리한다. */
int
qstar_fmt_file(const char *path, int check, int stdout_mode, FILE *out)
{
	struct fmt_buf formatted;
	char *src;
	int changed, rc;

	src = read_file(path);
	if (!src) {
		fprintf(stderr, "qstar fmt: could not read '%s'\n", path);
		return -1;
	}
	fmt_buf_init(&formatted);
	if (format_text(src, &formatted) < 0) {
		fprintf(stderr, "qstar fmt: unsupported formatting shape in '%s'\n", path);
		free(src);
		fmt_buf_free(&formatted);
		return -1;
	}
	changed = strcmp(src, formatted.data ? formatted.data : "") != 0;
	if (stdout_mode) {
		fputs(formatted.data ? formatted.data : "", out);
		rc = 0;
	} else if (check) {
		fprintf(out, "qstar fmt v1\npath %s\nstatus %s\n", path,
		    changed ? "needs-format" : "ok");
		rc = changed ? 1 : 0;
	} else {
		if (changed && write_file(path, formatted.data ? formatted.data : "") < 0) {
			fprintf(stderr, "qstar fmt: could not write '%s'\n", path);
			rc = -1;
		} else {
			fprintf(out, "qstar fmt v1\npath %s\nstatus %s\n", path,
			    changed ? "formatted" : "ok");
			rc = 0;
		}
	}
	free(src);
	fmt_buf_free(&formatted);
	return rc;
}
