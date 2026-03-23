# md4s Implementation Spec: Entity References and Hard Line Breaks

This document specifies the implementation of two features for the md4s
streaming markdown parser: generalized entity/character references and
hard line breaks.

Reference files:
- `md4s.h` — public API, event enum, detail struct
- `md4s.c` — parser implementation (~2308 lines)
- `tests/test_md4s.c` — MC/DC test suite

---

## Feature 1: Entity/Character References

### Current State

Lines 1118–1158 of `md4s.c` contain the entity handling code inside
`parse_inline_depth()`. It hardcodes six entities:

```c
if (text[pos] == '&') {
    /* Find the semicolon. */
    size_t semi = pos + 1;
    while (semi < length && semi < pos + 10 && text[semi] != ';')
        semi++;
    if (semi < length && text[semi] == ';') {
        size_t ent_len = semi - pos + 1;
        const char *replacement = NULL;
        if (ent_len == 5 && memcmp(text + pos, "&amp;", 5) == 0)
            replacement = "&";
        else if (ent_len == 4 && memcmp(text + pos, "&lt;", 4) == 0)
            replacement = "<";
        // ... 4 more
        if (replacement != NULL) {
            // flush plain text, emit replacement as MD4S_TEXT
        }
    }
}
```

Problems:
1. Only 6 of 2,231 HTML named entities are recognized.
2. No numeric references (`&#8212;`, `&#x2014;`) at all.
3. The parser decodes entities itself, which is wrong — the renderer
   should decide how to decode (it may want UTF-8, it may want to
   pass through, it may want to validate).

### Design: New `MD4S_ENTITY` Event

Instead of decoding entities in the parser, emit a new `MD4S_ENTITY`
event carrying the raw entity text. The renderer decides what to do.

**Why not keep decoding in the parser?** Because:
- Hardcoding 2,231 entities in a streaming parser is bloat.
- The renderer already knows the output format (terminal, HTML, etc.)
  and can decode appropriately.
- Numeric references require Unicode→UTF-8 conversion, which belongs
  in the rendering layer.

### 1a. Changes to `md4s.h`

#### New enum value

Add `MD4S_ENTITY` to the `enum md4s_event` block, in the "Content
events" section (after line 87, before `MD4S_PARTIAL_LINE`):

```c
/* Content events. */
MD4S_TEXT,
MD4S_CODE_TEXT,
MD4S_SOFTBREAK,
MD4S_NEWLINE,
MD4S_BLOCK_SEPARATOR,
MD4S_ENTITY,         /* NEW: raw entity reference (e.g., "&amp;") */
```

#### Detail struct — no changes needed

The existing `text` and `text_length` fields in `struct md4s_detail`
carry the entity text. No new fields required. The entity event will
be emitted via `emit_text(p, MD4S_ENTITY, text + pos, ent_len)` where
the text contains the full entity reference including `&` and `;`.

### 1b. Changes to `md4s.c` — Entity Parsing Algorithm

**Location:** Replace lines 1118–1158 (the `if (text[pos] == '&')` block
inside `parse_inline_depth()`).

**Context protection:** Entities are only parsed inside `parse_inline_depth()`,
which is never called for code spans (the code span handler at lines
1160–1186 emits `MD4S_TEXT` for code content without recursing) or code
blocks (which emit `MD4S_CODE_TEXT` directly from `process_line()`). HTML
blocks emit `MD4S_TEXT` directly without calling `parse_inline()`. So the
current call-site is already correct — entities are recognized in paragraphs,
headings, list items, blockquotes, and table cells, but NOT in code spans,
code blocks, or HTML blocks.

**New algorithm:**

```c
/* Entity/character reference. */
if (text[pos] == '&') {
    size_t semi = pos + 1;
    bool valid = false;

    if (semi < length && text[semi] == '#') {
        /* Numeric reference: &#digits; or &#xHEX; */
        semi++;
        if (semi < length &&
            (text[semi] == 'x' || text[semi] == 'X')) {
            /* Hex: &#x[0-9a-fA-F]{1,6}; */
            semi++;
            size_t hex_start = semi;
            while (semi < length && semi < hex_start + 6 &&
                   ((text[semi] >= '0' && text[semi] <= '9') ||
                    (text[semi] >= 'a' && text[semi] <= 'f') ||
                    (text[semi] >= 'A' && text[semi] <= 'F')))
                semi++;
            if (semi > hex_start && semi < length &&
                text[semi] == ';')
                valid = true;
        } else {
            /* Decimal: &#[0-9]{1,7}; */
            size_t dec_start = semi;
            while (semi < length && semi < dec_start + 7 &&
                   text[semi] >= '0' && text[semi] <= '9')
                semi++;
            if (semi > dec_start && semi < length &&
                text[semi] == ';')
                valid = true;
        }
    } else {
        /* Named entity: &[A-Za-z][A-Za-z0-9]{0,47}; */
        if (semi < length &&
            ((text[semi] >= 'A' && text[semi] <= 'Z') ||
             (text[semi] >= 'a' && text[semi] <= 'z'))) {
            semi++;
            size_t name_start = semi - 1; /* first alpha */
            while (semi < length &&
                   semi < pos + 1 + 48 && /* max 48 chars */
                   ((text[semi] >= 'A' && text[semi] <= 'Z') ||
                    (text[semi] >= 'a' && text[semi] <= 'z') ||
                    (text[semi] >= '0' && text[semi] <= '9')))
                semi++;
            if (semi < length && text[semi] == ';')
                valid = true;
        }
    }

    if (valid) {
        size_t ent_len = semi - pos + 1; /* includes & and ; */
        if (pos > plain_start)
            emit_text(p, MD4S_TEXT,
                      text + plain_start,
                      pos - plain_start);
        emit_text(p, MD4S_ENTITY,
                  text + pos, ent_len);
        pos = semi + 1;
        plain_start = pos;
        continue;
    }
    /* Not a valid entity — fall through, '&' emitted as literal text. */
}
```

### 1c. Step-by-Step Algorithm

1. Encounter `&` at position `pos`.
2. Check `pos+1`:
   - If `#`, go to numeric path.
   - If `[A-Za-z]`, go to named path.
   - Otherwise, not an entity — fall through.
3. **Numeric path:**
   - Check for `x`/`X` after `#`.
   - If hex: scan up to 6 hex digits `[0-9a-fA-F]`. Must have at least 1.
   - If decimal: scan up to 7 decimal digits `[0-9]`. Must have at least 1.
   - After digits, must find `;`. If not, not an entity.
4. **Named path:**
   - First char must be `[A-Za-z]`.
   - Subsequent chars: `[A-Za-z0-9]`, up to 47 more (48 total name chars).
   - After name, must find `;`. If not, not an entity.
5. If valid:
   - Flush any accumulated plain text before the `&`.
   - Emit `MD4S_ENTITY` with the full text from `&` through `;` inclusive.
   - Advance `pos` past `;`, update `plain_start`.
6. If not valid: do nothing special, let `&` be emitted as literal text.

### 1d. Removal of Hardcoded Decodings

The entire `if (replacement != NULL)` logic is removed. The six previously
hardcoded entities (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`, `&nbsp;`)
now flow through the same `MD4S_ENTITY` event as all other entities.

**Renderer impact:** Any renderer that previously expected `MD4S_TEXT`
with decoded content (e.g., `<` for `&lt;`) must now handle `MD4S_ENTITY`
events. The entity text arrives verbatim (e.g., `&lt;`). The renderer
must decode it. This is a **breaking change** to the event stream.

### 1e. MC/DC Test Cases

Each test uses `feed_and_finalize()` and checks the recorded events.

#### Named entities

| # | Input | Expected Event | Expected Text | Condition Tested |
|---|-------|---------------|---------------|-----------------|
| 1 | `&amp;` | `MD4S_ENTITY` | `&amp;` | Valid named entity (former hardcoded) |
| 2 | `&lt;` | `MD4S_ENTITY` | `&lt;` | Valid named entity (former hardcoded) |
| 3 | `&gt;` | `MD4S_ENTITY` | `&gt;` | Valid named entity (former hardcoded) |
| 4 | `&quot;` | `MD4S_ENTITY` | `&quot;` | Valid named entity (former hardcoded) |
| 5 | `&apos;` | `MD4S_ENTITY` | `&apos;` | Valid named entity (former hardcoded) |
| 6 | `&nbsp;` | `MD4S_ENTITY` | `&nbsp;` | Valid named entity (former hardcoded) |
| 7 | `&mdash;` | `MD4S_ENTITY` | `&mdash;` | Valid named entity (not previously recognized) |
| 8 | `&copy;` | `MD4S_ENTITY` | `&copy;` | Short named entity |
| 9 | `&CounterClockwiseContourIntegral;` | `MD4S_ENTITY` | full text | Long named entity (35 chars) |
| 10 | `&1abc;` | `MD4S_TEXT` (literal) | `&1abc;` | First char is digit — not valid |
| 11 | `&;` | `MD4S_TEXT` (literal) | `&;` | Empty name — not valid |
| 12 | `&abc` | `MD4S_TEXT` (literal) | `&abc` | No closing semicolon |
| 13 | `&abc def;` | `MD4S_TEXT` (literal) | full text | Space in name — semicolon scan doesn't match because space is not `[A-Za-z0-9]`, scan stops, no `;` found at that position |
| 14 | `hello &amp; world` | TEXT("hello "), ENTITY("&amp;"), TEXT(" world") | Entity in middle of text |
| 15 | `&amp;&lt;` | ENTITY, ENTITY | Adjacent entities |
| 16 | `&amp;` in heading `# &amp;` | ENTITY inside heading | Entity in heading context |
| 17 | `&amp;` in list item `- &amp;` | ENTITY inside list item | Entity in list context |
| 18 | `` `&amp;` `` (code span) | TEXT("&amp;") — no ENTITY | Entity NOT recognized in code span |
| 19 | ` ```\n&amp;\n``` ` (code block) | CODE_TEXT("&amp;") — no ENTITY | Entity NOT recognized in code block |
| 20 | `> &amp;` (blockquote) | ENTITY | Entity in blockquote |

#### Decimal numeric references

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 21 | `&#8212;` | ENTITY("&#8212;") | Valid decimal |
| 22 | `&#0;` | ENTITY("&#0;") | Zero (valid structurally; renderer decides) |
| 23 | `&#1234567;` | ENTITY("&#1234567;") | 7 digits (maximum) |
| 24 | `&#12345678;` | TEXT (literal) | 8 digits — exceeds 7-digit limit |
| 25 | `&#;` | TEXT (literal) | No digits |
| 26 | `&#abc;` | TEXT (literal) | Letters where digits expected |
| 27 | `&#123` | TEXT (literal) | No closing semicolon |

#### Hexadecimal numeric references

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 28 | `&#x1F4A9;` | ENTITY("&#x1F4A9;") | Valid hex, uppercase X implicit via lowercase x |
| 29 | `&#X1F4A9;` | ENTITY("&#X1F4A9;") | Uppercase X |
| 30 | `&#xAbCdEf;` | ENTITY("&#xAbCdEf;") | Mixed case hex digits, 6 digits (max) |
| 31 | `&#x0;` | ENTITY("&#x0;") | Single hex digit |
| 32 | `&#x1234567;` | TEXT (literal) | 7 hex digits — exceeds 6-digit limit |
| 33 | `&#x;` | TEXT (literal) | No hex digits |
| 34 | `&#xGHI;` | TEXT (literal) | Invalid hex chars |
| 35 | `&#x1F4A9` | TEXT (literal) | No closing semicolon |

#### Edge cases

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 36 | `&` (just ampersand) | TEXT("&") | Lone ampersand |
| 37 | `& ` (ampersand space) | TEXT("& ") | Ampersand followed by space |
| 38 | `&amp` (no semicolon, end of line) | TEXT("&amp") | Entity at end of input without `;` |
| 39 | Line is exactly `&amp;\n` | ENTITY in paragraph | Entity is entire line content |
| 40 | `&&amp;` | TEXT("&"), ENTITY("&amp;") | Double ampersand before entity |

---

## Feature 2: Hard Line Breaks

### Current State

md4s currently has `MD4S_SOFTBREAK` (emitted at line 1931 in
`process_line()` for paragraph continuation) and `MD4S_NEWLINE`
(emitted after most blocks as a line separator). There is no hard
break support.

The relevant paragraph continuation code is at lines 1928–1937:

```c
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
```

Currently `strip_newline()` (line 749) removes trailing `\n` and `\r`,
and `classify_line()` calls it. The trailing spaces are NOT stripped
by `strip_newline()` — they remain in the content. However, the
paragraph case at lines 1923–1927 strips leading spaces:

```c
/* Trim leading spaces (LLMs sometimes indent). */
while (cl.content_length > 0 && cl.content[0] == ' ') {
    cl.content++;
    cl.content_length--;
}
```

Trailing spaces on paragraph lines are currently preserved in the
inline text and passed to the renderer. This is where hard break
detection needs to happen.

### Design Decision: New Event vs Flag

**Use a new `MD4S_HARDBREAK` event.** Rationale:
- `MD4S_SOFTBREAK` is a standalone event (no detail fields).
- `MD4S_NEWLINE` is structural (block-level line separator).
- A new `MD4S_HARDBREAK` replaces `MD4S_SOFTBREAK` when conditions
  are met. Clean, simple, no flag needed.
- The renderer handles `HARDBREAK` by emitting `<br>` (HTML) or a
  real newline (terminal).

### 2a. Changes to `md4s.h`

Add `MD4S_HARDBREAK` to the "Content events" section:

```c
/* Content events. */
MD4S_TEXT,
MD4S_CODE_TEXT,
MD4S_SOFTBREAK,
MD4S_HARDBREAK,      /* NEW: hard line break (<br>) */
MD4S_NEWLINE,
MD4S_BLOCK_SEPARATOR,
MD4S_ENTITY,
```

No detail struct changes needed — `MD4S_HARDBREAK` carries no payload.

### 2b. Where Detection Happens

Hard breaks are a **line-level** concern, not an inline-parsing concern.
They are detected at the point where a paragraph continuation is about
to emit `MD4S_SOFTBREAK`.

**There are two orthogonal detections:**

1. **Trailing-space hard break:** The PREVIOUS paragraph line ended with
   2+ spaces. This must be detected BEFORE the softbreak is emitted for
   the continuation line.

2. **Backslash hard break:** The PREVIOUS paragraph line ended with `\`.
   Same timing — detected before softbreak emission.

Both detections happen at the same code location: the paragraph
continuation branch in `process_line()` (line 1929–1932).

### 2c. Algorithm — Trailing-Space Hard Break

**Problem:** By the time `process_line()` handles the continuation line,
the previous line's content has already been emitted. We need to know
whether the previous line ended with 2+ trailing spaces.

**Solution:** Add a `bool hard_break_pending` field to `struct md4s_parser`.
Set it when processing a paragraph line. Check it when emitting the
softbreak for the next continuation line.

#### Step-by-step:

1. **New parser field** (add after `partial_displayed` around line 116):
   ```c
   /* Hard break pending from previous paragraph line. */
   bool hard_break_pending;
   ```

2. **When processing a paragraph line** (in the `LINE_PARAGRAPH` case
   of `process_line()`, around lines 1920–1942):

   After the line content is determined but before `parse_inline()` is
   called, check for trailing spaces and backslash:

   ```c
   case LINE_PARAGRAPH:
       close_list_if_needed(p, cl.type);
       close_table_if_needed(p);
       /* Trim leading spaces. */
       while (cl.content_length > 0 && cl.content[0] == ' ') {
           cl.content++;
           cl.content_length--;
       }

       /* Detect hard break conditions on this line's trailing content.
        * This will take effect when the NEXT line continues the paragraph. */
       {
           bool has_hard_break = false;
           size_t content_len = cl.content_length;

           /* Check for backslash hard break: trailing '\' */
           if (content_len > 0 &&
               cl.content[content_len - 1] == '\\') {
               has_hard_break = true;
               cl.content_length--; /* consume the backslash */
           }
           /* Check for trailing-space hard break: 2+ trailing spaces */
           else {
               size_t trailing = 0;
               while (trailing < content_len &&
                      cl.content[content_len - 1 - trailing] == ' ')
                   trailing++;
               if (trailing >= 2) {
                   has_hard_break = true;
                   cl.content_length -= trailing; /* strip trailing spaces */
               }
           }

           /* Consecutive paragraph lines: softbreak or hardbreak. */
           if (p->last_type == LINE_PARAGRAPH &&
               !p->needs_separator &&
               p->emitted_count > 0) {
               if (p->hard_break_pending)
                   emit_simple(p, MD4S_HARDBREAK);
               else
                   emit_simple(p, MD4S_SOFTBREAK);
               parse_inline(p, cl.content, cl.content_length);
           } else {
               maybe_emit_separator(p, &cl);
               emit_simple(p, MD4S_PARAGRAPH_ENTER);
               parse_inline(p, cl.content, cl.content_length);
           }

           p->hard_break_pending = has_hard_break;
       }
       p->emitted_count++;
       p->last_type = LINE_PARAGRAPH;
       p->needs_separator = false;
       break;
   ```

3. **Reset `hard_break_pending`** when leaving paragraph context. This
   happens in several places:
   - `close_paragraph()` (line 1950): add `p->hard_break_pending = false;`
   - Any transition out of paragraph mode is handled by `close_paragraph()`
     or by the `needs_separator` flag, so this single reset point suffices.
   - Also reset in `md4s_create()` (implicitly zero from `calloc`).

### 2d. Algorithm — Backslash Hard Break

The backslash hard break detection is integrated into the same code
as the trailing-space detection (see algorithm above). Key difference:

- The `\` is consumed — it does NOT appear in the emitted text.
- Must distinguish from a backslash escape: `\` at end of line (before
  `\n`) is a hard break. `\` followed by any other character is handled
  by the existing escape logic in `parse_inline_depth()` (line 1100).

**Why this works:** `classify_line()` calls `strip_newline()` which
removes `\n`/`\r`. So by the time we see the content, a trailing `\`
really is the last character before the newline. The existing escape
handler in `parse_inline_depth()` requires a character after the `\`
(`pos + 1 < length`), so a trailing `\` with nothing after it would
just be emitted as literal text. By stripping it at the line level
before calling `parse_inline()`, we prevent it from ever reaching the
inline parser.

### 2e. Interaction with `strip_newline()`

`strip_newline()` removes `\n` and `\r` from the line end. It does NOT
touch spaces or backslashes. The hard break detection operates on the
content AFTER `strip_newline()` has run (via `classify_line()`), which
is correct.

The flow is:
1. `classify_line()` → `strip_newline()` removes `\n`/`\r`
2. Paragraph branch in `process_line()` trims leading spaces
3. NEW: Check trailing content for hard break markers
4. Strip trailing spaces (if hard break) or backslash (if backslash break)
5. Call `parse_inline()` with the trimmed content

### 2f. Where Hard Breaks Do NOT Apply

Per CommonMark, hard breaks only occur within paragraphs. They do NOT
apply in:
- Headings (both ATX and setext)
- Code blocks (fenced and indented)
- HTML blocks
- Table cells (GFM extension — could be added later)
- List items when on a single line (no continuation)

The current implementation naturally enforces this because:
- The hard break detection is ONLY in the `LINE_PARAGRAPH` case of
  `process_line()`.
- Headings, blockquotes, list items, etc. have their own cases that
  do not check `hard_break_pending`.
- `hard_break_pending` is reset when `close_paragraph()` is called.

**Blockquote consideration:** Blockquotes call `parse_inline()` for
their content but do NOT use the paragraph continuation softbreak
path. Each blockquote line is emitted independently with
`BLOCKQUOTE_ENTER`/`BLOCKQUOTE_LEAVE`. Hard breaks in blockquotes
would require separate handling. Per CommonMark, hard breaks CAN
occur inside blockquote paragraphs, but md4s's blockquote model
(each line is independent) means this is a separate feature. **Do
not implement hard breaks in blockquotes in this iteration.**

### 2g. MC/DC Test Cases

#### Trailing-space hard breaks

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 1 | `"hello  \nworld\n"` | PARA_ENTER, TEXT("hello"), HARDBREAK, TEXT("world"), PARA_LEAVE | 2 trailing spaces → hard break |
| 2 | `"hello   \nworld\n"` | PARA_ENTER, TEXT("hello"), HARDBREAK, TEXT("world"), PARA_LEAVE | 3 trailing spaces → still hard break |
| 3 | `"hello \nworld\n"` | PARA_ENTER, TEXT("hello "), SOFTBREAK, TEXT("world"), PARA_LEAVE | 1 trailing space → NOT a hard break (softbreak) |
| 4 | `"hello\nworld\n"` | PARA_ENTER, TEXT("hello"), SOFTBREAK, TEXT("world"), PARA_LEAVE | 0 trailing spaces → softbreak |
| 5 | `"hello  \n"` | PARA_ENTER, TEXT("hello"), PARA_LEAVE | Trailing spaces on last line of paragraph — no break (no continuation) |
| 6 | `"first  \nsecond  \nthird\n"` | PARA_ENTER, TEXT("first"), HARDBREAK, TEXT("second"), HARDBREAK, TEXT("third"), PARA_LEAVE | Consecutive hard breaks |
| 7 | `"# heading  \n"` | HEADING_ENTER, TEXT("heading"), HEADING_LEAVE | Trailing spaces in heading — NOT a hard break |
| 8 | `"- item  \n"` | LIST events, TEXT("item  ") or TEXT("item") | Trailing spaces in list — NOT a hard break |
| 9 | `"  \nworld\n"` | Two spaces then newline as first para line, then continuation — the first line is all spaces, which is a blank line, not a paragraph | Blank line edge case |
| 10 | `"a  \n  \nb\n"` | PARA_ENTER, TEXT("a"), PARA_LEAVE, SEPARATOR, PARA_ENTER, TEXT("b"), PARA_LEAVE | Hard break trailing spaces, then blank line → new paragraph (hard_break_pending is reset) |

#### Backslash hard breaks

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 11 | `"hello\\\nworld\n"` | PARA_ENTER, TEXT("hello"), HARDBREAK, TEXT("world"), PARA_LEAVE | Backslash at end of line → hard break, `\` consumed |
| 12 | `"hello\\\n"` | PARA_ENTER, TEXT("hello"), PARA_LEAVE | Backslash on last para line — no continuation, no break |
| 13 | `"hello\\world\n"` | PARA_ENTER, TEXT("hello"), TEXT("w"), TEXT("orld") or equivalent | Backslash NOT at end of line → escape, not break (handled by inline parser) |
| 14 | `"first\\\nsecond\\\nthird\n"` | PARA_ENTER, TEXT("first"), HARDBREAK, TEXT("second"), HARDBREAK, TEXT("third"), PARA_LEAVE | Consecutive backslash breaks |
| 15 | `"a\\  \nworld\n"` | Backslash followed by spaces at end — the backslash is not the LAST char, so trailing-space check applies (2 spaces). Content after stripping spaces is `"a\\"`. The `\\` is then a backslash escape handled by inline parser. Result: HARDBREAK from trailing spaces. | Backslash + trailing spaces interaction |

#### Mixed and edge cases

| # | Input | Expected | Condition |
|---|-------|----------|-----------|
| 16 | `"a  \nb\\\nc\n"` | PARA_ENTER, TEXT("a"), HARDBREAK, TEXT("b"), HARDBREAK, TEXT("c"), PARA_LEAVE | Trailing-space break then backslash break |
| 17 | `"only\n"` | PARA_ENTER, TEXT("only"), PARA_LEAVE | Single line paragraph — no break of any kind |
| 18 | `"> quote  \n> more\n"` | BLOCKQUOTE events — NO hard break | Hard break not applicable in blockquotes (current model) |
| 19 | `"```\nhello  \n```\n"` | CODE_BLOCK events, CODE_TEXT("hello  ") | Trailing spaces in code block preserved, no hard break |
| 20 | `"a\\\n\nb\n"` | PARA_ENTER, TEXT("a"), PARA_LEAVE, SEPARATOR, PARA_ENTER, TEXT("b"), PARA_LEAVE | Backslash break then blank line — break never fires because blank line ends the paragraph |

### 2h. Summary of All Code Changes

#### `md4s.h`
- Add `MD4S_HARDBREAK` and `MD4S_ENTITY` to `enum md4s_event`

#### `md4s.c`
1. **`struct md4s_parser`** (around line 116): Add `bool hard_break_pending;`
2. **`parse_inline_depth()`** (lines 1118–1158): Replace the entity block
   with the new algorithm that validates named, decimal, and hex entities
   and emits `MD4S_ENTITY` instead of decoding to replacement text.
3. **`process_line()` `LINE_PARAGRAPH` case** (lines 1920–1942): Replace
   with the new algorithm that detects trailing-space and backslash hard
   breaks, strips the markers, and emits `HARDBREAK` or `SOFTBREAK`.
4. **`close_paragraph()`** (line 1950): Add `p->hard_break_pending = false;`

#### `tests/test_md4s.c`
- Add `entity_text` field to `struct recorded_event` if distinct from
  `text` is desired (not needed — reuse `text`).
- Add ~60 new test functions covering the MC/DC tables above.

---

## Implementation Order

1. Add `MD4S_ENTITY` and `MD4S_HARDBREAK` to `md4s.h` enum.
2. Implement entity reference parsing in `parse_inline_depth()`.
3. Implement hard break detection in `process_line()` paragraph case.
4. Add `hard_break_pending` field and reset logic.
5. Write tests — entity tests first (simpler), then hard break tests.
6. Update any existing tests that assert `MD4S_TEXT` for the six
   previously-decoded entities (they now expect `MD4S_ENTITY`).

---

## Appendix: Constant Limits Rationale

- **Named entity max 48 chars:** The longest HTML5 entity name is
  `CounterClockwiseContourIntegral` at 31 characters. 48 provides
  generous headroom. The CommonMark spec says "1 to 31 alphanumeric
  characters" but some extended specs allow more.
- **Decimal max 7 digits:** Unicode max codepoint is U+10FFFF =
  1,114,111 (7 digits). Values beyond this are structurally valid
  per our parser but semantically invalid; the renderer can reject.
- **Hex max 6 digits:** U+10FFFF = 0x10FFFF (6 hex digits).
- **Semicolon search limit removed:** The current code limits the
  semicolon scan to `pos + 10`, which is too small for many named
  entities. The new code uses the character-class constraints
  (`[A-Za-z0-9]` for names, `[0-9]` for decimal, `[0-9a-fA-F]` for
  hex) as the natural limit instead of a fixed offset. The max-length
  limits (48, 7, 6) provide the upper bound.
