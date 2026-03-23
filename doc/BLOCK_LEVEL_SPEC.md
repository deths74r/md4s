# md4s Block-Level Implementation Specification

This document specifies the implementation of all remaining block-level gaps
between md4s and the CommonMark specification (and md4c's GFM extensions).
Each section is self-contained with data structures, algorithms, code
locations, and MC/DC test cases.

Reference files:
- `md4s.c` — parser implementation (~2308 lines)
- `md4s.h` — public API (209 lines)

---

## 1. Loose vs Tight Lists

### Problem

md4s emits LIST_ITEM_ENTER, inline content, LIST_ITEM_LEAVE for every list
item. It never emits PARAGRAPH_ENTER/LEAVE wrappers around list item content.
CommonMark distinguishes *tight* lists (no paragraphs) from *loose* lists
(items wrapped in paragraphs). A list becomes loose when any blank line
separates consecutive items or falls between blocks within an item.

### Data Structure Changes

**md4s.h — `struct md4s_detail`:**

```c
/* LIST_ENTER: true = tight list (no paragraph wrappers). */
bool is_tight;
```

This field is set on `MD4S_LIST_ENTER`. The consumer uses it to decide
whether to wrap item content in `<p>` tags.

**md4s.c — `struct md4s_parser`:**

```c
/* List tracking. */
bool in_list;
bool list_ordered;
int list_depth;
bool list_saw_blank;     /* NEW: blank line seen since last item */
bool list_is_loose;      /* NEW: current list is loose */
```

### Detection Algorithm

A list is **loose** when a blank line occurs between two list items. The
detection happens incrementally:

1. When a `LINE_BLANK` is encountered while `p->in_list` is true, set
   `p->list_saw_blank = true`.

2. When the next `LINE_UNORDERED_LIST` or `LINE_ORDERED_LIST` is encountered
   while `p->list_saw_blank` is true, set `p->list_is_loose = true`.

3. When `LIST_ENTER` is emitted, `d.is_tight = !p->list_is_loose`. Note:
   because the parser is streaming and the loose determination happens
   *after* LIST_ENTER, this requires a design choice.

**Streaming constraint:** The parser cannot know at LIST_ENTER time whether
the list will be loose. Two options:

**Option A — Deferred list emission (recommended):** Accumulate list items
in a buffer. When the list ends (non-list, non-blank line), determine
loose/tight and emit all events at once. This breaks streaming latency for
lists but gives correct semantics.

**Option B — Retroactive notification:** Emit `LIST_ENTER` immediately with
`is_tight = true` (optimistic). If a blank line is later seen between items,
emit a new event `MD4S_LIST_LOOSE` to notify the consumer to retrofit
paragraph wrappers. This preserves streaming but complicates the consumer.

**Recommended: Option A.** Lists are typically short (few items), so the
latency cost is negligible. The implementation buffers list events and
replays them on list close.

### Algorithm (Option A)

```
struct list_event_buffer:
    struct { enum md4s_event event; struct md4s_detail detail; }
        events[MAX_LIST_EVENTS]
    int count
    bool saw_blank_between_items
    int item_count
```

When `open_list_if_needed` fires:
1. Initialize list event buffer.
2. Set `buffering_list = true`.

While `buffering_list`:
1. All events that would be emitted go into the buffer instead.
2. On `LINE_BLANK`, set `saw_blank_between_items` if `item_count > 0`.
3. On new list item, increment `item_count`.

When list closes (`close_list_if_needed`):
1. Set `is_tight = !saw_blank_between_items`.
2. Replay all buffered events. For each `LIST_ITEM_ENTER`:
   - If loose: insert `PARAGRAPH_ENTER` before item content,
     `PARAGRAPH_LEAVE` before `LIST_ITEM_LEAVE`.
   - If tight: emit as-is.
3. Emit `LIST_LEAVE`.

### Code Locations

| Location | Change |
|----------|--------|
| `struct md4s_parser` (line 83) | Add `list_saw_blank`, `list_is_loose` fields |
| `struct md4s_detail` (line 103 in .h) | Add `bool is_tight` field |
| `open_list_if_needed` (line 1581) | Set `d.is_tight` on `LIST_ENTER` |
| `process_line`, `LINE_BLANK` case (line 1743) | Set `list_saw_blank = true` when `in_list` |
| `process_line`, list cases (line 1840) | Check `list_saw_blank` to set `list_is_loose` |
| `close_list_if_needed` (line 1569) | Finalize loose/tight before `LIST_LEAVE` |

### Event Emission Changes

**Tight list (current behavior, mostly unchanged):**
```
LIST_ENTER {is_tight=true}
  LIST_ITEM_ENTER
    TEXT "item 1"
  LIST_ITEM_LEAVE
  LIST_ITEM_ENTER
    TEXT "item 2"
  LIST_ITEM_LEAVE
LIST_LEAVE
```

**Loose list (new behavior):**
```
LIST_ENTER {is_tight=false}
  LIST_ITEM_ENTER
    PARAGRAPH_ENTER
      TEXT "item 1"
    PARAGRAPH_LEAVE
  LIST_ITEM_LEAVE
  LIST_ITEM_ENTER
    PARAGRAPH_ENTER
      TEXT "item 2"
    PARAGRAPH_LEAVE
  LIST_ITEM_LEAVE
LIST_LEAVE
```

### MC/DC Test Cases

| # | Input | Blank between items | Expected `is_tight` | Condition tested |
|---|-------|--------------------|--------------------|-----------------|
| T1 | `- a\n- b\n` | no | true | No blank, tight |
| T2 | `- a\n\n- b\n` | yes | false | Blank between items, loose |
| T3 | `- a\n- b\n- c\n` | no | true | 3 items, no blanks, tight |
| T4 | `- a\n\n- b\n- c\n` | yes (between 1st and 2nd) | false | One blank makes entire list loose |
| T5 | `- a\n- b\n\n- c\n` | yes (between 2nd and 3rd) | false | Blank later, still loose |
| T6 | `1. a\n\n2. b\n` | yes | false | Ordered loose list |
| T7 | `1. a\n2. b\n` | no | true | Ordered tight list |
| T8 | `- a\n\nparagraph` | yes, but only 1 item | true | Single-item list, blank after list (not between items) |

---

## 2. Multi-Line List Items

### Problem

md4s treats each list marker line as a complete item. In CommonMark, a list
item can span multiple lines: continuation lines indented to the item's
content column are part of the same item.

### Content Indent Calculation

The **content indent** for a list item is the column at which the item's
content begins, measured from the start of the line:

- Unordered: `leading_spaces + 2` (marker char + space)
  - e.g., `  - foo` → content indent = 4 (2 spaces + '-' + ' ')
- Ordered: `leading_spaces + digit_count + 2` (digits + '.' or ')' + space)
  - e.g., `  1. foo` → content indent = 5 (2 + 1 + 2)

A continuation line is any subsequent line whose indentation is >= the
content indent and which does not start a new list item at a lesser indent.

### Data Structure Changes

**md4s.c — `struct md4s_parser`:**

```c
/* Multi-line list item tracking. */
bool in_list_item;              /* Currently accumulating a list item */
int  list_item_content_indent;  /* Columns to content start */
int  list_item_marker_indent;   /* Leading spaces before marker */
```

### Container Stack Design

A container stack is needed for sub-blocks within list items (nested
blockquotes, code blocks, sub-lists). Each container entry represents an
open block context:

```c
#define MAX_CONTAINER_DEPTH 16

enum container_type {
    CONTAINER_LIST_ITEM,
    CONTAINER_BLOCKQUOTE,
};

struct container {
    enum container_type type;
    int content_indent;    /* For list items: where content starts */
    bool is_ordered;       /* For list items */
    int item_number;       /* For ordered list items */
};

/* In struct md4s_parser: */
struct container containers[MAX_CONTAINER_DEPTH];
int container_depth;
```

### Detection Algorithm

In `process_line` (and the feed loop), before calling `classify_line`:

```
1. If in_list_item and line is not blank:
   a. Count leading spaces of the raw line
   b. If leading_spaces >= list_item_content_indent:
      - Strip list_item_content_indent spaces from the front
      - Classify the *stripped* line (it may be a paragraph continuation,
        sub-block, etc.)
      - If classified as paragraph: emit SOFTBREAK + inline content
        (continuation of item paragraph)
      - If classified as code fence, heading, etc.: emit as sub-block
        within the item
      - Do NOT emit LIST_ITEM_LEAVE yet
      - Continue
   c. If leading_spaces < list_item_content_indent:
      - This line is NOT a continuation
      - Close the current list item (emit LIST_ITEM_LEAVE)
      - Process normally

2. If in_list_item and line is blank:
   - Do not close the item yet (blank lines within items are allowed
     in loose lists)
   - Set a flag; the next non-blank line determines if the item continues
```

### Interaction with Streaming Architecture

The line-at-a-time model works well here because continuation detection
only needs the current line's indentation compared to a stored threshold
(`list_item_content_indent`). No buffering of previous lines is needed
beyond what already exists.

The key change is that `LIST_ITEM_LEAVE` is no longer emitted at the end
of the list item's first line. Instead, it is deferred until a line arrives
that does not qualify as a continuation.

### Sub-blocks Within List Items

Once continuation is detected and the indent is stripped, the stripped
content is classified normally. This allows:

- **Code blocks:** A fenced code block inside a list item starts when the
  stripped line is `LINE_FENCE_OPEN`. The parser enters `STATE_FENCED_CODE`
  as usual, but subsequent lines also have the content indent stripped
  before fence-close detection.

- **Blockquotes:** A `>` prefix in the stripped content creates a nested
  blockquote within the list item.

- **Sub-lists:** A list marker in the stripped content creates a nested list
  (see Section 3).

- **Indented code:** 4+ spaces *after* stripping the content indent means
  an indented code block within the item.

### Code Locations

| Location | Change |
|----------|--------|
| `struct md4s_parser` (line 83) | Add `in_list_item`, `list_item_content_indent`, `list_item_marker_indent` |
| `classify_line` (line 756) | Accept a `strip_indent` parameter or pre-strip in caller |
| `process_line`, list cases (line 1840) | Set `in_list_item = true`, calculate content indent, defer LEAVE |
| `process_line`, all non-list cases | Check `in_list_item`, close item if indent insufficient |
| `md4s_feed` loop (line 1992) | Before classify, check continuation indent |
| `md4s_finalize` (line 2197) | Close any open list item |

### MC/DC Test Cases

| # | Input | Expected |
|---|-------|----------|
| M1 | `- line1\n  line2\n` | Single item: "line1" SOFTBREAK "line2" |
| M2 | `- line1\nline2\n` | line2 is NOT a continuation (0 < 2 indent); new paragraph |
| M3 | `- line1\n  line2\n  line3\n` | Single item with 2 continuations |
| M4 | `1. line1\n   line2\n` | Ordered item continuation (indent >= 3) |
| M5 | `- line1\n\n  line2\n` | Blank then continuation: loose item with 2 paragraphs |
| M6 | `- line1\n  > quote\n` | Blockquote sub-block inside list item |
| M7 | `- line1\n  ```\n  code\n  ```\n` | Code block sub-block inside list item |
| M8 | `- line1\n      code\n` | Indented code (4 spaces after 2-space content indent = 6 total) |
| M9 | `- line1\n- line2\n` | Two separate items (marker at same indent starts new item) |
| M10 | `  - line1\n    line2\n` | Indented item with continuation (content indent = 4) |

---

## 3. Proper List Nesting

### Problem

md4s tracks list depth as 0 or 1 (`(cl.indent > 0) ? 1 : 0` on line 1847).
CommonMark supports unlimited nesting: a list item's content can contain
another list, which can contain another, etc.

### Detection Algorithm

A nested list is detected when a list marker appears within the content
region of an outer list item. Specifically:

1. A line has a list marker at indent `I`.
2. If `I >= outer_item_content_indent`, this is a **sub-list** within the
   current item.
3. If `I < outer_item_content_indent` but `I >= outer_item_marker_indent`,
   this is a **sibling item** in the outer list.
4. If `I < outer_item_marker_indent`, this closes the outer list entirely.

The container stack (from Section 2) tracks nested list contexts:

```
Container stack example for:
- item 1
  - nested item a
    - deeply nested
  - nested item b
- item 2

Stack states:
[LIST_ITEM indent=0 content_indent=2]
[LIST_ITEM indent=0 content_indent=2] [LIST_ITEM indent=2 content_indent=4]
[LIST_ITEM indent=0 content_indent=2] [LIST_ITEM indent=2 content_indent=4] [LIST_ITEM indent=4 content_indent=6]
[LIST_ITEM indent=0 content_indent=2] [LIST_ITEM indent=2 content_indent=4]
[LIST_ITEM indent=0 content_indent=2]
```

### When New List Opens vs Existing Continues

- **New sub-list:** A list marker appears at an indent that falls within an
  existing item's content region, and the current innermost container is
  NOT a list at that indent level. Emit `LIST_ENTER` for the new sub-list.

- **Same list continues:** A list marker appears at the same indent as the
  current list's markers. Emit `LIST_ITEM_LEAVE` for the previous item,
  then `LIST_ITEM_ENTER` for the new one.

- **List type change:** An ordered marker appears where an unordered list
  was active (or vice versa) at the same indent. Close the old list
  (`LIST_LEAVE`), open a new one (`LIST_ENTER`).

### Event Nesting

```
Input:
- a
  - b
    - c
  - d
- e

Events:
LIST_ENTER {ordered=false}
  LIST_ITEM_ENTER {list_depth=0}
    TEXT "a"
    LIST_ENTER {ordered=false}
      LIST_ITEM_ENTER {list_depth=1}
        TEXT "b"
        LIST_ENTER {ordered=false}
          LIST_ITEM_ENTER {list_depth=2}
            TEXT "c"
          LIST_ITEM_LEAVE
        LIST_LEAVE
      LIST_ITEM_LEAVE
      LIST_ITEM_ENTER {list_depth=1}
        TEXT "d"
      LIST_ITEM_LEAVE
    LIST_LEAVE
  LIST_ITEM_LEAVE
  LIST_ITEM_ENTER {list_depth=0}
    TEXT "e"
  LIST_ITEM_LEAVE
LIST_LEAVE
```

### Data Structure Changes

Replace the single `in_list` / `list_ordered` / `list_depth` with a stack:

```c
#define MAX_LIST_DEPTH 16

struct list_level {
    bool ordered;
    int marker_indent;     /* Leading spaces before marker */
    int content_indent;    /* Column where content starts */
    int item_count;        /* Items emitted at this level */
    bool saw_blank;        /* For loose/tight detection */
};

/* In struct md4s_parser: */
struct list_level list_stack[MAX_LIST_DEPTH];
int list_stack_depth;   /* 0 = not in any list */
```

### Interaction with Multi-Line Items

Multi-line items and nesting interact through the container stack:

1. When a continuation line is detected (indent >= content_indent of
   innermost list item), strip the indent and re-classify.
2. If the stripped line contains a list marker, it starts a sub-list.
3. The sub-list's indent is relative to the stripped content.

### Code Locations

| Location | Change |
|----------|--------|
| `struct md4s_parser` (line 83) | Replace `in_list`, `list_ordered`, `list_depth` with `list_stack[]`, `list_stack_depth` |
| `open_list_if_needed` (line 1581) | Push onto list stack |
| `close_list_if_needed` (line 1569) | Pop from list stack; may need to pop multiple levels |
| `process_line`, list cases (line 1840) | Compare indent to stack to determine nesting |
| `classify_line` (line 756) | Indented code check must account for list nesting (currently `!p->in_list`) |
| `d.list_depth` (line 1847) | Set to `list_stack_depth` instead of `(indent > 0) ? 1 : 0` |

### MC/DC Test Cases

| # | Input | Expected depth sequence |
|---|-------|------------------------|
| N1 | `- a\n` | depth 0 only |
| N2 | `- a\n  - b\n` | depth 0, then depth 1 |
| N3 | `- a\n  - b\n    - c\n` | depth 0, 1, 2 |
| N4 | `- a\n  - b\n- c\n` | depth 0, 1, back to 0 (close sub-list) |
| N5 | `- a\n  1. b\n` | unordered depth 0, ordered depth 1 |
| N6 | `- a\n  - b\n  - c\n` | depth 0, depth 1 (two siblings at depth 1) |
| N7 | `- a\n  - b\n    - c\n  - d\n- e\n` | 0, 1, 2, back to 1, back to 0 |
| N8 | `1. a\n   - b\n` | ordered depth 0, unordered sub-list depth 1 |
| N9 | `- a\n  - b\n      - c\n` | depth 0, 1, 2 (deeper indent) |
| N10 | `- a\n- b\n  - c\n` | depth 0 (two items), depth 1 sub-list under item b |

---

## 4. HTML Block Types 1, 3, 4, 5, 7

### Problem

md4s handles HTML block types 2 (comments: `<!--`) and 6 (block-level tags
like `<div>`) but is missing types 1, 3, 4, 5, and 7.

Per CommonMark spec section 4.6:

| Type | Start condition | End condition |
|------|----------------|---------------|
| 1 | `<script>`, `<pre>`, `<style>`, `<textarea>` (case-insensitive) | Closing tag `</script>`, `</pre>`, `</style>`, `</textarea>` |
| 2 | `<!--` | `-->` |
| 3 | `<?` | `?>` |
| 4 | `<!` followed by uppercase letter | `>` |
| 5 | `<![CDATA[` | `]]>` |
| 6 | Block-level tag | Blank line |
| 7 | Complete open or close tag (not in the type 1-6 lists) | Blank line |

### Data Structure Changes

**md4s.c — `struct md4s_parser`:**

```c
int html_block_type;    /* NEW: 1-7, which type of HTML block is active */
```

### Changes to `is_html_block_start()`

The function (line 424) currently returns a simple bool. It must be changed
to return the HTML block type (0 = not an HTML block, 1-7 = type):

```c
static int detect_html_block_type(const char *line, size_t len,
                                  bool in_paragraph)
```

**Type 1 detection** (added before existing type 6 check):

```c
/* Type 1: <script, <pre, <style, <textarea (case-insensitive) */
static const char *type1_tags[] = {
    "script", "pre", "style", "textarea", NULL
};
/* Match: '<' optional_spaces tag_name (space, '>', or end) */
/* Check both open and close variants: <script> or </script> */
```

Compare the extracted tag name (already parsed by the existing tag
extraction code at lines 441-448) against the type 1 list. If matched,
return 1.

**Type 3 detection:**

```c
if (len >= 2 && line[0] == '<' && line[1] == '?')
    return 3;
```

**Type 4 detection:**

```c
if (len >= 2 && line[0] == '<' && line[1] == '!' &&
    len > 2 && line[2] >= 'A' && line[2] <= 'Z')
    return 4;
```

**Type 5 detection:**

```c
if (len >= 9 && memcmp(line, "<![CDATA[", 9) == 0)
    return 5;
```

**Type 7 detection** — the most complex:

A type 7 HTML block starts with a complete open tag or close tag. The tag
must NOT be one of the type 1 tags (script, pre, style, textarea) and must
NOT be a type 6 block-level tag. A type 7 block **cannot interrupt a
paragraph**.

```
Algorithm for type 7 open tag:
1. Match '<'
2. Match tag name (ASCII letters, then letters/digits)
3. Match zero or more attributes:
   - whitespace
   - attribute name (letters, digits, '-', '_', ':', '.')
   - optional: '=' then attribute value
     (unquoted: non-space non-quote non-= non-< non-> non-`
      single-quoted: '...'
      double-quoted: "...")
4. Match optional whitespace
5. Match optional '/'
6. Match '>'
7. Match only optional whitespace to end of line
8. Tag must NOT be a type 1 or type 6 tag
9. If in_paragraph: return 0 (cannot interrupt paragraph)

Algorithm for type 7 close tag:
1. Match '</'
2. Match tag name
3. Match optional whitespace
4. Match '>'
5. Rest of line is optional whitespace
6. Tag must NOT be type 1 or type 6
7. If in_paragraph: return 0
```

### Changes to `is_html_block_end()`

The function (line 498) currently checks for blank line or `-->`. It must
be replaced with a per-type check:

```c
static bool is_html_block_end(const char *line, size_t len,
                              int html_block_type)
```

| Type | End condition | Implementation |
|------|--------------|----------------|
| 1 | Line contains `</script>`, `</pre>`, `</style>`, or `</textarea>` (case-insensitive) | `strcasestr()` or manual scan |
| 2 | Line contains `-->` | Existing code (line 503-507) |
| 3 | Line contains `?>` | Scan for `?` followed by `>` |
| 4 | Line contains `>` | Scan for `>` |
| 5 | Line contains `]]>` | Scan for `]]>` |
| 6 | Blank line | `len == 0` |
| 7 | Blank line | `len == 0` |

### Mid-Line End Detection

Types 1-5 can end on the same line they start. Example:

```
<script>alert("hi")</script>
```

This is a single HTML block. The start detection fires, and end detection
must also be checked on the **same** line. The implementation:

```
In process_line, LINE_HTML_BLOCK case:
1. If this is the opening line (p->state != STATE_HTML_BLOCK yet):
   a. Detect the type: html_block_type = detect_html_block_type(...)
   b. Store p->html_block_type = html_block_type
   c. Emit HTML_BLOCK_ENTER
   d. Emit the line content as TEXT
   e. Check is_html_block_end(line, len, html_block_type)
   f. If end found: emit HTML_BLOCK_LEAVE immediately, stay in STATE_NORMAL
   g. If not: enter STATE_HTML_BLOCK

2. If this is a continuation line (p->state == STATE_HTML_BLOCK):
   a. Emit the line content as TEXT
   b. Check is_html_block_end(line, len, p->html_block_type)
   c. If end found: emit HTML_BLOCK_LEAVE, return to STATE_NORMAL
```

For types 1-5, the end marker can appear *mid-line*. The spec says the
HTML block includes everything up to and including the line containing the
end sequence. So the entire line is emitted; the parser does not split at
the end marker.

### `classify_line` Changes

In `classify_line` (line 784), the HTML block state currently just checks
for blank lines. This must change to call `is_html_block_end()` with the
stored type:

```c
if (p->state == STATE_HTML_BLOCK) {
    if (is_html_block_end(line, len, p->html_block_type)) {
        /* For types 6,7: blank line ends it, so return LINE_BLANK */
        if (p->html_block_type >= 6) {
            if (len == 0 || is_all_whitespace(line, len)) {
                cl.type = LINE_BLANK;
                return cl;
            }
        }
        /* For types 1-5: the end-marker line is part of the block */
        cl.type = LINE_HTML_BLOCK;
        cl.content = line;
        cl.content_length = len;
        /* Set a flag so process_line knows to close after emitting */
        return cl;
    }
    cl.type = LINE_HTML_BLOCK;
    cl.content = line;
    cl.content_length = len;
    return cl;
}
```

A cleaner approach: add a field to `classified_line`:

```c
struct classified_line {
    ...
    bool html_block_ends_here;   /* NEW: this line closes the HTML block */
};
```

### Code Locations

| Location | Change |
|----------|--------|
| `struct md4s_parser` (line 83) | Add `int html_block_type` |
| `is_html_block_start` (line 424) | Rename to `detect_html_block_type`, return int 0-7 |
| `is_html_block_end` (line 498) | Add `int html_block_type` parameter, per-type logic |
| `classify_line`, HTML block state (line 784) | Use `is_html_block_end(line, len, p->html_block_type)` |
| `classify_line`, HTML block detection (line 891) | Use `detect_html_block_type()` |
| `process_line`, HTML block opening (line 1898) | Store `p->html_block_type`, check mid-line end |
| `process_line`, HTML block continuation (line 1899) | Check per-type end condition |
| `process_line`, `LINE_BLANK` case (line 1743) | Only close HTML block for types 6,7 on blank |

### MC/DC Test Cases

**Type 1:**

| # | Input | Expected |
|---|-------|----------|
| H1a | `<script>\nalert(1)\n</script>\n` | HTML block: 3 lines |
| H1b | `<script>alert(1)</script>\n` | HTML block: 1 line (mid-line end) |
| H1c | `<SCRIPT>\nfoo\n</script>\n` | Case-insensitive start |
| H1d | `<pre>\ncode\n</pre>\n` | Type 1 with `<pre>` |
| H1e | `<style>\n.a{}\n</style>\n` | Type 1 with `<style>` |
| H1f | `<textarea>\ntext\n</textarea>\n` | Type 1 with `<textarea>` |
| H1g | `<script>\nno close\n` | At finalize, close HTML block |
| H1h | `<script>\nfoo\n\nbar\n</script>\n` | Blank line does NOT end type 1 |

**Type 3:**

| # | Input | Expected |
|---|-------|----------|
| H3a | `<?xml version="1.0"?>\n` | Single-line HTML block (mid-line end at `?>`) |
| H3b | `<?php\necho "hi";\n?>\n` | Multi-line, ends at `?>` |
| H3c | `<?php\nno close\n` | At finalize, close HTML block |

**Type 4:**

| # | Input | Expected |
|---|-------|----------|
| H4a | `<!DOCTYPE html>\n` | Single-line (ends at `>`) |
| H4b | `<!DOCTYPE\nhtml>\n` | Multi-line, ends when `>` found |
| H4c | `<!doctype html>\n` | NOT type 4 (lowercase after `<!`); may be type 6 or paragraph |

**Type 5:**

| # | Input | Expected |
|---|-------|----------|
| H5a | `<![CDATA[\ndata\n]]>\n` | Multi-line, ends at `]]>` |
| H5b | `<![CDATA[data]]>\n` | Single-line (mid-line end) |
| H5c | `<![CDATA[\nno close\n` | At finalize, close HTML block |

**Type 7:**

| # | Input | Expected |
|---|-------|----------|
| H7a | `<custom>\nfoo\n\n` | HTML block, ends at blank line |
| H7b | `<custom />\nfoo\n\n` | Self-closing tag, ends at blank |
| H7c | `</custom>\nfoo\n\n` | Close tag starts HTML block |
| H7d | `<custom>\n` (after paragraph) | NOT an HTML block (type 7 cannot interrupt paragraph) |
| H7e | `<div>\n` | Type 6, NOT type 7 (div is a block-level tag) |
| H7f | `<script>\n` | Type 1, NOT type 7 |
| H7g | `<custom attr="val">\n\n` | Open tag with attributes |
| H7h | `<custom attr>\nfoo\n\n` | Open tag with boolean attribute |

---

## 5. Tab Expansion

### Problem

md4s's `count_leading_spaces` (line 206) only counts space characters.
CommonMark specifies that tabs expand to the next tab stop at a multiple of
4. A tab at column 0 counts as 4 spaces, at column 1 counts as 3, at
column 2 counts as 2, at column 3 counts as 1, at column 4 counts as 4
again, etc.

### Where Tab Expansion Happens

Tab expansion should happen in a new function that replaces
`count_leading_spaces` for indentation measurement:

```c
/*
 * Count the effective indentation of leading whitespace, expanding
 * tabs to 4-column tab stops. Returns the effective indent and sets
 * *bytes_consumed to the number of raw bytes in the leading whitespace.
 */
static int count_indent(const char *s, size_t len, size_t *bytes_consumed)
{
    int indent = 0;
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        if (s[i] == '\t')
            indent = (indent + 4) & ~3;  /* next multiple of 4 */
        else
            indent++;
        i++;
    }
    if (bytes_consumed)
        *bytes_consumed = i;
    return indent;
}
```

### The Formula

For each tab character encountered at effective column `c`:

```
new_column = (c + 4) & ~3
```

This is equivalent to: `new_column = ((c / 4) + 1) * 4`, i.e., advance to
the next multiple of 4.

Equivalently, a tab at column `c` adds `4 - (c % 4)` spaces.

### Affected Constructs

Every block construct that uses indentation:

| Construct | Current code | Change needed |
|-----------|-------------|---------------|
| Indented code (4+ spaces) | `classify_line` line 881 | Use `count_indent` instead of checking `line[0..3] == ' '` |
| List markers (leading indent) | `classify_line` lines 900, 926 | `count_indent` for indent measurement |
| Blockquote (leading `>`) | `classify_line` line 869 | Allow up to 3 spaces (tabs expanded) before `>` |
| ATX heading | `classify_line` line 839 | Allow up to 3 spaces before `#` |
| Thematic break | `is_thematic_break` line 223 | Tabs count as whitespace (already works since tabs are skipped as spaces) |
| Fenced code | `classify_line` line 803 | Allow up to 3 spaces (tabs expanded) before fence |
| Fenced code indent stripping | `classify_line` line 768 | Tab-aware fence indent |

### Partial Tab Handling

When a tab partially contributes to indentation (e.g., a tab gives 4
effective spaces, but only 3 are consumed by a block structure like
blockquote's `>`), the remaining 1 space of the tab carries forward. This
requires tracking "virtual spaces remaining from a partially consumed tab."

```c
/* In the indent-stripping logic: */
int spaces_to_strip = desired_indent;
size_t byte_pos = 0;
int effective_col = 0;

while (spaces_to_strip > 0 && byte_pos < len) {
    if (line[byte_pos] == '\t') {
        int tab_width = 4 - (effective_col % 4);
        if (tab_width <= spaces_to_strip) {
            spaces_to_strip -= tab_width;
            effective_col += tab_width;
            byte_pos++;
        } else {
            /* Partial tab: consume part, leave virtual spaces */
            spaces_to_strip = 0;
            /* The remaining (tab_width - spaces_to_strip) virtual
               spaces become leading content spaces */
        }
    } else if (line[byte_pos] == ' ') {
        spaces_to_strip--;
        effective_col++;
        byte_pos++;
    } else {
        break;
    }
}
```

### Code Locations

| Location | Change |
|----------|--------|
| `count_leading_spaces` (line 206) | Keep but add `count_indent` alongside |
| `classify_line` (line 756) | Use `count_indent` for all indent calculations |
| `is_thematic_break` (line 223) | Handle tabs (already mostly works) |
| List continuation detection (Section 2) | Use `count_indent` |
| `is_all_whitespace` (line 214) | Already handles `\t` |

### MC/DC Test Cases

| # | Input | Expected |
|---|-------|----------|
| T1 | `\tfoo\n` | Indented code block (tab = 4 spaces) |
| T2 | `  \tfoo\n` | Indented code block (2 spaces + tab = 4 effective) |
| T3 | `   \tfoo\n` | Indented code block (3 spaces + tab(1) = 4 effective) |
| T4 | ` \tfoo\n` | Indented code block (1 space + tab(3) = 4 effective) |
| T5 | `\t- foo\n` | List item (tab = 4 spaces, but `>3` before marker means this is indented code — actually the `-` at column 4 makes this ambiguous; CommonMark says tab before `-` makes this an indented code block if not in a list) |
| T6 | `- foo\n\t  bar\n` | List item continuation (tab = 4, then 2 spaces = 6 >= content_indent of 2) |
| T7 | `>\tfoo\n` | Blockquote with tab after `>` (expands to 3 spaces, so content has extra whitespace) |
| T8 | `\t\tfoo\n` | Indented code block, content has 4 extra spaces of indent |
| T9 | `- foo\n\tbar\n` | List item continuation (tab = 4 >= content_indent of 2) |
| T10 | ` \t \t foo\n` | Effective indent: 1 + 3(tab) + 1 + 3(tab) + 1 = 9, indented code |

---

## 6. Paragraph Interruption Rules

### Problem

md4s allows any block construct to interrupt an in-progress paragraph.
CommonMark restricts this:

1. An ordered list whose start number is not 1 cannot interrupt a paragraph.
2. An empty list item (marker + space + nothing) cannot interrupt a paragraph.
3. An HTML block type 7 cannot interrupt a paragraph.

### How to Detect "Currently in a Paragraph"

The parser already tracks this: `p->last_type == LINE_PARAGRAPH &&
p->emitted_count > 0 && !p->needs_separator`.

This condition means a paragraph is open and the current line would
normally be a continuation. Define a helper:

```c
static bool in_open_paragraph(const struct md4s_parser *p)
{
    return p->last_type == LINE_PARAGRAPH &&
           p->emitted_count > 0 &&
           !p->needs_separator;
}
```

### Where Checks Go

In `classify_line` (or a post-classification filter in `process_line`),
after classifying a line, check interruption rules:

```c
/* In process_line, after classify_line returns: */
if (in_open_paragraph(p)) {
    /* Rule 1: Ordered list with start != 1 cannot interrupt. */
    if (cl.type == LINE_ORDERED_LIST && cl.ordered_number != 1) {
        cl.type = LINE_PARAGRAPH;
        /* Recalculate content to be the full line */
    }

    /* Rule 2: Empty list item cannot interrupt. */
    if ((cl.type == LINE_UNORDERED_LIST || cl.type == LINE_ORDERED_LIST) &&
        cl.content_length == 0) {
        cl.type = LINE_PARAGRAPH;
    }

    /* Rule 3: HTML block type 7 cannot interrupt. */
    /* (handled by detect_html_block_type receiving in_paragraph flag) */
}
```

### Interaction with Deferred Lines

The deferred line mechanism (for table/setext lookahead) complicates this.
When a paragraph line is deferred (because it contains `|`), the deferred
line has already been classified as `LINE_PARAGRAPH`. The interruption
check for the *next* line needs to know a paragraph is open.

The deferred line acts as a "virtual open paragraph" — set a flag:

```c
bool deferred_is_paragraph;  /* The deferred line was a paragraph */
```

When checking interruption for the line after a deferred paragraph, treat
`deferred_is_paragraph` as equivalent to `in_open_paragraph`.

### Code Locations

| Location | Change |
|----------|--------|
| `process_line` (line 1675) | Add interruption check after `classify_line` |
| `classify_line` ordered list (line 924) | No change needed (detection stays) |
| `classify_line` list markers (line 898) | No change needed |
| `detect_html_block_type` (Section 4) | Accept `bool in_paragraph` parameter |
| `md4s_feed` loop, table/setext handling (line 2090) | Pass interruption context |
| New helper `in_open_paragraph` | Add near line 1569 |

### MC/DC Test Cases

| # | Input | Expected |
|---|-------|----------|
| I1 | `para\n2. item\n` | Paragraph "para 2. item" (ordered start != 1 cannot interrupt) |
| I2 | `para\n1. item\n` | Paragraph "para", then ordered list "item" (start == 1 CAN interrupt) |
| I3 | `\n2. item\n` | Ordered list with start=2 (not interrupting paragraph, so allowed) |
| I4 | `para\n- \n` | Paragraph "para - " (empty list item cannot interrupt) |
| I5 | `para\n- foo\n` | Paragraph "para", then list item "foo" (non-empty CAN interrupt) |
| I6 | `para\n-\n` | Depends: `-` alone may be setext heading or thematic break |
| I7 | `para\n<custom>\n\n` | Paragraph "para <custom>" (type 7 cannot interrupt paragraph) |
| I8 | `para\n<div>\n\n` | Paragraph "para", then HTML block `<div>` (type 6 CAN interrupt) |
| I9 | `para\n<!-- comment -->\n` | Paragraph "para", then HTML block (type 2 CAN interrupt) |
| I10 | `\n<custom>\n\n` | HTML block type 7 (not interrupting, so allowed) |
| I11 | `para\n* \n` | Paragraph "para * " (empty unordered item cannot interrupt) |
| I12 | `para\n1) item\n` | Paragraph "para", then ordered list (start=1, `)` delimiter, CAN interrupt) |

---

## 7. Multi-Line Reference Definitions

### Problem

md4s requires reference link definitions on a single line (line 292):
`[label]: url`. CommonMark allows the label, URL, and title to span
multiple lines:

```
[label]:
  url
  "title spanning
  multiple lines"
```

### State Machine Design

Add a state for accumulating multi-line definitions:

```c
enum parser_state {
    STATE_NORMAL,
    STATE_FENCED_CODE,
    STATE_HTML_BLOCK,
    STATE_LINK_DEF,         /* NEW */
};
```

The link definition accumulation uses a small sub-state machine:

```c
enum link_def_phase {
    LINKDEF_LABEL,    /* Accumulating the [label]: part */
    LINKDEF_URL,      /* Accumulating the URL */
    LINKDEF_TITLE,    /* Accumulating the title */
};
```

**Data structures in `struct md4s_parser`:**

```c
/* Multi-line link definition accumulation. */
enum link_def_phase linkdef_phase;
char *linkdef_label;          /* Heap-allocated label text */
size_t linkdef_label_len;
char *linkdef_url;            /* Heap-allocated URL text */
size_t linkdef_url_len;
char linkdef_title_delim;     /* '"', '\'', or '(' */
char *linkdef_title;          /* Heap-allocated title text */
size_t linkdef_title_len;
char *linkdef_accum;          /* Line accumulator for multi-line parts */
size_t linkdef_accum_len;
size_t linkdef_accum_cap;
```

### Accumulation Algorithm

```
When classify_line returns LINE_PARAGRAPH and the line starts with '[':
  1. Try single-line parse (existing try_parse_link_def).
  2. If that fails, check if it looks like a partial definition:
     a. Line matches: [label]:
        - Start LINKDEF_URL phase
        - Store label, enter STATE_LINK_DEF
     b. Line matches: [label]: url
        - URL found, check for title start
        - If title delimiter at end, enter LINKDEF_TITLE
        - Otherwise, definition is complete
     c. Line matches: [label]: url "partial title
        - Enter LINKDEF_TITLE

When in STATE_LINK_DEF:
  Phase LINKDEF_URL:
    - Next line (trimmed) is the URL
    - If URL line also starts a title: enter LINKDEF_TITLE
    - If URL line is blank: abort, flush accumulated as paragraph

  Phase LINKDEF_TITLE:
    - Accumulate until closing delimiter found
    - Title delimiter pairs: "..." or '...' or (...)
    - Closing delimiter must be followed by only whitespace to EOL
    - On close: store definition, return to STATE_NORMAL
    - On blank line or non-continuation: abort, flush as paragraph

On abort (line doesn't continue definition):
  - Everything accumulated so far is flushed as paragraph content
  - Return to STATE_NORMAL
  - Process the aborting line normally
```

### Title Parsing

Three delimiter styles:

| Delimiter | Open | Close | Notes |
|-----------|------|-------|-------|
| Double quote | `"` | `"` | Most common |
| Single quote | `'` | `'` | Less common |
| Parentheses | `(` | `)` | Balanced; nested parens NOT allowed in CommonMark |

Multi-line titles: the title can span lines. Internal newlines become
spaces in the stored title. Example:

```
[foo]: /url "title
continued here"
```

Stored title: `"title continued here"`.

### Interaction with Streaming Architecture

The multi-line definition accumulation buffers lines, which slightly delays
event emission. However, link definitions are silent (they produce no
events), so the delay is invisible to the consumer. If the accumulation
aborts, the buffered lines are flushed as paragraph content.

The deferred line mechanism (for table/setext lookahead) interacts: if a
line starting with `[` is deferred for pipe-content table detection, the
link definition check must happen before or instead of the table check.
Since `[` lines don't contain `|` in the label part, this should rarely
conflict. However, a line like `[foo|bar]: url` would be deferred. The
fix: check for link definition *before* deferring.

### Detail Field Changes

**md4s.h — `struct md4s_detail`:**

The `title` field already exists on `LINK_ENTER` and `IMAGE_ENTER`. No
new fields needed, but `find_link_def` must return both URL and title:

```c
struct link_def_result {
    const char *url;
    size_t url_len;
    const char *title;
    size_t title_len;
};
```

Update the link definition storage:

```c
struct {
    char *label;
    char *url;
    char *title;       /* NEW: heap-allocated, NULL if no title */
} link_defs[MAX_LINK_DEFS];
```

### Code Locations

| Location | Change |
|----------|--------|
| `enum parser_state` (line 55) | Add `STATE_LINK_DEF` |
| `struct md4s_parser` (line 83) | Add linkdef accumulation fields |
| `try_parse_link_def` (line 292) | Extend to detect partial definitions |
| New function `try_start_multiline_link_def` | Detect `[label]:` pattern |
| New function `continue_link_def` | Called from process_line when in STATE_LINK_DEF |
| `process_line` (line 1675) | Check STATE_LINK_DEF before normal processing |
| `link_defs[]` (line 129) | Add `title` field |
| `find_link_def` (line 364) | Return title alongside URL |
| `parse_inline_depth`, reference link handling (line 1474) | Use title from definition |
| `md4s_destroy` (line 2293) | Free `linkdef_*` buffers and `link_defs[].title` |

### MC/DC Test Cases

| # | Input | Expected |
|---|-------|----------|
| R1 | `[foo]: /url\n\n[foo]\n` | Link to /url, no title |
| R2 | `[foo]:\n  /url\n\n[foo]\n` | Multi-line: URL on next line |
| R3 | `[foo]: /url "title"\n\n[foo]\n` | Single-line with title |
| R4 | `[foo]: /url\n  "title"\n\n[foo]\n` | URL then title on next line |
| R5 | `[foo]:\n  /url\n  "title"\n\n[foo]\n` | All three on separate lines |
| R6 | `[foo]: /url "multi\nline title"\n\n[foo]\n` | Title spans two lines |
| R7 | `[foo]: /url 'single'\n\n[foo]\n` | Single-quote title |
| R8 | `[foo]: /url (paren)\n\n[foo]\n` | Parenthesized title |
| R9 | `[foo]:\n\n[foo]\n` | Blank line aborts: `[foo]:` becomes paragraph |
| R10 | `[foo]: /url\n  "unclosed\n\n[foo]\n` | Blank line aborts title accumulation |
| R11 | `[foo]: /url "title"\nbar\n` | Complete definition, "bar" is new paragraph |
| R12 | `[FOO]: /url\n\n[foo]\n` | Case-insensitive label match |

---

## 8. Configuration Flags API

### Problem

md4s has no configuration mechanism. md4c provides 16 flags to control
parser behavior. md4s should support a subset of useful flags.

### API Design

Add a flags parameter to `md4s_create()`:

```c
/* md4s.h */

/* Parser configuration flags. */
#define MD4S_FLAG_TABLES          0x0001  /* GFM tables (default: on) */
#define MD4S_FLAG_STRIKETHROUGH   0x0002  /* ~~strikethrough~~ (default: on) */
#define MD4S_FLAG_TASKLISTS       0x0004  /* [x] task lists (default: on) */
#define MD4S_FLAG_NOHTMLBLOCKS    0x0008  /* Disable HTML blocks */
#define MD4S_FLAG_NOHTMLSPANS     0x0010  /* Disable inline HTML (<tag>) */
#define MD4S_FLAG_NOINDENTEDCODE  0x0020  /* Disable indented code blocks */
#define MD4S_FLAG_PERMISSIVE_AUTOLINKS  0x0040  /* www.example.com without <> */

/* Default flags: all extensions enabled, no restrictions. */
#define MD4S_FLAG_DEFAULT  (MD4S_FLAG_TABLES | MD4S_FLAG_STRIKETHROUGH | \
                            MD4S_FLAG_TASKLISTS)

/*
 * Creates a streaming markdown parser with configuration flags.
 *
 * callback  — Event handler. Must not be NULL.
 * user_data — Opaque pointer forwarded to every callback.
 * flags     — Bitwise OR of MD4S_FLAG_* constants, or 0 for defaults.
 *
 * Returns the parser handle, or NULL on allocation failure.
 */
struct md4s_parser *md4s_create(md4s_callback callback, void *user_data);

/* New overload — or modify existing signature: */
struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
                                   unsigned int flags);
```

**Backward compatibility:** Two options:

**Option A — Keep existing signature, add new function (recommended):**

`md4s_create()` continues to work with default flags. A new
`md4s_create_ex()` accepts flags. Internally, `md4s_create()` calls
`md4s_create_ex(callback, user_data, MD4S_FLAG_DEFAULT)`.

**Option B — Change existing signature:** Add `unsigned int flags` to
`md4s_create()`. This is an ABI break. Since md4s is pre-1.0, this may be
acceptable. Callers passing `0` get defaults (all extensions off, which is
a behavior change). Better: callers pass `MD4S_FLAG_DEFAULT` for current
behavior.

**Recommended: Option A.** No existing code breaks.

### Data Structure Changes

```c
/* In struct md4s_parser: */
unsigned int flags;
```

### Where Each Flag Is Checked

| Flag | Check location | Effect |
|------|---------------|--------|
| `MD4S_FLAG_TABLES` | `md4s_feed` loop, line 2117 (pipe detection/defer) | Skip table detection; treat pipe lines as paragraphs |
| `MD4S_FLAG_TABLES` | `process_table_header` (line 1620) | Not called if disabled |
| `MD4S_FLAG_STRIKETHROUGH` | `parse_inline_depth`, `~~` handling (line 1268) | Skip; treat `~~` as literal text |
| `MD4S_FLAG_TASKLISTS` | `process_line`, list cases, task detection (line 1848) | Skip; `[x]` becomes literal text |
| `MD4S_FLAG_NOHTMLBLOCKS` | `classify_line`, HTML block detection (line 891) | Return `LINE_PARAGRAPH` instead of `LINE_HTML_BLOCK` |
| `MD4S_FLAG_NOHTMLBLOCKS` | `classify_line`, HTML block state (line 784) | Never enter `STATE_HTML_BLOCK` |
| `MD4S_FLAG_NOHTMLSPANS` | `parse_inline_depth`, autolink `<` handling (line 1322) | Skip; `<url>` becomes literal text |
| `MD4S_FLAG_NOINDENTEDCODE` | `classify_line`, indented code (line 881) | Return `LINE_PARAGRAPH` instead |
| `MD4S_FLAG_PERMISSIVE_AUTOLINKS` | `parse_inline_depth` | Detect bare URLs like `http://...` without `<>` |

### Implementation Pattern

Each check follows the same pattern — a guard at the top of the relevant
code path:

```c
/* Example: table detection in md4s_feed */
if ((parser->flags & MD4S_FLAG_TABLES) &&
    cl.type == LINE_PARAGRAPH &&
    parser->state == STATE_NORMAL &&
    !parser->in_table) {
    /* ... existing pipe detection ... */
}

/* Example: strikethrough in parse_inline_depth */
if ((p->flags & MD4S_FLAG_STRIKETHROUGH) &&
    pos + 1 < length && text[pos] == '~' && text[pos + 1] == '~') {
    /* ... existing ~~ handling ... */
}
```

Note: since the default has these features enabled, the guard uses `&` to
check the flag is set (meaning the feature is active). Code that disables
features (NOHTMLBLOCKS, etc.) checks when the flag IS set:

```c
/* HTML blocks disabled */
if (p->flags & MD4S_FLAG_NOHTMLBLOCKS) {
    /* Skip HTML block detection, classify as paragraph */
}
```

### Changes to md4s.h

```c
/* Add after line 92 (enum md4s_event): */

/* ------------------------------------------------------------------ */
/* Configuration flags                                                */
/* ------------------------------------------------------------------ */

#define MD4S_FLAG_TABLES          0x0001
#define MD4S_FLAG_STRIKETHROUGH   0x0002
#define MD4S_FLAG_TASKLISTS       0x0004
#define MD4S_FLAG_NOHTMLBLOCKS    0x0008
#define MD4S_FLAG_NOHTMLSPANS     0x0010
#define MD4S_FLAG_NOINDENTEDCODE  0x0020
#define MD4S_FLAG_PERMISSIVE_AUTOLINKS  0x0040

#define MD4S_FLAG_DEFAULT  (MD4S_FLAG_TABLES | MD4S_FLAG_STRIKETHROUGH | \
                            MD4S_FLAG_TASKLISTS)

/* Add after md4s_create declaration: */
struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
                                   unsigned int flags);
```

### Code Locations

| Location | Change |
|----------|--------|
| `md4s.h` (after line 92) | Add flag constants |
| `md4s.h` (after line 177) | Add `md4s_create_ex` declaration |
| `struct md4s_parser` (line 83) | Add `unsigned int flags` |
| `md4s_create` (line 2163) | Call `md4s_create_ex(callback, user_data, MD4S_FLAG_DEFAULT)` |
| New `md4s_create_ex` (after line 2195) | Full initialization with flags |
| `classify_line` (line 756) | Check NOHTMLBLOCKS, NOINDENTEDCODE |
| `parse_inline_depth` (line 1085) | Check STRIKETHROUGH, NOHTMLSPANS |
| `md4s_feed` (line 2117) | Check TABLES |
| `process_line`, list cases (line 1848) | Check TASKLISTS |

### MC/DC Test Cases

| # | Flags | Input | Expected |
|---|-------|-------|----------|
| F1 | `MD4S_FLAG_DEFAULT` | `~~str~~\n` | Strikethrough span |
| F2 | `0` (no flags) | `~~str~~\n` | Literal text `~~str~~` |
| F3 | `MD4S_FLAG_DEFAULT` | `\| a \| b \|\n\|---\|---\|\n` | Table |
| F4 | `MD4S_FLAG_STRIKETHROUGH` (no TABLES) | `\| a \| b \|\n\|---\|---\|\n` | Paragraph (no table) |
| F5 | `MD4S_FLAG_DEFAULT` | `- [x] task\n` | Task list item (is_task=true) |
| F6 | `MD4S_FLAG_TABLES` (no TASKLISTS) | `- [x] task\n` | List item with literal "[x] task" |
| F7 | `MD4S_FLAG_NOHTMLBLOCKS` | `<div>\nfoo\n</div>\n` | Paragraph "<div> foo </div>" |
| F8 | `MD4S_FLAG_DEFAULT` | `<div>\nfoo\n</div>\n` | HTML block |
| F9 | `MD4S_FLAG_NOHTMLSPANS` | `<a href="#">link</a>\n` | Literal text including the tags |
| F10 | `MD4S_FLAG_NOINDENTEDCODE` | `    code\n` | Paragraph "code" (not indented code block) |
| F11 | `MD4S_FLAG_DEFAULT` | `    code\n` | Indented code block |
| F12 | `MD4S_FLAG_DEFAULT \| MD4S_FLAG_NOHTMLBLOCKS` | `<div>hi</div>\n` | Paragraph (HTML blocks disabled) |

---

## Implementation Order

The features have dependencies that constrain implementation order:

```
1. Tab Expansion (Section 5)
   └── No dependencies. Foundation for correct indentation everywhere.

2. Configuration Flags API (Section 8)
   └── No dependencies. Enables incremental testing of other features.

3. HTML Block Types (Section 4)
   └── Depends on: Tab Expansion (for leading indent), Flags (NOHTMLBLOCKS)

4. Paragraph Interruption Rules (Section 6)
   └── Depends on: HTML Block Types (type 7 rule)

5. Multi-Line Reference Definitions (Section 7)
   └── Depends on: Paragraph Interruption (definitions don't interrupt paragraphs)

6. Multi-Line List Items (Section 2)
   └── Depends on: Tab Expansion (continuation indent)

7. Proper List Nesting (Section 3)
   └── Depends on: Multi-Line List Items (nesting requires continuation awareness)

8. Loose vs Tight Lists (Section 1)
   └── Depends on: Multi-Line Items, List Nesting (blank detection needs both)
```

**Recommended implementation sequence:**
5 → 8 → 4 → 6 → 7 → 2 → 3 → 1

### Estimated Complexity

| Section | New/changed lines (est.) | Risk |
|---------|--------------------------|------|
| 5. Tab Expansion | ~60 | Low — isolated function |
| 8. Config Flags | ~80 | Low — guard checks only |
| 4. HTML Block Types | ~200 | Medium — multiple type-specific paths |
| 6. Paragraph Interruption | ~40 | Low — simple guards |
| 7. Multi-Line Ref Defs | ~250 | Medium — new state machine |
| 2. Multi-Line List Items | ~300 | High — significant control flow change |
| 3. List Nesting | ~250 | High — stack-based redesign |
| 1. Loose/Tight Lists | ~200 | High — buffering or retroactive events |

Total estimated: ~1380 new/changed lines of C, plus ~400 lines of tests.
