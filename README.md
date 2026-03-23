# md4s — Markdown for Streaming

A streaming markdown parser for C. Processes markdown incrementally — token by token — and emits semantic events through a callback interface. Completed lines are parsed once and never revisited, enabling renderers to append output without cursor repositioning or full-document re-rendering.

94% CommonMark spec compliance. Full GFM extensions. Zero dependencies.

## Why

Existing C markdown parsers are batch parsers — they need the complete document before they can produce any output. When markdown arrives incrementally (as it does from streaming LLM APIs), applications are forced to re-parse the entire accumulated text on every new token, then re-render the full output. This is wasteful and creates rendering artifacts in terminals.

md4s processes markdown as it arrives. Each completed line is parsed once, emitted as semantic events, and never revisited. Renderers built on md4s can append output incrementally without re-processing anything — they receive exactly the events they need, when they need them.

## Features

- **Forward-only** — completed lines are permanent, no re-rendering
- **Streaming-native** — feed 1 byte or 10KB, same result
- **CommonMark compliant** — 94% spec pass rate (102/108 examples)
- **Full GFM extensions** — tables, task lists, strikethrough, autolinks
- **Zero dependencies** — pure C11, single header + implementation + vendored gstr.h
- **Callback-driven** — never writes to stdout; you decide how to render
- **Configurable** — 7 feature flags via `md4s_create_ex()`
- **Security hardened** — bounded recursion, checked allocations, NUL replacement
- **Unicode-aware** — full UTF-8 emphasis flanking per CommonMark 0.31.2
- **Small** — ~3,200 lines of parser code, compiles in milliseconds

### Supported Markdown

| Block-Level | Inline |
|-------------|--------|
| ATX headings (`#`) | Bold (`**`, `__`) with delimiter stack |
| Setext headings (`===`, `---`) | Italic (`*`, `_`) with rule-of-three |
| Fenced code blocks (`` ``` ``, `~~~`) | Code spans with space stripping |
| Indented code blocks (4-space) | Strikethrough (`~~`) |
| Blockquotes (`>`) with content parsing | Links (`[text](url "title")`) |
| Ordered/unordered lists (nested, loose/tight) | Images (`![alt](url "title")`) |
| GFM tables with alignment | Autolinks (`<url>`, bare `https://`) |
| GFM task lists (`- [x]`) | Reference links (`[text][ref]`, `[text]`) |
| HTML blocks (all 7 CommonMark types) | Inline raw HTML (`<tag>`, `<!-- -->`) |
| Thematic breaks (`---`, `***`, `___`) | Entity references (`&amp;`, `&#x1F4A9;`) |
| | Hard line breaks (trailing spaces, `\`) |
| | Escaped characters |

### Configuration Flags

```c
MD4S_FLAG_TABLES          // GFM pipe tables (default: on)
MD4S_FLAG_STRIKETHROUGH   // ~~strikethrough~~ (default: on)
MD4S_FLAG_TASKLISTS       // [x] task lists (default: on)
MD4S_FLAG_GFM_AUTOLINKS   // Bare URL auto-linking (default: on)
MD4S_FLAG_NOHTMLBLOCKS    // Disable HTML block pass-through
MD4S_FLAG_NOHTMLSPANS     // Disable inline HTML
MD4S_FLAG_NOINDENTEDCODE  // Disable 4-space code blocks
```

## Quick Start

Drop `md4s.h`, `md4s.c`, and `gstr.h` into your project. No build system required.

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
/* Create a parser with default flags. */
struct md4s_parser *md4s_create(md4s_callback callback, void *user_data);

/* Create a parser with custom flags. */
struct md4s_parser *md4s_create_ex(md4s_callback callback, void *user_data,
                                    unsigned int flags);

/* Feed raw markdown bytes. Callbacks fire synchronously for completed lines. */
void md4s_feed(struct md4s_parser *parser, const char *data, size_t length);

/* Finalize: flush partial line, close open blocks, return raw markdown. */
char *md4s_finalize(struct md4s_parser *parser);

/* Cancel without finalizing. Returns raw markdown accumulated so far. */
char *md4s_cancel(struct md4s_parser *parser);

/* Free the parser. Passing NULL is safe. */
void md4s_destroy(struct md4s_parser *parser);
```

The callback receives `enum md4s_event` values with an `md4s_detail` struct carrying event-specific data (heading level, language, URL, title, text content, alignment, task state, etc.). See `md4s.h` for the complete event list and detail fields.

## Building

### As a library

```sh
make            # builds libmd4s.a
make test       # runs 361 unit tests
make spec       # runs 108 CommonMark spec examples (94% pass rate)
make fuzz       # builds libFuzzer harness (requires clang)
make clean      # removes build artifacts
```

### Drop-in

Just compile `md4s.c` alongside your project:

```sh
cc -std=c11 -c md4s.c -o md4s.o
```

## Design

The parser uses a line-based state machine with three states: **NORMAL**, **FENCED_CODE**, and **HTML_BLOCK**. Input accumulates in a line buffer byte-by-byte. Each newline triggers line classification and event emission.

Emphasis uses a full CommonMark **delimiter stack** algorithm with rule-of-three checking, delimiter run splitting, and 13 opener-bottom slots for O(n) amortized complexity.

Lists use a **container stack** (8 levels deep) for proper nesting, multi-line items, and loose/tight detection.

Events come in pairs (`_ENTER` / `_LEAVE`) for styled regions. Content arrives via `TEXT`, `CODE_TEXT`, and `ENTITY` events. Partial lines get `PARTIAL_LINE` / `PARTIAL_CLEAR` events for real-time display during streaming.

### Streaming Behavior

- **Partial lines**: As bytes arrive without a newline, `PARTIAL_LINE` events show the incomplete text. When the newline arrives, `PARTIAL_CLEAR` fires followed by the full semantic events.
- **Paragraph deferral**: Paragraph lines are deferred one line for setext heading and table detection. This adds one line of latency but enables correct classification.
- **Blockquote accumulation**: Consecutive `>` lines are accumulated and parsed as a unit for proper block-level structure within blockquotes.

## Tests

**361 unit tests** with MC/DC (Modified Condition/Decision Coverage) verify every compound decision in the parser.

**108 CommonMark spec examples** verify HTML output against the official specification. Current pass rate: **94%** (102/108).

**libFuzzer harness** for crash and undefined-behavior discovery.

```sh
make test       # unit tests
make spec       # spec compliance
make fuzz       # build fuzzer (then ./tests/fuzz_md4s)
```

## Known Limitations

Six CommonMark spec examples (6%) are intentional limitations of the streaming architecture:

| Limitation | Spec Examples | Reason |
|-----------|---------------|--------|
| Cross-line code spans | 349 | Inline parsing operates per-line; code spans cannot cross line boundaries in a streaming parser |
| Nested image alt text | 572 | `![foo ![bar](u)](u2)` requires outermost-first resolution which conflicts with left-to-right streaming |
| Loose list paragraph wrapping | 283, 304, 305 | `is_tight` is reported on LIST_ENTER; consumers can wrap items in `<p>` if needed, but the HTML renderer doesn't retroactively add wrappers |
| Complex HTML block nesting | 148 | `<pre>` inside `<table>` interaction is a rare edge case |

These do not affect real-world usage. LLMs do not produce cross-line code spans, nested images, or `<pre>` inside `<table>`. The loose list limitation is cosmetic — consumers receive the `is_tight` flag and can wrap accordingly.

## Acknowledgments

md4s was inspired by [md4c](https://github.com/mity/md4c) by Martin Mitáš — an excellent CommonMark parser for C. md4c served as the reference implementation we studied while designing md4s's streaming architecture. The name "md4s" (Markdown for Streaming) is a deliberate nod to md4c (Markdown for C). No code was derived from md4c; md4s is an independent implementation built around a fundamentally different design (line-based streaming vs. batch parsing), but md4c's thorough handling of CommonMark edge cases informed many of our decisions. Thank you, Martin.

Unicode-aware emphasis flanking uses [gstr](https://github.com/deths74r/gstr) for UTF-8 decoding, whitespace classification, and Unicode punctuation detection (General_Category P*+S* per CommonMark 0.31.2).

## License

MIT
