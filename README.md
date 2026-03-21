# md4s — Markdown for Streaming

A streaming markdown parser for C. Processes markdown incrementally — token by token — and emits semantic events through a callback interface.

**No cursor-up. No re-rendering. No batch processing.**

Completed lines are emitted once and never revisited.

## Why

Every C markdown parser (md4c, cmark, sundown) is a batch parser — it needs the complete document before producing output. This forces terminal applications into a cursor-up-and-clear loop that breaks on line wrapping, scrollback, tmux, and SSH latency.

md4s solves this by never moving the cursor backward. It is designed for AI CLI tools that stream LLM responses, but works anywhere markdown arrives incrementally.

## Features

- **Forward-only** — completed lines are permanent, no re-rendering
- **Streaming-native** — feed 1 byte or 10KB, same result
- **Zero dependencies** — pure C11, single header + implementation
- **Callback-driven** — never writes to stdout; you decide how to render
- **Small** — ~1900 lines of code, compiles in milliseconds

### Supported Markdown

| Block-Level | Inline |
|-------------|--------|
| ATX headings (`#`) | Bold (`**`, `__`) |
| Setext headings (`===`, `---`) | Italic (`*`, `_`) |
| Fenced code blocks | Code spans (`` ` ``) |
| Indented code blocks | Strikethrough (`~~`) |
| Blockquotes (`>`) | Links (`[text](url "title")`) |
| Ordered/unordered lists | Images (`![alt](url)`) |
| GFM tables | Autolinks (`<url>`) |
| GFM task lists (`- [x]`) | HTML entities |
| HTML blocks | Reference links (`[text][ref]`) |
| Thematic breaks | Escaped characters |

## Quick Start

Drop `md4s.h` and `md4s.c` into your project. No build system required.

```c
#include "md4s.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void on_event(enum md4s_event event,
                     const struct md4s_detail *detail,
                     void *user_data)
{
    switch (event) {
    case MD4S_HEADING_ENTER:
        printf("\n=== ");
        break;
    case MD4S_HEADING_LEAVE:
        printf(" ===\n");
        break;
    case MD4S_PARAGRAPH_ENTER:
        break;
    case MD4S_PARAGRAPH_LEAVE:
        printf("\n");
        break;
    case MD4S_TEXT:
        printf("%.*s", (int)detail->text_length, detail->text);
        break;
    case MD4S_CODE_TEXT:
        printf("    %.*s\n", (int)detail->text_length, detail->text);
        break;
    case MD4S_BOLD_ENTER:
        printf("\033[1m");
        break;
    case MD4S_BOLD_LEAVE:
        printf("\033[0m");
        break;
    case MD4S_SOFTBREAK:
        printf(" ");
        break;
    case MD4S_NEWLINE:
        break;
    default:
        break;
    }
}

int main(void)
{
    struct md4s_parser *p = md4s_create(on_event, NULL);

    /* Feed markdown in chunks (simulating streaming). */
    const char *chunk1 = "# Hello\n\nSome **bold";
    const char *chunk2 = "** text.\n";
    md4s_feed(p, chunk1, strlen(chunk1));
    md4s_feed(p, chunk2, strlen(chunk2));

    char *raw = md4s_finalize(p);
    free(raw);
    md4s_destroy(p);
}
```

## API

```c
/* Create a parser. Callback must not be NULL. */
struct md4s_parser *md4s_create(md4s_callback callback, void *user_data);

/* Feed raw markdown bytes. Callbacks fire synchronously for completed lines. */
void md4s_feed(struct md4s_parser *parser, const char *data, size_t length);

/* Finalize: flush partial line, close open blocks, return raw markdown. */
char *md4s_finalize(struct md4s_parser *parser);

/* Cancel without finalizing. Returns raw markdown accumulated so far. */
char *md4s_cancel(struct md4s_parser *parser);

/* Free the parser. Passing NULL is safe. */
void md4s_destroy(struct md4s_parser *parser);
```

The callback receives `enum md4s_event` values with an `md4s_detail` struct carrying event-specific data (heading level, language, URL, title, text content, etc.). See `md4s.h` for the complete event list and detail fields.

## Building

### As a library

```sh
make            # builds libmd4s.a
make test       # runs 250 tests
make clean      # removes build artifacts
```

### Drop-in

Just compile `md4s.c` alongside your project:

```sh
cc -std=c11 -c md4s.c -o md4s.o
```

Or include directly:

```c
#include "md4s.c"  /* single-file include */
```

## Design

Two parser states: **NORMAL** and **FENCED_CODE**. Input accumulates in a line buffer byte-by-byte. Each newline triggers line classification and event emission. No tree, no DOM, no block stack.

Events come in pairs (`_ENTER` / `_LEAVE`) for styled regions. Content arrives via `TEXT` and `CODE_TEXT` events. Partial lines get `PARTIAL_LINE` / `PARTIAL_CLEAR` events for real-time display during streaming.

### Streaming Behavior

- **Partial lines**: As bytes arrive without a newline, `PARTIAL_LINE` events show the incomplete text. When the newline arrives, `PARTIAL_CLEAR` fires followed by the full semantic events.
- **Table detection**: Lines containing `|` are deferred one line to check if the next line is a table separator. This adds one line of latency for potential table headers.
- **Setext headings**: Detected when `===` or `---` follows an open paragraph. Per CommonMark, setext heading takes priority over thematic break.

## Tests

250 MC/DC (Modified Condition/Decision Coverage) tests verify every compound decision in the parser. Each condition in every `if`/`while`/`for` with multiple conditions has been shown to independently affect the outcome.

```sh
make test
```

## Acknowledgments

md4s was inspired by [md4c](https://github.com/mity/md4c) by Martin Mitáš — an excellent CommonMark parser for C. md4c served as the reference implementation we studied while designing md4s's streaming architecture. The name "md4s" (Markdown for Streaming) is a deliberate nod to md4c (Markdown for C). No code was derived from md4c; md4s is an independent implementation built around a fundamentally different design (line-based streaming vs. batch parsing), but md4c's thorough handling of CommonMark edge cases informed many of our decisions. Thank you, Martin.

## License

MIT
