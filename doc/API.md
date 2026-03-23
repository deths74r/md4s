# md4s API Reference

## Lifecycle

### `md4s_create`

```c
struct md4s_parser *md4s_create(md4s_callback callback, void *user_data);
```

Creates a streaming markdown parser with default configuration flags (`MD4S_FLAG_DEFAULT`).

- **callback** — Function called for each parser event. Must not be NULL.
- **user_data** — Opaque pointer forwarded to every callback invocation.
- **Returns** — Parser handle, or NULL on allocation failure.

### `md4s_create_ex`

```c
struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
                                    unsigned int flags);
```

Creates a streaming markdown parser with custom configuration flags.

- **flags** — Bitwise OR of `MD4S_FLAG_*` constants. Use `0` to disable all extensions (strict CommonMark), or `MD4S_FLAG_DEFAULT` for the standard GFM-like configuration.

### `md4s_feed`

```c
void md4s_feed(struct md4s_parser *parser, const char *data, size_t length);
```

Feeds raw markdown bytes to the parser. May be called any number of times with arbitrarily sized chunks — 1 byte at a time, full lines, or the entire document. Callbacks fire synchronously during this call for any completed lines found in the input.

Passing NULL for `parser` or `data`, or 0 for `length`, is a safe no-op.

### `md4s_finalize`

```c
char *md4s_finalize(struct md4s_parser *parser);
```

Finalizes the stream. Flushes any buffered partial line as a completed line, closes any open blocks (unclosed fences, lists, blockquotes, HTML blocks), and emits final events.

- **Returns** — The accumulated raw markdown text as a heap-allocated string. Caller must `free()` the returned pointer. Returns NULL if no data was fed.

After this call, the parser should not receive further `md4s_feed()` calls.

### `md4s_cancel`

```c
char *md4s_cancel(struct md4s_parser *parser);
```

Cancels the stream without finalizing. No closing events are emitted. Open blocks are not closed.

- **Returns** — The accumulated raw markdown text. Caller must `free()`. Returns NULL if no data was fed.

### `md4s_destroy`

```c
void md4s_destroy(struct md4s_parser *parser);
```

Destroys the parser and frees all internal buffers. Passing NULL is safe.

---

## Callback

```c
typedef void (*md4s_callback)(
    enum md4s_event event,
    const struct md4s_detail *detail,
    void *user_data);
```

Called synchronously for every parser event. The `detail` pointer and all strings it references (`text`, `language`, `url`, `title`) are valid only for the duration of the callback. The consumer must copy any data it needs to retain.

---

## Events

### Block-Level Events

Block events come in `_ENTER` / `_LEAVE` pairs. They are never nested except where noted.

| Event | Detail Fields | Description |
|-------|--------------|-------------|
| `MD4S_HEADING_ENTER` | `heading_level` (1-6) | ATX or setext heading |
| `MD4S_HEADING_LEAVE` | | |
| `MD4S_PARAGRAPH_ENTER` | | |
| `MD4S_PARAGRAPH_LEAVE` | | |
| `MD4S_CODE_BLOCK_ENTER` | `language`, `language_length` | Fenced (with optional language) or indented |
| `MD4S_CODE_BLOCK_LEAVE` | | |
| `MD4S_BLOCKQUOTE_ENTER` | | May contain nested block events |
| `MD4S_BLOCKQUOTE_LEAVE` | | |
| `MD4S_LIST_ENTER` | `ordered`, `is_tight`, `item_number` | `item_number` is the start number for ordered lists |
| `MD4S_LIST_LEAVE` | | |
| `MD4S_LIST_ITEM_ENTER` | `item_number`, `list_depth`, `is_task`, `task_checked` | `list_depth` is 0 for top-level |
| `MD4S_LIST_ITEM_LEAVE` | | |
| `MD4S_THEMATIC_BREAK` | | `---`, `***`, or `___` |
| `MD4S_TABLE_ENTER` | `column_count` | GFM pipe table |
| `MD4S_TABLE_LEAVE` | | |
| `MD4S_TABLE_HEAD_ENTER` | | |
| `MD4S_TABLE_HEAD_LEAVE` | | |
| `MD4S_TABLE_BODY_ENTER` | | |
| `MD4S_TABLE_BODY_LEAVE` | | |
| `MD4S_TABLE_ROW_ENTER` | | |
| `MD4S_TABLE_ROW_LEAVE` | | |
| `MD4S_TABLE_CELL_ENTER` | `cell_alignment` | 0=none, 1=left, 2=center, 3=right |
| `MD4S_TABLE_CELL_LEAVE` | | |
| `MD4S_HTML_BLOCK_ENTER` | | Raw HTML block (all 7 CommonMark types) |
| `MD4S_HTML_BLOCK_LEAVE` | | |

### Inline Span Events

Inline events also pair. They may nest (e.g., bold inside italic inside a link).

| Event | Detail Fields | Description |
|-------|--------------|-------------|
| `MD4S_BOLD_ENTER` | | `**` or `__` |
| `MD4S_BOLD_LEAVE` | | |
| `MD4S_ITALIC_ENTER` | | `*` or `_` |
| `MD4S_ITALIC_LEAVE` | | |
| `MD4S_CODE_SPAN_ENTER` | | `` ` `` (backtick code span) |
| `MD4S_CODE_SPAN_LEAVE` | | |
| `MD4S_STRIKETHROUGH_ENTER` | | `~~` |
| `MD4S_STRIKETHROUGH_LEAVE` | | |
| `MD4S_LINK_ENTER` | `url`, `url_length`, `title`, `title_length` | `[text](url "title")`, `[text][ref]`, `[text]`, or autolink |
| `MD4S_LINK_LEAVE` | | |
| `MD4S_IMAGE_ENTER` | `url`, `url_length`, `title`, `title_length` | `![alt](url "title")` |
| `MD4S_IMAGE_LEAVE` | | |

### Content Events

| Event | Detail Fields | Description |
|-------|--------------|-------------|
| `MD4S_TEXT` | `text`, `text_length` | Plain text content (may be emitted multiple times per line) |
| `MD4S_CODE_TEXT` | `text`, `text_length` | Text inside a fenced or indented code block |
| `MD4S_ENTITY` | `text`, `text_length` | Raw entity text (e.g., `&amp;`, `&#8212;`, `&#x1F4A9;`). The consumer is responsible for decoding. |
| `MD4S_HTML_INLINE` | `text`, `text_length` | Raw inline HTML (e.g., `<br>`, `<!-- comment -->`) |
| `MD4S_SOFTBREAK` | | Line break within a paragraph (typically rendered as a space or newline) |
| `MD4S_HARDBREAK` | | Hard line break (trailing 2+ spaces or `\` before newline) |
| `MD4S_NEWLINE` | | Structural newline after a block (for output formatting) |
| `MD4S_BLOCK_SEPARATOR` | | Blank line between blocks (for output formatting) |

### Streaming Events

| Event | Detail Fields | Description |
|-------|--------------|-------------|
| `MD4S_PARTIAL_LINE` | `text`, `text_length` | Incomplete line preview — the text accumulated so far without a newline. May be cleared and replaced. |
| `MD4S_PARTIAL_CLEAR` | | Erase the previous partial line. Fired before a completed line's events or before a new partial update. |

---

## Detail Struct

```c
struct md4s_detail {
    int heading_level;          /* HEADING_ENTER: 1-6 */
    const char *language;       /* CODE_BLOCK_ENTER: info string (NULL if none) */
    size_t language_length;
    const char *url;            /* LINK_ENTER, IMAGE_ENTER: URL */
    size_t url_length;
    const char *title;          /* LINK_ENTER, IMAGE_ENTER: title (NULL if none) */
    size_t title_length;
    bool ordered;               /* LIST_ENTER: true = ordered */
    bool is_tight;              /* LIST_ENTER: true = tight (no paragraph wrappers) */
    int item_number;            /* LIST_ENTER: start number. LIST_ITEM_ENTER: 1-based number */
    int list_depth;             /* LIST_ITEM_ENTER: nesting depth (0 = top-level) */
    bool task_checked;          /* LIST_ITEM_ENTER: checkbox is checked */
    bool is_task;               /* LIST_ITEM_ENTER: this is a task list item */
    int cell_alignment;         /* TABLE_CELL_ENTER: 0=none, 1=left, 2=center, 3=right */
    int column_count;           /* TABLE_ENTER: number of columns */
    const char *text;           /* TEXT, CODE_TEXT, ENTITY, HTML_INLINE, PARTIAL_LINE */
    size_t text_length;
};
```

Only the fields relevant to the current event are meaningful. All other fields are zero/NULL. All pointers are valid only for the duration of the callback — copy if you need to retain.

---

## Configuration Flags

Flags are passed to `md4s_create_ex()`. `md4s_create()` uses `MD4S_FLAG_DEFAULT`.

| Flag | Value | Default | Description |
|------|-------|---------|-------------|
| `MD4S_FLAG_TABLES` | `0x0001` | On | GFM pipe-delimited tables |
| `MD4S_FLAG_STRIKETHROUGH` | `0x0002` | On | `~~strikethrough~~` |
| `MD4S_FLAG_TASKLISTS` | `0x0004` | On | `- [x]` / `- [ ]` task lists |
| `MD4S_FLAG_NOHTMLBLOCKS` | `0x0008` | Off | Disable HTML block recognition |
| `MD4S_FLAG_NOHTMLSPANS` | `0x0010` | Off | Disable inline HTML and autolinks |
| `MD4S_FLAG_NOINDENTEDCODE` | `0x0020` | Off | Disable 4-space indented code blocks |
| `MD4S_FLAG_GFM_AUTOLINKS` | `0x0040` | On | Bare `http://`/`https://` URL auto-linking |

```c
#define MD4S_FLAG_DEFAULT  (MD4S_FLAG_TABLES | MD4S_FLAG_STRIKETHROUGH | \
                            MD4S_FLAG_TASKLISTS | MD4S_FLAG_GFM_AUTOLINKS)
```

### Examples

```c
/* Default: tables, strikethrough, task lists, autolinks enabled. */
struct md4s_parser *p = md4s_create(cb, data);

/* Strict CommonMark (no extensions): */
struct md4s_parser *p = md4s_create_ex(cb, data, 0);

/* Tables only, no other extensions: */
struct md4s_parser *p = md4s_create_ex(cb, data, MD4S_FLAG_TABLES);

/* Everything on, but disable raw HTML for security: */
struct md4s_parser *p = md4s_create_ex(cb, data,
    MD4S_FLAG_DEFAULT | MD4S_FLAG_NOHTMLBLOCKS | MD4S_FLAG_NOHTMLSPANS);
```

---

## Event Sequences

### Heading

```
HEADING_ENTER {heading_level=2}
  TEXT {text="Hello"}
HEADING_LEAVE
NEWLINE
```

### Paragraph

```
PARAGRAPH_ENTER
  TEXT {text="First line"}
  SOFTBREAK
  TEXT {text="second line"}
PARAGRAPH_LEAVE
NEWLINE
```

### Fenced Code Block

```
CODE_BLOCK_ENTER {language="python"}
  CODE_TEXT {text="def foo():"}
  NEWLINE
  CODE_TEXT {text="    return 42"}
  NEWLINE
CODE_BLOCK_LEAVE
NEWLINE
```

### List

```
LIST_ENTER {ordered=false, is_tight=true}
  LIST_ITEM_ENTER {item_number=0, list_depth=0}
    TEXT {text="First item"}
  LIST_ITEM_LEAVE
  NEWLINE
  LIST_ITEM_ENTER {item_number=0, list_depth=0}
    TEXT {text="Second item"}
  LIST_ITEM_LEAVE
  NEWLINE
LIST_LEAVE
```

### Table

```
TABLE_ENTER {column_count=2}
  TABLE_HEAD_ENTER
    TABLE_ROW_ENTER
      TABLE_CELL_ENTER {cell_alignment=1}  /* left */
        TEXT {text="Name"}
      TABLE_CELL_LEAVE
      TABLE_CELL_ENTER {cell_alignment=3}  /* right */
        TEXT {text="Value"}
      TABLE_CELL_LEAVE
    TABLE_ROW_LEAVE
  TABLE_HEAD_LEAVE
  TABLE_BODY_ENTER
    TABLE_ROW_ENTER
      TABLE_CELL_ENTER {cell_alignment=1}
        TEXT {text="foo"}
      TABLE_CELL_LEAVE
      TABLE_CELL_ENTER {cell_alignment=3}
        TEXT {text="42"}
      TABLE_CELL_LEAVE
    TABLE_ROW_LEAVE
  TABLE_BODY_LEAVE
TABLE_LEAVE
```

### Link

```
LINK_ENTER {url="https://example.com", title="Example"}
  TEXT {text="click here"}
LINK_LEAVE
```

### Image

```
IMAGE_ENTER {url="photo.jpg", title="A photo"}
  TEXT {text="alt text"}
IMAGE_LEAVE
```

### Streaming (Partial Lines)

```
PARTIAL_LINE {text="# Hel"}      /* bytes arrive without newline */
PARTIAL_CLEAR                      /* newline arrives, clear partial */
HEADING_ENTER {heading_level=1}   /* full line events */
  TEXT {text="Hello"}
HEADING_LEAVE
NEWLINE
```

---

## Memory Management

- `md4s_create` / `md4s_create_ex` allocate the parser and internal buffers.
- `md4s_feed` may grow internal buffers via `realloc`. All allocations are checked; the parser stops processing if allocation fails.
- `md4s_finalize` and `md4s_cancel` return heap-allocated strings that the caller must `free()`.
- `md4s_destroy` frees all internal state. Passing NULL is safe.
- All strings in `md4s_detail` are valid only for the callback duration. Copy if needed.
- NUL bytes (0x00) in input are replaced with U+FFFD (replacement character).
- Internal buffers are capped at 256 MB. Reference link definitions are capped at 256.
- Inline parsing recursion is bounded at depth 32.

## Thread Safety

Each parser instance is independent with no shared mutable state. Multiple parsers may be used concurrently from different threads. A single parser must not be accessed from multiple threads simultaneously.
