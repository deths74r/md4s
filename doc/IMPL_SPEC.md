# md4s Implementation Specification

Five features specified against the current codebase (`md4s.c` at 2307 lines,
`test_md4s.c` at 264 tests). Each section gives exact code locations, algorithms,
test cases, and edge cases.

---

## 1. Shortcut Reference Links

### Current State

md4s handles two forms of reference links in `parse_inline_depth()` at lines
1474-1519:

- **Full reference**: `[text][ref]` -- looks up `ref` via `find_link_def()`
- **Collapsed reference**: `[text][]` -- uses `text` as the label

It does NOT handle the **shortcut form**: `[text]` alone (where the bracket
content is the label). Per CommonMark spec section 6.6, when `]` is not
followed by `(` or `[`, the parser should attempt to resolve the bracket
content as a reference label.

### Code Location

The change goes in `parse_inline_depth()` (line 1085), inside the
`text[pos] == '['` branch. Currently at line 1520, after the reference link
block falls through (both `[text](url)` and `[text][ref]` failed to match),
the code reaches line 1522:

```c
/* If image prefix didn't match, skip the '!' */
if (is_image) {
    pos++;
    continue;
}
```

The shortcut detection must be inserted between line 1519 (end of the
`[text][ref]` block) and line 1522 (the image fallthrough).

### Algorithm

```
After the closing ']' is found at position `bracket`, and neither
'(' nor '[' follows at bracket+1:

1. Extract the bracket content:
   label     = text + bracket_start + 1
   label_len = bracket - bracket_start - 1

2. Call find_link_def(p, label, label_len)

3. If url != NULL:
   a. Flush any accumulated plain text before pos
   b. Emit LINK_ENTER (or IMAGE_ENTER if is_image) with the resolved URL
   c. Recursively parse_inline_depth() over the bracket content
   d. Emit LINK_LEAVE (or IMAGE_LEAVE)
   e. Set pos = bracket + 1, plain_start = pos
   f. continue

4. If url == NULL:
   Fall through to existing behavior (emit '[' as literal text)
```

### Insertion Point (Pseudocode)

After line 1519 (`}` closing the `[text][ref]` block), before line 1522:

```c
/* Shortcut reference link: [text] where text is a defined label */
if (bracket < length && text[bracket] == ']') {
    const char *label = text + bracket_start + 1;
    size_t label_len = bracket - bracket_start - 1;
    const char *url = find_link_def(p, label, label_len);
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
        pos = bracket + 1;
        plain_start = pos;
        continue;
    }
}
```

### False Positive Avoidance

The only way `[word]` becomes a link is if `word` was previously defined via
`[word]: url`. Since link definitions are explicitly authored by the document
writer, this is intentional -- not a false positive. The existing
`find_link_def()` already does case-insensitive comparison, so `[Word]` would
match `[word]: url`.

There is no ambiguity with emphasis delimiters, code spans, or other inline
constructs because the bracket-matching code runs after those have been
checked. The bracket matcher already handles nested brackets via the depth
counter (line 1372-1385).

### Interaction with find_link_def()

No changes needed to `find_link_def()` (lines 364-386). It already accepts
arbitrary `(label, label_len)` pairs and does case-insensitive comparison.
The shortcut form simply passes the bracket content directly as the label.

### MC/DC Test Cases

Each test demonstrates the independent effect of one condition in the
three-condition decision: `(bracket found) && (no '(' or '[' after ']') && (label is defined)`.

| # | Test Name | Conditions | Expected |
|---|-----------|-----------|----------|
| S1a | `md4s_shortcut_ref_basic` | `[text]` where `text` is defined | LINK_ENTER with resolved URL |
| S1b | `md4s_shortcut_ref_undefined` | `[text]` where `text` is NOT defined | No LINK_ENTER, literal `[text]` |
| S1c | `md4s_shortcut_ref_case_insensitive` | `[TEXT]` matching `[text]: url` | LINK_ENTER (case-insensitive) |
| S1d | `md4s_shortcut_ref_image` | `![alt]` where `alt` is defined | IMAGE_ENTER with resolved URL |
| S1e | `md4s_shortcut_ref_in_sentence` | `Use [foo] for details.` | LINK_ENTER around "foo" |
| S1f | `md4s_shortcut_ref_priority` | `[text](url)` takes priority over shortcut | Inline link, not shortcut |
| S1g | `md4s_shortcut_ref_full_priority` | `[text][ref]` takes priority over shortcut | Full ref link, not shortcut |
| S1h | `md4s_shortcut_ref_in_emphasis` | `*[text]*` where text is defined | ITALIC_ENTER, LINK_ENTER, TEXT, LINK_LEAVE, ITALIC_LEAVE |
| S1i | `md4s_shortcut_ref_with_spaces` | `[multi word]` matching `[multi word]: url` | LINK_ENTER |
| S1j | `md4s_shortcut_ref_inline_content` | `[**bold**]` where `**bold**` is defined (unlikely) | Should match literally; inline parsing happens on the text inside |

### Edge Cases

- `[text]` followed by other text: `[foo] bar` -- should link "foo" if defined
- `[text]` inside emphasis: `*[foo]*` -- emphasis wraps the link
- `[text]` not defined, followed by `[`: `[a][b]` -- handled by existing full ref code, not shortcut
- `[text]` at end of line: should work (bracket == length-1)
- `[]` -- empty brackets, label_len == 0, `find_link_def` returns NULL (no match)
- `[text]` where text contains `\]`: the existing bracket-depth scanner handles escaped brackets
- Multiple `[text]` on one line: each resolved independently

---

## 2. Code Span Space Stripping

### Current State

Code spans are handled in `parse_inline_depth()` at lines 1160-1186. When a
matching closing backtick sequence is found, the code emits:

```c
emit_simple(p, MD4S_CODE_SPAN_ENTER);
emit_text(p, MD4S_TEXT, text + pos, close - pos);
emit_simple(p, MD4S_CODE_SPAN_LEAVE);
```

The content between the backtick delimiters (`text + pos` to `text + close`)
is emitted verbatim. CommonMark rules 2-3 (spec section 6.1) require
stripping one leading and one trailing space when:

1. The content begins with a space AND ends with a space
2. The content is NOT entirely spaces

### Code Location

The change goes at lines 1175-1177, between finding the close and emitting
the TEXT event. Replace:

```c
emit_text(p, MD4S_TEXT, text + pos, close - pos);
```

With the space-stripping logic.

### Algorithm

```c
const char *code_content = text + pos;
size_t code_len = close - pos;

/* CommonMark rules 2-3: strip one leading and one trailing space
 * if the content begins AND ends with a space, and the content
 * is not entirely spaces. */
if (code_len >= 2 &&
    code_content[0] == ' ' &&
    code_content[code_len - 1] == ' ') {
    /* Check if content is all spaces. */
    bool all_spaces = true;
    for (size_t k = 0; k < code_len; k++) {
        if (code_content[k] != ' ') {
            all_spaces = false;
            break;
        }
    }
    if (!all_spaces) {
        code_content++;
        code_len -= 2;
    }
}
emit_text(p, MD4S_TEXT, code_content, code_len);
```

The three-condition decision is:
`(code_len >= 2) && (starts with space && ends with space) && (!all_spaces)`

### MC/DC Test Cases

| # | Test Name | Input | Expected TEXT Content | Conditions |
|---|-----------|-------|----------------------|------------|
| C2a | `md4s_code_span_space_strip` | `` ` foo ` `` | `foo` | len>=2=T, spaces=T, all_spaces=F -> strip |
| C2b | `md4s_code_span_double_space_strip` | `` `  foo  ` `` | ` foo ` | len>=2=T, spaces=T, all_spaces=F -> strip one each side |
| C2c | `md4s_code_span_all_spaces` | `` ` ` `` | ` ` | len>=2=F (len==1), NOT stripped |
| C2d | `md4s_code_span_two_spaces` | `` `  ` `` | `  ` | len>=2=T, spaces=T, all_spaces=T -> NOT stripped |
| C2e | `md4s_code_span_three_spaces` | `` `   ` `` | `   ` | len>=2=T, spaces=T, all_spaces=T -> NOT stripped |
| C2f | `md4s_code_span_no_leading_space` | `` `foo ` `` | `foo ` | len>=2=T, spaces=F (no leading) -> NOT stripped |
| C2g | `md4s_code_span_no_trailing_space` | `` ` foo` `` | ` foo` | len>=2=T, spaces=F (no trailing) -> NOT stripped |
| C2h | `md4s_code_span_no_spaces` | `` `foo` `` | `foo` | len>=2=T, spaces=F -> NOT stripped |
| C2i | `md4s_code_span_empty` | ` `` `` ` | (empty string) | len>=2=F (len==0) -> NOT stripped |
| C2j | `md4s_code_span_single_space_only` | `` ` ` `` | ` ` | len>=2=F (len==1, single space) -> NOT stripped |
| C2k | `md4s_code_span_newline_as_space` | backticks with newline | newlines converted to spaces first (if applicable) | Interaction test |

### Edge Cases

- Empty code span (`` `` ``): `code_len == 0`, no stripping
- Single space (`` ` ` ``): `code_len == 1`, `code_len >= 2` is false, no stripping
- Two spaces (`` `  ` ``): `code_len == 2`, starts/ends with space, BUT all_spaces is true, no stripping
- Content is `" a "`: stripped to `"a"`
- Content is `"  a  "`: stripped to `" a "` (one space removed from each side)
- Content is `" \t "`: not all spaces (has tab), stripped to `"\t"` -- note: this follows CommonMark which only checks for U+0020 spaces, not tabs
- Unicode spaces: CommonMark specifies U+0020 only, not Unicode whitespace

### Note on Newline-to-Space Conversion

CommonMark also specifies that line endings within code spans are converted to
spaces. md4s is a line-based parser, so multi-line code spans are not possible
within a single line. This is a pre-existing limitation of the streaming
architecture and is separate from the space-stripping feature.

---

## 3. CommonMark Spec Test Suite

### Architecture Overview

Three components:

1. **HTML renderer** (`spec_render.c`, ~200 lines): Converts md4s events to HTML
2. **Spec data** (`spec.json`, embedded or downloaded): The 652 CommonMark examples
3. **Test driver** (`test_spec.c`, ~150 lines): Orchestrates parse-render-compare

### 3.1 HTML Renderer Design

The renderer collects md4s events and builds an HTML string. It operates as
a callback that accumulates output into a growable buffer.

```c
struct html_renderer {
    char *buf;
    size_t len;
    size_t cap;
};

static void html_callback(enum md4s_event event,
                           const struct md4s_detail *detail,
                           void *user_data);
```

#### Event-to-HTML Mapping

| md4s Event | HTML Output |
|------------|-------------|
| `HEADING_ENTER(level=N)` | `<hN>` |
| `HEADING_LEAVE` | `</hN>\n` (track level in renderer state) |
| `PARAGRAPH_ENTER` | `<p>` |
| `PARAGRAPH_LEAVE` | `</p>\n` |
| `CODE_BLOCK_ENTER` | `<pre><code>` or `<pre><code class="language-X">` if language set |
| `CODE_BLOCK_LEAVE` | `</code></pre>\n` |
| `BLOCKQUOTE_ENTER` | `<blockquote>\n` |
| `BLOCKQUOTE_LEAVE` | `</blockquote>\n` |
| `LIST_ENTER(ordered=false)` | `<ul>\n` |
| `LIST_ENTER(ordered=true)` | `<ol>\n` or `<ol start="N">\n` |
| `LIST_LEAVE` | `</ul>\n` or `</ol>\n` (track type) |
| `LIST_ITEM_ENTER` | `<li>` |
| `LIST_ITEM_LEAVE` | `</li>\n` |
| `THEMATIC_BREAK` | `<hr />\n` |
| `TABLE_ENTER` | `<table>\n` |
| `TABLE_LEAVE` | `</table>\n` |
| `TABLE_HEAD_ENTER` | `<thead>\n` |
| `TABLE_HEAD_LEAVE` | `</thead>\n` |
| `TABLE_BODY_ENTER` | `<tbody>\n` |
| `TABLE_BODY_LEAVE` | `</tbody>\n` |
| `TABLE_ROW_ENTER` | `<tr>\n` |
| `TABLE_ROW_LEAVE` | `</tr>\n` |
| `TABLE_CELL_ENTER` | `<th>` or `<td>` (based on head/body state), with optional `style="text-align: ..."` |
| `TABLE_CELL_LEAVE` | `</th>\n` or `</td>\n` |
| `HTML_BLOCK_ENTER` | (nothing -- raw HTML follows) |
| `HTML_BLOCK_LEAVE` | (nothing) |
| `BOLD_ENTER` | `<strong>` |
| `BOLD_LEAVE` | `</strong>` |
| `ITALIC_ENTER` | `<em>` |
| `ITALIC_LEAVE` | `</em>` |
| `CODE_SPAN_ENTER` | `<code>` |
| `CODE_SPAN_LEAVE` | `</code>` |
| `STRIKETHROUGH_ENTER` | `<del>` |
| `STRIKETHROUGH_LEAVE` | `</del>` |
| `LINK_ENTER` | `<a href="URL">` with optional `title="..."` |
| `LINK_LEAVE` | `</a>` |
| `IMAGE_ENTER` | `<img src="URL" alt="` (then collect text until leave) |
| `IMAGE_LEAVE` | `" />` (close the alt attribute) |
| `TEXT` | HTML-escaped text (& -> `&amp;`, < -> `&lt;`, > -> `&gt;`, " -> `&quot;`) |
| `CODE_TEXT` | HTML-escaped text (same escaping) |
| `SOFTBREAK` | `\n` |
| `HARDBREAK` | `<br />\n` |
| `BLOCK_SEPARATOR` | (nothing -- structural, controls inter-block spacing) |
| `NEWLINE` | (nothing -- structural, md4s uses this for line tracking) |
| `PARTIAL_LINE` | (ignored in spec testing) |
| `PARTIAL_CLEAR` | (ignored in spec testing) |

#### Image Handling

Images require special handling because the `alt` text must be collected
from TEXT events between IMAGE_ENTER and IMAGE_LEAVE. The renderer needs
a flag `in_image` and a secondary buffer for alt text:

```c
bool in_image;
char alt_buf[1024];
size_t alt_len;
```

When `in_image` is true, TEXT events append to `alt_buf` instead of the
main output. On IMAGE_LEAVE, emit `<img src="URL" alt="collected_text" />`.

#### Entity Handling

md4s currently decodes a small set of entities (&amp;, &lt;, &gt;, &quot;,
&apos;, &nbsp;) in `parse_inline_depth()` at lines 1118-1157 and emits the
decoded character as TEXT. For the HTML renderer, decoded characters that
are significant in HTML need re-escaping. The renderer's TEXT handler
already HTML-escapes output, so `&` (decoded from `&amp;`) gets re-escaped
to `&amp;` in the output, which is correct.

If an ENTITY event is added in the future, the renderer would need to
emit the decoded Unicode character, properly HTML-escaped.

#### Renderer State

The renderer needs to track:
- Current heading level (for closing tag)
- Whether currently in list, and list type (for closing tag)
- Whether currently in image (for alt-text collection)
- Whether currently in table head vs. body (for th vs. td)

```c
struct html_renderer {
    char *buf;
    size_t len;
    size_t cap;
    int heading_level;
    bool in_ordered_list;
    bool in_image;
    char alt_buf[1024];
    size_t alt_len;
    bool in_table_head;
};
```

### 3.2 Spec Data

Use `spec.json` from the CommonMark project. It is a JSON array of objects:

```json
[
  {
    "markdown": "# Heading\n",
    "html": "<h1>Heading</h1>\n",
    "example": 62,
    "section": "ATX headings",
    "start_line": 812,
    "end_line": 816
  },
  ...
]
```

**Embedding strategy**: Download `spec.json` (v0.31.2, ~320KB) and embed it
as a C string literal via `xxd -i spec.json > spec_data.h`, giving:

```c
unsigned char spec_json[] = { 0x5b, 0x0a, ... };
unsigned int spec_json_len = 327456;
```

Alternatively, ship `spec.json` alongside the test binary and read it at
runtime. The embedded approach is simpler for CI.

**JSON parsing**: Write a minimal spec-example extractor (~100 lines) that
finds `"markdown":`, `"html":`, and `"example":` fields. No need for a
full JSON parser -- the spec.json format is stable and simple.

### 3.3 Test Driver Design

```c
struct spec_example {
    int number;
    char *markdown;
    char *html_expected;
    char *section;
};

int main(void)
{
    /* Parse spec.json into array of spec_examples. */
    struct spec_example *examples;
    int count = parse_spec_json(spec_json, spec_json_len, &examples);

    int passed = 0, failed = 0, skipped = 0;

    for (int i = 0; i < count; i++) {
        /* Check if this example is in the skip list. */
        if (should_skip(examples[i].number)) {
            skipped++;
            continue;
        }

        /* Feed markdown to md4s. */
        struct html_renderer r = {0};
        html_renderer_init(&r);
        struct md4s_parser *p = md4s_create(html_callback, &r);
        md4s_feed(p, examples[i].markdown,
                  strlen(examples[i].markdown));
        char *raw = md4s_finalize(p);
        free(raw);
        md4s_destroy(p);

        /* Compare rendered HTML to expected. */
        if (strcmp(r.buf, examples[i].html_expected) == 0) {
            passed++;
        } else {
            failed++;
            printf("FAIL example %d (%s):\n"
                   "  expected: %s"
                   "  got:      %s\n",
                   examples[i].number,
                   examples[i].section,
                   examples[i].html_expected,
                   r.buf);
        }

        html_renderer_free(&r);
    }

    printf("\n%d passed, %d failed, %d skipped, %d total\n",
           passed, failed, skipped, count);
    return failed > 0 ? 1 : 0;
}
```

### 3.4 Expected Failure Categories

md4s is a streaming line-based parser with intentional design differences
from a batch parser. Expected failures fall into these categories:

#### Category A: Multi-line inline constructs (est. 30-50 examples)
md4s processes one line at a time. Inline constructs that span multiple
lines (emphasis spanning lines, multi-line link text) will differ.

Examples affected: Emphasis across line breaks, multi-line reference
link labels, multi-line inline code spans.

#### Category B: Container nesting (est. 40-60 examples)
md4s has simplified list and blockquote nesting. Deep list nesting,
lists inside blockquotes, blockquotes inside lists, and lazy continuation
edge cases will differ.

Examples affected: List item continuation paragraphs, nested blockquote
levels, tight vs. loose lists.

#### Category C: Delimiter stack edge cases (est. 20-30 examples)
CommonMark specifies a complex delimiter stack algorithm for emphasis.
md4s uses a simpler greedy approach that handles common cases but misses
some pathological nesting patterns.

Examples affected: Interleaved emphasis, `*foo**bar**baz*` decomposition,
multiple-of-3 rule.

#### Category D: Features not yet implemented (est. 10-20 examples)
- Shortcut reference links (Section 1 of this spec)
- Code span space stripping (Section 2 of this spec)
- Link destination with angle brackets in inline links
- Hard line breaks (trailing spaces)
- Backslash line breaks

#### Category E: HTML and raw content (est. 20-30 examples)
md4s has limited HTML block detection. Inline HTML, HTML block types 1-7
distinction, and CDATA sections will differ.

#### Category F: Paragraph and block interaction (est. 10-20 examples)
Edge cases in how block-level constructs interrupt paragraphs (e.g.,
can a heading interrupt a paragraph without a blank line, ordered list
starting at non-1).

### 3.5 Skip List Implementation

```c
/* Examples that are EXPECTED to fail due to intentional design
 * differences in md4s's streaming architecture. */
static const int skip_list[] = {
    /* Populated after initial test run. Run with no skips first
     * to establish the baseline, then categorize failures. */
};
static const int skip_count = sizeof(skip_list) / sizeof(skip_list[0]);

static bool should_skip(int example_number)
{
    for (int i = 0; i < skip_count; i++)
        if (skip_list[i] == example_number)
            return true;
    return false;
}
```

### 3.6 Estimated Pass Rate

Based on the category estimates:

| Category | Est. Failures | % of 652 |
|----------|---------------|----------|
| A: Multi-line inline | 40 | 6% |
| B: Container nesting | 50 | 8% |
| C: Delimiter stack | 25 | 4% |
| D: Missing features | 15 | 2% |
| E: HTML/raw | 25 | 4% |
| F: Block interaction | 15 | 2% |
| **Total expected failures** | **~170** | **~26%** |
| **Expected pass rate** | **~482/652** | **~74%** |

After implementing Sections 1 and 2 of this spec, Category D drops by ~10,
bringing the pass rate to ~75-76%.

### 3.7 Makefile Integration

```makefile
tests/spec_data.h: tests/spec.json
	xxd -i $< > $@

tests/run_spec: tests/test_spec.c tests/spec_render.c tests/spec_data.h md4s.c md4s.h
	$(CC) $(CFLAGS) -I. -g3 -O0 -o $@ tests/test_spec.c tests/spec_render.c md4s.c

spec: tests/run_spec
	@./tests/run_spec
```

### 3.8 File Organization

```
tests/
  test_spec.c       -- Test driver (main, JSON parser, comparison)
  spec_render.c     -- HTML renderer callback
  spec_render.h     -- Renderer API
  spec.json         -- CommonMark spec examples (downloaded)
  spec_data.h       -- Auto-generated from spec.json via xxd
```

---

## 4. Fuzz Testing Harness

### 4.1 Harness

```c
/* fuzz_md4s.c -- libFuzzer harness for md4s */
#include "md4s.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

static void noop_callback(enum md4s_event event,
                           const struct md4s_detail *detail,
                           void *user_data)
{
    (void)event;
    (void)detail;
    (void)user_data;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Limit input size to prevent OOM on huge inputs. */
    if (size > 1024 * 1024)
        return 0;

    struct md4s_parser *p = md4s_create(noop_callback, NULL);
    if (p == NULL)
        return 0;

    /* Feed in one shot. */
    md4s_feed(p, (const char *)data, size);

    /* Finalize. */
    char *raw = md4s_finalize(p);
    free(raw);

    md4s_destroy(p);
    return 0;
}
```

This is 34 lines. Optionally, add a variant that feeds data in random-sized
chunks to test the streaming path more thoroughly:

```c
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 1024 * 1024)
        return 0;

    /* Use first byte as chunk-size seed. */
    size_t chunk = (data[0] % 64) + 1;
    const char *input = (const char *)data + 1;
    size_t input_len = size - 1;

    struct md4s_parser *p = md4s_create(noop_callback, NULL);
    if (p == NULL)
        return 0;

    /* Feed in chunks. */
    size_t offset = 0;
    while (offset < input_len) {
        size_t n = chunk;
        if (offset + n > input_len)
            n = input_len - offset;
        md4s_feed(p, input + offset, n);
        offset += n;
    }

    char *raw = md4s_finalize(p);
    free(raw);
    md4s_destroy(p);
    return 0;
}
```

### 4.2 Compilation

```bash
# Basic fuzzing with AddressSanitizer
clang -std=c11 -g -O1 \
    -fsanitize=fuzzer,address,undefined \
    -I. \
    md4s.c fuzz_md4s.c \
    -o fuzz_md4s

# Run with corpus directory
mkdir -p corpus
./fuzz_md4s corpus/ -max_len=65536 -timeout=10
```

For the chunked variant:
```bash
clang -std=c11 -g -O1 \
    -fsanitize=fuzzer,address,undefined \
    -I. \
    md4s.c fuzz_md4s_chunked.c \
    -o fuzz_md4s_chunked
```

### 4.3 Corpus Seeding Strategy

Seed the corpus with existing known-good inputs to accelerate coverage:

1. **Extract test inputs**: Scrape every string literal passed to
   `feed_and_finalize()` in `test_md4s.c`. There are 264 tests, each
   with at least one input string. Write a script or do it manually:
   ```bash
   grep -oP "feed_and_finalize\(&ctx,\s*\"(.+?)\"" tests/test_md4s.c | \
       sed 's/feed_and_finalize(&ctx, "//;s/"$//' | \
       while IFS= read -r line; do
           echo -ne "$line" > "corpus/test_$(md5sum <<< "$line" | cut -c1-8).md"
       done
   ```

2. **CommonMark spec examples**: Extract markdown from `spec.json`:
   ```python
   import json
   with open('tests/spec.json') as f:
       examples = json.load(f)
   for ex in examples:
       with open(f'corpus/spec_{ex["example"]:04d}.md', 'w') as f:
           f.write(ex['markdown'])
   ```

3. **Synthetic edge cases**: Add handcrafted files targeting:
   - Deeply nested brackets: `[[[[[[[[` (100+ levels)
   - Repetitive delimiters: `***...***` (1000+ chars)
   - Maximum-length lines
   - Binary-like content (random bytes)
   - NUL bytes in various positions
   - Lines with only whitespace variations

4. **Existing markdown files**: Add any `.md` files from the project.

### 4.4 What This Catches

| Bug Class | Detection Method |
|-----------|-----------------|
| Heap buffer overflows | AddressSanitizer (ASan) |
| Stack buffer overflows | ASan |
| Use-after-free | ASan |
| Double-free | ASan |
| Memory leaks | LeakSanitizer (part of ASan) |
| Signed integer overflow | UndefinedBehaviorSanitizer (UBSan) |
| Null pointer dereference | ASan + UBSan |
| Infinite loops | libFuzzer `-timeout` flag |
| Excessive memory allocation | libFuzzer `-rss_limit_mb` flag (default 2GB) |
| Out-of-bounds reads | ASan |
| Uninitialized memory | MemorySanitizer (separate build with `-fsanitize=memory`) |

### 4.5 Known Risk Areas in md4s

Based on code review, the fuzzer is most likely to find issues in:

1. **Buffer growth** (`buf_ensure`, `buf_append`): overflow checks exist
   (line 188) but edge cases around `MAX_BUFFER_SIZE` (256MB) may exist
2. **Bracket matching** (lines 1371-1385): depth tracking with unclosed
   brackets and backslash escapes
3. **Table parsing** (lines 525-653): complex cell counting with backtick
   tracking and escaped pipes
4. **Backtick matching** (lines 973-991): matching different tick counts
5. **Link definition parsing** (lines 292-358): various malformed inputs

### 4.6 CI Integration (Optional)

```yaml
# .github/workflows/fuzz.yml
name: Fuzz
on:
  schedule:
    - cron: '0 4 * * 1'  # Weekly Monday 4am
  workflow_dispatch:

jobs:
  fuzz:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build fuzzer
        run: |
          clang -std=c11 -g -O1 \
              -fsanitize=fuzzer,address,undefined \
              -I. md4s.c fuzz_md4s.c -o fuzz_md4s
      - name: Seed corpus
        run: |
          mkdir -p corpus
          # Extract test inputs, spec examples
      - name: Run fuzzer (10 minutes)
        run: ./fuzz_md4s corpus/ -max_total_time=600 -max_len=65536
      - name: Upload crash artifacts
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: fuzz-crashes
          path: crash-*
```

---

## 5. GFM Extended Autolinks

### Current State

md4s handles only angle-bracket autolinks (`<url>` and `<email>`) in
`parse_inline_depth()` at lines 1322-1362. It checks for `://` (URL) or
`@` (email) inside the angle brackets.

GFM (GitHub Flavored Markdown) extends autolinks to recognize bare URLs,
emails, and www links without angle brackets.

### 5.1 Feature Flag

Extended autolinks should be behind a flag because they change the meaning
of existing text. A bare `http://example.com` in a document that previously
rendered as plain text would suddenly become a link.

**Approach**: Add a `flags` field to the parser struct and a new API function:

```c
/* In md4s.h */
#define MD4S_FLAG_GFM_AUTOLINKS  (1 << 0)

/* New API */
void md4s_set_flags(struct md4s_parser *parser, unsigned int flags);
```

Or, simpler: add `unsigned int flags` as a parameter to `md4s_create()`.
However, since the existing API has no flags parameter and changing the
signature is a breaking change, the setter approach is better:

```c
/* In struct md4s_parser (after line 133) */
unsigned int flags;
```

Check with `(p->flags & MD4S_FLAG_GFM_AUTOLINKS)` before running extended
autolink detection.

### 5.2 Code Location

All three autolink types go in `parse_inline_depth()`, in the main `while`
loop. They should be checked AFTER existing inline constructs (emphasis,
code spans, angle-bracket autolinks, links/images) but BEFORE the final
`pos++` at line 1529.

Insert before line 1529:

```c
/* GFM extended autolinks. */
if (p->flags & MD4S_FLAG_GFM_AUTOLINKS) {
    /* 5a: URL autolinks */
    /* 5b: Email autolinks */
    /* 5c: WWW autolinks */
}
```

Alternatively, each type can be a separate `if` block within the flag check.

### 5a. URL Autolinks

#### Detection

Scan for `http://` or `https://` at the current position. The scheme must
appear at a word boundary (preceded by start-of-text, whitespace, or
certain punctuation -- specifically `(`, `*`, `_`, `~`, `"`).

```
if (pos + 7 < length &&
    (memcmp(text + pos, "https://", 8) == 0 ||
     memcmp(text + pos, "http://", 7) == 0))
{
    /* Verify word boundary before the scheme. */
    if (pos == 0 || text[pos - 1] == ' ' ||
        text[pos - 1] == '(' || text[pos - 1] == '*' ||
        text[pos - 1] == '"' || text[pos - 1] == '\'' ||
        text[pos - 1] == '_' || text[pos - 1] == '~' ||
        text[pos - 1] == '\n' || text[pos - 1] == '\t')
    {
        size_t scheme_len = (text[pos + 4] == 's') ? 8 : 7;
        size_t url_end = scan_url_end(text, length, pos + scheme_len);
        /* url_end is the exclusive end of the URL. */
        ...
    }
}
```

#### URL End Scanning

Per the GFM spec (section 6.9), the URL extends until:
- Whitespace
- `<` character
- End of input

Then trailing characters are stripped using these rules:
1. `?`, `!`, `.`, `,`, `:`, `*`, `_`, `~` at the end are not part of URL
2. `)` at the end is only part of URL if the total `(` count >= `)` count
3. If the URL ends with `;`, check if it looks like an HTML entity
   (`&...;`) and exclude the entity portion

```c
static size_t scan_url_end(const char *text, size_t len, size_t start)
{
    size_t end = start;

    /* Scan forward until whitespace or '<'. */
    while (end < len && text[end] != ' ' && text[end] != '\t' &&
           text[end] != '\n' && text[end] != '<')
        end++;

    /* Strip trailing punctuation. */
    while (end > start) {
        char last = text[end - 1];
        if (last == '?' || last == '!' || last == '.' ||
            last == ',' || last == ':' || last == '*' ||
            last == '_' || last == '~' || last == '\'') {
            end--;
            continue;
        }
        if (last == ')') {
            /* Count parens in URL. */
            int open = 0, close = 0;
            for (size_t k = start; k < end; k++) {
                if (text[k] == '(') open++;
                if (text[k] == ')') close++;
            }
            if (close > open) {
                end--;
                continue;
            }
        }
        if (last == ';') {
            /* Check for entity-like ending: &...; */
            size_t amp = end - 1;
            while (amp > start && text[amp] != '&')
                amp--;
            if (text[amp] == '&' && (end - 1 - amp) <= 10) {
                end = amp;
                continue;
            }
        }
        break;
    }

    return end;
}
```

#### Emission

```c
if (url_end > pos) {
    if (pos > plain_start)
        emit_text(p, MD4S_TEXT, text + plain_start, pos - plain_start);
    struct md4s_detail d = {0};
    d.url = text + pos;
    d.url_length = url_end - pos;
    emit(p, MD4S_LINK_ENTER, &d);
    emit_text(p, MD4S_TEXT, text + pos, url_end - pos);
    emit_simple(p, MD4S_LINK_LEAVE);
    plain_start = url_end;
    pos = url_end;
    continue;
}
```

### 5b. Email Autolinks

#### Detection

Scan for `@` at the current position. If found, look backward for a valid
username and forward for a valid domain.

Since md4s processes text left-to-right and we need to look backward, the
detection should trigger when we encounter a character that could be the
start of a username. More practically, we can check at every `@`:

```
if (text[pos] == '@' && pos > 0) {
    /* Scan backward for username start. */
    size_t user_start = pos;
    while (user_start > 0 &&
           is_email_user_char(text[user_start - 1]))
        user_start--;
    if (user_start < pos) {
        /* Scan forward for domain. */
        size_t domain_end = pos + 1;
        ...
    }
}
```

However, this approach conflicts with the left-to-right text accumulation.
A cleaner approach: detect email-like patterns when scanning forward, by
keeping track of whether we're in a potential username:

**Better approach**: After the existing autolink checks, scan for patterns
that look like the start of an email username. The GFM spec says email
autolinks match `[a-zA-Z0-9.!#$%&'*+/=?^_{|}~-]+@[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*`.

Since the parser processes left-to-right and accumulates plain text,
the `@` check should:

1. When `text[pos] == '@'`, look backward in `text` (from `plain_start` to `pos-1`) to find the username portion
2. Look forward for the domain portion
3. If both are valid, emit the whole email as a link

```c
static bool is_email_user_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_' || c == '+';
}

static bool is_domain_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.';
}
```

The domain must:
- Contain at least one `.`
- Not start or end with `-` or `.`
- Each label (between dots) is 1-63 characters

```c
if (text[pos] == '@' && (p->flags & MD4S_FLAG_GFM_AUTOLINKS)) {
    /* Scan backward for username. */
    size_t user_start = pos;
    while (user_start > plain_start &&
           is_email_user_char(text[user_start - 1]))
        user_start--;

    if (user_start < pos) {
        /* Scan forward for domain. */
        size_t domain_end = pos + 1;
        bool has_dot = false;
        while (domain_end < length && is_domain_char(text[domain_end])) {
            if (text[domain_end] == '.') has_dot = true;
            domain_end++;
        }
        /* Strip trailing dots/hyphens. */
        while (domain_end > pos + 1 &&
               (text[domain_end - 1] == '.' || text[domain_end - 1] == '-'))
            domain_end--;

        if (has_dot && domain_end > pos + 1) {
            /* Emit the email as a mailto link. */
            size_t email_start = user_start;
            size_t email_len = domain_end - email_start;

            /* Flush plain text before the email. */
            if (email_start > plain_start)
                emit_text(p, MD4S_TEXT, text + plain_start,
                          email_start - plain_start);

            /* Build mailto: URL. */
            char mailto[512];
            snprintf(mailto, sizeof(mailto), "mailto:%.*s",
                     (int)email_len, text + email_start);

            struct md4s_detail d = {0};
            d.url = mailto;
            d.url_length = strlen(mailto);
            emit(p, MD4S_LINK_ENTER, &d);
            emit_text(p, MD4S_TEXT, text + email_start, email_len);
            emit_simple(p, MD4S_LINK_LEAVE);

            pos = domain_end;
            plain_start = pos;
            continue;
        }
    }
}
```

**Complication**: The email username starts before `pos`, and we may have
already accumulated plain text from `plain_start` that includes the
username. The backward scan handles this correctly by re-emitting the
prefix text (`plain_start` to `user_start`) and then the email as a link.

### 5c. WWW Autolinks

#### Detection

Scan for `www.` (case-insensitive) at a word boundary. The word boundary
is the same as for URL autolinks: preceded by start-of-text, whitespace,
or certain punctuation.

```c
if (text[pos] == 'w' || text[pos] == 'W') {
    if (pos + 4 <= length &&
        (text[pos+1] == 'w' || text[pos+1] == 'W') &&
        (text[pos+2] == 'w' || text[pos+2] == 'W') &&
        text[pos+3] == '.') {
        /* Verify word boundary. */
        if (pos == 0 || text[pos - 1] == ' ' ||
            text[pos - 1] == '(' || text[pos - 1] == '\t' ||
            text[pos - 1] == '\n') {
            size_t url_end = scan_url_end(text, length, pos + 4);
            if (url_end > pos + 4) {
                /* Must have at least one dot after www. */
                bool has_dot = false;
                for (size_t k = pos + 4; k < url_end; k++) {
                    if (text[k] == '.') { has_dot = true; break; }
                }
                if (has_dot) {
                    /* Emit with http:// prefix. */
                    ...
                }
            }
        }
    }
}
```

#### URL Prefix

The URL in the LINK_ENTER detail needs the `http://` prefix prepended:

```c
char prefixed[2048];
size_t www_len = url_end - pos;
snprintf(prefixed, sizeof(prefixed), "http://%.*s",
         (int)www_len, text + pos);

struct md4s_detail d = {0};
d.url = prefixed;
d.url_length = strlen(prefixed);
emit(p, MD4S_LINK_ENTER, &d);
emit_text(p, MD4S_TEXT, text + pos, www_len);
emit_simple(p, MD4S_LINK_LEAVE);
```

### 5.4 Processing Order

Within `parse_inline_depth()`, the extended autolink checks should go in
this order to avoid conflicts:

1. URL autolinks (`http://`, `https://`) -- checked first because they
   are the most specific (fixed scheme prefix)
2. WWW autolinks (`www.`) -- checked next
3. Email autolinks (`@` scan) -- checked last because the backward scan
   is the most expensive

All three go inside a single `if (p->flags & MD4S_FLAG_GFM_AUTOLINKS)`
guard to avoid the overhead when the feature is disabled.

### 5.5 Interaction with Existing Angle-Bracket Autolinks

No conflict. Angle-bracket autolinks (`<url>`) are detected by the `<`
character at line 1323. Extended autolinks are detected by different
trigger characters (`h`, `w`, `@`). The angle-bracket form takes priority
because it appears earlier in the `if` chain.

The only subtle interaction: `<http://example.com>` should still produce
an angle-bracket autolink, not an extended autolink wrapped in `< >` text.
This is already correct because the `<` handler runs first.

### 5.6 MC/DC Test Cases

#### URL Autolinks

| # | Test Name | Input | Expected |
|---|-----------|-------|----------|
| A5a-1 | `md4s_autolink_url_https` | `Visit https://example.com today` | LINK_ENTER url="https://example.com" |
| A5a-2 | `md4s_autolink_url_http` | `See http://x.com` | LINK_ENTER url="http://x.com" |
| A5a-3 | `md4s_autolink_url_trailing_punct` | `Check https://x.com.` | URL excludes trailing `.` |
| A5a-4 | `md4s_autolink_url_trailing_paren` | `(https://x.com)` | URL excludes trailing `)` (unbalanced) |
| A5a-5 | `md4s_autolink_url_balanced_parens` | `https://en.wikipedia.org/wiki/Foo_(bar)` | URL includes `(bar)` |
| A5a-6 | `md4s_autolink_url_no_boundary` | `foohttps://x.com` | No link (no word boundary) |
| A5a-7 | `md4s_autolink_url_entity_strip` | `https://x.com&amp;` | Entity stripped from URL |
| A5a-8 | `md4s_autolink_url_path` | `https://x.com/path/to?q=1` | Full URL with path and query |
| A5a-9 | `md4s_autolink_url_disabled` | `https://x.com` (no flag) | No LINK_ENTER (feature disabled) |

#### Email Autolinks

| # | Test Name | Input | Expected |
|---|-----------|-------|----------|
| A5b-1 | `md4s_autolink_email_basic` | `Contact user@example.com` | LINK_ENTER url="mailto:user@example.com" |
| A5b-2 | `md4s_autolink_email_plus` | `user+tag@example.com` | LINK_ENTER |
| A5b-3 | `md4s_autolink_email_no_domain` | `user@` | No link (no domain) |
| A5b-4 | `md4s_autolink_email_no_user` | `@example.com` | No link (no username) |
| A5b-5 | `md4s_autolink_email_no_dot` | `user@localhost` | No link (no dot in domain) |
| A5b-6 | `md4s_autolink_email_subdomain` | `a@sub.example.com` | LINK_ENTER |
| A5b-7 | `md4s_autolink_email_trailing_punct` | `user@x.com.` | Trailing `.` excluded from email |

#### WWW Autolinks

| # | Test Name | Input | Expected |
|---|-----------|-------|----------|
| A5c-1 | `md4s_autolink_www_basic` | `Visit www.example.com` | LINK_ENTER url="http://www.example.com" |
| A5c-2 | `md4s_autolink_www_path` | `www.example.com/path` | LINK_ENTER url="http://www.example.com/path" |
| A5c-3 | `md4s_autolink_www_no_domain` | `www.` | No link (no domain after www.) |
| A5c-4 | `md4s_autolink_www_no_boundary` | `foowww.example.com` | No link (no word boundary) |
| A5c-5 | `md4s_autolink_www_uppercase` | `WWW.EXAMPLE.COM` | LINK_ENTER (case-insensitive detection) |
| A5c-6 | `md4s_autolink_www_paren_context` | `(www.example.com)` | LINK_ENTER, URL excludes trailing `)` |

### 5.7 Edge Cases

- URL immediately at start of line: word boundary satisfied (pos == 0)
- URL at end of line (no trailing content): `url_end == length`
- URL followed by `>`: stops at `>`
- Multiple URLs on one line: each detected independently
- URL inside emphasis: `*https://x.com*` -- emphasis takes priority (checked first)
- URL inside code span: not detected (code spans handled before autolinks)
- URL in link text: `[https://x.com](other)` -- inline link takes priority
- Email with very long username: limited by line length
- `www` without dot: not detected
- `http://` without anything after: not detected (url_end == start, no content)
- Consecutive autolinks: `http://a.com http://b.com` -- both detected

### 5.8 Memory Considerations

The `mailto:` prefix and `http://` prefix for www links require temporary
buffers. The current approach uses stack-allocated buffers (`char mailto[512]`,
`char prefixed[2048]`). This is fine because:

1. URLs in practice are under 2000 characters
2. Stack allocation is fast and automatically freed
3. The buffer is only alive for the duration of the emit call

For extremely long URLs, the `snprintf` truncates safely. If this is
a concern, use heap allocation with `malloc`/`free`, but the complexity
cost is not justified for realistic inputs.
