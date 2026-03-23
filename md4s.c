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
#define MAX_LIST_DEPTH        8

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

/* Per-level list tracking for the list stack. */
struct list_level {
	bool ordered;
	char marker_char;      /* Unordered: '-', '*', '+'. Ordered: '.' or ')' */
	int marker_indent;     /* Leading spaces before marker */
	int content_indent;    /* Column where content starts */
	int item_count;        /* Items emitted at this level */
	bool saw_blank;        /* Blank line seen between items */
	bool item_open;        /* An item is open at this level */
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

	/* List stack — replaces flat in_list/list_ordered/list_depth. */
	struct list_level list_stack[MAX_LIST_DEPTH];
	int list_depth;  /* 0 = not in any list, 1+ = nesting depth. */

	/* Multi-line list item tracking. */
	bool in_list_item;
	int list_item_content_indent;
	int list_item_marker_indent;
	bool list_item_blank_pending;  /* Blank line seen, item not yet closed. */

	/* Whether a partial line is currently displayed. */
	bool partial_displayed;

	/* Hard break: pending from previous paragraph line. */
	bool hard_break_pending;

	/* Paragraph close guard — prevents double PARAGRAPH_LEAVE. */
	bool paragraph_closed;

	/* True when close_paragraph was called for the just-preceding
	 * paragraph. Used by interruption rules in process_line. */
	bool was_paragraph;

	/* Table tracking. */
	bool in_table;
	int table_columns;
	int table_alignments[TABLE_MAX_COLUMNS]; /* 0=none,1=left,2=center,3=right */
	bool table_head_done;

	/* HTML block type tracking (1-7). */
	int html_block_type;

	/* Indented code: deferred blank lines (trailing blanks
	 * are not included in the code block per CommonMark). */
	int icode_pending_blanks;
	char *icode_pending_content[16]; /* Content of deferred blank lines */

	/* Configuration flags. */
	unsigned int flags;

	/* Blockquote content accumulator. */
	char *bq_buf;
	size_t bq_len;
	size_t bq_cap;
	bool in_blockquote;

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

/*
 * Count effective indentation, expanding tabs to 4-column tab stops.
 * Returns the effective indent. Sets *bytes_consumed to the number
 * of raw whitespace bytes consumed (can be NULL).
 */
static int count_indent(const char *s, size_t len, size_t *bytes_consumed)
{
	int indent = 0;
	size_t i = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t')) {
		if (s[i] == '\t')
			indent = (indent + 4) & ~3; /* next tab stop */
		else
			indent++;
		i++;
	}
	if (bytes_consumed)
		*bytes_consumed = i;
	return indent;
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
 * Three-level character classification for CommonMark flanking.
 * Returns 0 = whitespace/boundary, 1 = punctuation, 2 = other.
 *
 * When before is true, examines the codepoint ending at pos.
 * When false, examines the codepoint starting at pos.
 */
static int char_class(const char *text, size_t len, size_t pos, bool before)
{
	if (before && pos == 0) return 0;
	if (!before && pos >= len) return 0;

	uint32_t cp;
	if (before) {
		size_t prev = utf8_prev(text, len, pos);
		utf8_decode(text + prev, pos - prev, &cp);
	} else {
		utf8_decode(text + pos, len - pos, &cp);
	}

	if (cp == 0) return 0;
	if (gstr_is_whitespace_cp(cp)) return 0;
	if (gstr_is_unicode_punctuation(cp)) return 1;
	return 2;
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
 * Case-insensitive tag name match helper.
 * Returns true if tag at line[tag_start..tag_start+tag_len) matches name.
 */
static bool tag_name_eq(const char *line, size_t tag_start, size_t tag_len,
			const char *name)
{
	size_t nlen = strlen(name);
	if (nlen != tag_len)
		return false;
	for (size_t j = 0; j < tag_len; j++) {
		char a = line[tag_start + j];
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (a != name[j])
			return false;
	}
	return true;
}

/*
 * Detect HTML block type per CommonMark spec 4.6.
 * Returns 0 if not an HTML block, 1-7 for the type.
 *
 * in_paragraph: true if currently in an open paragraph (type 7 cannot
 * interrupt a paragraph).
 */
static int detect_html_block_type(const char *line, size_t len,
				  bool in_paragraph)
{
	if (len < 2 || line[0] != '<')
		return 0;

	/* Type 2: HTML comment <!-- */
	if (len >= 4 && line[1] == '!' && line[2] == '-' && line[3] == '-')
		return 2;

	/* Type 5: <![CDATA[ */
	if (len >= 9 && memcmp(line, "<![CDATA[", 9) == 0)
		return 5;

	/* Type 4: <! followed by uppercase letter */
	if (len >= 3 && line[1] == '!' && line[2] >= 'A' && line[2] <= 'Z')
		return 4;

	/* Type 3: <? processing instruction */
	if (line[1] == '?')
		return 3;

	/* Extract tag name for types 1, 6, 7. */
	size_t pos = 1;
	bool closing = false;
	if (pos < len && line[pos] == '/') {
		closing = true;
		pos++;
	}

	size_t tag_start = pos;
	while (pos < len && ((line[pos] >= 'a' && line[pos] <= 'z') ||
			     (line[pos] >= 'A' && line[pos] <= 'Z') ||
			     (pos > tag_start && line[pos] >= '0' &&
			      line[pos] <= '9')))
		pos++;
	size_t tag_len = pos - tag_start;
	if (tag_len == 0)
		return 0;

	/* Must be followed by space, tab, >, />, newline, or end. */
	if (pos < len && line[pos] != ' ' && line[pos] != '>' &&
	    line[pos] != '/' && line[pos] != '\t' && line[pos] != '\n')
		return 0;

	/* Type 1: script, pre, style, textarea */
	static const char *type1_tags[] = {
		"script", "pre", "style", "textarea", NULL
	};
	for (int i = 0; type1_tags[i] != NULL; i++) {
		if (tag_name_eq(line, tag_start, tag_len, type1_tags[i]))
			return 1;
	}

	/* Type 6: block-level tags */
	static const char *block_tags[] = {
		"address", "article", "aside", "base", "basefont",
		"blockquote", "body", "caption", "center", "col",
		"colgroup", "dd", "details", "dialog", "dir", "div",
		"dl", "dt", "fieldset", "figcaption", "figure",
		"footer", "form", "frame", "frameset", "h1", "h2",
		"h3", "h4", "h5", "h6", "head", "header", "hr",
		"html", "iframe", "legend", "li", "link", "main",
		"menu", "menuitem", "nav", "noframes", "ol",
		"optgroup", "option", "p", "param",
		"section", "source", "summary", "table",
		"tbody", "td", "template", "tfoot", "th", "thead",
		"title", "tr", "track", "ul", NULL
	};

	for (int i = 0; block_tags[i] != NULL; i++) {
		if (tag_name_eq(line, tag_start, tag_len, block_tags[i]))
			return 6;
	}

	/* Type 7: any complete open/close tag alone on a line.
	 * Cannot interrupt a paragraph. Tag must not be type 1 or 6
	 * (already checked above). */
	if (in_paragraph)
		return 0;

	if (closing) {
		/* Close tag: </ tagname optional-whitespace > */
		while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
			pos++;
		if (pos < len && line[pos] == '>') {
			pos++;
			/* Rest must be whitespace. */
			while (pos < len && (line[pos] == ' ' ||
					     line[pos] == '\t'))
				pos++;
			if (pos >= len)
				return 7;
		}
		return 0;
	}

	/* Open tag: < tagname attributes* optional-/ > */
	/* Skip attributes. */
	for (;;) {
		/* Skip whitespace. */
		bool had_ws = false;
		while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
			pos++;
			had_ws = true;
		}
		if (pos >= len)
			return 0;
		/* Check for end of tag. */
		if (line[pos] == '>') {
			pos++;
			break;
		}
		if (line[pos] == '/' && pos + 1 < len &&
		    line[pos + 1] == '>') {
			pos += 2;
			break;
		}
		/* Attribute name must follow whitespace. */
		if (!had_ws)
			return 0;
		/* Attribute name: [a-zA-Z_:][a-zA-Z0-9_.:-]* */
		if (!((line[pos] >= 'a' && line[pos] <= 'z') ||
		      (line[pos] >= 'A' && line[pos] <= 'Z') ||
		      line[pos] == '_' || line[pos] == ':'))
			return 0;
		pos++;
		while (pos < len && ((line[pos] >= 'a' && line[pos] <= 'z') ||
				     (line[pos] >= 'A' && line[pos] <= 'Z') ||
				     (line[pos] >= '0' && line[pos] <= '9') ||
				     line[pos] == '_' || line[pos] == '.' ||
				     line[pos] == ':' || line[pos] == '-'))
			pos++;
		/* Optional attribute value. */
		/* Skip whitespace. */
		while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
			pos++;
		if (pos < len && line[pos] == '=') {
			pos++;
			while (pos < len && (line[pos] == ' ' ||
					     line[pos] == '\t'))
				pos++;
			if (pos >= len)
				return 0;
			if (line[pos] == '"') {
				pos++;
				while (pos < len && line[pos] != '"')
					pos++;
				if (pos >= len)
					return 0;
				pos++; /* skip closing " */
			} else if (line[pos] == '\'') {
				pos++;
				while (pos < len && line[pos] != '\'')
					pos++;
				if (pos >= len)
					return 0;
				pos++; /* skip closing ' */
			} else {
				/* Unquoted value. */
				while (pos < len && line[pos] != ' ' &&
				       line[pos] != '\t' &&
				       line[pos] != '"' &&
				       line[pos] != '\'' &&
				       line[pos] != '=' &&
				       line[pos] != '<' &&
				       line[pos] != '>' &&
				       line[pos] != '`')
					pos++;
			}
		}
	}
	/* Rest of line must be whitespace. */
	while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
		pos++;
	if (pos >= len)
		return 7;
	return 0;
}

/*
 * Case-insensitive substring search for a closing tag like </script>.
 * Returns true if found anywhere in line.
 */
static bool contains_closing_tag(const char *line, size_t len,
				 const char *tag)
{
	size_t tlen = strlen(tag);
	/* We search for "</" + tag + ">" case-insensitively. */
	size_t need = 2 + tlen + 1; /* </ + tag + > */
	if (len < need)
		return false;
	for (size_t i = 0; i + need <= len; i++) {
		if (line[i] == '<' && line[i + 1] == '/') {
			bool match = true;
			for (size_t j = 0; j < tlen; j++) {
				char a = line[i + 2 + j];
				if (a >= 'A' && a <= 'Z')
					a += 32;
				if (a != tag[j]) {
					match = false;
					break;
				}
			}
			if (match && line[i + 2 + tlen] == '>')
				return true;
		}
	}
	return false;
}

/*
 * Check if a line ends an HTML block of the given type.
 * Per CommonMark spec 4.6:
 *   Type 1: closing tag </script>, </pre>, </style>, </textarea>
 *   Type 2: -->
 *   Type 3: ?>
 *   Type 4: >
 *   Type 5: ]]>
 *   Type 6: blank line
 *   Type 7: blank line
 */
static bool is_html_block_end(const char *line, size_t len,
			      int html_block_type)
{
	switch (html_block_type) {
	case 1: {
		static const char *type1_closers[] = {
			"script", "pre", "style", "textarea", NULL
		};
		for (int i = 0; type1_closers[i] != NULL; i++) {
			if (contains_closing_tag(line, len, type1_closers[i]))
				return true;
		}
		return false;
	}
	case 2:
		for (size_t i = 0; i + 2 < len; i++) {
			if (line[i] == '-' && line[i + 1] == '-' &&
			    line[i + 2] == '>')
				return true;
		}
		return false;
	case 3:
		for (size_t i = 0; i + 1 < len; i++) {
			if (line[i] == '?' && line[i + 1] == '>')
				return true;
		}
		return false;
	case 4:
		for (size_t i = 0; i < len; i++) {
			if (line[i] == '>')
				return true;
		}
		return false;
	case 5:
		for (size_t i = 0; i + 2 < len; i++) {
			if (line[i] == ']' && line[i + 1] == ']' &&
			    line[i + 2] == '>')
				return true;
		}
		return false;
	case 6:
	case 7:
		if (len == 0)
			return true;
		return false;
	default:
		return len == 0;
	}
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
	char list_marker;       /* '-', '*', '+', '.', ')' for list items. */
	char fence_char;        /* For fence open. */
	int fence_length;       /* For fence open. */
	const char *info_string; /* For fence open (language). */
	size_t info_length;
	int html_type;          /* HTML block type detected (1-7). */
	bool html_block_ends_here; /* This line closes the HTML block. */
};

/* Forward declarations for functions used before their definitions. */
static void close_paragraph(struct md4s_parser *p);
static void close_list_if_needed(struct md4s_parser *p, enum line_type type);
static void maybe_emit_separator(struct md4s_parser *p,
				 const struct classified_line *cl);

/*
 * Returns true if a paragraph is currently open (for paragraph
 * interruption rule checks).
 */
static bool in_open_paragraph(const struct md4s_parser *p)
{
	return p->last_type == LINE_PARAGRAPH &&
	       p->emitted_count > 0 &&
	       !p->needs_separator;
}

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
		/* Check for closing fence (allow 0-3 leading spaces). */
		size_t fi = count_leading_spaces(line, len);
		if (fi > 3) fi = 0;
		int n = count_leading(line + fi, len - fi, p->fence_char);
		if (n >= p->fence_length) {
			bool rest_blank = is_all_whitespace(
				line + fi + (size_t)n,
				len - fi - (size_t)n);
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

	/* Inside an HTML block — per-type end detection. */
	if (p->state == STATE_HTML_BLOCK) {
		/* Types 6,7 end on blank line. */
		if (p->html_block_type >= 6) {
			if (len == 0 || is_all_whitespace(line, len)) {
				cl.type = LINE_BLANK;
				return cl;
			}
		}
		/* Types 1-5 check for end marker in this line. */
		if (is_html_block_end(line, len, p->html_block_type)) {
			cl.type = LINE_HTML_BLOCK;
			cl.content = line;
			cl.content_length = len;
			cl.html_block_ends_here = true;
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

	/* Skip up to 3 leading spaces for block constructs. */
	size_t indent = count_leading_spaces(line, len);
	if (indent > 3) indent = 0; /* 4+ = indented code, don't skip */
	const char *lp = line + indent;
	size_t llen = len - indent;

	/* Fence open: 3+ backticks or tildes. */
	if (llen >= 3 && (lp[0] == '`' || lp[0] == '~')) {
		char ch = lp[0];
		int n = count_leading(lp, llen, ch);
		if (n >= 3) {
			bool valid = true;
			/* Backtick fences: info string must not contain backticks. */
			if (ch == '`') {
				for (size_t i = (size_t)n; i < llen; i++) {
					if (lp[i] == '`') {
						valid = false;
						break;
					}
				}
			}
			if (valid) {
				cl.type = LINE_FENCE_OPEN;
				cl.fence_char = ch;
				cl.fence_length = n;
				/* Extract info string: first word only
				 * (CommonMark spec 4.5). */
				const char *info = lp + n;
				size_t info_len = llen - (size_t)n;
				while (info_len > 0 && info[0] == ' ') {
					info++;
					info_len--;
				}
				while (info_len > 0 &&
				       info[info_len - 1] == ' ')
					info_len--;
				/* Truncate at first space (first word). */
				for (size_t fi = 0; fi < info_len; fi++) {
					if (info[fi] == ' ') {
						info_len = fi;
						break;
					}
				}
				cl.info_string = info;
				cl.info_length = info_len;
				return cl;
			}
		}
	}

	/* ATX heading: 1-6 '#' followed by space or end. */
	if (lp[0] == '#') {
		int level = count_leading(lp, llen, '#');
		if (level >= 1 && level <= 6 &&
		    ((size_t)level == llen || lp[level] == ' ')) {
			cl.type = LINE_HEADING;
			cl.heading_level = level;
			cl.content = lp + level;
			cl.content_length = llen - (size_t)level;
			/* Skip leading space after hashes. */
			if (cl.content_length > 0 && cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
			/* CommonMark closing sequence stripping:
			 * 1. Strip trailing spaces
			 * 2. If trailing '#' preceded by space (or
			 *    content is all '#'), strip them
			 * 3. Check for escaped '#' (\#)
			 * 4. Strip trailing spaces again
			 * 5. Strip leading spaces from content */
			/* Step 1: strip trailing spaces. */
			while (cl.content_length > 0 &&
			       cl.content[cl.content_length - 1] == ' ')
				cl.content_length--;
			/* Step 2: check for closing '#' sequence. */
			if (cl.content_length > 0 &&
			    cl.content[cl.content_length - 1] == '#') {
				/* Find where trailing '#' run starts. */
				size_t hend = cl.content_length;
				while (hend > 0 &&
				       cl.content[hend - 1] == '#')
					hend--;
				/* Check for escaped hash: \# at end
				 * means the '#' is literal. */
				if (hend > 0 &&
				    cl.content[hend - 1] == '\\') {
					/* Escaped — don't strip. */
				} else if (hend == 0 ||
					   cl.content[hend - 1] == ' ') {
					/* Valid closing sequence:
					 * preceded by space or
					 * content is all '#'. */
					cl.content_length = hend;
					/* Strip trailing spaces after
					 * removing hashes. */
					while (cl.content_length > 0 &&
					       cl.content[
						cl.content_length - 1]
					       == ' ')
						cl.content_length--;
				}
				/* else: '#' not preceded by space
				 * (e.g., "foo#"), keep it. */
			}
			/* Step 5: strip leading spaces. */
			while (cl.content_length > 0 &&
			       cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
			return cl;
		}
	}

	/* Indented code block: 4+ effective indent (tabs expand).
	 * Not inside list. Disabled by NOINDENTEDCODE flag.
	 * Must be checked BEFORE thematic break because 4-space-indented
	 * `***` is indented code, not a thematic break (example 48). */
	if (p->list_depth == 0 && !(p->flags & MD4S_FLAG_NOINDENTEDCODE) &&
	    !in_open_paragraph(p)) {
		size_t ws_bytes = 0;
		int eff_indent = count_indent(line, len, &ws_bytes);
		if (eff_indent >= 4 && ws_bytes > 0) {
			cl.type = LINE_INDENTED_CODE;
			/* Strip exactly 4 columns of indent. For simple
			 * spaces, skip 4 bytes. For tabs, skip the bytes
			 * consumed by count_indent but content starts at
			 * the point where 4 columns are consumed. */
			if (line[0] == '\t') {
				/* A leading tab provides 4 columns. */
				cl.content = line + 1;
				cl.content_length = len - 1;
			} else {
				/* Leading spaces: skip 4 bytes. */
				cl.content = line + 4;
				cl.content_length = len - 4;
			}
			return cl;
		}
	}

	/* Thematic break. */
	if (is_thematic_break(line, len)) {
		cl.type = LINE_THEMATIC_BREAK;
		return cl;
	}

	/* Blockquote: starts with '>'. */
	if (lp[0] == '>') {
		cl.type = LINE_BLOCKQUOTE;
		cl.content = lp + 1;
		cl.content_length = llen - 1;
		if (cl.content_length > 0 && cl.content[0] == ' ') {
			cl.content++;
			cl.content_length--;
		}
		return cl;
	}

	/* HTML block: detect type. Disabled by NOHTMLBLOCKS flag. */
	if (!(p->flags & MD4S_FLAG_NOHTMLBLOCKS)) {
		int html_type = detect_html_block_type(
			line, len, in_open_paragraph(p));
		if (html_type > 0) {
			cl.type = LINE_HTML_BLOCK;
			cl.html_type = html_type;
			cl.content = line;
			cl.content_length = len;
			return cl;
		}
	}

	/* Unordered list: optional indent, then [-*+] space. */
	{
		size_t ws_bytes = 0;
		int eff_indent = count_indent(line, len, &ws_bytes);
		if (ws_bytes < len - 1) {
			char marker = line[ws_bytes];
			if ((marker == '-' || marker == '*' ||
			     marker == '+') &&
			    ws_bytes + 1 < len && line[ws_bytes + 1] == ' ') {
				/* Make sure '-' isn't a thematic break. */
				if (marker != '-' || !is_thematic_break(line, len)) {
					cl.type = LINE_UNORDERED_LIST;
					cl.indent = eff_indent;
					cl.list_marker = marker;
					cl.content = line + ws_bytes + 2;
					cl.content_length =
						len - ws_bytes - 2;
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
		size_t ws_bytes = 0;
		int eff_indent = count_indent(line, len, &ws_bytes);
		size_t i = ws_bytes;
		while (i < len && i < ws_bytes + MAX_OLIST_DIGITS &&
		       line[i] >= '0' && line[i] <= '9')
			i++;
		if (i > ws_bytes && i < len &&
		    (line[i] == '.' || line[i] == ')') &&
		    i + 1 < len && line[i + 1] == ' ') {
			cl.type = LINE_ORDERED_LIST;
			cl.indent = eff_indent;
			cl.list_marker = line[i]; /* '.' or ')' */
			/* Parse the number. */
			cl.ordered_number = 0;
			for (size_t j = ws_bytes; j < i; j++)
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
 * Recursively parse and emit inline spans for a text region.
 */
static void parse_inline(struct md4s_parser *p, const char *text,
			 size_t length)
{
	parse_inline_depth(p, text, length, 0);
}

/* ------------------------------------------------------------------ */
/* Delimiter stack data structures                                     */
/* ------------------------------------------------------------------ */

#define MARK_OPENER   0x01
#define MARK_CLOSER   0x02
#define MARK_RESOLVED 0x04
#define MARK_BOTH     (MARK_OPENER | MARK_CLOSER)

struct md4s_mark {
	size_t pos;       /* Byte offset in text where this run starts. */
	size_t len;       /* Current (remaining) run length.            */
	size_t orig_len;  /* Original run length before splitting.      */
	char ch;          /* Delimiter character: '*', '_', or '~'.     */
	uint8_t flags;    /* MARK_OPENER | MARK_CLOSER | MARK_RESOLVED. */
	uint8_t mod3;     /* orig_len % 3, cached for rule-of-three.   */
};

struct md4s_emph_match {
	size_t open_pos;  /* Position of opening delimiter (inner edge). */
	size_t close_pos; /* Position of closing delimiter.              */
	uint8_t len;      /* 1 = italic, 2 = bold/strike.               */
	char ch;          /* '*', '_', or '~'.                           */
};

/* Segment types for eager constructs recorded during collection. */
enum seg_type {
	SEG_ESCAPE,       /* Backslash escape: emit text[pos+1] as TEXT. */
	SEG_ENTITY,       /* Entity reference: emit as ENTITY.           */
	SEG_CODE_SPAN,    /* Code span: emit ENTER/TEXT/LEAVE.           */
	SEG_HTML_INLINE,  /* Inline HTML: emit as HTML_INLINE.           */
	SEG_AUTOLINK,     /* Autolink: emit LINK_ENTER/TEXT/LINK_LEAVE.  */
	SEG_LINK,         /* Link [text](url): recursive text parsing.   */
	SEG_IMAGE,        /* Image ![alt](url): recursive text parsing.  */
};

struct seg_entry {
	enum seg_type type;
	size_t start;     /* Byte offset of segment start in text.       */
	size_t end;       /* Byte offset past segment end.               */
	/* For code spans: content start/len (after space stripping). */
	size_t cs_start;
	size_t cs_len;
	/* For links/images: URL and title pointers + bracket text range. */
	const char *url;
	size_t url_len;
	const char *title;
	size_t title_len;
	size_t link_text_start;
	size_t link_text_len;
};

/* Event types for the sorted emission list. */
struct pos_event {
	size_t pos;
	uint8_t skip;           /* Bytes to skip (delimiter length).    */
	enum md4s_event event;
	int order;              /* 0=LEAVE (first), 1=ENTER (second).   */
};

#define MARK_INITIAL_CAP  64
#define MARK_MAX_CAP      512
#define MATCH_INITIAL_CAP 64
#define MATCH_MAX_CAP     256
#define SEG_INITIAL_CAP   64
#define SEG_MAX_CAP       512
#define OPENER_BOTTOM_COUNT 13

static int opener_bottom_idx(char ch, uint8_t flags, uint8_t mod3)
{
	if (ch == '~') return 12;
	int ch_idx = (ch == '*') ? 0 : 1;
	int both = (flags & MARK_BOTH) == MARK_BOTH ? 1 : 0;
	return ch_idx * 6 + both * 3 + mod3;
}

static int pos_event_cmp(const void *a, const void *b)
{
	const struct pos_event *ea = (const struct pos_event *)a;
	const struct pos_event *eb = (const struct pos_event *)b;
	if (ea->pos != eb->pos)
		return (ea->pos < eb->pos) ? -1 : 1;
	if (ea->order != eb->order)
		return ea->order - eb->order;
	/* For same pos+order: LEAVEs (order=0) sort by skip ASC,
	 * ENTERs (order=1) sort by skip DESC. */
	if (ea->order == 0)
		return (int)ea->skip - (int)eb->skip;
	return (int)eb->skip - (int)ea->skip;
}

/*
 * process_emphasis: resolve delimiter marks into emphasis matches.
 * Implements the CommonMark algorithm with rule-of-three and
 * opener-bottom tracking.
 */
static int process_emphasis(struct md4s_mark *marks, int mark_count,
			    struct md4s_emph_match *matches, int match_cap)
{
	int match_count = 0;
	int opener_bottom[OPENER_BOTTOM_COUNT];
	for (int i = 0; i < OPENER_BOTTOM_COUNT; i++)
		opener_bottom[i] = -1;

	for (int ci = 0; ci < mark_count; ci++) {
		struct md4s_mark *closer = &marks[ci];
		if (!(closer->flags & MARK_CLOSER))
			continue;
		if (closer->flags & MARK_RESOLVED)
			continue;
		if (closer->len == 0)
			continue;

		int bottom_idx = opener_bottom_idx(closer->ch,
			closer->flags, closer->mod3);
		int bottom = opener_bottom[bottom_idx];

		bool found = false;
		for (int oi = ci - 1; oi > bottom; oi--) {
			struct md4s_mark *opener = &marks[oi];
			if (opener->ch != closer->ch)
				continue;
			if (!(opener->flags & MARK_OPENER))
				continue;
			if (opener->flags & MARK_RESOLVED)
				continue;
			if (opener->len == 0)
				continue;

			/* Rule of three. */
			if (closer->ch != '~') {
				if (((opener->flags & MARK_BOTH) == MARK_BOTH ||
				     (closer->flags & MARK_BOTH) == MARK_BOTH)) {
					if ((opener->orig_len + closer->orig_len) % 3 == 0 &&
					    opener->orig_len % 3 != 0 &&
					    closer->orig_len % 3 != 0)
						continue;
				}
			}

			/* Match found. Compute use_len. */
			size_t use_len;
			if (closer->ch == '~')
				use_len = 2;
			else
				use_len = (opener->len >= 2 &&
					   closer->len >= 2) ? 2 : 1;

			if (match_count < match_cap) {
				/* Opener gives rightmost chars. */
				size_t open_pos = opener->pos +
					opener->len - use_len;
				/* Closer gives leftmost chars. */
				size_t close_pos = closer->pos;

				matches[match_count].open_pos = open_pos;
				matches[match_count].close_pos = close_pos;
				matches[match_count].len = (uint8_t)use_len;
				matches[match_count].ch = closer->ch;
				match_count++;
			}

			opener->len -= use_len;
			closer->pos += use_len;
			closer->len -= use_len;

			if (opener->len == 0)
				opener->flags |= MARK_RESOLVED;

			/* Deactivate marks between opener and closer. */
			for (int k = oi + 1; k < ci; k++) {
				if (!(marks[k].flags & MARK_RESOLVED))
					marks[k].flags |= MARK_RESOLVED;
			}

			found = true;

			/* If closer still has length, re-process it. */
			if (closer->len > 0)
				ci--;
			else
				closer->flags |= MARK_RESOLVED;
			break;
		}

		if (!found) {
			/* Update opener bottom. */
			opener_bottom[bottom_idx] = ci - 1;
			/* If closer-only, remove opener flag. */
			if ((closer->flags & MARK_BOTH) != MARK_BOTH)
				closer->flags &= ~MARK_OPENER;
		}
	}

	return match_count;
}

static void parse_inline_depth(struct md4s_parser *p, const char *text,
			       size_t length, int depth)
{
	if (depth >= MAX_INLINE_DEPTH) {
		if (length > 0)
			emit_text(p, MD4S_TEXT, text, length);
		return;
	}

	/* ---- Allocate arrays ---- */
	struct md4s_mark mark_stack[MARK_INITIAL_CAP];
	struct md4s_mark *marks = mark_stack;
	int mark_count = 0;
	int mark_cap = MARK_INITIAL_CAP;
	bool marks_heap = false;

	struct seg_entry seg_stack[SEG_INITIAL_CAP];
	struct seg_entry *segs = seg_stack;
	int seg_count = 0;
	int seg_cap = SEG_INITIAL_CAP;
	bool segs_heap = false;

	/* ============================================================ */
	/* Phase 1: Collection                                          */
	/* ============================================================ */

	size_t pos = 0;
	while (pos < length) {
		/* Backslash escape. */
		if (text[pos] == '\\' && pos + 1 < length) {
			char next = text[pos + 1];
			if ((next >= 0x21 && next <= 0x2F) ||
			    (next >= 0x3A && next <= 0x40) ||
			    (next >= 0x5B && next <= 0x60) ||
			    (next >= 0x7B && next <= 0x7E)) {
				if (seg_count < seg_cap) {
					segs[seg_count].type = SEG_ESCAPE;
					segs[seg_count].start = pos;
					segs[seg_count].end = pos + 2;
					seg_count++;
				} else if (seg_cap < SEG_MAX_CAP) {
					int new_cap = seg_cap * 2;
					if (new_cap > SEG_MAX_CAP)
						new_cap = SEG_MAX_CAP;
					struct seg_entry *ns;
					if (segs_heap)
						ns = realloc(segs, (size_t)new_cap * sizeof(*segs));
					else {
						ns = malloc((size_t)new_cap * sizeof(*segs));
						if (ns) memcpy(ns, segs, (size_t)seg_count * sizeof(*segs));
					}
					if (ns) {
						segs = ns;
						seg_cap = new_cap;
						segs_heap = true;
						segs[seg_count].type = SEG_ESCAPE;
						segs[seg_count].start = pos;
						segs[seg_count].end = pos + 2;
						seg_count++;
					}
				}
				pos += 2;
				continue;
			}
		}

		/* Entity/character reference. */
		if (text[pos] == '&' && pos + 2 < length) {
			size_t estart = pos + 1;
			bool valid = false;
			if (text[estart] == '#') {
				size_t nstart = estart + 1;
				if (nstart < length &&
				    (text[nstart] == 'x' ||
				     text[nstart] == 'X')) {
					size_t d = nstart + 1;
					while (d < length && d < nstart + 7 &&
					       ((text[d] >= '0' && text[d] <= '9') ||
						(text[d] >= 'a' && text[d] <= 'f') ||
						(text[d] >= 'A' && text[d] <= 'F')))
						d++;
					if (d > nstart + 1 && d < length &&
					    text[d] == ';')
						valid = true;
				} else {
					size_t d = nstart;
					while (d < length && d < nstart + 7 &&
					       text[d] >= '0' && text[d] <= '9')
						d++;
					if (d > nstart && d < length &&
					    text[d] == ';')
						valid = true;
				}
			} else if ((text[estart] >= 'A' &&
				    text[estart] <= 'Z') ||
				   (text[estart] >= 'a' &&
				    text[estart] <= 'z')) {
				size_t d = estart + 1;
				while (d < length && d < estart + 48 &&
				       ((text[d] >= 'A' && text[d] <= 'Z') ||
					(text[d] >= 'a' && text[d] <= 'z') ||
					(text[d] >= '0' && text[d] <= '9')))
					d++;
				if (d > estart && d < length &&
				    text[d] == ';')
					valid = true;
			}
			if (valid) {
				size_t semi = estart;
				while (semi < length && text[semi] != ';')
					semi++;
				if (seg_count < seg_cap) {
					segs[seg_count].type = SEG_ENTITY;
					segs[seg_count].start = pos;
					segs[seg_count].end = semi + 1;
					seg_count++;
				}
				pos = semi + 1;
				continue;
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
				const char *cs = text + pos;
				size_t cs_len = close - pos;
				if (cs_len >= 2 && cs[0] == ' ' &&
				    cs[cs_len - 1] == ' ') {
					bool all_spaces = true;
					for (size_t si = 0; si < cs_len; si++) {
						if (cs[si] != ' ') {
							all_spaces = false;
							break;
						}
					}
					if (!all_spaces) {
						cs++;
						cs_len -= 2;
					}
				}
				if (seg_count < seg_cap) {
					segs[seg_count].type = SEG_CODE_SPAN;
					segs[seg_count].start = start;
					segs[seg_count].end = close + (size_t)ticks;
					segs[seg_count].cs_start = (size_t)(cs - text);
					segs[seg_count].cs_len = cs_len;
					seg_count++;
				}
				pos = close + (size_t)ticks;
				continue;
			}
			/* No close — literal backtick, advance one. */
			pos = start + 1;
			continue;
		}

		/* Inline HTML (skipped when NOHTMLSPANS is set). */
		if (!(p->flags & MD4S_FLAG_NOHTMLSPANS) &&
		    text[pos] == '<' && pos + 1 < length) {
			size_t hstart = pos + 1;
			size_t hend = 0;
			bool is_html = false;
			if (hstart + 2 < length && text[hstart] == '!' &&
			    text[hstart + 1] == '-' &&
			    text[hstart + 2] == '-') {
				size_t s = hstart + 3;
				while (s + 2 < length) {
					if (text[s] == '-' &&
					    text[s + 1] == '-' &&
					    text[s + 2] == '>') {
						hend = s + 3;
						is_html = true;
						break;
					}
					s++;
				}
			}
			else if (text[hstart] == '?') {
				size_t s = hstart + 1;
				while (s + 1 < length) {
					if (text[s] == '?' &&
					    text[s + 1] == '>') {
						hend = s + 2;
						is_html = true;
						break;
					}
					s++;
				}
			}
			else if (text[hstart] == '/' &&
				 hstart + 1 < length &&
				 ((text[hstart + 1] >= 'a' &&
				   text[hstart + 1] <= 'z') ||
				  (text[hstart + 1] >= 'A' &&
				   text[hstart + 1] <= 'Z'))) {
				size_t s = hstart + 2;
				while (s < length &&
				       ((text[s] >= 'a' && text[s] <= 'z') ||
					(text[s] >= 'A' && text[s] <= 'Z') ||
					(text[s] >= '0' && text[s] <= '9') ||
					text[s] == '-'))
					s++;
				while (s < length && text[s] == ' ')
					s++;
				if (s < length && text[s] == '>') {
					hend = s + 1;
					is_html = true;
				}
			}
			else if ((text[hstart] >= 'a' &&
				  text[hstart] <= 'z') ||
				 (text[hstart] >= 'A' &&
				  text[hstart] <= 'Z')) {
				bool looks_like_url = false;
				for (size_t ck = hstart; ck < length &&
				     text[ck] != '>'; ck++) {
					if (text[ck] == '@') {
						looks_like_url = true;
						break;
					}
					if (ck + 2 < length &&
					    text[ck] == ':' &&
					    text[ck + 1] == '/' &&
					    text[ck + 2] == '/') {
						looks_like_url = true;
						break;
					}
				}
				if (!looks_like_url) {
					size_t s = hstart + 1;
					while (s < length &&
					       ((text[s] >= 'a' && text[s] <= 'z') ||
						(text[s] >= 'A' && text[s] <= 'Z') ||
						(text[s] >= '0' && text[s] <= '9') ||
						text[s] == '-'))
						s++;
					while (s < length && text[s] != '>') {
						if (text[s] == '\'' || text[s] == '"') {
							char q = text[s++];
							while (s < length &&
							       text[s] != q)
								s++;
							if (s < length)
								s++;
						} else {
							s++;
						}
					}
					if (s < length && text[s] == '>') {
						hend = s + 1;
						is_html = true;
					}
				}
			}
			if (is_html) {
				if (seg_count < seg_cap) {
					segs[seg_count].type = SEG_HTML_INLINE;
					segs[seg_count].start = pos;
					segs[seg_count].end = hend;
					seg_count++;
				}
				pos = hend;
				continue;
			}
		}

		/* Autolink: <url> or <email>.
		 * Also skipped when NOHTMLSPANS is set. */
		if (!(p->flags & MD4S_FLAG_NOHTMLSPANS) &&
		    text[pos] == '<') {
			size_t close = pos + 1;
			while (close < length && text[close] != '>' &&
			       text[close] != ' ' && text[close] != '\n')
				close++;
			if (close < length && text[close] == '>' &&
			    close > pos + 1) {
				const char *inner = text + pos + 1;
				size_t inner_len = close - pos - 1;
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
					if (seg_count < seg_cap) {
						segs[seg_count].type = SEG_AUTOLINK;
						segs[seg_count].start = pos;
						segs[seg_count].end = close + 1;
						segs[seg_count].url = inner;
						segs[seg_count].url_len = inner_len;
						seg_count++;
					}
					pos = close + 1;
					continue;
				}
			}
		}

		/* GFM extended autolink: bare http:// or https:// URLs.
		 * Gated by MD4S_FLAG_GFM_AUTOLINKS.
		 * Skip if preceded by '<' (angle-bracket autolink). */
		if ((p->flags & MD4S_FLAG_GFM_AUTOLINKS) &&
		    text[pos] == 'h' && pos + 8 < length &&
		    (pos == 0 || text[pos - 1] != '<')) {
			const char *rest = text + pos;
			size_t remain = length - pos;
			size_t scheme_len = 0;
			if (remain >= 8 &&
			    rest[0] == 'h' && rest[1] == 't' &&
			    rest[2] == 't' && rest[3] == 'p' &&
			    rest[4] == 's' && rest[5] == ':' &&
			    rest[6] == '/' && rest[7] == '/')
				scheme_len = 8;
			else if (remain >= 7 &&
				 rest[0] == 'h' && rest[1] == 't' &&
				 rest[2] == 't' && rest[3] == 'p' &&
				 rest[4] == ':' && rest[5] == '/' &&
				 rest[6] == '/')
				scheme_len = 7;
			if (scheme_len > 0 && scheme_len < remain) {
				/* Scan for URL end: first whitespace
				 * or '<' or end of text. */
				size_t url_end = pos + scheme_len;
				while (url_end < length &&
				       text[url_end] != ' ' &&
				       text[url_end] != '\t' &&
				       text[url_end] != '\n' &&
				       text[url_end] != '\r' &&
				       text[url_end] != '<')
					url_end++;
				/* Strip trailing punctuation unless
				 * balanced parens. */
				while (url_end > pos + scheme_len) {
					char last = text[url_end - 1];
					if (last == '.' || last == ',' ||
					    last == ';' || last == ':' ||
					    last == '!' || last == '?' ||
					    last == '"' || last == '\'') {
						url_end--;
						continue;
					}
					if (last == ')') {
						/* Count parens in URL. */
						int open = 0, close = 0;
						for (size_t k = pos;
						     k < url_end; k++) {
							if (text[k] == '(')
								open++;
							else if (text[k] == ')')
								close++;
						}
						if (close > open) {
							url_end--;
							continue;
						}
					}
					break;
				}
				size_t url_len = url_end - pos;
				if (url_len > scheme_len) {
					if (seg_count < seg_cap) {
						segs[seg_count].type =
							SEG_AUTOLINK;
						segs[seg_count].start = pos;
						segs[seg_count].end = url_end;
						segs[seg_count].url =
							text + pos;
						segs[seg_count].url_len =
							url_len;
						seg_count++;
					}
					pos = url_end;
					continue;
				}
			}
		}

		/* Image or Link: ![alt](url) or [text](url). */
		if (text[pos] == '[' ||
		    (text[pos] == '!' && pos + 1 < length &&
		     text[pos + 1] == '[')) {
			bool is_image = (text[pos] == '!');
			size_t bracket_start = is_image ? pos + 1 : pos;
			size_t bracket = bracket_start + 1;
			int bdepth = 1;
			while (bracket < length && bdepth > 0) {
				if (text[bracket] == '\\' &&
				    bracket + 1 < length) {
					bracket += 2;
					continue;
				}
				if (text[bracket] == '[')
					bdepth++;
				else if (text[bracket] == ']')
					bdepth--;
				if (bdepth > 0)
					bracket++;
			}
			if (bracket < length && text[bracket] == ']' &&
			    bracket + 1 < length &&
			    text[bracket + 1] == '(') {
				size_t paren_start = bracket + 2;
				size_t paren = paren_start;
				const char *url_start = text + paren_start;
				size_t url_len = 0;
				const char *title_start = NULL;
				size_t title_len = 0;
				while (paren < length && text[paren] == ' ')
					paren++;
				size_t url_begin = paren;
				/* Angle-bracket URL: <url> */
				if (paren < length && text[paren] == '<') {
					paren++; /* skip < */
					url_begin = paren;
					while (paren < length &&
					       text[paren] != '>')
						paren++;
					if (paren < length) {
						url_start = text + url_begin;
						url_len = paren - url_begin;
						paren++; /* skip > */
					} else {
						/* No closing > — fail. */
						url_start = text + url_begin;
						url_len = 0;
					}
				} else {
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
				}
				if (paren < length && text[paren] == ' ') {
					while (paren < length &&
					       text[paren] == ' ')
						paren++;
					if (paren < length &&
					    (text[paren] == '"' ||
					     text[paren] == '\'' ||
					     text[paren] == '(')) {
						char open_ch = text[paren];
						char close_ch = (open_ch == '(')
							? ')' : open_ch;
						paren++;
						size_t ts = paren;
						while (paren < length &&
						       text[paren] != close_ch)
							paren++;
						if (paren < length) {
							title_start =
								text + ts;
							title_len =
								paren - ts;
							paren++;
						}
						while (paren < length &&
						       text[paren] == ' ')
							paren++;
					}
				}
				if (paren < length && text[paren] == ')') {
					if (seg_count < seg_cap) {
						segs[seg_count].type =
							is_image ? SEG_IMAGE : SEG_LINK;
						segs[seg_count].start = pos;
						segs[seg_count].end = paren + 1;
						segs[seg_count].url = url_start;
						segs[seg_count].url_len = url_len;
						segs[seg_count].title = title_start;
						segs[seg_count].title_len = title_len;
						segs[seg_count].link_text_start =
							bracket_start + 1;
						segs[seg_count].link_text_len =
							bracket - bracket_start - 1;
						seg_count++;
					}
					pos = paren + 1;
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
						label = text + bracket_start + 1;
						label_len = bracket -
							bracket_start - 1;
					} else {
						label = text + ref_start;
						label_len = ref_end - ref_start;
					}
					const char *url =
						find_link_def(p, label,
							      label_len);
					if (url != NULL) {
						if (seg_count < seg_cap) {
							segs[seg_count].type =
								is_image ? SEG_IMAGE : SEG_LINK;
							segs[seg_count].start = pos;
							segs[seg_count].end = ref_end + 1;
							segs[seg_count].url = url;
							segs[seg_count].url_len =
								strlen(url);
							segs[seg_count].title = NULL;
							segs[seg_count].title_len = 0;
							segs[seg_count].link_text_start =
								bracket_start + 1;
							segs[seg_count].link_text_len =
								bracket - bracket_start - 1;
							seg_count++;
						}
						pos = ref_end + 1;
						continue;
					}
				}
			}

			/* Shortcut reference: [text] alone. */
			if (bracket < length && text[bracket] == ']') {
				const char *label =
					text + bracket_start + 1;
				size_t label_len =
					bracket - bracket_start - 1;
				const char *url = find_link_def(
					p, label, label_len);
				if (url != NULL) {
					if (seg_count < seg_cap) {
						segs[seg_count].type =
							is_image ? SEG_IMAGE : SEG_LINK;
						segs[seg_count].start = pos;
						segs[seg_count].end = bracket + 1;
						segs[seg_count].url = url;
						segs[seg_count].url_len =
							strlen(url);
						segs[seg_count].title = NULL;
						segs[seg_count].title_len = 0;
						segs[seg_count].link_text_start =
							bracket_start + 1;
						segs[seg_count].link_text_len =
							label_len;
						seg_count++;
					}
					pos = bracket + 1;
					continue;
				}
			}

			if (is_image) {
				pos++;
				continue;
			}
		}

		/* Delimiter run: *, _, ~. */
		if (text[pos] == '*' || text[pos] == '_' ||
		    text[pos] == '~') {
			char ch = text[pos];
			size_t run_start = pos;
			while (pos < length && text[pos] == ch)
				pos++;
			size_t run_len = pos - run_start;

			/* Tilde: only runs of exactly 2 are valid,
			 * and only when strikethrough is enabled. */
			if (ch == '~' && (run_len != 2 ||
			    !(p->flags & MD4S_FLAG_STRIKETHROUGH))) {
				/* Treat as literal text — no mark. */
				continue;
			}

			/* Classify flanking. */
			int before = char_class(text, length,
						run_start, true);
			int after = char_class(text, length, pos, false);

			bool left_flanking = (after != 0) &&
				(after != 1 || before <= 1);
			bool right_flanking = (before != 0) &&
				(before != 1 || after <= 1);

			uint8_t flags = 0;
			if (ch == '_') {
				if (left_flanking &&
				    (!right_flanking || before == 1))
					flags |= MARK_OPENER;
				if (right_flanking &&
				    (!left_flanking || after == 1))
					flags |= MARK_CLOSER;
			} else {
				/* '*' and '~' */
				if (left_flanking)
					flags |= MARK_OPENER;
				if (right_flanking)
					flags |= MARK_CLOSER;
			}

			if (flags == 0)
				continue; /* Literal text. */

			/* Record the mark. */
			if (mark_count >= mark_cap) {
				if (mark_cap >= MARK_MAX_CAP)
					continue;
				int new_cap = mark_cap * 2;
				if (new_cap > MARK_MAX_CAP)
					new_cap = MARK_MAX_CAP;
				struct md4s_mark *nm;
				if (marks_heap)
					nm = realloc(marks,
						(size_t)new_cap * sizeof(*marks));
				else {
					nm = malloc(
						(size_t)new_cap * sizeof(*marks));
					if (nm)
						memcpy(nm, marks,
						       (size_t)mark_count *
						       sizeof(*marks));
				}
				if (!nm) continue;
				marks = nm;
				mark_cap = new_cap;
				marks_heap = true;
			}
			marks[mark_count].pos = run_start;
			marks[mark_count].len = run_len;
			marks[mark_count].orig_len = run_len;
			marks[mark_count].ch = ch;
			marks[mark_count].flags = flags;
			marks[mark_count].mod3 = (uint8_t)(run_len % 3);
			mark_count++;
			continue;
		}

		pos++;
	}

	/* ============================================================ */
	/* Phase 2: Resolution                                          */
	/* ============================================================ */

	struct md4s_emph_match match_stack[MATCH_INITIAL_CAP];
	struct md4s_emph_match *matches = match_stack;
	int match_cap = MATCH_INITIAL_CAP;

	int match_count = process_emphasis(marks, mark_count,
					   matches, match_cap);

	/* ============================================================ */
	/* Phase 3: Emission                                            */
	/* ============================================================ */

	/* Build sorted event list from matches. */
	int event_count = match_count * 2;
	struct pos_event *events = NULL;
	struct pos_event event_stack[128];
	bool events_heap = false;
	if (event_count <= 128) {
		events = event_stack;
	} else {
		events = malloc((size_t)event_count * sizeof(*events));
		if (!events) {
			events = event_stack;
			event_count = 0;
		} else {
			events_heap = true;
		}
	}

	for (int i = 0; i < match_count; i++) {
		enum md4s_event enter_ev, leave_ev;
		if (matches[i].ch == '~') {
			enter_ev = MD4S_STRIKETHROUGH_ENTER;
			leave_ev = MD4S_STRIKETHROUGH_LEAVE;
		} else if (matches[i].len == 2) {
			enter_ev = MD4S_BOLD_ENTER;
			leave_ev = MD4S_BOLD_LEAVE;
		} else {
			enter_ev = MD4S_ITALIC_ENTER;
			leave_ev = MD4S_ITALIC_LEAVE;
		}
		events[i * 2].pos = matches[i].open_pos;
		events[i * 2].skip = matches[i].len;
		events[i * 2].event = enter_ev;
		events[i * 2].order = 1;

		events[i * 2 + 1].pos = matches[i].close_pos;
		events[i * 2 + 1].skip = matches[i].len;
		events[i * 2 + 1].event = leave_ev;
		events[i * 2 + 1].order = 0;
	}

	if (event_count > 1)
		qsort(events, (size_t)event_count, sizeof(*events),
		      pos_event_cmp);

	/* Build a set of byte ranges consumed by resolved marks so
	 * that unresolved mark text emits as literal. We need to know
	 * which mark bytes were consumed vs which should be literal. */

	/* Walk text left-to-right, emitting text, segments, and events. */
	size_t cursor = 0;
	int ei = 0; /* event index */
	int si = 0; /* segment index */

	while (cursor < length) {
		/* Find the next event or segment. */
		size_t next_event_pos = (ei < event_count)
			? events[ei].pos : length;
		size_t next_seg_pos = (si < seg_count)
			? segs[si].start : length;

		/* Determine what comes next. */
		size_t next_pos = length;
		if (next_event_pos < next_pos)
			next_pos = next_event_pos;
		if (next_seg_pos < next_pos)
			next_pos = next_seg_pos;

		/* Emit plain text up to next_pos. */
		if (next_pos > cursor) {
			/* But skip over delimiter mark bytes that are
			 * part of unresolved marks (emit as literal). */
			emit_text(p, MD4S_TEXT, text + cursor,
				  next_pos - cursor);
			cursor = next_pos;
		}

		if (cursor >= length)
			break;

		/* Process all events at this position. */
		while (ei < event_count && events[ei].pos == cursor) {
			emit_simple(p, events[ei].event);
			/* Skip the delimiter bytes for this event.
			 * But only advance cursor if this is a position
			 * that we need to skip over (opener=ENTER,
			 * closer=LEAVE). The cursor tracks consumed text. */
			if (events[ei].order == 1) {
				/* ENTER: skip opener bytes. */
				cursor += events[ei].skip;
			} else {
				/* LEAVE: skip closer bytes. */
				cursor += events[ei].skip;
			}
			ei++;
		}

		/* Process segment at this position. */
		if (si < seg_count && segs[si].start == cursor) {
			struct seg_entry *seg = &segs[si];
			switch (seg->type) {
			case SEG_ESCAPE:
				emit_text(p, MD4S_TEXT,
					  &text[seg->start + 1], 1);
				break;
			case SEG_ENTITY:
				emit_text(p, MD4S_ENTITY,
					  text + seg->start,
					  seg->end - seg->start);
				break;
			case SEG_CODE_SPAN:
				emit_simple(p, MD4S_CODE_SPAN_ENTER);
				emit_text(p, MD4S_TEXT,
					  text + seg->cs_start,
					  seg->cs_len);
				emit_simple(p, MD4S_CODE_SPAN_LEAVE);
				break;
			case SEG_HTML_INLINE:
				emit_text(p, MD4S_HTML_INLINE,
					  text + seg->start,
					  seg->end - seg->start);
				break;
			case SEG_AUTOLINK: {
				struct md4s_detail d = {0};
				d.url = seg->url;
				d.url_length = seg->url_len;
				emit(p, MD4S_LINK_ENTER, &d);
				emit_text(p, MD4S_TEXT,
					  seg->url, seg->url_len);
				emit_simple(p, MD4S_LINK_LEAVE);
				break;
			}
			case SEG_LINK:
			case SEG_IMAGE: {
				struct md4s_detail d = {0};
				d.url = seg->url;
				d.url_length = seg->url_len;
				d.title = seg->title;
				d.title_length = seg->title_len;
				enum md4s_event enter =
					(seg->type == SEG_IMAGE)
					? MD4S_IMAGE_ENTER
					: MD4S_LINK_ENTER;
				enum md4s_event leave =
					(seg->type == SEG_IMAGE)
					? MD4S_IMAGE_LEAVE
					: MD4S_LINK_LEAVE;
				emit(p, enter, &d);
				parse_inline_depth(p,
					text + seg->link_text_start,
					seg->link_text_len,
					depth + 1);
				emit_simple(p, leave);
				break;
			}
			}
			cursor = seg->end;
			si++;
		}
	}

	/* Clean up. */
	if (marks_heap)
		free(marks);
	if (segs_heap)
		free(segs);
	if (events_heap)
		free(events);
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

/*
 * Close the item at the given list level if one is open.
 */
static void close_item_at_level(struct md4s_parser *p, int level)
{
	if (level < 0 || level >= p->list_depth)
		return;
	if (!p->list_stack[level].item_open)
		return;
	emit_simple(p, MD4S_LIST_ITEM_LEAVE);
	emit_simple(p, MD4S_NEWLINE);
	p->list_stack[level].item_open = false;
}

/*
 * Close the innermost open list item (convenience wrapper).
 */
static void close_list_item(struct md4s_parser *p)
{
	if (p->list_depth > 0)
		close_item_at_level(p, p->list_depth - 1);
	p->in_list_item = false;
	p->list_item_blank_pending = false;
}

/*
 * Pop list levels down to target_depth (0 = close all lists).
 * Closes open items and emits LIST_LEAVE for each popped level.
 */
static void close_lists_to_depth(struct md4s_parser *p, int target_depth)
{
	while (p->list_depth > target_depth) {
		/* Close any open item at this level. */
		close_item_at_level(p, p->list_depth - 1);
		emit_simple(p, MD4S_LIST_LEAVE);
		p->list_depth--;
	}
	p->in_list_item = false;
	p->list_item_blank_pending = false;
	/* If we still have levels open, check if the current level
	 * has an open item. */
	if (p->list_depth > 0 &&
	    p->list_stack[p->list_depth - 1].item_open) {
		p->in_list_item = true;
	}
}

static void close_list_if_needed(struct md4s_parser *p, enum line_type type)
{
	if (p->list_depth == 0)
		return;

	if (type != LINE_UNORDERED_LIST && type != LINE_ORDERED_LIST &&
	    type != LINE_BLANK) {
		close_lists_to_depth(p, 0);
	}
}

/*
 * Calculate the content indent for a list item.
 * Unordered: marker_indent + 2 (marker char + space)
 * Ordered: marker_indent + digit_count + 2 (digits + delimiter + space)
 */
static int calc_content_indent(const struct classified_line *cl)
{
	if (cl->type == LINE_UNORDERED_LIST) {
		return cl->indent + 2;
	} else {
		/* Count digits in ordered_number. */
		int n = cl->ordered_number;
		int digits = 0;
		do {
			digits++;
			n /= 10;
		} while (n > 0);
		return cl->indent + digits + 2;
	}
}

static void open_list_if_needed(struct md4s_parser *p,
				const struct classified_line *cl)
{
	bool ordered = (cl->type == LINE_ORDERED_LIST);
	int content_indent = calc_content_indent(cl);

	if (p->list_depth == 0) {
		/* No list open yet — start a new one. */
		struct md4s_detail d = {0};
		d.ordered = ordered;
		d.is_tight = true;  /* Optimistic: assume tight. */
		d.item_number = cl->ordered_number;
		emit(p, MD4S_LIST_ENTER, &d);
		p->list_depth = 1;
		struct list_level *lvl = &p->list_stack[0];
		lvl->ordered = ordered;
		lvl->marker_char = cl->list_marker;
		lvl->marker_indent = cl->indent;
		lvl->content_indent = content_indent;
		lvl->item_count = 0;
		lvl->saw_blank = false;
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
/* Blockquote accumulation and flush                                  */
/* ------------------------------------------------------------------ */

/*
 * Flush accumulated blockquote content: emit BLOCKQUOTE_ENTER,
 * recursively parse the collected lines as a sub-document,
 * then emit BLOCKQUOTE_LEAVE.
 */
static void flush_blockquote(struct md4s_parser *p)
{
	if (!p->in_blockquote || p->bq_len == 0) {
		p->in_blockquote = false;
		return;
	}

	emit_simple(p, MD4S_BLOCKQUOTE_ENTER);

	/* Create a sub-parser to process the blockquote content. */
	struct md4s_parser *sub = md4s_create_ex(
		p->callback, p->user_data, p->flags);
	if (sub != NULL) {
		/* Copy reference link definitions to sub-parser. */
		for (int i = 0; i < p->link_def_count; i++) {
			if (sub->link_def_count < MAX_LINK_DEFS) {
				sub->link_defs[sub->link_def_count].label =
					strdup(p->link_defs[i].label);
				sub->link_defs[sub->link_def_count].url =
					strdup(p->link_defs[i].url);
				sub->link_def_count++;
			}
		}
		md4s_feed(sub, p->bq_buf, p->bq_len);
		char *raw = md4s_finalize(sub);
		free(raw);
		md4s_destroy(sub);
	}

	emit_simple(p, MD4S_BLOCKQUOTE_LEAVE);

	p->bq_len = 0;
	p->in_blockquote = false;
}

/*
 * Append a line to the blockquote buffer.
 */
static void bq_append_line(struct md4s_parser *p,
			    const char *content, size_t len)
{
	if (p->bq_buf == NULL) {
		p->bq_cap = 256;
		p->bq_buf = malloc(p->bq_cap);
		if (!p->bq_buf) return;
		p->bq_len = 0;
	}
	buf_append(&p->bq_buf, &p->bq_len, &p->bq_cap, content, len);
	buf_append(&p->bq_buf, &p->bq_len, &p->bq_cap, "\n", 1);
}

/* ------------------------------------------------------------------ */
/* Process a single completed line                                    */
/* ------------------------------------------------------------------ */

static void process_line(struct md4s_parser *p, const char *line,
			 size_t raw_len)
{
	struct classified_line cl = classify_line(p, line, raw_len);

	/* ---- Multi-line list item continuation detection ---- */
	if (p->in_list_item && p->list_depth > 0) {
		size_t len = strip_newline(line, raw_len);

		if (cl.type == LINE_BLANK) {
			/* Blank line within a list item: don't close yet.
			 * Mark as pending — next line determines if the
			 * item continues or ends. Also mark list as loose. */
			p->list_item_blank_pending = true;
			struct list_level *lvl =
				&p->list_stack[p->list_depth - 1];
			lvl->saw_blank = true;
			p->needs_separator = true;
			return;
		}

		/* Check if this line is a continuation of the current item. */
		size_t ws_bytes = 0;
		int eff_indent = count_indent(line, len, &ws_bytes);

		/* A new list marker at the same indent as the current list
		 * is a sibling item, not a continuation. A list marker at
		 * greater indent could be a sub-list. */
		bool is_list_line = (cl.type == LINE_UNORDERED_LIST ||
				     cl.type == LINE_ORDERED_LIST);

		if (is_list_line && cl.indent == p->list_item_marker_indent) {
			/* Sibling item — fall through to normal processing. */
		} else if (is_list_line &&
			   cl.indent >= p->list_item_content_indent) {
			/* Sub-list — fall through to normal processing.
			 * The list case will handle nesting. */
		} else if (!is_list_line &&
			   eff_indent >= p->list_item_content_indent) {
			/* Continuation line: content indented enough to
			 * belong to the current item. */
			if (p->list_item_blank_pending) {
				/* Blank line before continuation makes
				 * this a loose list. The item continues
				 * but with paragraph separation. */
				p->list_item_blank_pending = false;
			}
			/* Strip content indent and emit as continuation. */
			const char *cont = line + ws_bytes;
			size_t cont_len = len - ws_bytes;
			/* Trim leading spaces beyond content_indent. */
			while (cont_len > 0 && cont[0] == ' ') {
				cont++;
				cont_len--;
			}
			emit_simple(p, MD4S_SOFTBREAK);
			parse_inline(p, cont, cont_len);
			p->emitted_count++;
			p->needs_separator = false;
			return;
		} else if (!is_list_line) {
			/* Line with insufficient indent — ends the item.
			 * Close lists back to where this line belongs. */
			/* Fall through to normal processing, which will
			 * call close_list_if_needed. */
		}
		/* If we had a pending blank and the next line is not
		 * a continuation, the blank ended the item. */
		p->list_item_blank_pending = false;
	}

	/* Paragraph interruption rules (CommonMark spec).
	 * Check was_paragraph too (set when paragraph was just closed
	 * before process_line, e.g. after deferred line flush). */
	if (in_open_paragraph(p) || p->was_paragraph) {
		/* Rule 1: Ordered list with start != 1 cannot interrupt. */
		if (cl.type == LINE_ORDERED_LIST && cl.ordered_number != 1) {
			cl.type = LINE_PARAGRAPH;
			cl.content = line;
			cl.content_length = strip_newline(line, raw_len);
			while (cl.content_length > 0 && cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
		}
		/* Rule 2: Empty list item cannot interrupt. */
		if ((cl.type == LINE_UNORDERED_LIST ||
		     cl.type == LINE_ORDERED_LIST) &&
		    cl.content_length == 0) {
			cl.type = LINE_PARAGRAPH;
			cl.content = line;
			cl.content_length = strip_newline(line, raw_len);
			while (cl.content_length > 0 && cl.content[0] == ' ') {
				cl.content++;
				cl.content_length--;
			}
		}
		/* Rule 3: HTML block type 7 cannot interrupt a paragraph.
		 * (Already handled by detect_html_block_type's
		 * in_paragraph parameter.) */
	}

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
	    p->last_type == LINE_BLOCKQUOTE && !p->needs_separator &&
	    p->in_blockquote) {
		/* Append to blockquote buffer as continuation. */
		bq_append_line(p, cl.content, cl.content_length);
		p->emitted_count++;
		p->needs_separator = false;
		return;
	}

	/* Close indented code block when transitioning to other types. */
	if (p->last_type == LINE_INDENTED_CODE &&
	    cl.type != LINE_INDENTED_CODE && cl.type != LINE_BLANK) {
		/* Discard trailing blank lines (they're deferred). */
		for (int bi = 0; bi < p->icode_pending_blanks && bi < 16; bi++) {
			free(p->icode_pending_content[bi]);
			p->icode_pending_content[bi] = NULL;
		}
		p->icode_pending_blanks = 0;
		emit_simple(p, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->needs_separator = true;
	}

	/* Setext heading detection: if we have an open paragraph and
	 * the current line is a setext underline, convert to heading.
	 * Per CommonMark, setext heading takes priority over thematic break.
	 * Note: single-line setext is handled in md4s_feed before
	 * close_paragraph fires. This fallback handles edge cases. */
	if (p->last_type == LINE_PARAGRAPH && p->emitted_count > 0 &&
	    !p->needs_separator && !p->paragraph_closed) {
		size_t slen = strip_newline(line, raw_len);
		int setext = is_setext_underline(line, slen);
		if (setext > 0) {
			close_paragraph(p);
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

	/* Flush accumulated blockquote when transitioning to non-bq. */
	if (p->in_blockquote && cl.type != LINE_BLOCKQUOTE) {
		flush_blockquote(p);
		p->needs_separator = true;
	}

	/* Clear was_paragraph after interruption rules have used it. */
	p->was_paragraph = false;

	/* Close any open paragraph before processing non-paragraph
	 * lines. This is done here (after interruption rules have
	 * had a chance to reclassify the line) instead of in
	 * md4s_feed, to ensure correct paragraph continuations. */
	if (cl.type != LINE_PARAGRAPH)
		close_paragraph(p);

	switch (cl.type) {
	case LINE_BLANK:
		/* Close HTML block on blank line (types 6,7 only).
		 * Types 1-5 are NOT closed by blank lines. */
		if (p->state == STATE_HTML_BLOCK &&
		    p->html_block_type >= 6) {
			emit_simple(p, MD4S_HTML_BLOCK_LEAVE);
			emit_simple(p, MD4S_NEWLINE);
			p->state = STATE_NORMAL;
			p->needs_separator = true;
			break;
		}
		/* Blank lines inside indented code: defer emission.
		 * Per CommonMark, trailing blank lines are not part
		 * of the code block, so we count them and only emit
		 * when followed by more code. */
		if (p->last_type == LINE_INDENTED_CODE) {
			if (p->icode_pending_blanks < 16) {
				size_t slen = strip_newline(line, raw_len);
				if (slen >= 4)
					p->icode_pending_content[
						p->icode_pending_blanks] =
						strndup(line + 4, slen - 4);
				else
					p->icode_pending_content[
						p->icode_pending_blanks] =
						strdup("");
			}
			p->icode_pending_blanks++;
			break;
		}
		/* Track blank lines for loose/tight list detection. */
		if (p->list_depth > 0) {
			struct list_level *lvl =
				&p->list_stack[p->list_depth - 1];
			lvl->saw_blank = true;
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
		if (!p->in_blockquote)
			maybe_emit_separator(p, &cl);
		/* Accumulate blockquote content for recursive parsing. */
		p->in_blockquote = true;
		bq_append_line(p, cl.content, cl.content_length);
		p->emitted_count++;
		p->last_type = LINE_BLOCKQUOTE;
		p->needs_separator = false;
		break;

	case LINE_UNORDERED_LIST:
	case LINE_ORDERED_LIST: {
		bool ordered = (cl.type == LINE_ORDERED_LIST);
		int content_indent = calc_content_indent(&cl);

		/* Determine relationship to existing list stack. */
		if (p->list_depth > 0) {
			struct list_level *cur =
				&p->list_stack[p->list_depth - 1];

			if (cl.indent >= cur->content_indent &&
			    p->list_depth < MAX_LIST_DEPTH) {
				/* Sub-list: marker is within the content
				 * region of the current item. Push a new
				 * list level. Don't close the outer item. */
				struct md4s_detail d = {0};
				d.ordered = ordered;
				d.is_tight = true;
				d.item_number = cl.ordered_number;
				emit(p, MD4S_LIST_ENTER, &d);
				p->list_depth++;
				struct list_level *nlvl =
					&p->list_stack[p->list_depth - 1];
				nlvl->ordered = ordered;
				nlvl->marker_char = cl.list_marker;
				nlvl->marker_indent = cl.indent;
				nlvl->content_indent = content_indent;
				nlvl->item_count = 0;
				nlvl->saw_blank = false;
			} else if (cl.indent < cur->marker_indent) {
				/* Dedent: pop levels until we find one
				 * whose marker_indent matches or is less. */
				while (p->list_depth > 1) {
					struct list_level *top =
						&p->list_stack[
							p->list_depth - 1];
					if (cl.indent >= top->marker_indent)
						break;
					close_list_item(p);
					emit_simple(p, MD4S_LIST_LEAVE);
					p->list_depth--;
				}
				cur = &p->list_stack[p->list_depth - 1];
				/* Check for list type/marker change. */
				if (cur->ordered != ordered ||
				    cur->marker_char != cl.list_marker) {
					close_list_item(p);
					emit_simple(p, MD4S_LIST_LEAVE);
					p->list_depth--;
					/* Open new list. */
					open_list_if_needed(p, &cl);
				} else {
					/* Sibling item in this list. */
					close_list_item(p);
				}
			} else if (cur->ordered != ordered ||
				   cur->marker_char != cl.list_marker) {
				/* Same indent but type/marker change:
				 * close old list, open new one. Per
				 * CommonMark, different markers (-, +, *)
				 * create separate lists. */
				close_list_item(p);
				emit_simple(p, MD4S_LIST_LEAVE);
				p->list_depth--;
				open_list_if_needed(p, &cl);
			} else {
				/* Sibling item at same indent. */
				close_list_item(p);
			}
		}

		maybe_emit_separator(p, &cl);
		if (p->list_depth == 0)
			open_list_if_needed(p, &cl);

		struct list_level *lvl =
			&p->list_stack[p->list_depth - 1];
		lvl->item_count++;

		{
			struct md4s_detail d = {0};
			d.item_number = cl.ordered_number;
			d.list_depth = p->list_depth - 1;
			/* Task list detection: [x], [X], or [ ] prefix.
			 * Only when TASKLISTS flag is enabled. */
			const char *item_text = cl.content;
			size_t item_len = cl.content_length;
			if ((p->flags & MD4S_FLAG_TASKLISTS) &&
			    item_len >= 3 && item_text[0] == '[' &&
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
		/* Don't emit LIST_ITEM_LEAVE yet — next line might
		 * continue this item (multi-line list items). */
		p->in_list_item = true;
		p->list_stack[p->list_depth - 1].item_open = true;
		p->list_item_content_indent = content_indent;
		p->list_item_marker_indent = cl.indent;
		p->list_item_blank_pending = false;
		p->emitted_count++;
		p->last_type = cl.type;
		p->needs_separator = false;
		break;
	}

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
		/* Emit any deferred blank lines (they were not
		 * trailing since more code follows). */
		for (int bi = 0; bi < p->icode_pending_blanks && bi < 16; bi++) {
			char *bc = p->icode_pending_content[bi];
			if (bc) {
				emit_text(p, MD4S_CODE_TEXT, bc, strlen(bc));
				free(bc);
				p->icode_pending_content[bi] = NULL;
			} else {
				emit_text(p, MD4S_CODE_TEXT, "", 0);
			}
			emit_simple(p, MD4S_NEWLINE);
		}
		p->icode_pending_blanks = 0;
		emit_text(p, MD4S_CODE_TEXT, cl.content,
			  cl.content_length);
		emit_simple(p, MD4S_NEWLINE);
		p->emitted_count++;
		p->last_type = LINE_INDENTED_CODE;
		p->needs_separator = false;
		break;

	case LINE_HTML_BLOCK:
		if (p->state == STATE_HTML_BLOCK) {
			/* Continuation line — emit text. */
			emit_text(p, MD4S_TEXT, cl.content,
				  cl.content_length);
			emit_simple(p, MD4S_NEWLINE);
			/* Check if this line ends the HTML block
			 * (types 1-5 have mid-line end markers). */
			if (cl.html_block_ends_here) {
				emit_simple(p, MD4S_HTML_BLOCK_LEAVE);
				emit_simple(p, MD4S_NEWLINE);
				p->state = STATE_NORMAL;
				p->needs_separator = true;
			}
		} else {
			/* Opening line. */
			close_table_if_needed(p);
			close_list_if_needed(p, cl.type);
			maybe_emit_separator(p, &cl);
			emit_simple(p, MD4S_HTML_BLOCK_ENTER);
			emit_text(p, MD4S_TEXT, cl.content,
				  cl.content_length);
			emit_simple(p, MD4S_NEWLINE);
			p->html_block_type = cl.html_type;
			/* Check if the opening line also contains the
			 * end marker (single-line HTML blocks). */
			if (is_html_block_end(cl.content, cl.content_length,
					      cl.html_type)) {
				/* Close immediately. */
				emit_simple(p, MD4S_HTML_BLOCK_LEAVE);
				emit_simple(p, MD4S_NEWLINE);
				p->state = STATE_NORMAL;
				p->needs_separator = true;
			} else {
				p->state = STATE_HTML_BLOCK;
			}
			p->emitted_count++;
			p->last_type = LINE_HTML_BLOCK;
			if (p->state == STATE_HTML_BLOCK)
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
		/* Consecutive paragraph lines: softbreak or hardbreak. */
		if (p->last_type == LINE_PARAGRAPH && !p->needs_separator &&
		    p->emitted_count > 0 && !p->paragraph_closed) {
			if (p->hard_break_pending) {
				emit_simple(p, MD4S_HARDBREAK);
				p->hard_break_pending = false;
			} else {
				emit_simple(p, MD4S_SOFTBREAK);
			}
			parse_inline(p, cl.content, cl.content_length);
		} else {
			p->hard_break_pending = false;
			maybe_emit_separator(p, &cl);
			emit_simple(p, MD4S_PARAGRAPH_ENTER);
			p->paragraph_closed = false;
			parse_inline(p, cl.content, cl.content_length);
		}
		/* Check for trailing hard break indicators. */
		{
			size_t clen = cl.content_length;
			const char *ct = cl.content;
			/* Backslash at end of content → hard break. */
			if (clen > 0 && ct[clen - 1] == '\\') {
				p->hard_break_pending = true;
			}
			/* Two or more trailing spaces → hard break. */
			else if (clen >= 2 && ct[clen - 1] == ' ' &&
				 ct[clen - 2] == ' ') {
				p->hard_break_pending = true;
			}
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
	if (p->last_type == LINE_PARAGRAPH && p->emitted_count > 0 &&
	    !p->paragraph_closed) {
		emit_simple(p, MD4S_PARAGRAPH_LEAVE);
		emit_simple(p, MD4S_NEWLINE);
		p->paragraph_closed = true;
		p->was_paragraph = true;
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
					/* Trim leading/trailing whitespace. */
					const char *htext = parser->deferred_line;
					while (hlen > 0 && (htext[0] == ' ' ||
					       htext[0] == '\t')) {
						htext++;
						hlen--;
					}
					while (hlen > 0 && (htext[hlen - 1] == ' ' ||
					       htext[hlen - 1] == '\t'))
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

				/* Check for table separator (only when
				 * tables are enabled). */
				int dummy[TABLE_MAX_COLUMNS] = {0};
				int ncols = (parser->flags & MD4S_FLAG_TABLES)
					? parse_table_separator(
						parser->line_buf, slen, dummy)
					: 0;
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

				/* Check if current line is a paragraph
				 * continuation. If so, append to deferred
				 * buffer for multi-line setext support. */
				struct classified_line ccl = classify_line(
					parser, parser->line_buf,
					parser->line_len);
				if (ccl.type == LINE_PARAGRAPH &&
				    parser->state == STATE_NORMAL &&
				    !parser->in_table) {
					/* Append current line to deferred. */
					size_t newlen = parser->deferred_len +
						parser->line_len;
					char *newbuf = realloc(
						parser->deferred_line,
						newlen + 1);
					if (newbuf) {
						memcpy(newbuf +
						       parser->deferred_len,
						       parser->line_buf,
						       parser->line_len);
						newbuf[newlen] = '\0';
						parser->deferred_line = newbuf;
						parser->deferred_len = newlen;
					}
					parser->line_len = 0;
					continue;
				}

				/* Not a continuation — flush deferred as
				 * normal. Process each line individually
				 * to preserve softbreaks/hardbreaks. */
				{
					char *dl = parser->deferred_line;
					size_t dlen = parser->deferred_len;
					parser->deferred_line = NULL;
					parser->deferred_len = 0;
					/* Process each line in the buffer. */
					size_t off = 0;
					while (off < dlen) {
						size_t end = off;
						while (end < dlen &&
						       dl[end] != '\n')
							end++;
						if (end < dlen)
							end++; /* include \n */
						process_line(parser,
							     dl + off,
							     end - off);
						off = end;
					}
					free(dl);
				}
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

			/* Defer first paragraph line for setext/table
			 * lookahead. A paragraph line that starts a new
			 * paragraph (not a continuation) is deferred so
			 * the next line can be checked for setext
			 * underlines or table separators. */
			if (cl.type == LINE_PARAGRAPH &&
			    parser->state == STATE_NORMAL &&
			    !parser->in_table &&
			    parser->deferred_line == NULL &&
			    (parser->last_type != LINE_PARAGRAPH ||
			     parser->needs_separator ||
			     parser->emitted_count == 0)) {
				parser->deferred_line =
					strndup(parser->line_buf,
						parser->line_len);
				parser->deferred_len =
					parser->line_len;
				parser->line_len = 0;
				continue;
			}

			/* Process the completed line. process_line handles
			 * paragraph closing internally so interruption
			 * rules can fire before the paragraph is closed. */
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
	return md4s_create_ex(callback, user_data, MD4S_FLAG_DEFAULT);
}

struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
				   unsigned int flags)
{
	if (callback == NULL)
		return NULL;

	struct md4s_parser *p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	p->callback = callback;
	p->user_data = user_data;
	p->state = STATE_NORMAL;
	p->flags = flags;

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

	/* Flush any deferred line(s) (table/setext lookahead).
	 * The buffer may contain multiple lines if paragraph
	 * continuation lines were accumulated. Process each
	 * line individually to preserve softbreaks. */
	if (parser->deferred_line != NULL) {
		char *dl = parser->deferred_line;
		size_t dlen = parser->deferred_len;
		parser->deferred_line = NULL;
		parser->deferred_len = 0;
		size_t off = 0;
		while (off < dlen) {
			size_t end = off;
			while (end < dlen && dl[end] != '\n')
				end++;
			if (end < dlen)
				end++; /* include \n */
			process_line(parser, dl + off, end - off);
			off = end;
		}
		free(dl);
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

	/* Flush any open blockquote. */
	if (parser->in_blockquote)
		flush_blockquote(parser);

	/* Close any open indented code block. */
	if (parser->last_type == LINE_INDENTED_CODE) {
		emit_simple(parser, MD4S_CODE_BLOCK_LEAVE);
		emit_simple(parser, MD4S_NEWLINE);
	}

	/* Close any open table. */
	close_table_if_needed(parser);

	/* Close any open list items and list levels. */
	close_lists_to_depth(parser, 0);

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
	free(parser->bq_buf);
	for (int i = 0; i < parser->link_def_count; i++) {
		free(parser->link_defs[i].label);
		free(parser->link_defs[i].url);
	}
	free(parser);
}
