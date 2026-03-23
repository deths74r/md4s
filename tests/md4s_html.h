/*
 * md4s_html.h — Single-header HTML renderer for md4s spec testing.
 *
 * Converts md4s parser events into an HTML string for comparison
 * against CommonMark spec expected output. Include this header in
 * exactly one translation unit.
 *
 * Usage:
 *   char *html = md4s_to_html(markdown, strlen(markdown));
 *   // compare html against expected
 *   free(html);
 */
#ifndef MD4S_HTML_H
#define MD4S_HTML_H

#include "../md4s.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Growable buffer                                                     */
/* ------------------------------------------------------------------ */

struct md4s_html {
	char *buf;
	size_t len;
	size_t cap;

	/* Renderer state */
	int heading_level;
	bool in_ordered_list;
	bool in_image;
	char alt_buf[1024];
	size_t alt_len;
	bool in_table_head;
	bool in_code_block;
	bool in_html_block;
	char image_url[1024];
	char image_title[1024];
};

static void html_grow(struct md4s_html *h, size_t need)
{
	if (h->len + need + 1 <= h->cap)
		return;
	size_t newcap = h->cap ? h->cap * 2 : 256;
	while (newcap < h->len + need + 1)
		newcap *= 2;
	h->buf = realloc(h->buf, newcap);
	h->cap = newcap;
}

static void html_append(struct md4s_html *h, const char *s, size_t n)
{
	if (n == 0)
		return;
	html_grow(h, n);
	memcpy(h->buf + h->len, s, n);
	h->len += n;
	h->buf[h->len] = '\0';
}

static void html_appends(struct md4s_html *h, const char *s)
{
	html_append(h, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* HTML escaping                                                       */
/* ------------------------------------------------------------------ */

static void html_append_escaped(struct md4s_html *h, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		switch (s[i]) {
		case '&':  html_appends(h, "&amp;");  break;
		case '<':  html_appends(h, "&lt;");   break;
		case '>':  html_appends(h, "&gt;");   break;
		case '"':  html_appends(h, "&quot;"); break;
		default:   html_append(h, &s[i], 1);  break;
		}
	}
}

/* Append escaped text to the alt buffer (for images). */
static void alt_append_escaped(struct md4s_html *h, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		size_t remaining = sizeof(h->alt_buf) - h->alt_len - 1;
		if (remaining == 0)
			break;
		switch (s[i]) {
		case '&':
			if (remaining >= 5) {
				memcpy(h->alt_buf + h->alt_len, "&amp;", 5);
				h->alt_len += 5;
			}
			break;
		case '<':
			if (remaining >= 4) {
				memcpy(h->alt_buf + h->alt_len, "&lt;", 4);
				h->alt_len += 4;
			}
			break;
		case '>':
			if (remaining >= 4) {
				memcpy(h->alt_buf + h->alt_len, "&gt;", 4);
				h->alt_len += 4;
			}
			break;
		case '"':
			if (remaining >= 6) {
				memcpy(h->alt_buf + h->alt_len, "&quot;", 6);
				h->alt_len += 6;
			}
			break;
		default:
			h->alt_buf[h->alt_len++] = s[i];
			break;
		}
	}
	h->alt_buf[h->alt_len] = '\0';
}

/* Append plain (unescaped) text to the alt buffer (for images). */
static void alt_append_plain(struct md4s_html *h, const char *s, size_t n)
{
	size_t remaining = sizeof(h->alt_buf) - h->alt_len - 1;
	if (n > remaining)
		n = remaining;
	memcpy(h->alt_buf + h->alt_len, s, n);
	h->alt_len += n;
	h->alt_buf[h->alt_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Event callback                                                      */
/* ------------------------------------------------------------------ */

static void md4s_html_callback(enum md4s_event event,
				const struct md4s_detail *detail,
				void *user_data)
{
	struct md4s_html *h = user_data;

	switch (event) {

	/* ---- Block-level events ---- */

	case MD4S_HEADING_ENTER: {
		char tag[8];
		h->heading_level = detail->heading_level;
		snprintf(tag, sizeof(tag), "<h%d>", detail->heading_level);
		html_appends(h, tag);
		break;
	}
	case MD4S_HEADING_LEAVE: {
		char tag[16];
		snprintf(tag, sizeof(tag), "</h%d>\n", h->heading_level);
		html_appends(h, tag);
		break;
	}

	case MD4S_PARAGRAPH_ENTER:
		html_appends(h, "<p>");
		break;
	case MD4S_PARAGRAPH_LEAVE:
		html_appends(h, "</p>\n");
		break;

	case MD4S_CODE_BLOCK_ENTER:
		h->in_code_block = true;
		if (detail->language && detail->language_length > 0) {
			html_appends(h, "<pre><code class=\"language-");
			html_append_escaped(h, detail->language,
					    detail->language_length);
			html_appends(h, "\">");
		} else {
			html_appends(h, "<pre><code>");
		}
		break;
	case MD4S_CODE_BLOCK_LEAVE:
		h->in_code_block = false;
		html_appends(h, "</code></pre>\n");
		break;

	case MD4S_BLOCKQUOTE_ENTER:
		html_appends(h, "<blockquote>\n");
		break;
	case MD4S_BLOCKQUOTE_LEAVE:
		html_appends(h, "</blockquote>\n");
		break;

	case MD4S_LIST_ENTER:
		h->in_ordered_list = detail->ordered;
		if (detail->ordered) {
			if (detail->item_number != 1) {
				char tag[32];
				snprintf(tag, sizeof(tag),
					 "<ol start=\"%d\">\n",
					 detail->item_number);
				html_appends(h, tag);
			} else {
				html_appends(h, "<ol>\n");
			}
		} else {
			html_appends(h, "<ul>\n");
		}
		break;
	case MD4S_LIST_LEAVE:
		if (h->in_ordered_list)
			html_appends(h, "</ol>\n");
		else
			html_appends(h, "</ul>\n");
		break;

	case MD4S_LIST_ITEM_ENTER:
		html_appends(h, "<li>");
		break;
	case MD4S_LIST_ITEM_LEAVE:
		html_appends(h, "</li>\n");
		break;

	case MD4S_THEMATIC_BREAK:
		html_appends(h, "<hr />\n");
		break;

	case MD4S_HTML_BLOCK_ENTER:
		h->in_html_block = true;
		break;
	case MD4S_HTML_BLOCK_LEAVE:
		h->in_html_block = false;
		break;

	/* ---- Table events ---- */

	case MD4S_TABLE_ENTER:
		html_appends(h, "<table>\n");
		break;
	case MD4S_TABLE_LEAVE:
		html_appends(h, "</table>\n");
		break;
	case MD4S_TABLE_HEAD_ENTER:
		h->in_table_head = true;
		html_appends(h, "<thead>\n");
		break;
	case MD4S_TABLE_HEAD_LEAVE:
		h->in_table_head = false;
		html_appends(h, "</thead>\n");
		break;
	case MD4S_TABLE_BODY_ENTER:
		html_appends(h, "<tbody>\n");
		break;
	case MD4S_TABLE_BODY_LEAVE:
		html_appends(h, "</tbody>\n");
		break;
	case MD4S_TABLE_ROW_ENTER:
		html_appends(h, "<tr>\n");
		break;
	case MD4S_TABLE_ROW_LEAVE:
		html_appends(h, "</tr>\n");
		break;
	case MD4S_TABLE_CELL_ENTER:
		if (h->in_table_head) {
			if (detail->cell_alignment == 1)
				html_appends(h, "<th style=\"text-align: left;\">");
			else if (detail->cell_alignment == 2)
				html_appends(h, "<th style=\"text-align: center;\">");
			else if (detail->cell_alignment == 3)
				html_appends(h, "<th style=\"text-align: right;\">");
			else
				html_appends(h, "<th>");
		} else {
			if (detail->cell_alignment == 1)
				html_appends(h, "<td style=\"text-align: left;\">");
			else if (detail->cell_alignment == 2)
				html_appends(h, "<td style=\"text-align: center;\">");
			else if (detail->cell_alignment == 3)
				html_appends(h, "<td style=\"text-align: right;\">");
			else
				html_appends(h, "<td>");
		}
		break;
	case MD4S_TABLE_CELL_LEAVE:
		if (h->in_table_head)
			html_appends(h, "</th>\n");
		else
			html_appends(h, "</td>\n");
		break;

	/* ---- Inline span events ---- */

	case MD4S_BOLD_ENTER:
		html_appends(h, "<strong>");
		break;
	case MD4S_BOLD_LEAVE:
		html_appends(h, "</strong>");
		break;

	case MD4S_ITALIC_ENTER:
		html_appends(h, "<em>");
		break;
	case MD4S_ITALIC_LEAVE:
		html_appends(h, "</em>");
		break;

	case MD4S_CODE_SPAN_ENTER:
		html_appends(h, "<code>");
		break;
	case MD4S_CODE_SPAN_LEAVE:
		html_appends(h, "</code>");
		break;

	case MD4S_STRIKETHROUGH_ENTER:
		html_appends(h, "<del>");
		break;
	case MD4S_STRIKETHROUGH_LEAVE:
		html_appends(h, "</del>");
		break;

	case MD4S_LINK_ENTER: {
		html_appends(h, "<a href=\"");
		if (detail->url && detail->url_length > 0)
			html_append_escaped(h, detail->url,
					    detail->url_length);
		html_appends(h, "\"");
		if (detail->title && detail->title_length > 0) {
			html_appends(h, " title=\"");
			html_append_escaped(h, detail->title,
					    detail->title_length);
			html_appends(h, "\"");
		}
		html_appends(h, ">");
		break;
	}
	case MD4S_LINK_LEAVE:
		html_appends(h, "</a>");
		break;

	case MD4S_IMAGE_ENTER: {
		h->in_image = true;
		h->alt_len = 0;
		h->alt_buf[0] = '\0';
		/* Save URL and title for IMAGE_LEAVE. */
		h->image_url[0] = '\0';
		h->image_title[0] = '\0';
		if (detail->url && detail->url_length > 0) {
			size_t n = detail->url_length;
			if (n > sizeof(h->image_url) - 1)
				n = sizeof(h->image_url) - 1;
			memcpy(h->image_url, detail->url, n);
			h->image_url[n] = '\0';
		}
		if (detail->title && detail->title_length > 0) {
			size_t n = detail->title_length;
			if (n > sizeof(h->image_title) - 1)
				n = sizeof(h->image_title) - 1;
			memcpy(h->image_title, detail->title, n);
			h->image_title[n] = '\0';
		}
		break;
	}
	case MD4S_IMAGE_LEAVE: {
		h->in_image = false;
		html_appends(h, "<img src=\"");
		html_append_escaped(h, h->image_url, strlen(h->image_url));
		html_appends(h, "\" alt=\"");
		/* alt_buf already contains escaped text. */
		html_append(h, h->alt_buf, h->alt_len);
		html_appends(h, "\"");
		if (h->image_title[0] != '\0') {
			html_appends(h, " title=\"");
			html_append_escaped(h, h->image_title,
					    strlen(h->image_title));
			html_appends(h, "\"");
		}
		html_appends(h, " />");
		break;
	}

	/* ---- Content events ---- */

	case MD4S_TEXT:
		if (detail->text && detail->text_length > 0) {
			if (h->in_image)
				alt_append_escaped(h, detail->text,
						   detail->text_length);
			else
				html_append_escaped(h, detail->text,
						    detail->text_length);
		}
		break;

	case MD4S_CODE_TEXT:
		if (detail->text && detail->text_length > 0)
			html_append_escaped(h, detail->text,
					    detail->text_length);
		break;

	case MD4S_ENTITY:
		/* Entity text is already HTML — output verbatim. */
		if (detail->text && detail->text_length > 0) {
			if (h->in_image)
				alt_append_plain(h, detail->text,
						 detail->text_length);
			else
				html_append(h, detail->text,
					    detail->text_length);
		}
		break;

	case MD4S_HTML_INLINE:
		/* Raw HTML — output verbatim. */
		if (detail->text && detail->text_length > 0)
			html_append(h, detail->text, detail->text_length);
		break;

	case MD4S_SOFTBREAK:
		html_appends(h, "\n");
		break;

	case MD4S_HARDBREAK:
		html_appends(h, "<br />\n");
		break;

	/* ---- Structural / ignored events ---- */

	case MD4S_NEWLINE:
		/* Inside code blocks and HTML blocks, structural newlines
		 * correspond to actual line breaks in the output. */
		if (h->in_code_block || h->in_html_block)
			html_appends(h, "\n");
		break;

	case MD4S_BLOCK_SEPARATOR:
	case MD4S_PARTIAL_LINE:
	case MD4S_PARTIAL_CLEAR:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Convenience: markdown → HTML string                                 */
/* ------------------------------------------------------------------ */

static char *md4s_to_html(const char *markdown, size_t length)
{
	struct md4s_html h = {0};
	html_grow(&h, 256);
	h.buf[0] = '\0';

	struct md4s_parser *p = md4s_create(md4s_html_callback, &h);
	if (!p) {
		free(h.buf);
		return NULL;
	}

	md4s_feed(p, markdown, length);
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);

	if (h.buf == NULL) {
		h.buf = malloc(1);
		h.buf[0] = '\0';
	}
	return h.buf;
}

#endif /* MD4S_HTML_H */
