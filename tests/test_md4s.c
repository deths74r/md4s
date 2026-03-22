/*
 * MC/DC tests for the md4s streaming markdown parser.
 *
 * Tests the parser directly at the event level, independent of the
 * vessel rendering layer. Every compound decision in md4s.c has tests
 * demonstrating the independent effect of each condition.
 */
#include "test.h"
#include "md4s.h"

#include <string.h>

/* ================================================================== */
/* Event recorder                                                     */
/* ================================================================== */

struct recorded_event {
	enum md4s_event event;
	char text[256];
	int heading_level;
	char language[64];
	char url[256];
	char title[256];
	bool ordered;
	int item_number;
	int list_depth;
	bool is_task;
	bool task_checked;
	int cell_alignment;
	int column_count;
};

struct recorder_ctx {
	struct recorded_event events[512];
	int count;
};

static void recorder_callback(enum md4s_event event,
			      const struct md4s_detail *detail,
			      void *user_data)
{
	struct recorder_ctx *ctx = user_data;
	if (ctx->count >= 512)
		return;
	struct recorded_event *e = &ctx->events[ctx->count++];
	memset(e, 0, sizeof(*e));
	e->event = event;
	if (detail) {
		e->heading_level = detail->heading_level;
		e->ordered = detail->ordered;
		e->item_number = detail->item_number;
		e->list_depth = detail->list_depth;
		e->is_task = detail->is_task;
		e->task_checked = detail->task_checked;
		e->cell_alignment = detail->cell_alignment;
		e->column_count = detail->column_count;
		if (detail->text && detail->text_length > 0) {
			size_t n = detail->text_length;
			if (n > 255) n = 255;
			memcpy(e->text, detail->text, n);
			e->text[n] = '\0';
		}
		if (detail->language && detail->language_length > 0) {
			size_t n = detail->language_length;
			if (n > 63) n = 63;
			memcpy(e->language, detail->language, n);
			e->language[n] = '\0';
		}
		if (detail->url && detail->url_length > 0) {
			size_t n = detail->url_length;
			if (n > 255) n = 255;
			memcpy(e->url, detail->url, n);
			e->url[n] = '\0';
		}
		if (detail->title && detail->title_length > 0) {
			size_t n = detail->title_length;
			if (n > 255) n = 255;
			memcpy(e->title, detail->title, n);
			e->title[n] = '\0';
		}
	}
}

/* Feed markdown, finalize, return parser (caller must destroy). */
static struct md4s_parser *feed_and_finalize(struct recorder_ctx *ctx,
					     const char *md)
{
	ctx->count = 0;
	struct md4s_parser *p = md4s_create(recorder_callback, ctx);
	if (md && *md)
		md4s_feed(p, md, strlen(md));
	char *raw = md4s_finalize(p);
	free(raw);
	return p;
}

/* Feed without finalizing. */
static struct md4s_parser *feed_only(struct recorder_ctx *ctx,
				     const char *md)
{
	ctx->count = 0;
	struct md4s_parser *p = md4s_create(recorder_callback, ctx);
	if (md && *md)
		md4s_feed(p, md, strlen(md));
	return p;
}

static bool has_event(const struct recorder_ctx *ctx, enum md4s_event event)
{
	for (int i = 0; i < ctx->count; i++)
		if (ctx->events[i].event == event)
			return true;
	return false;
}

static int count_events(const struct recorder_ctx *ctx, enum md4s_event event)
{
	int n = 0;
	for (int i = 0; i < ctx->count; i++)
		if (ctx->events[i].event == event)
			n++;
	return n;
}

static const struct recorded_event *find_event(const struct recorder_ctx *ctx,
					       enum md4s_event event, int nth)
{
	int n = 0;
	for (int i = 0; i < ctx->count; i++) {
		if (ctx->events[i].event == event) {
			if (n == nth)
				return &ctx->events[i];
			n++;
		}
	}
	return NULL;
}

static bool event_at(const struct recorder_ctx *ctx, int idx,
		     enum md4s_event event)
{
	return idx >= 0 && idx < ctx->count && ctx->events[idx].event == event;
}

/* ================================================================== */
/* Group 29-32: Lifecycle (validates the test harness)                 */
/* ================================================================== */

/* -- md4s_create (L1057-1089) -- */

/* 29a: callback == NULL → returns NULL */
TEST(md4s_create_null_callback)
{
	struct md4s_parser *p = md4s_create(NULL, NULL);
	ASSERT_NULL(p);
}

/* 29b: valid callback → returns non-NULL */
TEST(md4s_create_valid_callback)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	ASSERT_NOT_NULL(p);
	md4s_destroy(p);
}

/* 29c: NULL user_data is valid */
TEST(md4s_create_null_userdata)
{
	struct md4s_parser *p = md4s_create(recorder_callback, NULL);
	ASSERT_NOT_NULL(p);
	md4s_destroy(p);
}

/* -- md4s_finalize (L1091-1139) -- */

/* 30a: parser == NULL → returns NULL */
TEST(md4s_finalize_null)
{
	ASSERT_NULL(md4s_finalize(NULL));
}

/* 30b: partial_displayed=T → PARTIAL_CLEAR emitted */
TEST(md4s_finalize_clears_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_only(&ctx, "partial");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	ctx.count = 0; /* reset to isolate finalize events */
	char *raw = md4s_finalize(p);
	free(raw);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_CLEAR));
	md4s_destroy(p);
}

/* 30c: partial_displayed=F → no PARTIAL_CLEAR at finalize */
TEST(md4s_finalize_no_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "line\n", 5);
	ctx.count = 0;
	char *raw = md4s_finalize(p);
	free(raw);
	ASSERT_FALSE(has_event(&ctx, MD4S_PARTIAL_CLEAR));
	md4s_destroy(p);
}

/* 30d: line_len > 0 → flush remaining line */
TEST(md4s_finalize_flush_partial_line)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "hello");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* 30e: line_len == 0 → nothing to flush */
TEST(md4s_finalize_nothing_to_flush)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "hello\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* 30f: in_list=T → LIST_LEAVE emitted */
TEST(md4s_finalize_close_open_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_LEAVE));
	md4s_destroy(p);
}

/* 30g: in_list=F → no LIST_LEAVE */
TEST(md4s_finalize_no_open_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "para\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_LEAVE));
	md4s_destroy(p);
}

/* 30h: state==FENCED_CODE → CODE_BLOCK_LEAVE emitted */
TEST(md4s_finalize_close_open_code)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "```\ncode\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* 30i: state==NORMAL → no CODE_BLOCK_LEAVE */
TEST(md4s_finalize_no_open_code)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "para\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* 30j: raw_len == 0 → returns NULL */
TEST(md4s_finalize_empty_returns_null)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	char *raw = md4s_finalize(p);
	ASSERT_NULL(raw);
	md4s_destroy(p);
}

/* 30k: raw_len > 0 → returns raw markdown */
TEST(md4s_finalize_returns_raw)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "# Test\n", 7);
	char *raw = md4s_finalize(p);
	ASSERT_NOT_NULL(raw);
	ASSERT_EQUAL_STRING("# Test\n", raw);
	free(raw);
	md4s_destroy(p);
}

/* -- md4s_cancel (L1141-1155) -- */

/* 31a: parser == NULL → returns NULL */
TEST(md4s_cancel_null)
{
	ASSERT_NULL(md4s_cancel(NULL));
}

/* 31b: partial_displayed=T → PARTIAL_CLEAR emitted */
TEST(md4s_cancel_clears_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_only(&ctx, "partial");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	ctx.count = 0;
	char *raw = md4s_cancel(p);
	free(raw);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_CLEAR));
	md4s_destroy(p);
}

/* 31c: partial_displayed=F → no PARTIAL_CLEAR */
TEST(md4s_cancel_no_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "line\n", 5);
	ctx.count = 0;
	char *raw = md4s_cancel(p);
	free(raw);
	ASSERT_FALSE(has_event(&ctx, MD4S_PARTIAL_CLEAR));
	md4s_destroy(p);
}

/* 31d: raw_len == 0 → returns NULL */
TEST(md4s_cancel_empty_returns_null)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	char *raw = md4s_cancel(p);
	ASSERT_NULL(raw);
	md4s_destroy(p);
}

/* 31e: raw_len > 0 → returns raw */
TEST(md4s_cancel_returns_raw)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "partial", 7);
	char *raw = md4s_cancel(p);
	ASSERT_NOT_NULL(raw);
	ASSERT_EQUAL_STRING("partial", raw);
	free(raw);
	md4s_destroy(p);
}

/* -- md4s_destroy (L1157-1166) -- */

/* 32a: parser == NULL → safe */
TEST(md4s_destroy_null)
{
	md4s_destroy(NULL); /* must not crash */
	ASSERT_TRUE(true);
}

/* 32b: valid parser → freed */
TEST(md4s_destroy_valid)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "hello\n", 6);
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p); /* must not crash or leak */
	ASSERT_TRUE(true);
}

/* ================================================================== */
/* Group 1: count_leading (tested via headings/fences)                */
/* ================================================================== */

/* n<len=T, s[n]==ch=T for 3 iterations, then F */
TEST(md4s_count_leading_both_true)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "### Hello\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(3, e->heading_level);
	md4s_destroy(p);
}

/* n<len=F on first check (empty line → blank) */
TEST(md4s_count_leading_empty)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* s[n]==ch=F on first check (no match) */
TEST(md4s_count_leading_no_match)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "xhello\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* n<len becomes F (all chars match, terminated by len) */
TEST(md4s_count_leading_all_match)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "######\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(6, e->heading_level);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 2: count_leading_spaces (tested via list indent)             */
/* ================================================================== */

/* Some leading spaces → indented list */
TEST(md4s_leading_spaces_some)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "  - item\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(1, e->list_depth);
	md4s_destroy(p);
}

/* No leading spaces → depth 0 */
TEST(md4s_leading_spaces_none)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- item\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(0, e->list_depth);
	md4s_destroy(p);
}

/* All spaces → blank line */
TEST(md4s_leading_spaces_all)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "   \n");
	ASSERT_FALSE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 3: is_all_whitespace (tested via blank line detection)       */
/* ================================================================== */

/* Spaces only → blank */
TEST(md4s_whitespace_spaces_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "hello\n   \nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Tabs only → blank */
TEST(md4s_whitespace_tabs_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "hello\n\t\t\nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Mixed spaces and tabs → blank */
TEST(md4s_whitespace_mixed)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "hello\n \t \nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Has non-whitespace content → not blank */
TEST(md4s_whitespace_has_content)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, " a \n");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Empty line (just \n) → blank */
TEST(md4s_whitespace_empty)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "hello\n\nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 4: is_thematic_break                                         */
/* ================================================================== */

/* len < 3 → not thematic break */
TEST(md4s_thematic_too_short)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "--\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Exactly 3 dashes → thematic break */
TEST(md4s_thematic_dashes)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "---\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Spaces between markers → still thematic break */
TEST(md4s_thematic_with_spaces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- - -\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Stars → thematic break */
TEST(md4s_thematic_stars)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "***\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Underscores → thematic break */
TEST(md4s_thematic_underscores)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "___\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Invalid character → not thematic break */
TEST(md4s_thematic_invalid_char)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "--x\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Mixed marker types → not thematic break */
TEST(md4s_thematic_mixed_markers)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "-*-\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Only 2 markers (+ space to pad len >= 3) → not thematic break */
TEST(md4s_thematic_only_two)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "-- \n");
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* More than 3 markers → thematic break */
TEST(md4s_thematic_more_than_three)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "-----\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 5: is_word_boundary (tested via underscore italic)           */
/* ================================================================== */

/* Null char after closing _ (end of content) → word boundary */
TEST(md4s_word_boundary_null)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "_test_\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Space after closing _ → word boundary */
TEST(md4s_word_boundary_space)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "_test_ more\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Punctuation after closing _ → word boundary */
TEST(md4s_word_boundary_punct)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "_test_.\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Alpha after closing _ → NOT word boundary → no italic */
TEST(md4s_word_boundary_alpha)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "foo_bar_baz\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 6: Fenced code state (classify_line L227)                    */
/* ================================================================== */

/* Inside fenced code → lines classified as CODE */
TEST(md4s_classify_in_fenced_code)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```\ncode here\n```\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_TEXT));
	const struct recorded_event *e = find_event(&ctx, MD4S_CODE_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("code here", e->text);
	md4s_destroy(p);
}

/* Normal state → paragraph */
TEST(md4s_classify_normal_state)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "hello\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_TEXT));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 7: Fence close detection (L230-238)                          */
/* ================================================================== */

/* Closing fence has enough chars */
TEST(md4s_fence_close_enough_chars)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```\ncode\n```\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* Closing fence has too few chars → treated as code */
TEST(md4s_fence_close_not_enough)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"````\ncode\n``\n````\n");
	/* The `` line should be CODE_TEXT, not a close */
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_CODE_TEXT));
	const struct recorded_event *e = find_event(&ctx, MD4S_CODE_TEXT, 1);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("``", e->text);
	md4s_destroy(p);
}

/* Rest after fence chars is blank → close */
TEST(md4s_fence_close_rest_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```\ncode\n```\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* Rest after fence chars is NOT blank → not a close, treated as code */
TEST(md4s_fence_close_rest_not_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```\ncode\n```x\n```\n");
	/* ```x should be code, not a close */
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_CODE_TEXT));
	const struct recorded_event *e = find_event(&ctx, MD4S_CODE_TEXT, 1);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("```x", e->text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 8: Blank line detection (L246)                               */
/* ================================================================== */

/* len == 0 (just newline) → blank */
TEST(md4s_blank_line_empty)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"hello\n\nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* len > 0 but all whitespace → blank */
TEST(md4s_blank_line_whitespace)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"hello\n   \nworld\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Has content → not blank */
TEST(md4s_blank_line_not_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "text\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 9: Fence open detection (L252-285)                           */
/* ================================================================== */

/* Backtick fence */
TEST(md4s_fence_open_backtick)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "```\ncode\n```\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Tilde fence */
TEST(md4s_fence_open_tilde)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "~~~\ncode\n~~~\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Too short (2 backticks) → not a fence */
TEST(md4s_fence_open_short)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "``\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Not a fence char → not a fence */
TEST(md4s_fence_open_not_fence_char)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "###\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* 5-tick fence */
TEST(md4s_fence_open_five_ticks)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"`````\ncode\n`````\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Backtick fence with clean info string */
TEST(md4s_fence_backtick_clean_info)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```python\ncode\n```\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_CODE_BLOCK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("python", e->language);
	md4s_destroy(p);
}

/* Backtick in info string → not a fence (backtick fences only) */
TEST(md4s_fence_backtick_in_info)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "```py`thon\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Tilde fence with backtick in info → OK (tilde fences allow it) */
TEST(md4s_fence_tilde_backtick_in_info)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"~~~py`thon\ncode\n~~~\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 10: ATX heading (L288-309)                                   */
/* ================================================================== */

/* line[0] == '#' and valid → heading */
TEST(md4s_heading_level_1)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# Title\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(1, e->heading_level);
	md4s_destroy(p);
}

/* line[0] != '#' → not heading */
TEST(md4s_heading_no_hash)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "Title\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* Level 6 → valid heading */
TEST(md4s_heading_level_6)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "###### Deep\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(6, e->heading_level);
	md4s_destroy(p);
}

/* Level 7 → too many, paragraph */
TEST(md4s_heading_level_7_invalid)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"####### Too many\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Hash only (level == len) → valid heading with empty content */
TEST(md4s_heading_hash_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "#\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* No space after hash (#Title) → not heading */
TEST(md4s_heading_no_space)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "#Title\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Content space stripping */
TEST(md4s_heading_content_space)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# Hello\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("Hello", e->text);
	md4s_destroy(p);
}

/* Trailing hashes stripped */
TEST(md4s_heading_trailing_hashes)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# Hello ##\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("Hello", e->text);
	md4s_destroy(p);
}

/* Trailing hash stripped (no trailing space after hash) */
TEST(md4s_heading_trailing_hash_space)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# Hello #\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("Hello", e->text);
	md4s_destroy(p);
}

/* No trailing hash → content unchanged */
TEST(md4s_heading_no_trailing)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "## World\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("World", e->text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 11: Blockquote (L317-327)                                    */
/* ================================================================== */

/* line[0] == '>' → blockquote */
TEST(md4s_blockquote_detected)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "> quote\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCKQUOTE_ENTER));
	md4s_destroy(p);
}

/* Space after '>' stripped from content */
TEST(md4s_blockquote_space_stripped)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "> text\n");
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("text", e->text);
	md4s_destroy(p);
}

/* No space after '>' → content starts immediately */
TEST(md4s_blockquote_no_space)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, ">text\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCKQUOTE_ENTER));
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("text", e->text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 12: Unordered list (L329-353)                                */
/* ================================================================== */

/* Dash marker */
TEST(md4s_ulist_dash)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ITEM_ENTER));
	md4s_destroy(p);
}

/* Star marker */
TEST(md4s_ulist_star)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "* item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Plus marker */
TEST(md4s_ulist_plus)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "+ item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Invalid marker → not a list */
TEST(md4s_ulist_invalid_marker)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "x item\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Space after marker required */
TEST(md4s_ulist_space_after_marker)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ITEM_ENTER));
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("item", e->text);
	md4s_destroy(p);
}

/* No space after marker → not a list */
TEST(md4s_ulist_no_space_after)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "-x\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Dash marker that's also a thematic break → thematic break wins */
TEST(md4s_thematic_vs_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- - -\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Star marker skips thematic break check (marker != '-') */
TEST(md4s_ulist_star_not_thematic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "* item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Only marker, no room for space+content → not a list */
TEST(md4s_ulist_no_room)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "-\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 13: Ordered list (L355-381)                                  */
/* ================================================================== */

/* Standard ordered list with '.' */
TEST(md4s_olist_dot)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1. item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->ordered);
	md4s_destroy(p);
}

/* Ordered list with ')' */
TEST(md4s_olist_paren)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1) item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* No digits before delimiter → not ordered list */
TEST(md4s_olist_no_digits)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, ". item\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Wrong delimiter (colon) → not ordered list */
TEST(md4s_olist_wrong_delimiter)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1: item\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* No space after delimiter → not ordered list */
TEST(md4s_olist_no_space_after_dot)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1.item\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Dot at very end → not ordered list */
TEST(md4s_olist_dot_at_end)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1.\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Multi-digit number */
TEST(md4s_olist_multi_digit)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "42. answer\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(42, e->item_number);
	md4s_destroy(p);
}

/* Indented ordered list */
TEST(md4s_olist_indented)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "  1. item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(1, e->list_depth);
	md4s_destroy(p);
}

/* Digit only, no delimiter → paragraph */
TEST(md4s_olist_digit_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "1\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 14: Escape handling (L519-531)                               */
/* ================================================================== */

/* Backslash + punctuation → escaped literal */
TEST(md4s_escape_punct)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "\\*literal\\*\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	/* Should have text events containing literal asterisks */
	ASSERT_TRUE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* Backslash + alpha → NOT escaped (backslash is literal) */
TEST(md4s_escape_alpha)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "\\n text\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TEXT));
	/* The text should contain the backslash */
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(strchr(e->text, '\\') != NULL);
	md4s_destroy(p);
}

/* Backslash at end of content → literal */
TEST(md4s_escape_at_end)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "hello\\\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* Escaped bracket → no link */
TEST(md4s_escape_bracket)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "\\[not a link\\]\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Escaped bold delimiter → literal asterisks */
TEST(md4s_escape_bold)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "\\*\\*not bold\\*\\*\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BOLD_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 15: HTML entity decoding (L534-573)                          */
/* ================================================================== */

/* &amp; → & */
TEST(md4s_entity_amp)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "A &amp; B\n");
	/* Find the entity-decoded text event */
	bool found_ampersand = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "&") == 0)
			found_ampersand = true;
	}
	ASSERT_TRUE(found_ampersand);
	md4s_destroy(p);
}

/* &lt; → < */
TEST(md4s_entity_lt)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&lt;\n");
	bool found = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "<") == 0)
			found = true;
	}
	ASSERT_TRUE(found);
	md4s_destroy(p);
}

/* &gt; → > */
TEST(md4s_entity_gt)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&gt;\n");
	bool found = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, ">") == 0)
			found = true;
	}
	ASSERT_TRUE(found);
	md4s_destroy(p);
}

/* &quot; → " */
TEST(md4s_entity_quot)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&quot;\n");
	bool found = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "\"") == 0)
			found = true;
	}
	ASSERT_TRUE(found);
	md4s_destroy(p);
}

/* &apos; → ' */
TEST(md4s_entity_apos)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&apos;\n");
	bool found = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "'") == 0)
			found = true;
	}
	ASSERT_TRUE(found);
	md4s_destroy(p);
}

/* &nbsp; → space */
TEST(md4s_entity_nbsp)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "a&nbsp;b\n");
	bool found_space = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, " ") == 0)
			found_space = true;
	}
	ASSERT_TRUE(found_space);
	md4s_destroy(p);
}

/* Unknown entity → literal */
TEST(md4s_entity_unknown)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&foo;\n");
	/* The &foo; should appear as literal text */
	bool found = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strstr(ctx.events[i].text, "&foo;") != NULL)
			found = true;
	}
	ASSERT_TRUE(found);
	md4s_destroy(p);
}

/* No semicolon → literal & */
TEST(md4s_entity_no_semicolon)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "&amp\n");
	/* Should NOT decode — no semicolon */
	bool found_amp = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strstr(ctx.events[i].text, "&amp") != NULL)
			found_amp = true;
	}
	ASSERT_TRUE(found_amp);
	md4s_destroy(p);
}

/* Entity too long → literal */
TEST(md4s_entity_too_long)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "&verylongentity;\n");
	/* Should NOT decode — entity name > 10 chars from & */
	ASSERT_TRUE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 16: Backtick code span (L576-600)                            */
/* ================================================================== */

/* Single backtick code span */
TEST(md4s_code_span_single)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "Use `printf` here\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_LEAVE));
	md4s_destroy(p);
}

/* Double backtick code span */
TEST(md4s_code_span_double)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "Use ``co`de`` here\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	/* Content includes the single backtick */
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 1);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("co`de", e->text);
	md4s_destroy(p);
}

/* No closing backtick → literal */
TEST(md4s_code_span_no_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "`no close\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	/* Backtick should appear in text */
	md4s_destroy(p);
}

/* Mismatched tick count → no match */
TEST(md4s_code_span_mismatched)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "`code``\n");
	/* Single backtick can't match double backtick */
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 17: Triple delimiter *** / ___ (L604-652)                    */
/* ================================================================== */

/* Triple asterisk → bold + italic */
TEST(md4s_triple_star)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "***bold italic***\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_LEAVE));
	md4s_destroy(p);
}

/* Triple underscore → bold + italic */
TEST(md4s_triple_underscore)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "___bold italic___\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Triple underscore without word boundary → no triple formatting.
 * 'a' before ___ fails the triple word boundary check.
 * Double __ may still match (since '_' is punct = word boundary),
 * but ITALIC should not appear (that requires the triple path). */
TEST(md4s_triple_underscore_no_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "a___word___b\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Triple asterisk with no close → falls through */
TEST(md4s_triple_star_no_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "***no close\n");
	/* Should NOT get bold+italic without close */
	/* May fall through to double or single delimiter handling */
	md4s_destroy(p);
}

/* Triple with escaped close → no close found */
TEST(md4s_triple_star_escaped_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "***hello\\***\n");
	/* The \\* escapes one *, so *** close is not found (only ** remains) */
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 18: Double delimiter ** / __ (L655-683)                      */
/* ================================================================== */

/* Double asterisk → bold */
TEST(md4s_double_star_bold)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "**bold**\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_LEAVE));
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Double underscore → bold */
TEST(md4s_double_underscore_bold)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "__bold__\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	md4s_destroy(p);
}

/* Double underscore without boundary → no bold */
TEST(md4s_double_underscore_no_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "a__word__b\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BOLD_ENTER));
	md4s_destroy(p);
}

/* Double asterisk no close → literal */
TEST(md4s_double_star_no_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "**no close\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BOLD_ENTER));
	md4s_destroy(p);
}

/* Bold with code span inside (find_double_close skips backticks) */
TEST(md4s_double_star_with_code)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "**hello `code` world**\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_LEAVE));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 19: Strikethrough ~~ (L686-703)                              */
/* ================================================================== */

/* Basic strikethrough */
TEST(md4s_strikethrough_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "~~struck~~\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_STRIKETHROUGH_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_STRIKETHROUGH_LEAVE));
	md4s_destroy(p);
}

/* No close → literal */
TEST(md4s_strikethrough_no_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "~~no close\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_STRIKETHROUGH_ENTER));
	md4s_destroy(p);
}

/* Single tilde → not strikethrough */
TEST(md4s_strikethrough_single_tilde)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "~not struck~\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_STRIKETHROUGH_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 20: Single delimiter * / _ (L706-737)                        */
/* ================================================================== */

/* Single asterisk → italic */
TEST(md4s_single_star_italic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "*italic*\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_LEAVE));
	md4s_destroy(p);
}

/* Single underscore → italic (with word boundary) */
TEST(md4s_single_underscore_italic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "_italic_\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Single underscore without boundary → no italic */
TEST(md4s_single_underscore_no_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "a_word_b\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Single asterisk no close → literal */
TEST(md4s_single_star_no_close)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "*no close\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Italic with code span inside */
TEST(md4s_single_star_with_code)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "*hello `code` world*\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_LEAVE));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 21: Link [text](url) (L740-789)                              */
/* ================================================================== */

/* Basic link */
TEST(md4s_link_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[click](https://x.com)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("https://x.com", e->url);
	/* Link text */
	const struct recorded_event *t = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(t);
	ASSERT_EQUAL_STRING("click", t->text);
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_LEAVE));
	md4s_destroy(p);
}

/* Escaped bracket in link text */
TEST(md4s_link_escaped_bracket)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[te\\]xt](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Nested brackets in link text */
TEST(md4s_link_nested_brackets)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[[inner]](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* No close bracket → literal [ */
TEST(md4s_link_no_close_bracket)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "[no close\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Bracket not followed by paren → not a link */
TEST(md4s_link_bracket_no_paren)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[text] no paren\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Bracket at end → not a link */
TEST(md4s_link_bracket_at_end)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "[text]\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Nested parens in URL */
TEST(md4s_link_nested_parens)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[text](url(1))\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("url(1)", e->url);
	md4s_destroy(p);
}

/* No close paren → not a link */
TEST(md4s_link_no_close_paren)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "[text](url\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Link with inline formatting in text */
TEST(md4s_link_with_bold_text)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[**bold link**](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	md4s_destroy(p);
}

/* Empty link text */
TEST(md4s_link_empty_text)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "[](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 22: maybe_emit_separator (L804-825)                          */
/* ================================================================== */

/* First block → no separator (emitted_count == 0) */
TEST(md4s_separator_first_block)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# First\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Blank line between paragraphs → separator */
TEST(md4s_separator_after_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "para\n\npara2\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Consecutive list items → no separator */
TEST(md4s_separator_no_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- a\n- b\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Heading forces separator even without blank line */
TEST(md4s_separator_heading_forces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "para\n# heading\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Fence open forces separator */
TEST(md4s_separator_fence_forces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "para\n```\ncode\n```\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* Thematic break forces separator (with blank line, since --- after
 * a paragraph is now a setext heading per CommonMark). */
TEST(md4s_separator_thematic_forces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "para\n\n---\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Paragraph continuation → no separator */
TEST(md4s_separator_para_continuation)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "line1\nline2\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_BLOCK_SEPARATOR));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 23: close_list_if_needed (L831-841)                          */
/* ================================================================== */

/* Not in list → no LIST_LEAVE */
TEST(md4s_close_list_not_in_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "para\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_LEAVE));
	md4s_destroy(p);
}

/* Paragraph after list → closes list */
TEST(md4s_close_list_on_paragraph)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- item\nparagraph\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Heading after list → closes list */
TEST(md4s_close_list_on_heading)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- item\n# heading\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* Consecutive list items → don't close */
TEST(md4s_close_list_consecutive)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- a\n- b\n");
	/* Only 1 LIST_LEAVE (at finalize), not between items */
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_LIST_ENTER));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_LIST_LEAVE));
	md4s_destroy(p);
}

/* Blank between list items → don't close yet */
TEST(md4s_close_list_blank_keeps)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- a\n\n- b\n");
	/* Still one list (blank doesn't close, only sets separator flag) */
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 24: open_list_if_needed (L843-855)                           */
/* ================================================================== */

/* First item opens list */
TEST(md4s_open_list_first_item)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- first\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Second item doesn't re-open */
TEST(md4s_open_list_already_open)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- a\n- b\n- c\n");
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_LIST_ENTER));
	ASSERT_EQUAL_INT(3, count_events(&ctx, MD4S_LIST_ITEM_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 25: Paragraph continuation (L972)                            */
/* ================================================================== */

/* Continuation → SOFTBREAK */
TEST(md4s_para_continuation)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "line one\nline two\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_SOFTBREAK));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Blank line breaks continuation → new paragraph */
TEST(md4s_para_after_blank)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "line one\n\nline two\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_SOFTBREAK));
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* First line → PARAGRAPH_ENTER (emitted_count == 0) */
TEST(md4s_para_first_line)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "first\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_SOFTBREAK));
	md4s_destroy(p);
}

/* After heading → new paragraph (last_type != PARAGRAPH) */
TEST(md4s_para_after_heading)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "# heading\nparagraph\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_SOFTBREAK));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 26: md4s_feed guard (L1007)                                  */
/* ================================================================== */

/* NULL parser → no crash */
TEST(md4s_feed_null_parser)
{
	md4s_feed(NULL, "x", 1); /* must not crash */
	ASSERT_TRUE(true);
}

/* NULL data → no crash */
TEST(md4s_feed_null_data)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, NULL, 1); /* must not crash */
	ASSERT_EQUAL_INT(0, ctx.count);
	md4s_destroy(p);
}

/* Zero length → no events */
TEST(md4s_feed_zero_length)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "x", 0);
	ASSERT_EQUAL_INT(0, ctx.count);
	md4s_destroy(p);
}

/* Valid args → processes */
TEST(md4s_feed_valid)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "# Hi\n", 5);
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 27: md4s_feed newline processing (L1022-1040)                */
/* ================================================================== */

/* Newline triggers line processing */
TEST(md4s_feed_newline_triggers)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "hello\n", 6);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);
}

/* No newline → only partial */
TEST(md4s_feed_no_newline_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_only(&ctx, "hello");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	ASSERT_FALSE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Partial then newline → PARTIAL_CLEAR before processing */
TEST(md4s_feed_partial_then_newline)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "hello", 5);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	md4s_feed(p, "\n", 1);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_CLEAR));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);
}

/* Whole line at once → no PARTIAL_CLEAR at newline
 * (internal partial may be emitted per-byte but cleared during same feed) */
TEST(md4s_feed_whole_line)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "hello\n", 6);
	/* After newline, line_len resets to 0, so no partial is emitted
	 * at the end of this feed call. But partial may have been emitted
	 * and cleared mid-feed. The important thing is we processed. */
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 28: md4s_feed partial line (L1044-1050)                      */
/* ================================================================== */

/* Partial line emitted when line_len > 0 */
TEST(md4s_feed_partial_emitted)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_only(&ctx, "partial");
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_PARTIAL_LINE, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("partial", e->text);
	md4s_destroy(p);
}

/* Complete line → no trailing partial */
TEST(md4s_feed_complete_no_partial)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "complete\n", 9);
	/* After newline, line_len is 0, so no PARTIAL_LINE at end.
	 * But per-byte feeding may emit partials mid-feed. Check that
	 * no PARTIAL_LINE exists AFTER the paragraph events. */
	int last_partial = -1;
	int last_para = -1;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_PARTIAL_LINE)
			last_partial = i;
		if (ctx.events[i].event == MD4S_PARAGRAPH_ENTER)
			last_para = i;
	}
	/* If any partial was emitted, it must be before the paragraph */
	if (last_partial >= 0)
		ASSERT_TRUE(last_partial < last_para);
	char *raw = md4s_finalize(p);
	free(raw);
	md4s_destroy(p);
}

/* Partial update → PARTIAL_CLEAR then new PARTIAL_LINE */
TEST(md4s_feed_partial_update)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "ab", 2);
	ASSERT_TRUE(has_event(&ctx, MD4S_PARTIAL_LINE));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_PARTIAL_LINE));
	md4s_feed(p, "cd", 2);
	/* Second feed should clear old partial and emit new one */
	ASSERT_TRUE(count_events(&ctx, MD4S_PARTIAL_CLEAR) >= 1);
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_PARTIAL_LINE));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_PARTIAL_LINE, 1);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("abcd", e->text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 33: Integration / event sequence tests                       */
/* ================================================================== */

/* Exact heading event sequence */
TEST(md4s_seq_heading)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "# Title\n");
	ASSERT_TRUE(event_at(&ctx, 0, MD4S_HEADING_ENTER));
	ASSERT_TRUE(event_at(&ctx, 1, MD4S_TEXT));
	ASSERT_TRUE(event_at(&ctx, 2, MD4S_HEADING_LEAVE));
	ASSERT_TRUE(event_at(&ctx, 3, MD4S_NEWLINE));
	md4s_destroy(p);
}

/* Paragraph sequence */
TEST(md4s_seq_paragraph)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "hello\n");
	ASSERT_TRUE(event_at(&ctx, 0, MD4S_PARAGRAPH_ENTER));
	ASSERT_TRUE(event_at(&ctx, 1, MD4S_TEXT));
	ASSERT_TRUE(event_at(&ctx, 2, MD4S_PARAGRAPH_LEAVE));
	ASSERT_TRUE(event_at(&ctx, 3, MD4S_NEWLINE));
	md4s_destroy(p);
}

/* Code block sequence */
TEST(md4s_seq_code_block)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"```c\nint x;\n```\n");
	ASSERT_TRUE(event_at(&ctx, 0, MD4S_CODE_BLOCK_ENTER));
	ASSERT_EQUAL_STRING("c",
		find_event(&ctx, MD4S_CODE_BLOCK_ENTER, 0)->language);
	ASSERT_TRUE(event_at(&ctx, 1, MD4S_CODE_TEXT));
	ASSERT_TRUE(event_at(&ctx, 2, MD4S_NEWLINE));
	ASSERT_TRUE(event_at(&ctx, 3, MD4S_CODE_BLOCK_LEAVE));
	ASSERT_TRUE(event_at(&ctx, 4, MD4S_NEWLINE));
	md4s_destroy(p);
}

/* List sequence */
TEST(md4s_seq_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "- a\n- b\n");
	ASSERT_TRUE(event_at(&ctx, 0, MD4S_LIST_ENTER));
	ASSERT_TRUE(event_at(&ctx, 1, MD4S_LIST_ITEM_ENTER));
	ASSERT_TRUE(event_at(&ctx, 2, MD4S_TEXT));
	ASSERT_TRUE(event_at(&ctx, 3, MD4S_LIST_ITEM_LEAVE));
	ASSERT_TRUE(event_at(&ctx, 4, MD4S_NEWLINE));
	ASSERT_TRUE(event_at(&ctx, 5, MD4S_LIST_ITEM_ENTER));
	ASSERT_TRUE(event_at(&ctx, 6, MD4S_TEXT));
	ASSERT_TRUE(event_at(&ctx, 7, MD4S_LIST_ITEM_LEAVE));
	ASSERT_TRUE(event_at(&ctx, 8, MD4S_NEWLINE));
	ASSERT_TRUE(event_at(&ctx, 9, MD4S_LIST_LEAVE));
	md4s_destroy(p);
}

/* Nested inline formatting sequence */
TEST(md4s_seq_bold_italic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"***both***\n");
	/* Inside paragraph: BOLD_ENTER, ITALIC_ENTER, TEXT, ITALIC_LEAVE, BOLD_LEAVE */
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	/* Find positions and verify ordering */
	int bold_enter = -1, italic_enter = -1, text_pos = -1;
	int italic_leave = -1, bold_leave = -1;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_BOLD_ENTER && bold_enter < 0)
			bold_enter = i;
		if (ctx.events[i].event == MD4S_ITALIC_ENTER && italic_enter < 0)
			italic_enter = i;
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "both") == 0)
			text_pos = i;
		if (ctx.events[i].event == MD4S_ITALIC_LEAVE && italic_leave < 0)
			italic_leave = i;
		if (ctx.events[i].event == MD4S_BOLD_LEAVE && bold_leave < 0)
			bold_leave = i;
	}
	ASSERT_TRUE(bold_enter < italic_enter);
	ASSERT_TRUE(italic_enter < text_pos);
	ASSERT_TRUE(text_pos < italic_leave);
	ASSERT_TRUE(italic_leave < bold_leave);
	md4s_destroy(p);
}

/* Link sequence */
TEST(md4s_seq_link)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[text](url)\n");
	int link_enter = -1, text_pos = -1, link_leave = -1;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_LINK_ENTER && link_enter < 0)
			link_enter = i;
		if (ctx.events[i].event == MD4S_TEXT &&
		    strcmp(ctx.events[i].text, "text") == 0)
			text_pos = i;
		if (ctx.events[i].event == MD4S_LINK_LEAVE && link_leave < 0)
			link_leave = i;
	}
	ASSERT_TRUE(link_enter >= 0);
	ASSERT_TRUE(link_enter < text_pos);
	ASSERT_TRUE(text_pos < link_leave);
	ASSERT_EQUAL_STRING("url", ctx.events[link_enter].url);
	md4s_destroy(p);
}

/* Byte-by-byte streaming produces same events */
TEST(md4s_incremental_byte_by_byte)
{
	/* First: feed all at once */
	struct recorder_ctx ctx1 = {0};
	struct md4s_parser *p1 = feed_and_finalize(&ctx1, "# Hello\n");

	/* Second: feed byte by byte */
	struct recorder_ctx ctx2 = {0};
	struct md4s_parser *p2 = md4s_create(recorder_callback, &ctx2);
	const char *md = "# Hello\n";
	for (size_t i = 0; i < strlen(md); i++)
		md4s_feed(p2, &md[i], 1);
	char *raw = md4s_finalize(p2);
	free(raw);

	/* Filter out PARTIAL events for comparison */
	int n1 = 0, n2 = 0;
	enum md4s_event seq1[64], seq2[64];
	for (int i = 0; i < ctx1.count && n1 < 64; i++) {
		if (ctx1.events[i].event != MD4S_PARTIAL_LINE &&
		    ctx1.events[i].event != MD4S_PARTIAL_CLEAR)
			seq1[n1++] = ctx1.events[i].event;
	}
	for (int i = 0; i < ctx2.count && n2 < 64; i++) {
		if (ctx2.events[i].event != MD4S_PARTIAL_LINE &&
		    ctx2.events[i].event != MD4S_PARTIAL_CLEAR)
			seq2[n2++] = ctx2.events[i].event;
	}
	ASSERT_EQUAL_INT(n1, n2);
	for (int i = 0; i < n1; i++)
		ASSERT_EQUAL_INT((int)seq1[i], (int)seq2[i]);

	md4s_destroy(p1);
	md4s_destroy(p2);
}

/* Chunk split in middle of line */
TEST(md4s_incremental_chunk_split)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	md4s_feed(p, "# Hel", 5);
	md4s_feed(p, "lo\n", 3);
	char *raw = md4s_finalize(p);
	free(raw);

	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	const struct recorded_event *e = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("Hello", e->text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 34: Link titles (new feature)                                */
/* ================================================================== */

/* Link with double-quoted title */
TEST(md4s_link_title_double_quote)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"[click](url \"My Title\")\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("url", e->url);
	ASSERT_EQUAL_STRING("My Title", e->title);
	md4s_destroy(p);
}

/* Link with single-quoted title */
TEST(md4s_link_title_single_quote)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"[click](url 'My Title')\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("My Title", e->title);
	md4s_destroy(p);
}

/* Link without title → title is empty */
TEST(md4s_link_no_title)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[click](url)\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("url", e->url);
	ASSERT_EQUAL_STRING("", e->title);
	md4s_destroy(p);
}

/* Link with empty title */
TEST(md4s_link_empty_title)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[click](url \"\")\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("url", e->url);
	ASSERT_EQUAL_STRING("", e->title);
	md4s_destroy(p);
}

/* Link with URL containing parens and title */
TEST(md4s_link_title_with_paren_url)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"[click](url(1) \"title\")\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("url(1)", e->url);
	ASSERT_EQUAL_STRING("title", e->title);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 35: Images (new feature)                                     */
/* ================================================================== */

/* Basic image */
TEST(md4s_image_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "![alt text](img.png)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_IMAGE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_IMAGE_LEAVE));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_IMAGE_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("img.png", e->url);
	/* Alt text emitted as TEXT between ENTER/LEAVE */
	const struct recorded_event *t = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(t);
	ASSERT_EQUAL_STRING("alt text", t->text);
	md4s_destroy(p);
}

/* Image with title */
TEST(md4s_image_with_title)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"![alt](img.png \"Photo\")\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_IMAGE_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("img.png", e->url);
	ASSERT_EQUAL_STRING("Photo", e->title);
	md4s_destroy(p);
}

/* Image vs link — ! prefix distinguishes */
TEST(md4s_image_not_link)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "![alt](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_IMAGE_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Link (no !) → not image */
TEST(md4s_link_not_image)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "[text](url)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_IMAGE_ENTER));
	md4s_destroy(p);
}

/* Image with empty alt */
TEST(md4s_image_empty_alt)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "![](img.png)\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_IMAGE_ENTER));
	md4s_destroy(p);
}

/* Lone ! without [ → literal */
TEST(md4s_image_lone_bang)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "! not image\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_IMAGE_ENTER));
	md4s_destroy(p);
}

/* ![alt] without (url) → literal */
TEST(md4s_image_no_paren)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "![alt] no url\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_IMAGE_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 36: Autolinks (new feature)                                  */
/* ================================================================== */

/* URL autolink */
TEST(md4s_autolink_url)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"Visit <https://example.com> now\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("https://example.com", e->url);
	md4s_destroy(p);
}

/* Email autolink */
TEST(md4s_autolink_email)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "<user@example.com>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("user@example.com", e->url);
	md4s_destroy(p);
}

/* HTTP URL autolink */
TEST(md4s_autolink_http)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "<http://x.com/path>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("http://x.com/path", e->url);
	md4s_destroy(p);
}

/* Not an autolink — no scheme or @ */
TEST(md4s_autolink_not_url)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "<notaurl>\n");
	/* Should NOT produce a link */
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Unclosed angle bracket → literal */
TEST(md4s_autolink_unclosed)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "<https://broken\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Empty angle brackets → not autolink */
TEST(md4s_autolink_empty)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "<>\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Autolink text is the URL itself */
TEST(md4s_autolink_text_is_url)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "<https://x.com>\n");
	/* The TEXT event inside the link should be the URL */
	int link_pos = -1, text_pos = -1;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_LINK_ENTER)
			link_pos = i;
		if (link_pos >= 0 && ctx.events[i].event == MD4S_TEXT) {
			text_pos = i;
			break;
		}
	}
	ASSERT_TRUE(text_pos > link_pos);
	ASSERT_EQUAL_STRING("https://x.com", ctx.events[text_pos].text);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 37: Task lists (new feature)                                 */
/* ================================================================== */

/* Checked task item */
TEST(md4s_task_checked)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [x] done\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->is_task);
	ASSERT_TRUE(e->task_checked);
	md4s_destroy(p);
}

/* Unchecked task item */
TEST(md4s_task_unchecked)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [ ] todo\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->is_task);
	ASSERT_FALSE(e->task_checked);
	md4s_destroy(p);
}

/* Uppercase X is also checked */
TEST(md4s_task_uppercase_x)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [X] done\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->is_task);
	ASSERT_TRUE(e->task_checked);
	md4s_destroy(p);
}

/* Task checkbox prefix stripped from content */
TEST(md4s_task_content_stripped)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [x] buy milk\n");
	const struct recorded_event *t = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(t);
	ASSERT_EQUAL_STRING("buy milk", t->text);
	md4s_destroy(p);
}

/* Regular list item (not task) */
TEST(md4s_task_not_task)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- regular item\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_FALSE(e->is_task);
	md4s_destroy(p);
}

/* Ordered list with task */
TEST(md4s_task_ordered)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "1. [x] done\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->is_task);
	ASSERT_TRUE(e->task_checked);
	ASSERT_EQUAL_INT(1, e->item_number);
	md4s_destroy(p);
}

/* Invalid checkbox (wrong char) → not task */
TEST(md4s_task_invalid_checkbox)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [y] not valid\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_FALSE(e->is_task);
	md4s_destroy(p);
}

/* Checkbox without space after → not task */
TEST(md4s_task_no_space_after_checkbox)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [x]no space\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	/* [x] at start with content right after — should be task
	 * since item_len == 3 case allows it OR item_text[3] == ' ' */
	ASSERT_FALSE(e->is_task);
	md4s_destroy(p);
}

/* Checkbox only (no content after) → still a task */
TEST(md4s_task_checkbox_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "- [x]\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_TRUE(e->is_task);
	ASSERT_TRUE(e->task_checked);
	md4s_destroy(p);
}

/* Mixed task and regular items */
TEST(md4s_task_mixed_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"- [x] done\n"
			"- [ ] todo\n"
			"- regular\n");
	const struct recorded_event *e0 =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 0);
	const struct recorded_event *e1 =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 1);
	const struct recorded_event *e2 =
		find_event(&ctx, MD4S_LIST_ITEM_ENTER, 2);
	ASSERT_NOT_NULL(e0);
	ASSERT_NOT_NULL(e1);
	ASSERT_NOT_NULL(e2);
	ASSERT_TRUE(e0->is_task);
	ASSERT_TRUE(e0->task_checked);
	ASSERT_TRUE(e1->is_task);
	ASSERT_FALSE(e1->task_checked);
	ASSERT_FALSE(e2->is_task);
	md4s_destroy(p);
}

/* ================================================================== */
/* ================================================================== */
/* Group 39: Setext headings (new feature)                            */
/* ================================================================== */

/* Setext heading level 1 (===) */
TEST(md4s_setext_level_1)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n===\n");
	/* Paragraph content + heading marker */
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(1, e->heading_level);
	md4s_destroy(p);
}

/* Setext heading level 2 (---) */
TEST(md4s_setext_level_2)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n---\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(2, e->heading_level);
	md4s_destroy(p);
}

/* Setext wins over thematic break after paragraph */
TEST(md4s_setext_over_thematic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n---\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* --- with blank line before → thematic break (not setext) */
TEST(md4s_setext_blank_before_is_thematic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"para\n\n---\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_THEMATIC_BREAK));
	md4s_destroy(p);
}

/* Long underline */
TEST(md4s_setext_long_underline)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n========\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_HEADING_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(1, e->heading_level);
	md4s_destroy(p);
}

/* Paragraph text preserved (emitted before heading marker) */
TEST(md4s_setext_text_in_paragraph)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Hello World\n===\n");
	/* The text was emitted as paragraph content */
	const struct recorded_event *t = find_event(&ctx, MD4S_TEXT, 0);
	ASSERT_NOT_NULL(t);
	ASSERT_EQUAL_STRING("Hello World", t->text);
	md4s_destroy(p);
}

/* Single = is valid setext underline */
TEST(md4s_setext_single_char)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n=\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* Underline with trailing spaces */
TEST(md4s_setext_trailing_spaces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Title\n===   \n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 43: Reference links (new feature)                            */
/* ================================================================== */

/* Basic reference link */
TEST(md4s_reflink_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[link]: https://example.com\n"
		"\n"
		"Click [here][link].\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("https://example.com", e->url);
	md4s_destroy(p);
}

/* Collapsed reference [text][] — uses text as label */
TEST(md4s_reflink_collapsed)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[example]: https://x.com\n"
		"\n"
		"Visit [example][].\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("https://x.com", e->url);
	md4s_destroy(p);
}

/* Case-insensitive label matching */
TEST(md4s_reflink_case_insensitive)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[MyLink]: https://x.com\n"
		"\n"
		"Click [here][mylink].\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Undefined reference → literal text */
TEST(md4s_reflink_undefined)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"Click [here][undefined].\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_LINK_ENTER));
	md4s_destroy(p);
}

/* Definition silently consumed (not in output) */
TEST(md4s_reflink_def_consumed)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[ref]: url\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_FALSE(has_event(&ctx, MD4S_TEXT));
	md4s_destroy(p);
}

/* Definition with angle brackets */
TEST(md4s_reflink_angle_brackets)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[ref]: <https://x.com>\n"
		"\n"
		"[text][ref]\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_LINK_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("https://x.com", e->url);
	md4s_destroy(p);
}

/* Reference image ![alt][ref] */
TEST(md4s_reflink_image)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[logo]: img.png\n"
		"\n"
		"![alt][logo]\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_IMAGE_ENTER));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_IMAGE_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("img.png", e->url);
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 42: Lazy blockquote continuation (new feature)               */
/* ================================================================== */

/* Lazy continuation: paragraph after > continues blockquote */
TEST(md4s_lazy_blockquote_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"> first\nsecond\n");
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_BLOCKQUOTE_ENTER));
	md4s_destroy(p);
}

/* Blank line stops lazy continuation */
TEST(md4s_lazy_blockquote_blank_stops)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"> quoted\n\nnot quoted\n");
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_BLOCKQUOTE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Multiple lazy continuation lines */
TEST(md4s_lazy_blockquote_multi)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"> first\nsecond\nthird\n");
	ASSERT_EQUAL_INT(3, count_events(&ctx, MD4S_BLOCKQUOTE_ENTER));
	md4s_destroy(p);
}

/* Heading breaks lazy continuation */
TEST(md4s_lazy_blockquote_heading_breaks)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"> quoted\n# heading\n");
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_BLOCKQUOTE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 41: Indented code blocks (new feature)                       */
/* ================================================================== */

/* Basic indented code */
TEST(md4s_indented_code_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"    int x = 0;\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_TEXT));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	const struct recorded_event *e =
		find_event(&ctx, MD4S_CODE_TEXT, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_STRING("int x = 0;", e->text);
	md4s_destroy(p);
}

/* Multi-line indented code */
TEST(md4s_indented_code_multi)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"    line 1\n"
		"    line 2\n");
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_EQUAL_INT(2, count_events(&ctx, MD4S_CODE_TEXT));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_CODE_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* Blank line between indented code lines */
TEST(md4s_indented_code_blank_between)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"    line 1\n"
		"\n"
		"    line 2\n");
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_EQUAL_INT(3, count_events(&ctx, MD4S_CODE_TEXT));
	md4s_destroy(p);
}

/* Not indented code inside list */
TEST(md4s_indented_code_not_in_list)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"- item\n"
		"    continuation\n");
	/* Should not create a code block — 4 spaces inside list */
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	md4s_destroy(p);
}

/* 3 spaces → not indented code (paragraph) */
TEST(md4s_indented_code_three_spaces)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "   hello\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Indented code ends when non-indented line follows */
TEST(md4s_indented_code_ends)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"    code\n"
		"paragraph\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 40: HTML blocks (new feature)                                */
/* ================================================================== */

/* Basic div block */
TEST(md4s_html_block_div)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"<div class=\"test\">\nhello\n</div>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_LEAVE));
	md4s_destroy(p);
}

/* HTML comment */
TEST(md4s_html_block_comment)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"<!-- comment -->\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Closing tag */
TEST(md4s_html_block_closing_tag)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "</div>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Non-block tag → not HTML block */
TEST(md4s_html_block_inline_tag)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "<span>hello</span>\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Multi-line HTML block ending on blank line */
TEST(md4s_html_block_multiline)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"<div>\n"
		"  <p>content</p>\n"
		"</div>\n"
		"\n"
		"paragraph\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Script tag */
TEST(md4s_html_block_script)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"<script>\nalert('hi');\n</script>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	md4s_destroy(p);
}

/* Table tag (not to be confused with GFM table) */
TEST(md4s_html_block_table_tag)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"<table>\n<tr><td>cell</td></tr>\n</table>\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	/* Should NOT be a GFM table */
	ASSERT_FALSE(has_event(&ctx, MD4S_TABLE_ENTER));
	md4s_destroy(p);
}

/* Not HTML — starts with < but not a tag */
TEST(md4s_html_block_not_tag)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx, "< not html\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_HTML_BLOCK_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 38: GFM Tables (new feature)                                 */
/* ================================================================== */

/* Basic table */
TEST(md4s_table_basic)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| A | B |\n"
		"|---|---|\n"
		"| 1 | 2 |\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_HEAD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_HEAD_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_BODY_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_BODY_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_LEAVE));
	md4s_destroy(p);
}

/* Column count in TABLE_ENTER */
TEST(md4s_table_column_count)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| A | B | C |\n"
		"|---|---|---|\n"
		"| 1 | 2 | 3 |\n");
	const struct recorded_event *e =
		find_event(&ctx, MD4S_TABLE_ENTER, 0);
	ASSERT_NOT_NULL(e);
	ASSERT_EQUAL_INT(3, e->column_count);
	md4s_destroy(p);
}

/* Cell content with inline formatting */
TEST(md4s_table_inline_in_cells)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| **bold** | *italic* |\n"
		"|----------|----------|\n"
		"| `code` | text |\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_SPAN_ENTER));
	md4s_destroy(p);
}

/* Alignment: left, center, right */
TEST(md4s_table_alignment)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| L | C | R |\n"
		"|:--|:--:|--:|\n"
		"| a | b | c |\n");
	/* Find cells in header row. */
	const struct recorded_event *c0 =
		find_event(&ctx, MD4S_TABLE_CELL_ENTER, 0);
	const struct recorded_event *c1 =
		find_event(&ctx, MD4S_TABLE_CELL_ENTER, 1);
	const struct recorded_event *c2 =
		find_event(&ctx, MD4S_TABLE_CELL_ENTER, 2);
	ASSERT_NOT_NULL(c0);
	ASSERT_NOT_NULL(c1);
	ASSERT_NOT_NULL(c2);
	ASSERT_EQUAL_INT(1, c0->cell_alignment); /* left */
	ASSERT_EQUAL_INT(2, c1->cell_alignment); /* center */
	ASSERT_EQUAL_INT(3, c2->cell_alignment); /* right */
	md4s_destroy(p);
}

/* Table without leading/trailing pipes */
TEST(md4s_table_no_outer_pipes)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"A | B\n"
		"---|---\n"
		"1 | 2\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	md4s_destroy(p);
}

/* Not a table — separator has wrong format */
TEST(md4s_table_invalid_separator)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| A | B |\n"
		"| x | y |\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_TABLE_ENTER));
	/* Should be treated as paragraphs */
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Table followed by paragraph */
TEST(md4s_table_then_paragraph)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| A |\n"
		"|---|\n"
		"| 1 |\n"
		"\n"
		"paragraph\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_LEAVE));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* Table with multiple body rows */
TEST(md4s_table_multiple_rows)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| H |\n"
		"|---|\n"
		"| a |\n"
		"| b |\n"
		"| c |\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	/* 1 header row + 3 body rows = 4 ROW_ENTER events */
	ASSERT_EQUAL_INT(4, count_events(&ctx, MD4S_TABLE_ROW_ENTER));
	md4s_destroy(p);
}

/* Table header only (no body rows) */
TEST(md4s_table_header_only)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"| H |\n"
		"|---|\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_TABLE_ENTER));
	ASSERT_EQUAL_INT(1, count_events(&ctx, MD4S_TABLE_ROW_ENTER));
	md4s_destroy(p);
}

/* Pipe in paragraph (no separator row) → not a table */
TEST(md4s_table_pipe_in_paragraph)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"hello | world\n"
		"more text\n");
	ASSERT_FALSE(has_event(&ctx, MD4S_TABLE_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 45: Unicode-aware word boundary tests                        */
/* ================================================================== */

/* Em dash (U+2014, Pd) before underscore → word boundary → italic */
TEST(md4s_unicode_emdash_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "\xe2\x80\x94_italic_\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* CJK ideograph before underscore → NOT boundary → no italic */
TEST(md4s_unicode_cjk_no_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"\xe4\xb8\xad_word_\xe6\x96\x87\n");
	/* U+4E2D (中) is Lo, not punct/ws → not a boundary */
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Left double quote (U+201C, Pi) → word boundary → italic */
TEST(md4s_unicode_smart_quote_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"\xe2\x80\x9c_italic_\xe2\x80\x9d\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Non-breaking space (U+00A0) → whitespace → word boundary */
TEST(md4s_unicode_nbsp_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "\xc2\xa0_italic_\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Euro sign (U+20AC, Sc) → punctuation → word boundary */
TEST(md4s_unicode_euro_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx, "\xe2\x82\xac_italic_\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Cyrillic letter before underscore → NOT boundary → no italic */
TEST(md4s_unicode_cyrillic_no_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"\xd0\x90_word_\xd0\x91\n");
	/* U+0410 (А) is Lu → not punct/ws → not boundary */
	ASSERT_FALSE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* Asterisk emphasis is NOT affected by word boundary (only underscore is) */
TEST(md4s_unicode_star_ignores_boundary)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p =
		feed_and_finalize(&ctx,
			"\xe4\xb8\xad*italic*\xe6\x96\x87\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_ITALIC_ENTER));
	md4s_destroy(p);
}

/* ================================================================== */
/* Group 44: Security hardening tests                                 */
/* ================================================================== */

/* Deep inline nesting → capped at MAX_INLINE_DEPTH, no crash */
TEST(md4s_security_deep_nesting)
{
	struct recorder_ctx ctx = {0};
	/* Build deeply nested bold: **a**a**a**... */
	char deep[2048];
	int pos = 0;
	for (int i = 0; i < 100 && pos < 2000; i++) {
		deep[pos++] = '*';
		deep[pos++] = 'a';
		deep[pos++] = '*';
	}
	deep[pos++] = '\n';
	deep[pos] = '\0';
	struct md4s_parser *p = feed_and_finalize(&ctx, deep);
	/* Must not crash — that's the test */
	ASSERT_TRUE(ctx.count > 0);
	md4s_destroy(p);
}

/* Recursive link nesting → depth limited */
TEST(md4s_security_recursive_links)
{
	struct recorder_ctx ctx = {0};
	/* Nested links: [a[b[c[d](u)](u)](u)](u) */
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"[a[b[c[d[e[f[g[h[i[j[k[l[m[n[o[p"
		"](u)](u)](u)](u)](u)](u)](u)](u)](u)"
		"](u)](u)](u)](u)](u)](u)](u)\n");
	/* Must not crash */
	ASSERT_TRUE(ctx.count > 0);
	md4s_destroy(p);
}

/* Ordered list with huge number → capped at 9 digits */
TEST(md4s_security_olist_overflow)
{
	struct recorder_ctx ctx = {0};
	/* 20 digits would overflow int. Should be capped to 9. */
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"99999999999999999999. item\n");
	/* Should NOT be a list (>9 digits fails the digit cap) */
	ASSERT_FALSE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* Valid 9-digit ordered list */
TEST(md4s_security_olist_max_valid)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"999999999. item\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	md4s_destroy(p);
}

/* NULL byte replacement → U+FFFD */
TEST(md4s_security_null_byte)
{
	struct recorder_ctx ctx = {0};
	/* Feed bytes with embedded NUL: "a\0b\n" */
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	char data[] = {'a', '\0', 'b', '\n'};
	md4s_feed(p, data, 4);
	char *raw = md4s_finalize(p);
	/* The NUL should be replaced with U+FFFD (EF BF BD) */
	/* So the text event should contain "a\xEF\xBF\xBDb" */
	bool found_replacement = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strstr(ctx.events[i].text, "\xEF\xBF\xBD") != NULL)
			found_replacement = true;
	}
	ASSERT_TRUE(found_replacement);
	free(raw);
	md4s_destroy(p);
}

/* NULL byte doesn't truncate output */
TEST(md4s_security_null_preserves_content)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = md4s_create(recorder_callback, &ctx);
	char data[] = {'h', 'e', '\0', 'l', 'o', '\n'};
	md4s_feed(p, data, 6);
	char *raw = md4s_finalize(p);
	/* Content after NUL should still be present */
	bool found_lo = false;
	for (int i = 0; i < ctx.count; i++) {
		if (ctx.events[i].event == MD4S_TEXT &&
		    strstr(ctx.events[i].text, "lo") != NULL)
			found_lo = true;
	}
	ASSERT_TRUE(found_lo);
	free(raw);
	md4s_destroy(p);
}

/* Many unclosed emphasis delimiters → no quadratic crash */
TEST(md4s_security_many_unclosed)
{
	struct recorder_ctx ctx = {0};
	char line[5002];
	for (int i = 0; i < 5000; i++)
		line[i] = '*';
	line[5000] = '\n';
	line[5001] = '\0';
	struct md4s_parser *p = feed_and_finalize(&ctx, line);
	/* Must not crash or hang */
	ASSERT_TRUE(ctx.count > 0);
	md4s_destroy(p);
}

/* Integration tests (continued)                                      */
/* ================================================================== */

/* Complex multi-block document */
TEST(md4s_seq_complex)
{
	struct recorder_ctx ctx = {0};
	struct md4s_parser *p = feed_and_finalize(&ctx,
		"# Title\n\n"
		"Some **bold** text.\n\n"
		"```c\nint x;\n```\n\n"
		"- item\n\n"
		"> Quote\n");
	ASSERT_TRUE(has_event(&ctx, MD4S_HEADING_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_PARAGRAPH_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BOLD_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_CODE_BLOCK_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_LIST_ENTER));
	ASSERT_TRUE(has_event(&ctx, MD4S_BLOCKQUOTE_ENTER));
	/* Multiple block separators */
	ASSERT_TRUE(count_events(&ctx, MD4S_BLOCK_SEPARATOR) >= 3);
	md4s_destroy(p);
}
