/*
 * md4s — Markdown for Streaming
 *
 * A streaming markdown parser for C. Processes markdown text
 * incrementally — token by token — and emits semantic events
 * through a callback interface. No cursor-up, no re-rendering,
 * no batch processing.
 *
 * Inspired by md4c (https://github.com/mity/md4c) by Martin Mitáš.
 *
 * Usage:
 *   struct md4s_parser *p = md4s_create(my_callback, my_data);
 *   md4s_feed(p, chunk1, len1);
 *   md4s_feed(p, chunk2, len2);
 *   char *raw = md4s_finalize(p);
 *   free(raw);
 *   md4s_destroy(p);
 *
 * License: MIT
 */
#ifndef MD4S_H
#define MD4S_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

/*
 * Semantic events emitted by the parser. Block-level events come in
 * ENTER/LEAVE pairs. Inline span events also pair. Content events
 * (TEXT, CODE_TEXT) carry the actual text payload.
 */
enum md4s_event {
	/* Block-level events. */
	MD4S_HEADING_ENTER,
	MD4S_HEADING_LEAVE,
	MD4S_PARAGRAPH_ENTER,
	MD4S_PARAGRAPH_LEAVE,
	MD4S_CODE_BLOCK_ENTER,
	MD4S_CODE_BLOCK_LEAVE,
	MD4S_BLOCKQUOTE_ENTER,
	MD4S_BLOCKQUOTE_LEAVE,
	MD4S_LIST_ENTER,
	MD4S_LIST_LEAVE,
	MD4S_LIST_ITEM_ENTER,
	MD4S_LIST_ITEM_LEAVE,
	MD4S_THEMATIC_BREAK,
	MD4S_TABLE_ENTER,
	MD4S_TABLE_LEAVE,
	MD4S_TABLE_HEAD_ENTER,
	MD4S_TABLE_HEAD_LEAVE,
	MD4S_TABLE_BODY_ENTER,
	MD4S_TABLE_BODY_LEAVE,
	MD4S_TABLE_ROW_ENTER,
	MD4S_TABLE_ROW_LEAVE,
	MD4S_TABLE_CELL_ENTER,
	MD4S_TABLE_CELL_LEAVE,
	MD4S_HTML_BLOCK_ENTER,
	MD4S_HTML_BLOCK_LEAVE,

	/* Inline span events. */
	MD4S_BOLD_ENTER,
	MD4S_BOLD_LEAVE,
	MD4S_ITALIC_ENTER,
	MD4S_ITALIC_LEAVE,
	MD4S_CODE_SPAN_ENTER,
	MD4S_CODE_SPAN_LEAVE,
	MD4S_STRIKETHROUGH_ENTER,
	MD4S_STRIKETHROUGH_LEAVE,
	MD4S_LINK_ENTER,
	MD4S_LINK_LEAVE,
	MD4S_IMAGE_ENTER,
	MD4S_IMAGE_LEAVE,

	/* Content events. */
	MD4S_TEXT,
	MD4S_CODE_TEXT,
	MD4S_ENTITY,
	MD4S_HTML_INLINE,
	MD4S_SOFTBREAK,
	MD4S_HARDBREAK,
	MD4S_NEWLINE,
	MD4S_BLOCK_SEPARATOR,

	/* Partial line events. */
	MD4S_PARTIAL_LINE,
	MD4S_PARTIAL_CLEAR,
};

/* ------------------------------------------------------------------ */
/* Configuration flags                                                */
/* ------------------------------------------------------------------ */

#define MD4S_FLAG_TABLES          0x0001  /* GFM tables (default: on) */
#define MD4S_FLAG_STRIKETHROUGH   0x0002  /* ~~strikethrough~~ (default: on) */
#define MD4S_FLAG_TASKLISTS       0x0004  /* [x] task lists (default: on) */
#define MD4S_FLAG_NOHTMLBLOCKS    0x0008  /* Disable HTML blocks */
#define MD4S_FLAG_NOHTMLSPANS     0x0010  /* Disable inline HTML (<tag>) */
#define MD4S_FLAG_NOINDENTEDCODE  0x0020  /* Disable indented code blocks */
#define MD4S_FLAG_GFM_AUTOLINKS  0x0040  /* Bare URL autolinks (default: on) */

#define MD4S_FLAG_DEFAULT  (MD4S_FLAG_TABLES | MD4S_FLAG_STRIKETHROUGH | \
                            MD4S_FLAG_TASKLISTS | MD4S_FLAG_GFM_AUTOLINKS)

/* ------------------------------------------------------------------ */
/* Event detail                                                       */
/* ------------------------------------------------------------------ */

/*
 * Additional data carried by certain events. Only the fields
 * relevant to the current event are meaningful; others are
 * zero/NULL. Pointers are valid only for the callback duration.
 */
struct md4s_detail {
	/* HEADING_ENTER: heading level (1-6). */
	int heading_level;

	/* CODE_BLOCK_ENTER: language from info string (NULL if none). */
	const char *language;
	size_t language_length;

	/* LINK_ENTER, IMAGE_ENTER: URL string. */
	const char *url;
	size_t url_length;

	/* LINK_ENTER, IMAGE_ENTER: title string (NULL if none). */
	const char *title;
	size_t title_length;

	/* LIST_ENTER: true = ordered, false = unordered. */
	bool ordered;

	/* LIST_ENTER: true = tight list (no paragraph wrappers needed). */
	bool is_tight;

	/* LIST_ITEM_ENTER: 1-based item number for ordered lists. */
	int item_number;

	/* LIST_ITEM_ENTER: nesting depth (0 = top-level). */
	int list_depth;

	/* LIST_ITEM_ENTER: true if task list checkbox checked. */
	bool task_checked;

	/* LIST_ITEM_ENTER: true if this is a task list item. */
	bool is_task;

	/* TABLE_CELL_ENTER: column alignment (0=none, 1=left, 2=center, 3=right). */
	int cell_alignment;

	/* TABLE_ENTER: number of columns. */
	int column_count;

	/* TEXT, CODE_TEXT, PARTIAL_LINE: text content. */
	const char *text;
	size_t text_length;
};

/* ------------------------------------------------------------------ */
/* Callback                                                           */
/* ------------------------------------------------------------------ */

/*
 * Callback invoked for every event. The detail pointer is valid
 * only for the duration of the call. The consumer must copy any
 * data it needs to retain.
 */
typedef void (*md4s_callback)(
	enum md4s_event event,
	const struct md4s_detail *detail,
	void *user_data);

/* ------------------------------------------------------------------ */
/* Parser handle                                                      */
/* ------------------------------------------------------------------ */

struct md4s_parser;

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * Creates a streaming markdown parser.
 *
 * callback  — Event handler. Must not be NULL.
 * user_data — Opaque pointer forwarded to every callback.
 *
 * Returns the parser handle, or NULL on allocation failure.
 */
struct md4s_parser *md4s_create(md4s_callback callback, void *user_data);

/*
 * Creates a streaming markdown parser with configuration flags.
 *
 * callback  — Event handler. Must not be NULL.
 * user_data — Opaque pointer forwarded to every callback.
 * flags     — Bitwise OR of MD4S_FLAG_* constants.
 *
 * Returns the parser handle, or NULL on allocation failure.
 */
struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
                                   unsigned int flags);

/*
 * Feeds raw markdown bytes to the parser. May be called any number
 * of times with arbitrarily sized chunks. Callbacks fire
 * synchronously during this call for any completed lines.
 */
void md4s_feed(struct md4s_parser *parser, const char *data, size_t length);

/*
 * Finalizes the stream. Flushes any partial line, closes open
 * blocks, emits final events. Returns the accumulated raw
 * markdown (caller must free). Returns NULL if nothing was fed.
 */
char *md4s_finalize(struct md4s_parser *parser);

/*
 * Cancels without finalizing. Returns the raw markdown accumulated
 * so far (caller must free). No closing events are emitted.
 */
char *md4s_cancel(struct md4s_parser *parser);

/*
 * Destroys the parser and frees all internal buffers.
 * Passing NULL is safe.
 */
void md4s_destroy(struct md4s_parser *parser);

#ifdef __cplusplus
}
#endif

#endif /* MD4S_H */
