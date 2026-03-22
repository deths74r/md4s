/*
 * md4s — Markdown for Streaming
 *
 * A line-based streaming markdown parser. Processes input byte-by-byte
 * into a line buffer. Each completed line (terminated by '\n') is
 * classified, parsed for inline spans, and emitted as semantic events.
 * Completed lines are never revisited.
 *
 * Inspired by md4c (https://github.com/mity/md4c) by Martin Mitáš.
 *
 * Two states: NORMAL and FENCED_CODE. Everything else is inter-line
 * flags (list depth, blockquote depth, previous line type).
 */
#define _GNU_SOURCE
#include "md4s.h"
#include "gstr.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Portability: strndup is POSIX but not available on all platforms. */
#if defined(_MSC_VER) || (!defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE))
static char *md4s_strndup_(const char *s, size_t n)
{
	size_t len = 0;
	while (len < n && s[len])
		len++;
	char *r = (char *)malloc(len + 1);
	if (r) {
		memcpy(r, s, len);
		r[len] = '\0';
	}
	return r;
}
#define strndup md4s_strndup_
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define LINE_BUFFER_INITIAL   1024
#define RAW_BUFFER_INITIAL    4096
#define GROWTH_FACTOR         2
#define MAX_LINK_DEFS         256
#define MAX_INLINE_DEPTH      32
#define MAX_OLIST_DIGITS      9
#define MAX_BUFFER_SIZE       (256 * 1024 * 1024)  /* 256 MB */

/* ------------------------------------------------------------------ */
/* Internal types                                                     */
/* ------------------------------------------------------------------ */

enum parser_state {
	STATE_NORMAL,
	STATE_FENCED_CODE,
	STATE_HTML_BLOCK,
};

enum line_type {
	LINE_PARAGRAPH,
	LINE_HEADING,
	LINE_FENCE_OPEN,
	LINE_FENCE_CLOSE,
	LINE_CODE,
	LINE_UNORDERED_LIST,
	LINE_ORDERED_LIST,
	LINE_BLOCKQUOTE,
	LINE_THEMATIC_BREAK,
	LINE_BLANK,
	LINE_TABLE_ROW,
	LINE_HTML_BLOCK,
	LINE_INDENTED_CODE,
};

#define TABLE_MAX_COLUMNS 64

/* ------------------------------------------------------------------ */
/* Parser struct                                                      */
/* ------------------------------------------------------------------ */

struct md4s_parser {
	md4s_callback callback;
	void *user_data;

	/* State machine. */
	enum parser_state state;

	/* Fenced code block tracking. */
	char fence_char;
	int fence_length;
	char *code_language;

	/* Line buffer — accumulates bytes until '\n'. */
	char *line_buf;
	size_t line_len;
	size_t line_cap;

	/* Raw buffer — all bytes fed, for finalize/cancel return. */
	char *raw_buf;
	size_t raw_len;
	size_t raw_cap;

	/* Block context. */
	enum line_type last_type;
	int emitted_count;
	bool needs_separator;

	/* List tracking. */
	bool in_list;
	bool list_ordered;
	int list_depth;  /* Nesting depth (0 = not in list). */

	/* Whether a partial line is currently displayed. */
	bool partial_displayed;

	/* Table tracking. */
	bool in_table;
	int table_columns;
	int table_alignments[TABLE_MAX_COLUMNS]; /* 0=none,1=left,2=center,3=right */
	bool table_head_done;

	/* Deferred line for table lookahead. */
	char *deferred_line;
	size_t deferred_len;

	/* Reference link definitions. */
	struct {
		char *label;    /* lowercase, heap-allocated */
		char *url;      /* heap-allocated */
	} link_defs[MAX_LINK_DEFS];
	int link_def_count;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void emit(struct md4s_parser *p, enum md4s_event event,
		 const struct md4s_detail *detail)
{
	p->callback(event, detail, p->user_data);
}

static void emit_simple(struct md4s_parser *p, enum md4s_event event)
{
	struct md4s_detail d = {0};
	emit(p, event, &d);
}

static void emit_text(struct md4s_parser *p, enum md4s_event event,
		      const char *text, size_t length)
{
	struct md4s_detail d = {0};
	d.text = text;
	d.text_length = length;
	emit(p, event, &d);
}

static bool buf_ensure(char **buf, size_t *cap, size_t needed)
{
	if (needed <= *cap)
		return true;
	if (needed > MAX_BUFFER_SIZE)
		return false;
	size_t new_cap = *cap;
	while (new_cap < needed) {
		size_t next = new_cap * GROWTH_FACTOR;
		if (next <= new_cap || next > MAX_BUFFER_SIZE) {
			new_cap = needed; /* clamp to exact size */
			break;
		}
		new_cap = next;
	}
	char *new_buf = realloc(*buf, new_cap);
	if (new_buf == NULL)
		return false;
	*buf = new_buf;
	*cap = new_cap;
	return true;
}

static bool buf_append(char **buf, size_t *len, size_t *cap,
		       const char *data, size_t data_len)
{
	size_t needed = *len + data_len + 1;
	if (needed < *len) /* overflow check */
		return false;
	if (!buf_ensure(buf, cap, needed))
		return false;
	memcpy(*buf + *len, data, data_len);
	*len += data_len;
	(*buf)[*len] = '\0';
	return true;
}

static int count_leading(const char *s, size_t len, char ch)
{
	int n = 0;
	while ((size_t)n < len && s[n] == ch)
		n++;
	return n;
}

static size_t count_leading_spaces(const char *s, size_t len)
{
	size_t n = 0;
	while (n < len && s[n] == ' ')
		n++;
	return n;
}

static bool is_all_whitespace(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t')
			return false;
	}
	return true;
}

static bool is_thematic_break(const char *s, size_t len)
{
	if (len < 3)
		return false;

	char ch = 0;
	int count = 0;

	for (size_t i = 0; i < len; i++) {
		if (s[i] == ' ')
			continue;
		if (s[i] == '-' || s[i] == '*' || s[i] == '_') {
			if (ch == 0)
				ch = s[i];
			else if (s[i] != ch)
				return false;
			count++;
		} else {
			return false;
		}
	}

	return count >= 3;
}

/*
 * Unicode-aware word boundary check for emphasis flanking.
 * Classifies the codepoint at the given position.
 * Returns true if the position is at a boundary (start/end of text,
 * whitespace, or punctuation/symbol).
 *
 * When look_before is true, examines the codepoint ending at pos.
 * When false, examines the codepoint starting at pos.
 */
static bool is_word_boundary(const char *text, size_t len, size_t pos,
			     bool look_before)
{
	if (look_before && pos == 0)
		return true;
	if (!look_before && pos >= len)
		return true;

	uint32_t cp;
	if (look_before) {
		size_t prev = utf8_prev(text, len, pos);
		utf8_decode(text + prev, pos - prev, &cp);
	} else {
		utf8_decode(text + pos, len - pos, &cp);
	}

	if (cp == 0)
		return true;
	/* Note: ZWSP (U+200B) is intentionally "other", not whitespace.
	 * See CommonMark spec and gstr spec 11 Change 10. */
	if (gstr_is_whitespace_cp(cp))
		return true;
	if (gstr_is_unicode_punctuation(cp))
		return true;
	return false;
}

/* ------------------------------------------------------------------ */
/* Reference link definitions                                         */
/* ------------------------------------------------------------------ */

/*
 * Try to parse a link definition: [label]: url
 * Returns true if parsed, and stores the definition.
 */
static bool try_parse_link_def(struct md4s_parser *p,
			       const char *line, size_t len)
{
	if (len < 5 || line[0] != '[')
		return false;

	/* Find closing ]. */
	size_t pos = 1;
	while (pos < len && line[pos] != ']')
		pos++;
	if (pos >= len || pos == 1)
		return false;

	size_t label_start = 1;
	size_t label_len = pos - 1;
	pos++; /* skip ] */

	/* Must have ': ' after label. */
	if (pos >= len || line[pos] != ':')
		return false;
	pos++; /* skip : */

	/* Skip spaces. */
	while (pos < len && line[pos] == ' ')
		pos++;
	if (pos >= len)
		return false;

	/* Rest is the URL (trim trailing spaces). */
	size_t url_start = pos;
	size_t url_len = len - pos;
	while (url_len > 0 && line[url_start + url_len - 1] == ' ')
		url_len--;
	if (url_len == 0)
		return false;

	/* Strip angle brackets if present. */
	if (url_len >= 2 && line[url_start] == '<' &&
	    line[url_start + url_len - 1] == '>') {
		url_start++;
		url_len -= 2;
	}

	if (p->link_def_count >= MAX_LINK_DEFS)
		return false;

	/* Store with lowercase label. */
	char *label = strndup(line + label_start, label_len);
	for (size_t i = 0; i < label_len; i++) {
		if (label[i] >= 'A' && label[i] <= 'Z')
			label[i] += 32;
	}

	/* Check for duplicate. */
	for (int i = 0; i < p->link_def_count; i++) {
		if (strcmp(p->link_defs[i].label, label) == 0) {
			free(label);
			return true; /* duplicate, ignore */
		}
	}

	p->link_defs[p->link_def_count].label = label;
	p->link_defs[p->link_def_count].url =
		strndup(line + url_start, url_len);
	p->link_def_count++;
	return true;
}

/*
 * Look up a reference link definition by label.
 * Returns the URL or NULL if not found.
 */
static const char *find_link_def(const struct md4s_parser *p,
				 const char *label, size_t label_len)
{
	/* Lowercase compare. */
	for (int i = 0; i < p->link_def_count; i++) {
		size_t def_len = strlen(p->link_defs[i].label);
		if (def_len != label_len)
			continue;
		bool match = true;
		for (size_t j = 0; j < label_len; j++) {
			char a = label[j];
			if (a >= 'A' && a <= 'Z')
				a += 32;
			if (a != p->link_defs[i].label[j]) {
				match = false;
				break;
			}
		}
		if (match)
			return p->link_defs[i].url;
	}
	return NULL;
}

/*
 * Check if a line is a setext heading underline.
 * Returns 1 for '=' (level 1), 2 for '-' (level 2), 0 if not.
 */
static int is_setext_underline(const char *line, size_t len)
{
	if (len == 0)
		return 0;
	size_t pos = 0;
	/* Skip leading spaces (max 3). */
	while (pos < len && pos < 3 && line[pos] == ' ')
		pos++;
	if (pos >= len)
		return 0;
	char ch = line[pos];
	if (ch != '=' && ch != '-')
		return 0;
	/* Count the underline characters. */
	int count = 0;
	while (pos < len && line[pos] == ch) {
		count++;
		pos++;
	}
	/* Skip trailing spaces. */
	while (pos < len && line[pos] == ' ')
		pos++;
	/* Must be at end and have at least 1 char. */
	if (pos != len || count < 1)
		return 0;
	return (ch == '=') ? 1 : 2;
}

/*
 * Check if a line starts an HTML block. Returns true if the line
 * begins with a block-level HTML tag or HTML comment.
 */
static bool is_html_block_start(const char *line, size_t len)
{
	if (len < 2 || line[0] != '<')
		return false;

	/* HTML comment: <!-- */
	if (len >= 4 && line[1] == '!' && line[2] == '-' && line[3] == '-')
		return true;

	/* Skip optional '/' for closing tags. */
	size_t pos = 1;
	bool closing = false;
	if (pos < len && line[pos] == '/') {
		closing = true;
		pos++;
	}

	/* Extract tag name. */
	size_t tag_start = pos;
	while (pos < len && ((line[pos] >= 'a' && line[pos] <= 'z') ||
			     (line[pos] >= 'A' && line[pos] <= 'Z') ||
			     (pos > tag_start && line[pos] >= '0' &&
			      line[pos] <= '9')))
		pos++;
	size_t tag_len = pos - tag_start;
	if (tag_len == 0)
		return false;

	/* Must be followed by space, >, />, or end. */
	if (pos < len && line[pos] != ' ' && line[pos] != '>' &&
	    line[pos] != '/' && line[pos] != '\t')
		return false;

	/* Check against block-level tags. */
	static const char *block_tags[] = {
		"address", "article", "aside", "base", "basefont",
		"blockquote", "body", "caption", "center", "col",
		"colgroup", "dd", "details", "dialog", "dir", "div",
		"dl", "dt", "fieldset", "figcaption", "figure",
		"footer", "form", "frame", "frameset", "h1", "h2",
		"h3", "h4", "h5", "h6", "head", "header", "hr",
		"html", "iframe", "legend", "li", "link", "main",
		"menu", "menuitem", "nav", "noframes", "ol",
		"optgroup", "option", "p", "param", "pre", "script",
		"section", "source", "style", "summary", "table",
		"tbody", "td", "template", "tfoot", "th", "thead",
		"title", "tr", "track", "ul", NULL
	};

	for (int i = 0; block_tags[i] != NULL; i++) {
		size_t bt_len = strlen(block_tags[i]);
		if (bt_len == tag_len) {
			bool match = true;
			for (size_t j = 0; j < tag_len; j++) {
				char a = line[tag_start + j];
				char b = block_tags[i][j];
				if (a >= 'A' && a <= 'Z')
					a += 32;
				if (a != b) {
					match = false;
					break;
				}
			}
			if (match)
				return true;
		}
	}
	return false;
}

/*
 * Check if a line ends an HTML block. Returns true if it contains
 * a closing tag or comment end for block-level HTML, or is blank.
 */
static bool is_html_block_end(const char *line, size_t len)
{
	if (len == 0)
		return true; /* blank line ends HTML block */
	/* Check for --> (comment end). */
	for (size_t i = 0; i + 2 < len; i++) {
		if (line[i] == '-' && line[i + 1] == '-' &&
		    line[i + 2] == '>')
			return true;
	}
	return false;
}

/* Forward declaration: parse_inline is used by emit_table_row. */
static void parse_inline_depth(struct md4s_parser *p, const char *text,
			       size_t length, int depth);
static void parse_inline(struct md4s_parser *p, const char *text,
			 size_t length);

/* ------------------------------------------------------------------ */
/* Table helpers                                                      */
/* ------------------------------------------------------------------ */

/*
 * Count pipe-separated cells in a line. Returns 0 if the line
 * doesn't look like a table row (must contain at least one |).
 */
static int count_table_cells(const char *line, size_t len)
{
	int pipes = 0;
	bool in_backtick = false;
	for (size_t i = 0; i < len; i++) {
		if (line[i] == '\\' && i + 1 < len) {
			i++;
			continue;
		}
		if (line[i] == '`')
			in_backtick = !in_backtick;
		if (line[i] == '|' && !in_backtick)
			pipes++;
	}
	if (pipes == 0)
		return 0;

	/* Count cells: pipes - leading pipe - trailing pipe + 1.
	 * But we handle edge cases by counting segments between pipes. */
	int cells = 0;
	size_t start = 0;
	/* Skip leading pipe. */
	while (start < len && line[start] == ' ')
		start++;
	bool leading_pipe = (start < len && line[start] == '|');
	if (leading_pipe)
		start++;

	/* Skip trailing pipe. */
	size_t end = len;
	while (end > start && line[end - 1] == ' ')
		end--;
	bool trailing_pipe = (end > start && line[end - 1] == '|');
	if (trailing_pipe)
		end--;

	/* Count segments. */
	cells = 1;
	in_backtick = false;
	for (size_t i = start; i < end; i++) {
		if (line[i] == '\\' && i + 1 < end) {
			i++;
			continue;
		}
		if (line[i] == '`')
			in_backtick = !in_backtick;
		if (line[i] == '|' && !in_backtick)
			cells++;
	}
	return cells;
}

/*
 * Check if a line is a table separator row (e.g., |---|:---:|---:|).
 * If valid, fills alignments[] and returns the column count.
 * Returns 0 if not a valid separator.
 */
static int parse_table_separator(const char *line, size_t len,
				 int alignments[TABLE_MAX_COLUMNS])
{
	size_t pos = 0;

	/* Skip leading whitespace. */
	while (pos < len && line[pos] == ' ')
		pos++;
	/* Skip optional leading pipe. */
	if (pos < len && line[pos] == '|')
		pos++;

	int col = 0;
	while (pos < len && col < TABLE_MAX_COLUMNS) {
		/* Skip whitespace. */
		while (pos < len && line[pos] == ' ')
			pos++;
		if (pos >= len)
			break;

		/* Optional trailing pipe at end. */
		if (line[pos] == '|' && pos + 1 >= len)
			break;
		/* Empty segment at end (trailing pipe). */
		if (pos >= len)
			break;

		bool left_colon = false;
		if (pos < len && line[pos] == ':') {
			left_colon = true;
			pos++;
		}

		/* Must have at least one dash. */
		int dashes = 0;
		while (pos < len && line[pos] == '-') {
			dashes++;
			pos++;
		}
		if (dashes == 0)
			return 0;

		bool right_colon = false;
		if (pos < len && line[pos] == ':') {
			right_colon = true;
			pos++;
		}

		/* Skip trailing whitespace in cell. */
		while (pos < len && line[pos] == ' ')
			pos++;

		/* Must be followed by pipe or end. */
		if (pos < len && line[pos] != '|')
			return 0;
		if (pos < len)
			pos++; /* skip | */

		/* Determine alignment. */
		if (left_colon && right_colon)
			alignments[col] = 2; /* center */
		else if (right_colon)
			alignments[col] = 3; /* right */
		else if (left_colon)
			alignments[col] = 1; /* left */
		else
			alignments[col] = 0; /* none */
		col++;
	}

	return (col >= 1) ? col : 0;
}

/*
 * Emit events for a single table row. Splits on unescaped pipes
 * and emits CELL_ENTER/inline content/CELL_LEAVE for each cell.
 */
static void emit_table_row(struct md4s_parser *p, const char *line,
			   size_t len, const int *alignments, int ncols)
{
	emit_simple(p, MD4S_TABLE_ROW_ENTER);

	size_t pos = 0;
	/* Skip leading whitespace and optional leading pipe. */
	while (pos < len && line[pos] == ' ')
		pos++;
	if (pos < len && line[pos] == '|')
		pos++;

	int col = 0;
	while (pos <= len && col < ncols) {
		/* Find end of cell (next unescaped pipe or end). */
		size_t cell_start = pos;
		bool in_bt = false;
		while (pos < len) {
			if (line[pos] == '\\' && pos + 1 < len) {
				pos += 2;
				continue;
			}
			if (line[pos] == '`')
				in_bt = !in_bt;
			if (line[pos] == '|' && !in_bt)
				break;
			pos++;
		}
		size_t cell_end = pos;
		if (pos < len)
			pos++; /* skip pipe */

		/* Trim cell content. */
		while (cell_start < cell_end &&
		       line[cell_start] == ' ')
			cell_start++;
		while (cell_end > cell_start &&
		       line[cell_end - 1] == ' ')
			cell_end--;

		struct md4s_detail d = {0};
		d.cell_alignment = (col < ncols)
			? alignments[col] : 0;
		emit(p, MD4S_TABLE_CELL_ENTER, &d);
		if (cell_end > cell_start)
			parse_inline(p, line + cell_start,
				     cell_end - cell_start);
		emit_simple(p, MD4S_TABLE_CELL_LEAVE);
		col++;
	}

	/* Emit empty cells for remaining columns. */
	while (col < ncols) {
		struct md4s_detail d = {0};
		d.cell_alignment = alignments[col];
		emit(p, MD4S_TABLE_CELL_ENTER, &d);
		emit_simple(p, MD4S_TABLE_CELL_LEAVE);
		col++;
	}

	emit_simple(p, MD4S_TABLE_ROW_LEAVE);
	emit_simple(p, MD4S_NEWLINE);
}

/* ------------------------------------------------------------------ */
/* Line classification                                                */
/* ------------------------------------------------------------------ */

struct classified_line {
	enum line_type type;
	const char *content;    /* Points past any prefix syntax. */
	size_t content_length;
	int heading_level;      /* 1-6 for headings. */
	int ordered_number;     /* For ordered lists. */
	int indent;             /* Leading spaces before marker. */
	char fence_char;        /* For fence open. */
	int fence_length;       /* For fence open. */
	const char *info_string; /* For fence open (language). */
	size_t info_length;
};

/* Forward declarations for functions used before their definitions. */
static void close_paragraph(struct md4s_parser *p);
static void close_list_if_needed(struct md4s_parser *p, enum line_type type);
static void maybe_emit_separator(struct md4s_parser *p,
				 const struct classified_line *cl);

/*
 * Strips trailing \n and \r from a line, returning the trimmed length.
 */
static size_t strip_newline(const char *line, size_t len)
{
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	return len;
}

static struct classified_line classify_line(const struct md4s_parser *p,
					    const char *line, size_t raw_len)
{
	struct classified_line cl = {0};
	size_t len = strip_newline(line, raw_len);

	cl.content = line;
	cl.content_length = len;

	/* Inside a fenced code block. */
	if (p->state == STATE_FENCED_CODE) {
		/* Check for closing fence. */
		int n = count_leading(line, len, p->fence_char);
		if (n >= p->fence_length) {
			/* Rest must be whitespace only. */
			bool rest_blank = is_all_whitespace(
				line + n, len - (size_t)n);
			if (rest_blank) {
				cl.type = LINE_FENCE_CLOSE;
				return cl;
			}
		}
		cl.type = LINE_CODE;
		cl.content = line;
		cl.content_length = len;
		return cl;
	}

	/* Inside an HTML block — pass through until blank line. */
	if (p->state == STATE_HTML_BLOCK) {
		if (len == 0 || is_all_whitespace(line, len)) {
			cl.type = LINE_BLANK;
			return cl;
		}
		cl.type = LINE_HTML_BLOCK;
		cl.content = line;
		cl.content_length = len;
		return cl;
	}

	/* Blank line. */
	if (len == 0 || is_all_whitespace(line, len)) {
		cl.type = LINE_BLANK;
		return cl;
	}

	/* Fence open: 3+ backticks or tildes. */
	if (len >= 3 && (line[0] == '`' || line[0] == '~')) {
		char ch = line[0];
		int n = count_leading(line, len, ch);
		if (n >= 3) {
			bool valid = true;
			/* Backtick fences: info string must not contain backticks. */
			if (ch == '`') {
				for (size_t i = (size_t)n; i < len; i++) {
					if (line[i] == '`') {
						valid = false;
						break;
					}
				}
			}
			if (valid) {
				cl.type = LINE_FENCE_OPEN;
				cl.fence_char = ch;
				cl.fence_length = n;
				/* Extract info string (trim whitespace). */
				const char *info = line + n;
				size_t info_len = len - (size_t)n;
				while (info_len > 0 && info[0] == ' ') {
					info++;
					info_len--;
				}
				while (info_len > 0 &&
				       info[info_len - 1] == ' ')
					info_len--;
				cl.info_string = info;
				cl.info_length = info_len;
				return cl;
			}
		}
	}

	/* ATX heading: 1-6 '#' followed by space or end. */
	if (line[0] == '#') {
		int level = count_leading(line, len, '#');
		if (level >= 1 && level <= 6 &&
		    ((size_t)level == len || line[level] == ' ')) {
			cl.type = LINE_HEADING;
			cl.heading_level = level;
			cl.content = line + level;
			cl.content_length = len - (size_t)level;
			if (cl.content_length > 0 && cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
			/* Strip trailing '#' and spaces. */
			while (cl.content_length > 0 &&
			       cl.content[cl.content_length - 1] == '#')
				cl.content_length--;
			while (cl.content_length > 0 &&
			       cl.content[cl.content_length - 1] == ' ')
				cl.content_length--;
			return cl;
		}
	}

	/* Thematic break. */
	if (is_thematic_break(line, len)) {
		cl.type = LINE_THEMATIC_BREAK;
		return cl;
	}

	/* Blockquote: starts with '>'. */
	if (line[0] == '>') {
		cl.type = LINE_BLOCKQUOTE;
		cl.content = line + 1;
		cl.content_length = len - 1;
		if (cl.content_length > 0 && cl.content[0] == ' ') {
			cl.content++;
			cl.content_length--;
		}
		return cl;
	}

	/* Indented code block: 4+ spaces (not inside list). */
	if (!p->in_list && len >= 4 &&
	    line[0] == ' ' && line[1] == ' ' &&
	    line[2] == ' ' && line[3] == ' ') {
		cl.type = LINE_INDENTED_CODE;
		cl.content = line + 4;
		cl.content_length = len - 4;
		return cl;
	}

	/* HTML block: starts with block-level HTML tag. */
	if (is_html_block_start(line, len)) {
		cl.type = LINE_HTML_BLOCK;
		cl.content = line;
		cl.content_length = len;
		return cl;
	}

	/* Unordered list: optional indent, then [-*+] space. */
	{
		size_t indent = count_leading_spaces(line, len);
		if (indent < len - 1) {
			char marker = line[indent];
			if ((marker == '-' || marker == '*' ||
			     marker == '+') &&
			    indent + 1 < len && line[indent + 1] == ' ') {
				/* Make sure '-' isn't a thematic break. */
				if (marker != '-' || !is_thematic_break(line, len)) {
					cl.type = LINE_UNORDERED_LIST;
					cl.indent = (int)indent;
					cl.content = line + indent + 2;
					cl.content_length =
						len - indent - 2;
					while (cl.content_length > 0 &&
					       cl.content[0] == ' ') {
						cl.content++;
						cl.content_length--;
					}
					return cl;
				}
			}
		}
	}

	/* Ordered list: optional indent, 1-9 digits, [.)] space. */
	{
		size_t indent = count_leading_spaces(line, len);
		size_t i = indent;
		while (i < len && i < indent + MAX_OLIST_DIGITS &&
		       line[i] >= '0' && line[i] <= '9')
			i++;
		if (i > indent && i < len &&
		    (line[i] == '.' || line[i] == ')') &&
		    i + 1 < len && line[i + 1] == ' ') {
			cl.type = LINE_ORDERED_LIST;
			cl.indent = (int)indent;
			/* Parse the number. */
			cl.ordered_number = 0;
			for (size_t j = indent; j < i; j++)
				cl.ordered_number =
					cl.ordered_number * 10 +
					(line[j] - '0');
			cl.content = line + i + 2;
			cl.content_length = len - i - 2;
			while (cl.content_length > 0 &&
			       cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
			return cl;
		}
	}

	/* Default: paragraph. Trim leading spaces. */
	cl.type = LINE_PARAGRAPH;
	cl.content = line;
	cl.content_length = len;
	while (cl.content_length > 0 && cl.content[0] == ' ') {
		cl.content++;
		cl.content_length--;
	}
	return cl;
}

/* ------------------------------------------------------------------ */
/* Inline span parsing                                                */
/* ------------------------------------------------------------------ */

/*
 * Find matching closing backticks (same count) starting from pos.
 * Returns the position of the first backtick of the closing sequence,
 * or len if not found.
 */
static size_t find_backtick_close(const char *text, size_t len,
				  size_t pos, int tick_count)
{
	while (pos <= len - (size_t)tick_count) {
		if (text[pos] == '`') {
			int n = 0;
			size_t start = pos;
			while (pos < len && text[pos] == '`') {
				n++;
				pos++;
			}
			if (n == tick_count)
				return start;
		} else {
			pos++;
		}
	}
	return len;
}

/*
 * Find matching double delimiter (** or ~~ or __) starting from pos.
 * Respects nesting by skipping backtick code spans.
 */
static size_t find_double_close(const char *text, size_t len,
				size_t pos, char ch)
{
	while (pos + 1 < len) {
		if (text[pos] == '\\' && pos + 1 < len) {
			pos += 2;
			continue;
		}
		if (text[pos] == '`') {
			/* Skip code span. */
			int ticks = 0;
			size_t start = pos;
			while (pos < len && text[pos] == '`') {
				ticks++;
				pos++;
			}
			size_t close = find_backtick_close(
				text, len, pos, ticks);
			if (close < len)
				pos = close + (size_t)ticks;
			else
				pos = start + 1;
			continue;
		}
		if (text[pos] == ch && text[pos + 1] == ch)
			return pos;
		pos++;
	}
	return len;
}

/*
 * Find matching single delimiter (* or _) starting from pos.
 * For underscore, requires word boundary at the close.
 */
static size_t find_single_close(const char *text, size_t len,
				size_t pos, char ch)
{
	while (pos < len) {
		if (text[pos] == '\\' && pos + 1 < len) {
			pos += 2;
			continue;
		}
		if (text[pos] == '`') {
			int ticks = 0;
			size_t start = pos;
			while (pos < len && text[pos] == '`') {
				ticks++;
				pos++;
			}
			size_t close = find_backtick_close(
				text, len, pos, ticks);
			if (close < len)
				pos = close + (size_t)ticks;
			else
				pos = start + 1;
			continue;
		}
		if (text[pos] == ch) {
			/* Don't match double delimiter as single. */
			if (pos + 1 < len && text[pos + 1] == ch) {
				pos += 2;
				continue;
			}
			/* Underscore: require word boundary after. */
			if (ch == '_') {
				if (!is_word_boundary(text, len,
						     pos + 1, false)) {
					pos++;
					continue;
				}
			}
			return pos;
		}
		pos++;
	}
	return len;
}

/*
 * Recursively parse and emit inline spans for a text region.
 */
static void parse_inline(struct md4s_parser *p, const char *text,
			 size_t length)
{
	parse_inline_depth(p, text, length, 0);
}

static void parse_inline_depth(struct md4s_parser *p, const char *text,
			       size_t length, int depth)
{
	if (depth >= MAX_INLINE_DEPTH) {
		/* Too deep — emit remaining text as literal. */
		if (length > 0)
			emit_text(p, MD4S_TEXT, text, length);
		return;
	}

	size_t pos = 0;
	size_t plain_start = 0;

	while (pos < length) {
		/* Escaped character. */
		if (text[pos] == '\\' && pos + 1 < length) {
			char next = text[pos + 1];
			/* CommonMark 2.4: only ASCII punctuation can be escaped. */
			if ((next >= 0x21 && next <= 0x2F) ||
			    (next >= 0x3A && next <= 0x40) ||
			    (next >= 0x5B && next <= 0x60) ||
			    (next >= 0x7B && next <= 0x7E)) {
				if (pos > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  pos - plain_start);
				emit_text(p, MD4S_TEXT, &text[pos + 1], 1);
				pos += 2;
				plain_start = pos;
				continue;
			}
		}

		/* HTML entity decoding. */
		if (text[pos] == '&') {
			/* Find the semicolon. */
			size_t semi = pos + 1;
			while (semi < length && semi < pos + 10 &&
			       text[semi] != ';')
				semi++;
			if (semi < length && text[semi] == ';') {
				size_t ent_len = semi - pos + 1;
				const char *replacement = NULL;
				if (ent_len == 5 &&
				    memcmp(text + pos, "&amp;", 5) == 0)
					replacement = "&";
				else if (ent_len == 4 &&
					 memcmp(text + pos, "&lt;", 4) == 0)
					replacement = "<";
				else if (ent_len == 4 &&
					 memcmp(text + pos, "&gt;", 4) == 0)
					replacement = ">";
				else if (ent_len == 6 &&
					 memcmp(text + pos, "&quot;", 6) == 0)
					replacement = "\"";
				else if (ent_len == 6 &&
					 memcmp(text + pos, "&apos;", 6) == 0)
					replacement = "'";
				else if (ent_len == 6 &&
					 memcmp(text + pos, "&nbsp;", 6) == 0)
					replacement = " ";
				if (replacement != NULL) {
					if (pos > plain_start)
						emit_text(p, MD4S_TEXT,
							  text + plain_start,
							  pos - plain_start);
					emit_text(p, MD4S_TEXT, replacement,
						  strlen(replacement));
					pos = semi + 1;
					plain_start = pos;
					continue;
				}
			}
		}

		/* Backtick code span. */
		if (text[pos] == '`') {
			int ticks = 0;
			size_t start = pos;
			while (pos < length && text[pos] == '`') {
				ticks++;
				pos++;
			}
			size_t close = find_backtick_close(
				text, length, pos, ticks);
			if (close < length) {
				if (start > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  start - plain_start);
				emit_simple(p, MD4S_CODE_SPAN_ENTER);
				emit_text(p, MD4S_TEXT, text + pos,
					  close - pos);
				emit_simple(p, MD4S_CODE_SPAN_LEAVE);
				pos = close + (size_t)ticks;
				plain_start = pos;
				continue;
			}
			/* No close — emit backticks as literal. */
			pos = start + 1;
			continue;
		}

		/* Triple delimiter: ***bold italic*** or ___. */
		if (pos + 2 < length &&
		    ((text[pos] == '*' && text[pos + 1] == '*' &&
		      text[pos + 2] == '*') ||
		     (text[pos] == '_' && text[pos + 1] == '_' &&
		      text[pos + 2] == '_'))) {
			char ch = text[pos];
			/* Underscore: check word boundary before opener. */
			if (ch == '_') {
				if (!is_word_boundary(text, length,
						     pos, true)) {
					pos++;
					continue;
				}
			}
			/* Find closing ***. */
			size_t inner_start = pos + 3;
			/* Search for *** close. */
			size_t close = inner_start;
			while (close + 2 < length) {
				if (text[close] == ch &&
				    text[close + 1] == ch &&
				    text[close + 2] == ch) {
					break;
				}
				if (text[close] == '\\' &&
				    close + 1 < length) {
					close += 2;
					continue;
				}
				close++;
			}
			if (close + 2 < length) {
				if (pos > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  pos - plain_start);
				emit_simple(p, MD4S_BOLD_ENTER);
				emit_simple(p, MD4S_ITALIC_ENTER);
				parse_inline_depth(p, text + inner_start,
					     close - inner_start,
					     depth + 1);
				emit_simple(p, MD4S_ITALIC_LEAVE);
				emit_simple(p, MD4S_BOLD_LEAVE);
				pos = close + 3;
				plain_start = pos;
				continue;
			}
		}

		/* Double delimiter: **bold** or __bold__. */
		if (pos + 1 < length &&
		    ((text[pos] == '*' && text[pos + 1] == '*') ||
		     (text[pos] == '_' && text[pos + 1] == '_'))) {
			char ch = text[pos];
			if (ch == '_') {
				if (!is_word_boundary(text, length,
						     pos, true)) {
					pos++;
					continue;
				}
			}
			size_t close = find_double_close(
				text, length, pos + 2, ch);
			if (close < length) {
				if (pos > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  pos - plain_start);
				emit_simple(p, MD4S_BOLD_ENTER);
				parse_inline_depth(p, text + pos + 2,
					     close - pos - 2,
					     depth + 1);
				emit_simple(p, MD4S_BOLD_LEAVE);
				pos = close + 2;
				plain_start = pos;
				continue;
			}
		}

		/* Double tilde: ~~strikethrough~~. */
		if (pos + 1 < length && text[pos] == '~' &&
		    text[pos + 1] == '~') {
			size_t close = find_double_close(
				text, length, pos + 2, '~');
			if (close < length) {
				if (pos > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  pos - plain_start);
				emit_simple(p, MD4S_STRIKETHROUGH_ENTER);
				parse_inline_depth(p, text + pos + 2,
					     close - pos - 2,
					     depth + 1);
				emit_simple(p, MD4S_STRIKETHROUGH_LEAVE);
				pos = close + 2;
				plain_start = pos;
				continue;
			}
		}

		/* Single delimiter: *italic* or _italic_. */
		if (text[pos] == '*' || text[pos] == '_') {
			char ch = text[pos];
			/* Skip if this is a double (handled above). */
			if (pos + 1 < length && text[pos + 1] == ch) {
				pos++;
				continue;
			}
			if (ch == '_') {
				if (!is_word_boundary(text, length,
						     pos, true)) {
					pos++;
					continue;
				}
			}
			size_t close = find_single_close(
				text, length, pos + 1, ch);
			if (close < length) {
				if (pos > plain_start)
					emit_text(p, MD4S_TEXT,
						  text + plain_start,
						  pos - plain_start);
				emit_simple(p, MD4S_ITALIC_ENTER);
				parse_inline_depth(p, text + pos + 1,
					     close - pos - 1,
					     depth + 1);
				emit_simple(p, MD4S_ITALIC_LEAVE);
				pos = close + 1;
				plain_start = pos;
				continue;
			}
		}

		/* Autolink: <url> or <email>. */
		if (text[pos] == '<') {
			size_t close = pos + 1;
			while (close < length && text[close] != '>' &&
			       text[close] != ' ' && text[close] != '\n')
				close++;
			if (close < length && text[close] == '>' &&
			    close > pos + 1) {
				const char *inner = text + pos + 1;
				size_t inner_len = close - pos - 1;
				/* Check for URL scheme (contains "://") or
				 * email (contains "@" and no spaces). */
				bool is_url = false;
				bool has_at = false;
				for (size_t k = 0; k < inner_len; k++) {
					if (k + 2 < inner_len &&
					    inner[k] == ':' &&
					    inner[k + 1] == '/' &&
					    inner[k + 2] == '/')
						is_url = true;
					if (inner[k] == '@')
						has_at = true;
				}
				if (is_url || has_at) {
					if (pos > plain_start)
						emit_text(p, MD4S_TEXT,
							  text + plain_start,
							  pos - plain_start);
					struct md4s_detail d = {0};
					d.url = inner;
					d.url_length = inner_len;
					emit(p, MD4S_LINK_ENTER, &d);
					emit_text(p, MD4S_TEXT,
						  inner, inner_len);
					emit_simple(p, MD4S_LINK_LEAVE);
					pos = close + 1;
					plain_start = pos;
					continue;
				}
			}
		}

		/* Image: ![alt](url) or Link: [text](url). */
		if (text[pos] == '[' ||
		    (text[pos] == '!' && pos + 1 < length &&
		     text[pos + 1] == '[')) {
			bool is_image = (text[pos] == '!');
			size_t bracket_start = is_image ? pos + 1 : pos;
			/* Find closing ]. */
			size_t bracket = bracket_start + 1;
			int depth = 1;
			while (bracket < length && depth > 0) {
				if (text[bracket] == '\\' &&
				    bracket + 1 < length) {
					bracket += 2;
					continue;
				}
				if (text[bracket] == '[')
					depth++;
				else if (text[bracket] == ']')
					depth--;
				if (depth > 0)
					bracket++;
			}
			if (bracket < length && text[bracket] == ']' &&
			    bracket + 1 < length &&
			    text[bracket + 1] == '(') {
				/* Find closing ), handling title. */
				size_t paren_start = bracket + 2;
				size_t paren = paren_start;
				const char *url_start = text + paren_start;
				size_t url_len = 0;
				const char *title_start = NULL;
				size_t title_len = 0;
				/* Skip leading spaces. */
				while (paren < length && text[paren] == ' ')
					paren++;
				/* Scan URL (stop at space, quote, or close paren). */
				size_t url_begin = paren;
				int paren_depth = 0;
				while (paren < length) {
					if (text[paren] == '(')
						paren_depth++;
					else if (text[paren] == ')') {
						if (paren_depth > 0)
							paren_depth--;
						else
							break;
					} else if (text[paren] == ' ' ||
						   text[paren] == '"' ||
						   text[paren] == '\'')
						break;
					paren++;
				}
				url_start = text + url_begin;
				url_len = paren - url_begin;
				/* Check for title. */
				if (paren < length && text[paren] == ' ') {
					while (paren < length &&
					       text[paren] == ' ')
						paren++;
					if (paren < length &&
					    (text[paren] == '"' ||
					     text[paren] == '\'')) {
						char quote = text[paren];
						paren++;
						size_t ts = paren;
						while (paren < length &&
						       text[paren] != quote)
							paren++;
						if (paren < length) {
							title_start =
								text + ts;
							title_len =
								paren - ts;
							paren++; /* skip close quote */
						}
						/* Skip trailing spaces. */
						while (paren < length &&
						       text[paren] == ' ')
							paren++;
					}
				}
				if (paren < length && text[paren] == ')') {
					if (pos > plain_start)
						emit_text(p, MD4S_TEXT,
							  text + plain_start,
							  pos - plain_start);
					struct md4s_detail d = {0};
					d.url = url_start;
					d.url_length = url_len;
					d.title = title_start;
					d.title_length = title_len;
					enum md4s_event enter =
						is_image
						? MD4S_IMAGE_ENTER
						: MD4S_LINK_ENTER;
					enum md4s_event leave =
						is_image
						? MD4S_IMAGE_LEAVE
						: MD4S_LINK_LEAVE;
					emit(p, enter, &d);
					parse_inline_depth(p,
						text + bracket_start + 1,
						bracket - bracket_start - 1,
						depth + 1);
					emit_simple(p, leave);
					pos = paren + 1;
					plain_start = pos;
					continue;
				}
			}
			/* Reference link: [text][ref] or [text][] */
			if (bracket < length && text[bracket] == ']' &&
			    bracket + 1 < length &&
			    text[bracket + 1] == '[') {
				size_t ref_start = bracket + 2;
				size_t ref_end = ref_start;
				while (ref_end < length &&
				       text[ref_end] != ']')
					ref_end++;
				if (ref_end < length) {
					const char *label;
					size_t label_len;
					if (ref_end == ref_start) {
						/* [text][] — use text as label */
						label = text + bracket_start + 1;
						label_len = bracket - bracket_start - 1;
					} else {
						label = text + ref_start;
						label_len = ref_end - ref_start;
					}
					const char *url =
						find_link_def(p, label,
							      label_len);
					if (url != NULL) {
						if (pos > plain_start)
							emit_text(p, MD4S_TEXT,
								  text + plain_start,
								  pos - plain_start);
						struct md4s_detail d = {0};
						d.url = url;
						d.url_length = strlen(url);
						emit(p, is_image
							? MD4S_IMAGE_ENTER
							: MD4S_LINK_ENTER, &d);
						parse_inline_depth(p,
							text + bracket_start + 1,
							bracket - bracket_start - 1,
							depth + 1);
						emit_simple(p, is_image
							? MD4S_IMAGE_LEAVE
							: MD4S_LINK_LEAVE);
						pos = ref_end + 1;
						plain_start = pos;
						continue;
					}
				}
			}

			/* If image prefix didn't match, skip the '!' */
			if (is_image) {
				pos++;
				continue;
			}
		}

		pos++;
	}

	/* Remaining plain text. */
	if (pos > plain_start)
		emit_text(p, MD4S_TEXT, text + plain_start,
			  pos - plain_start);
}

/* ------------------------------------------------------------------ */
/* Block separator logic                                              */
/* ------------------------------------------------------------------ */

static void maybe_emit_separator(struct md4s_parser *p,
				 const struct classified_line *cl)
{
	if (p->emitted_count == 0)
		return;

	bool need = false;

	/* Blank lines set the separator flag. */
	if (p->needs_separator)
		need = true;

	/* Certain blocks always get a separator even without blank line. */
	if (cl->type == LINE_HEADING || cl->type == LINE_FENCE_OPEN ||
	    cl->type == LINE_THEMATIC_BREAK)
		need = true;

	if (need) {
		emit_simple(p, MD4S_BLOCK_SEPARATOR);
		p->needs_separator = false;
	}
}

/* ------------------------------------------------------------------ */
/* List tracking                                                      */
/* ------------------------------------------------------------------ */

static void close_list_if_needed(struct md4s_parser *p, enum line_type type)
{
	if (!p->in_list)
		return;

	if (type != LINE_UNORDERED_LIST && type != LINE_ORDERED_LIST &&
	    type != LINE_BLANK)  {
		emit_simple(p, MD4S_LIST_LEAVE);
		p->in_list = false;
	}
}

static void open_list_if_needed(struct md4s_parser *p,
				const struct classified_line *cl)
{
	bool ordered = (cl->type == LINE_ORDERED_LIST);

	if (!p->in_list) {
		struct md4s_detail d = {0};
		d.ordered = ordered;
		emit(p, MD4S_LIST_ENTER, &d);
		p->in_list = true;
		p->list_ordered = ordered;
	}
}

/* ------------------------------------------------------------------ */
/* Table tracking                                                     */
/* ------------------------------------------------------------------ */

static void close_table_if_needed(struct md4s_parser *p)
{
	if (!p->in_table)
		return;
	if (!p->table_head_done) {
		emit_simple(p, MD4S_TABLE_HEAD_LEAVE);
	} else {
		emit_simple(p, MD4S_TABLE_BODY_LEAVE);
	}
	emit_simple(p, MD4S_TABLE_LEAVE);
	emit_simple(p, MD4S_NEWLINE);
	p->in_table = false;
	p->table_columns = 0;
	p->table_head_done = false;
	p->needs_separator = true;
}

/*
 * Process a deferred line as a table header. Called when the next line
 * confirms it's a table separator.
 */
static void process_table_header(struct md4s_parser *p,
				 const char *header_line, size_t header_len,
				 const char *sep_line, size_t sep_len)
{
	size_t hlen = header_len;
	/* Strip trailing newline from header. */
	while (hlen > 0 && (header_line[hlen - 1] == '\n' ||
			    header_line[hlen - 1] == '\r'))
		hlen--;

	size_t slen = sep_len;
	while (slen > 0 && (sep_line[slen - 1] == '\n' ||
			    sep_line[slen - 1] == '\r'))
		slen--;

	/* Parse separator to get alignments and column count. */
	int alignments[TABLE_MAX_COLUMNS] = {0};
	int ncols = parse_table_separator(sep_line, slen, alignments);
	if (ncols == 0)
		return;

	close_list_if_needed(p, LINE_TABLE_ROW);
	close_paragraph(p);

	maybe_emit_separator(p, &(struct classified_line){
		.type = LINE_HEADING}); /* force separator */

	/* TABLE_ENTER with column count. */
	struct md4s_detail td = {0};
	td.column_count = ncols;
	emit(p, MD4S_TABLE_ENTER, &td);

	p->in_table = true;
	p->table_columns = ncols;
	memcpy(p->table_alignments, alignments, sizeof(int) * ncols);
	p->table_head_done = false;

	/* Emit head. */
	emit_simple(p, MD4S_TABLE_HEAD_ENTER);
	emit_table_row(p, header_line, hlen, alignments, ncols);
	emit_simple(p, MD4S_TABLE_HEAD_LEAVE);

	/* Open body for subsequent rows. */
	emit_simple(p, MD4S_TABLE_BODY_ENTER);
	p->table_head_done = true;

	p->emitted_count++;
	p->last_type = LINE_TABLE_ROW;
	p->needs_separator = false;
}

/* ------------------------------------------------------------------ */
/* Process a single completed line                                    */
/* ------------------------------------------------------------------ */

static void process_line(struct md4s_parser *p, const char *line,
			 size_t raw_len)
{
	struct classified_line cl = classify_line(p, line, raw_len);

	/* Reference link definition: [label]: url
	 * Consumed silently — not emitted as events. */
	if (cl.type == LINE_PARAGRAPH) {
		size_t slen = strip_newline(line, raw_len);
		if (try_parse_link_def(p, cl.content, slen))
			return;
	}

	/* Lazy blockquote continuation: paragraph after blockquote
	 * continues the blockquote without '>' prefix. */
	if (cl.type == LINE_PARAGRAPH &&
	    p->last_type == LINE_BLOCKQUOTE && !p->needs_separator) {
		/* Treat as blockquote continuation. */
		emit_simple(p, MD4S_BLOCKQUOTE_ENTER);
		parse_inline(p, cl.content, cl.content_length);
		emit_simple(p, MD4S_BLOCKQUOTE_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		/* Keep last_type as BLOCKQUOTE for further continuation. */
		p->emitted_count++;
		p->needs_separator = false;
		return;
	}

	/* Close indented code block when transitioning to other types. */
	if (p->last_type == LINE_INDENTED_CODE &&
	    cl.type != LINE_INDENTED_CODE && cl.type != LINE_BLANK) {
		emit_simple(p, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->needs_separator = true;
	}

	/* Setext heading detection: if we have an open paragraph and
	 * the current line is a setext underline, convert to heading.
	 * Per CommonMark, setext heading takes priority over thematic break. */
	if (p->last_type == LINE_PARAGRAPH && p->emitted_count > 0 &&
	    !p->needs_separator) {
		size_t slen = strip_newline(line, raw_len);
		int setext = is_setext_underline(line, slen);
		if (setext > 0) {
			/* The paragraph's content was already emitted.
			 * Replace PARAGRAPH with HEADING: emit leave for
			 * the paragraph block, then heading enter/leave
			 * with no content (consumer uses the paragraph text). */
			emit_simple(p, MD4S_PARAGRAPH_LEAVE);
			emit_simple(p, MD4S_NEWLINE);
			/* Now emit a heading marker event. The heading level
			 * is signaled, and since we can't re-emit content,
			 * we rely on consumers to retroactively interpret
			 * the preceding paragraph as heading content.
			 * For practical purposes (LLM streaming), we emit
			 * HEADING_ENTER/LEAVE to indicate the level. */
			struct md4s_detail d = {0};
			d.heading_level = setext;
			emit(p, MD4S_HEADING_ENTER, &d);
			emit_simple(p, MD4S_HEADING_LEAVE);
			emit_simple(p, MD4S_NEWLINE);
			p->last_type = LINE_HEADING;
			p->needs_separator = true;
			return;
		}
	}

	switch (cl.type) {
	case LINE_BLANK:
		/* Close HTML block on blank line. */
		if (p->state == STATE_HTML_BLOCK) {
			emit_simple(p, MD4S_HTML_BLOCK_LEAVE);
			emit_simple(p, MD4S_NEWLINE);
			p->state = STATE_NORMAL;
			p->needs_separator = true;
			break;
		}
		/* Blank lines inside indented code emit empty code line. */
		if (p->last_type == LINE_INDENTED_CODE) {
			emit_text(p, MD4S_CODE_TEXT, "", 0);
			emit_simple(p, MD4S_NEWLINE);
			break;
		}
		p->needs_separator = true;
		/* Close list on blank line followed by non-list. */
		/* We set the flag; the actual close happens on next non-list line. */
		break;

	case LINE_FENCE_OPEN:
		close_table_if_needed(p);
		close_list_if_needed(p, cl.type);
		maybe_emit_separator(p, &cl);
		p->state = STATE_FENCED_CODE;
		p->fence_char = cl.fence_char;
		p->fence_length = cl.fence_length;
		free(p->code_language);
		p->code_language = NULL;
		if (cl.info_length > 0)
			p->code_language =
				strndup(cl.info_string, cl.info_length);
		{
			struct md4s_detail d = {0};
			d.language = p->code_language;
			d.language_length = cl.info_length;
			emit(p, MD4S_CODE_BLOCK_ENTER, &d);
		}
		p->emitted_count++;
		p->last_type = LINE_FENCE_OPEN;
		p->needs_separator = false;
		break;

	case LINE_FENCE_CLOSE:
		emit_simple(p, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->state = STATE_NORMAL;
		p->last_type = LINE_FENCE_CLOSE;
		p->needs_separator = true;
		break;

	case LINE_CODE:
		emit_text(p, MD4S_CODE_TEXT, cl.content, cl.content_length);
		emit_simple(p, MD4S_NEWLINE);
		break;

	case LINE_HEADING:
		close_table_if_needed(p);
		close_list_if_needed(p, cl.type);
		maybe_emit_separator(p, &cl);
		{
			struct md4s_detail d = {0};
			d.heading_level = cl.heading_level;
			emit(p, MD4S_HEADING_ENTER, &d);
		}
		parse_inline(p, cl.content, cl.content_length);
		emit_simple(p, MD4S_HEADING_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = LINE_HEADING;
		p->needs_separator = true;
		break;

	case LINE_THEMATIC_BREAK:
		close_table_if_needed(p);
		close_list_if_needed(p, cl.type);
		maybe_emit_separator(p, &cl);
		emit_simple(p, MD4S_THEMATIC_BREAK);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = LINE_THEMATIC_BREAK;
		p->needs_separator = true;
		break;

	case LINE_BLOCKQUOTE:
		close_table_if_needed(p);
		close_list_if_needed(p, cl.type);
		maybe_emit_separator(p, &cl);
		emit_simple(p, MD4S_BLOCKQUOTE_ENTER);
		parse_inline(p, cl.content, cl.content_length);
		emit_simple(p, MD4S_BLOCKQUOTE_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = LINE_BLOCKQUOTE;
		p->needs_separator = false;
		break;

	case LINE_UNORDERED_LIST:
	case LINE_ORDERED_LIST:
		maybe_emit_separator(p, &cl);
		open_list_if_needed(p, &cl);
		{
			struct md4s_detail d = {0};
			d.item_number = cl.ordered_number;
			d.list_depth = (cl.indent > 0) ? 1 : 0;
			/* Task list detection: [x], [X], or [ ] prefix. */
			const char *item_text = cl.content;
			size_t item_len = cl.content_length;
			if (item_len >= 3 && item_text[0] == '[' &&
			    (item_text[1] == 'x' || item_text[1] == 'X' ||
			     item_text[1] == ' ') &&
			    item_text[2] == ']' &&
			    (item_len == 3 || item_text[3] == ' ')) {
				d.is_task = true;
				d.task_checked =
					(item_text[1] == 'x' ||
					 item_text[1] == 'X');
				item_text += 3;
				item_len -= 3;
				if (item_len > 0 && item_text[0] == ' ') {
					item_text++;
					item_len--;
				}
			}
			emit(p, MD4S_LIST_ITEM_ENTER, &d);
			parse_inline(p, item_text, item_len);
		}
		emit_simple(p, MD4S_LIST_ITEM_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = cl.type;
		p->needs_separator = false;
		break;

	case LINE_TABLE_ROW:
		/* Should not reach here — tables are handled in feed. */
		break;

	case LINE_INDENTED_CODE:
		close_table_if_needed(p);
		close_list_if_needed(p, cl.type);
		if (p->last_type != LINE_INDENTED_CODE) {
			/* Start a new indented code block. */
			maybe_emit_separator(p, &cl);
			struct md4s_detail d = {0};
			emit(p, MD4S_CODE_BLOCK_ENTER, &d);
		}
		emit_text(p, MD4S_CODE_TEXT, cl.content,
			  cl.content_length);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = LINE_INDENTED_CODE;
		p->needs_separator = false;
		break;

	case LINE_HTML_BLOCK:
		if (p->state == STATE_HTML_BLOCK) {
			/* Continuation line — just emit text. */
			emit_text(p, MD4S_TEXT, cl.content,
				  cl.content_length);
			emit_simple(p, MD4S_NEWLINE);
		} else {
			/* Opening line. */
			close_table_if_needed(p);
			close_list_if_needed(p, cl.type);
			maybe_emit_separator(p, &cl);
			emit_simple(p, MD4S_HTML_BLOCK_ENTER);
			emit_text(p, MD4S_TEXT, cl.content,
				  cl.content_length);
			emit_simple(p, MD4S_NEWLINE);
			p->state = STATE_HTML_BLOCK;
			p->emitted_count++;
			p->last_type = LINE_HTML_BLOCK;
			p->needs_separator = false;
		}
		break;

	case LINE_PARAGRAPH:
		close_list_if_needed(p, cl.type);
		close_table_if_needed(p);
		/* Trim leading spaces (LLMs sometimes indent). */
		while (cl.content_length > 0 && cl.content[0] == ' ') {
			cl.content++;
			cl.content_length--;
		}
		/* Consecutive paragraph lines: softbreak. */
		if (p->last_type == LINE_PARAGRAPH && !p->needs_separator &&
		    p->emitted_count > 0) {
			emit_simple(p, MD4S_SOFTBREAK);
			parse_inline(p, cl.content, cl.content_length);
		} else {
			maybe_emit_separator(p, &cl);
			emit_simple(p, MD4S_PARAGRAPH_ENTER);
			parse_inline(p, cl.content, cl.content_length);
		}
		/* Don't emit PARAGRAPH_LEAVE yet — next line might continue. */
		p->emitted_count++;
		p->last_type = LINE_PARAGRAPH;
		p->needs_separator = false;
		break;
	}
}

/*
 * Close any open paragraph. Called before a non-paragraph line or
 * at finalize.
 */
static void close_paragraph(struct md4s_parser *p)
{
	if (p->last_type == LINE_PARAGRAPH && p->emitted_count > 0) {
		emit_simple(p, MD4S_PARAGRAPH_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
	}
}

/* ------------------------------------------------------------------ */
/* Feed                                                               */
/* ------------------------------------------------------------------ */

void md4s_feed(struct md4s_parser *parser, const char *data, size_t length)
{
	if (parser == NULL || data == NULL || length == 0)
		return;

	/* Append to raw buffer. */
	buf_append(&parser->raw_buf, &parser->raw_len,
		   &parser->raw_cap, data, length);

	/* Process byte by byte looking for newlines. */
	for (size_t i = 0; i < length; i++) {
		/* Append to line buffer, replacing NUL with U+FFFD. */
		if (data[i] == '\0') {
			/* U+FFFD REPLACEMENT CHARACTER = EF BF BD */
			if (!buf_ensure(&parser->line_buf,
					&parser->line_cap,
					parser->line_len + 5))
				return;
			parser->line_buf[parser->line_len++] = (char)0xEF;
			parser->line_buf[parser->line_len++] = (char)0xBF;
			parser->line_buf[parser->line_len++] = (char)0xBD;
			parser->line_buf[parser->line_len] = '\0';
			continue;
		}
		if (!buf_ensure(&parser->line_buf, &parser->line_cap,
				parser->line_len + 2))
			return;
		parser->line_buf[parser->line_len++] = data[i];
		parser->line_buf[parser->line_len] = '\0';

		if (data[i] == '\n') {
			/* Clear partial line if displayed. */
			if (parser->partial_displayed) {
				emit_simple(parser, MD4S_PARTIAL_CLEAR);
				parser->partial_displayed = false;
			}

			/* Table lookahead: if we have a deferred line,
			 * check if this line is a table separator. */
			if (parser->deferred_line != NULL) {
				size_t slen = parser->line_len;
				while (slen > 0 &&
				       (parser->line_buf[slen - 1] == '\n' ||
					parser->line_buf[slen - 1] == '\r'))
					slen--;

				/* Check for setext heading underline. */
				int setext_level = is_setext_underline(
					parser->line_buf, slen);
				if (setext_level > 0) {
					/* Deferred line is a setext heading. */
					close_paragraph(parser);
					close_table_if_needed(parser);
					close_list_if_needed(parser, LINE_HEADING);
					size_t hlen = parser->deferred_len;
					while (hlen > 0 &&
					       (parser->deferred_line[hlen - 1] == '\n' ||
						parser->deferred_line[hlen - 1] == '\r'))
						hlen--;
					/* Trim leading/trailing spaces. */
					const char *htext = parser->deferred_line;
					while (hlen > 0 && htext[0] == ' ') {
						htext++;
						hlen--;
					}
					while (hlen > 0 && htext[hlen - 1] == ' ')
						hlen--;
					struct classified_line fcl = {
						.type = LINE_HEADING};
					maybe_emit_separator(parser, &fcl);
					struct md4s_detail d = {0};
					d.heading_level = setext_level;
					emit(parser, MD4S_HEADING_ENTER, &d);
					parse_inline(parser, htext, hlen);
					emit_simple(parser, MD4S_HEADING_LEAVE);
					emit_simple(parser, MD4S_NEWLINE);
					parser->emitted_count++;
					parser->last_type = LINE_HEADING;
					parser->needs_separator = true;
					free(parser->deferred_line);
					parser->deferred_line = NULL;
					parser->deferred_len = 0;
					parser->line_len = 0;
					continue;
				}

				/* Check for table separator. */
				int dummy[TABLE_MAX_COLUMNS] = {0};
				int ncols = parse_table_separator(
					parser->line_buf, slen, dummy);
				if (ncols > 0) {
					/* Check deferred line has pipes. */
					bool has_pipe = false;
					for (size_t k = 0;
					     k < parser->deferred_len; k++) {
						if (parser->deferred_line[k] == '|') {
							has_pipe = true;
							break;
						}
					}
					if (has_pipe) {
						close_paragraph(parser);
						process_table_header(parser,
							parser->deferred_line,
							parser->deferred_len,
							parser->line_buf,
							parser->line_len);
						free(parser->deferred_line);
						parser->deferred_line = NULL;
						parser->deferred_len = 0;
						parser->line_len = 0;
						continue;
					}
				}

				/* Neither — flush deferred as normal. */
				char *dl = parser->deferred_line;
				size_t dlen = parser->deferred_len;
				parser->deferred_line = NULL;
				parser->deferred_len = 0;
				struct classified_line dcl = classify_line(
					parser, dl, dlen);
				if (dcl.type != LINE_PARAGRAPH)
					close_paragraph(parser);
				process_line(parser, dl, dlen);
				free(dl);
			}

			struct classified_line cl = classify_line(
				parser, parser->line_buf,
				parser->line_len);

			/* If in a table, check if this row continues it. */
			if (parser->in_table) {
				size_t tlen = parser->line_len;
				while (tlen > 0 &&
				       (parser->line_buf[tlen - 1] == '\n' ||
					parser->line_buf[tlen - 1] == '\r'))
					tlen--;
				if (tlen > 0 &&
				    count_table_cells(parser->line_buf,
						     tlen) > 0) {
					emit_table_row(parser,
						parser->line_buf, tlen,
						parser->table_alignments,
						parser->table_columns);
					parser->line_len = 0;
					continue;
				}
				/* Not a table row — close table. */
				close_table_if_needed(parser);
			}

			/* Defer pipe-containing paragraph lines for
			 * table detection. */
			if (cl.type == LINE_PARAGRAPH &&
			    parser->state == STATE_NORMAL &&
			    !parser->in_table) {
				bool has_pipe = false;
				for (size_t k = 0;
				     k < parser->line_len; k++) {
					if (parser->line_buf[k] == '|') {
						has_pipe = true;
						break;
					}
				}
				if (has_pipe) {
					parser->deferred_line =
						strndup(parser->line_buf,
							parser->line_len);
					parser->deferred_len =
						parser->line_len;
					parser->line_len = 0;
					continue;
				}
			}

			if (cl.type != LINE_PARAGRAPH)
				close_paragraph(parser);

			/* Process the completed line. */
			process_line(parser, parser->line_buf,
				     parser->line_len);
			parser->line_len = 0;
		}
	}

	/* Emit partial line preview if we have buffered content. */
	if (parser->line_len > 0) {
		if (parser->partial_displayed)
			emit_simple(parser, MD4S_PARTIAL_CLEAR);
		emit_text(parser, MD4S_PARTIAL_LINE, parser->line_buf,
			  parser->line_len);
		parser->partial_displayed = true;
	}
}

/* ------------------------------------------------------------------ */
/* Create / Finalize / Cancel / Destroy                               */
/* ------------------------------------------------------------------ */

struct md4s_parser *md4s_create(md4s_callback callback, void *user_data)
{
	if (callback == NULL)
		return NULL;

	struct md4s_parser *p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	p->callback = callback;
	p->user_data = user_data;
	p->state = STATE_NORMAL;

	p->line_buf = malloc(LINE_BUFFER_INITIAL);
	p->line_cap = LINE_BUFFER_INITIAL;
	p->line_len = 0;

	p->raw_buf = malloc(RAW_BUFFER_INITIAL);
	p->raw_cap = RAW_BUFFER_INITIAL;
	p->raw_len = 0;

	if (p->line_buf == NULL || p->raw_buf == NULL) {
		free(p->line_buf);
		free(p->raw_buf);
		free(p);
		return NULL;
	}

	p->line_buf[0] = '\0';
	p->raw_buf[0] = '\0';

	return p;
}

char *md4s_finalize(struct md4s_parser *parser)
{
	if (parser == NULL)
		return NULL;

	/* Clear partial display. */
	if (parser->partial_displayed) {
		emit_simple(parser, MD4S_PARTIAL_CLEAR);
		parser->partial_displayed = false;
	}

	/* Flush any deferred line (table lookahead). */
	if (parser->deferred_line != NULL) {
		struct classified_line dcl = classify_line(
			parser, parser->deferred_line,
			parser->deferred_len);
		if (dcl.type != LINE_PARAGRAPH)
			close_paragraph(parser);
		process_line(parser, parser->deferred_line,
			     parser->deferred_len);
		free(parser->deferred_line);
		parser->deferred_line = NULL;
		parser->deferred_len = 0;
	}

	/* Flush remaining line buffer as a completed line. */
	if (parser->line_len > 0) {
		/* Append a virtual newline for classification. */
		buf_ensure(&parser->line_buf, &parser->line_cap,
			   parser->line_len + 2);
		parser->line_buf[parser->line_len++] = '\n';
		parser->line_buf[parser->line_len] = '\0';

		struct classified_line cl = classify_line(
			parser, parser->line_buf, parser->line_len);
		if (cl.type != LINE_PARAGRAPH)
			close_paragraph(parser);

		process_line(parser, parser->line_buf, parser->line_len);
		parser->line_len = 0;
	}

	/* Close any open paragraph. */
	close_paragraph(parser);

	/* Close any open indented code block. */
	if (parser->last_type == LINE_INDENTED_CODE) {
		emit_simple(parser, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(parser, MD4S_NEWLINE);
	}

	/* Close any open table. */
	close_table_if_needed(parser);

	/* Close any open list. */
	if (parser->in_list) {
		emit_simple(parser, MD4S_LIST_LEAVE);
		parser->in_list = false;
	}

	/* Close any open code block. */
	if (parser->state == STATE_FENCED_CODE) {
		emit_simple(parser, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(parser, MD4S_NEWLINE);
		parser->state = STATE_NORMAL;
	}

	/* Close any open HTML block. */
	if (parser->state == STATE_HTML_BLOCK) {
		emit_simple(parser, MD4S_HTML_BLOCK_LEAVE);
		emit_simple(parser, MD4S_NEWLINE);
		parser->state = STATE_NORMAL;
	}

	if (parser->raw_len == 0)
		return NULL;

	return strndup(parser->raw_buf, parser->raw_len);
}

char *md4s_cancel(struct md4s_parser *parser)
{
	if (parser == NULL)
		return NULL;

	if (parser->partial_displayed) {
		emit_simple(parser, MD4S_PARTIAL_CLEAR);
		parser->partial_displayed = false;
	}

	if (parser->raw_len == 0)
		return NULL;

	return strndup(parser->raw_buf, parser->raw_len);
}

void md4s_destroy(struct md4s_parser *parser)
{
	if (parser == NULL)
		return;

	free(parser->line_buf);
	free(parser->raw_buf);
	free(parser->code_language);
	free(parser->deferred_line);
	for (int i = 0; i < parser->link_def_count; i++) {
		free(parser->link_defs[i].label);
		free(parser->link_defs[i].url);
	}
	free(parser);
}
