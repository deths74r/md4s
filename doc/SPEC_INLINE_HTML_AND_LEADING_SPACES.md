# md4s Implementation Spec: Inline Raw HTML & Leading Space Tolerance

This spec covers two features needed to bring md4s closer to CommonMark compliance. Both are block/inline classification changes that affect `md4s.c` and `md4s.h`.

---

## Feature 1: Inline Raw HTML

### 1.1 Problem

When `<div>` or `<!-- comment -->` appears inside flowing paragraph text, md4s emits it as literal `MD4S_TEXT`. CommonMark specifies that raw HTML appearing inline should be passed through as raw HTML, not escaped or treated as text. Currently, the only `<`-triggered inline behavior is autolink detection (`<url>` / `<email>`), and any `<` that fails the autolink test falls through to literal text.

### 1.2 CommonMark Inline HTML Types

CommonMark section 6.6 defines six kinds of inline raw HTML:

| Type | Pattern | Example |
|------|---------|---------|
| Open tag | `<tagname` + optional attrs + `>` or `/>` | `<div>`, `<br/>`, `<span class="x">` |
| Closing tag | `</tagname>` | `</div>` |
| HTML comment | `<!--` ... `-->` | `<!-- TODO -->` |
| Processing instruction | `<?` ... `?>` | `<?xml version="1.0"?>` |
| Declaration | `<!` + uppercase letter ... `>` | `<!DOCTYPE html>` |
| CDATA | `<![CDATA[` ... `]]>` | `<![CDATA[data]]>` |

For a streaming parser focused on LLM output, open/closing tags and comments are the most important. PI/declaration/CDATA are very rare in practice but should be handled for correctness.

### 1.3 New Event

Add a new event to the `md4s_event` enum:

```c
/* In md4s.h, after MD4S_IMAGE_LEAVE: */
MD4S_HTML_INLINE,
```

This is a content event (not a span pair). The raw HTML text is delivered in `detail->text` / `detail->text_length`. The detail carries the complete raw HTML including angle brackets, e.g. `<div class="foo">`.

**Rationale for a single event (not ENTER/LEAVE pair):** Inline HTML is opaque raw content, not a semantic span. There is nothing to nest inside it. A consumer that renders HTML passes it through verbatim. A consumer that renders to a terminal ignores it or displays it dimly. The single-event approach matches how `MD4S_TEXT` and `MD4S_CODE_TEXT` work.

**Rationale for a new event (not reusing MD4S_TEXT):** Consumers need to distinguish "this is raw HTML to pass through" from "this is text content to escape." If we reused `MD4S_TEXT`, consumers rendering to HTML would double-escape the angle brackets.

### 1.4 Tag Name Validation

A valid HTML tag name (per CommonMark):
- Starts with an ASCII letter (`[a-zA-Z]`)
- Followed by zero or more ASCII letters, digits, or hyphens (`[a-zA-Z0-9-]`)

This rules out `<123>`, `<-tag>`, `<.component>`. It allows `<my-component>`, `<h1>`, `<br>`.

```c
/*
 * Check if ch is a valid tag name start character.
 */
static bool is_tag_name_start(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/*
 * Check if ch is a valid tag name continuation character.
 */
static bool is_tag_name_char(char ch)
{
    return is_tag_name_start(ch) || (ch >= '0' && ch <= '9') || ch == '-';
}
```

### 1.5 Attribute Parsing

After the tag name, an open tag may have attributes. CommonMark specifies:

- An attribute name: `[a-zA-Z_:][a-zA-Z0-9_.:-]*`
- Optionally followed by `=` and a value:
  - Unquoted: one or more chars not including `"`, `'`, `=`, `<`, `>`, `` ` ``, or space
  - Single-quoted: `'...'` (no `'` inside)
  - Double-quoted: `"..."` (no `"` inside)
  - Bare (no value): just the attribute name

The attribute loop:
1. Skip whitespace
2. If we see `>` or `/>`, the tag is complete
3. If we see `[a-zA-Z_:]`, parse attribute name
4. Skip whitespace, check for `=`
5. If `=`, skip whitespace, parse value (quoted or unquoted)
6. If anything else, this is not valid inline HTML -- abort

```c
/*
 * Check if ch is a valid attribute name start character.
 */
static bool is_attr_name_start(char ch)
{
    return is_tag_name_start(ch) || ch == '_' || ch == ':';
}

/*
 * Check if ch is a valid attribute name continuation character.
 */
static bool is_attr_name_char(char ch)
{
    return is_attr_name_start(ch) ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
}
```

### 1.6 The Parsing Function

```c
/*
 * Try to parse inline HTML starting at text[pos] where text[pos] == '<'.
 * On success, returns the position just past the closing '>'.
 * On failure, returns 0 (meaning: not inline HTML, treat '<' as literal).
 *
 * This function handles all six CommonMark inline HTML types.
 */
static size_t try_parse_inline_html(const char *text, size_t length, size_t pos)
```

**Algorithm:**

```
pos points to '<'
next = pos + 1

CASE 1: Closing tag  (next char is '/')
  - Skip '/'
  - Parse tag name (must be >= 1 char)
  - Skip whitespace
  - Must see '>'
  - Return pos after '>'

CASE 2: HTML comment  (next chars are '!--')
  - Must see '!--' at pos+1..pos+3
  - Scan forward for '-->'
  - CONSTRAINT: the text immediately after <!-- must not be '>'
    and must not be '->' (per CommonMark)
  - The comment body must not contain '--'... wait, actually CommonMark
    is more permissive than XML here. The rule is: scan for '-->' as
    the terminator. The comment must not start with '>' or '->'.
  - If '-->' not found within the current line, return 0 (see 1.8)
  - Return pos after '-->'

CASE 3: Processing instruction  (next char is '?')
  - Scan forward for '?>'
  - If not found on this line, return 0
  - Return pos after '?>'

CASE 4: Declaration  (next chars are '!' + uppercase letter)
  - Must see '!' at pos+1, then [A-Z] at pos+2
  - Scan forward for '>'
  - If not found on this line, return 0
  - Return pos after '>'

CASE 5: CDATA  (next chars are '![CDATA[')
  - Must see '![CDATA[' at pos+1..pos+8
  - Scan forward for ']]>'
  - If not found on this line, return 0
  - Return pos after ']]>'

CASE 6: Open tag  (next char is a tag name start)
  - Parse tag name (>= 1 char)
  - Parse zero or more attributes (see 1.5)
  - Allow optional whitespace
  - Allow optional '/' before final '>'
  - Must see '>'
  - Return pos after '>'

DEFAULT: return 0 (not inline HTML)
```

### 1.7 Integration Point in parse_inline_depth()

The inline HTML check must be inserted at the right priority level in `parse_inline_depth()`. CommonMark priority order for `<`:

1. **Code spans** (backticks) -- already handled, comes first
2. **Inline HTML** -- NEW, must come before autolinks
3. **Autolinks** -- existing `<url>` / `<email>` detection

The key insight: autolinks are `<scheme://...>` or `<user@host>`. HTML tags are `<tagname ...>`. These are distinguishable because:
- Autolinks contain `://` or `@` and no spaces
- HTML tags start with a letter (open), `/` (close), `!` (comment/decl/CDATA), or `?` (PI)

But there is ambiguity: `<a>` is both a valid HTML tag and could theoretically be confused with autolink processing. However, autolinks require `://` or `@` in the content, so `<a>` will never be an autolink. The correct order is: **try inline HTML first, then fall through to autolink if it fails.**

Wait -- actually, there's a subtlety. Consider `<https://example.com>`. This starts with `<` followed by a letter (`h`), so the inline HTML parser would try to parse `https` as a tag name. It would then look for `>` or attributes. It would encounter `:` which is not a valid character after a tag name in the positions where we expect whitespace or `>` or `/`. So `try_parse_inline_html` would return 0, and we'd fall through to the autolink check. This is correct behavior.

Actually, re-examining: after parsing the tag name `https`, the next char is `:`. The attribute parsing starts by skipping whitespace, then checking for `>`, `/>`, or attribute name start. `:` is an attribute name start character per CommonMark (`[a-zA-Z_:]`). So `://example.com` could be parsed as an attribute name `:` with... no, `//` is not a valid attribute name character. The attribute name parser would consume `:`, then fail at `/`. Then we'd check if the next char is `>` -- it's not, it's `/`. We'd check for `/>` -- the char after `/` is `/`, not `>`. So we'd return 0 (not valid inline HTML) and fall through to autolink. This is correct.

However, to be safe and avoid any ambiguity, the implementation should:

1. Try inline HTML first (inside `parse_inline_depth`, at the `<` case)
2. If `try_parse_inline_html` returns 0, fall through to the existing autolink check
3. If it returns a valid end position, emit `MD4S_HTML_INLINE` with the raw text

**Code insertion point** -- replace the current autolink block (lines 1322-1362) with:

```c
/* Inline HTML and Autolink: both triggered by '<'. */
if (text[pos] == '<') {
    /* Try inline HTML first (higher priority than autolinks). */
    size_t html_end = try_parse_inline_html(text, length, pos);
    if (html_end > 0) {
        if (pos > plain_start)
            emit_text(p, MD4S_TEXT, text + plain_start,
                      pos - plain_start);
        emit_text(p, MD4S_HTML_INLINE, text + pos,
                  html_end - pos);
        pos = html_end;
        plain_start = pos;
        continue;
    }

    /* Autolink: <url> or <email> (existing code, unchanged). */
    size_t close = pos + 1;
    while (close < length && text[close] != '>' &&
           text[close] != ' ' && text[close] != '\n')
        close++;
    if (close < length && text[close] == '>' &&
        close > pos + 1) {
        /* ... existing autolink code ... */
    }
}
```

Actually, there's a cleaner approach. Since `try_parse_inline_html` validates tag names, and autolinks require `://` or `@`, there is no overlap. Any valid autolink will fail the inline HTML parser (tag names can't contain `://`). So the order is safe. But for maximum clarity, the inline HTML check should come first and the autolink check should only run if inline HTML fails.

### 1.8 Multi-line Inline HTML

md4s processes one line at a time. Inline parsing happens on a single line's content (after newline stripping). CommonMark says inline HTML can span lines -- for example, an HTML comment `<!-- ... -->` can span multiple lines.

**Decision: single-line only for inline HTML.**

Rationale:
1. md4s is a streaming parser. Lines are processed and forgotten. There is no mechanism to defer inline content across lines.
2. LLM output virtually never produces multi-line inline HTML within flowing text. Multi-line HTML is block-level (and md4s already handles block HTML via `STATE_HTML_BLOCK`).
3. Adding multi-line inline HTML would require an `STATE_INLINE_HTML` or similar buffering mechanism, which conflicts with the one-line-at-a-time architecture.
4. If `<!--` appears and `-->` is not found on the same line, the `<` is emitted as literal text. This is a known deviation from CommonMark but acceptable for the streaming use case.

**Document this limitation** in the header file comment for `MD4S_HTML_INLINE`.

### 1.9 Interaction with Existing Features

**Code spans:** Code spans have higher priority. Inside `` `<div>` ``, the `<div>` is code text, not inline HTML. This is already correct because code span detection comes before the `<` check in `parse_inline_depth`.

**Backslash escapes:** `\<div>` should emit `<` as literal text followed by `div>` as literal text. This is already correct because escape handling comes before the `<` check. The escaped `<` is emitted as `MD4S_TEXT("<")`, and then `div>` is literal text.

**HTML entities:** `&lt;div>` is not inline HTML because `&lt;` is decoded to `<` before inline parsing -- wait, no. md4s decodes entities inline during `parse_inline_depth`. The entity `&lt;` at position N is decoded and emitted as `MD4S_TEXT("<")`. The remaining `div>` is then literal text. So `&lt;div>` correctly does NOT trigger inline HTML. Good.

**HTML blocks:** Block-level HTML (`<div>` at the start of a line, matching `is_html_block_start`) enters `STATE_HTML_BLOCK` and the entire line is emitted as `MD4S_HTML_BLOCK_ENTER` / `MD4S_TEXT`. Inline HTML only applies inside paragraphs and other inline contexts. There is no conflict.

**Links and images:** `[text](url)` and `![alt](url)` parsing scans for `]` and `(`, not `<`. However, link/image alt text can contain inline HTML: `[<em>text</em>](url)`. Since `parse_inline_depth` recurses into link/image content, inline HTML will naturally be detected inside alt text. This is correct behavior.

### 1.10 Detailed Algorithm: try_parse_inline_html

```c
static size_t try_parse_inline_html(const char *text, size_t length,
                                     size_t pos)
{
    /* pos points to '<'. Must have at least one more character. */
    if (pos + 1 >= length)
        return 0;

    size_t start = pos;
    pos++; /* skip '<' */

    /* ---- Closing tag: </tagname> ---- */
    if (text[pos] == '/') {
        pos++;
        if (pos >= length || !is_tag_name_start(text[pos]))
            return 0;
        while (pos < length && is_tag_name_char(text[pos]))
            pos++;
        /* Skip optional whitespace. */
        while (pos < length && (text[pos] == ' ' || text[pos] == '\t'))
            pos++;
        if (pos >= length || text[pos] != '>')
            return 0;
        return pos + 1; /* past '>' */
    }

    /* ---- HTML comment: <!-- ... --> ---- */
    if (pos + 2 < length && text[pos] == '!' &&
        text[pos + 1] == '-' && text[pos + 2] == '-') {
        pos += 3; /* past '!--' */
        /* CommonMark: must not start with '>' or '->' */
        if (pos < length && text[pos] == '>')
            return 0;
        if (pos + 1 < length && text[pos] == '-' && text[pos + 1] == '>')
            return 0;
        /* Scan for '-->' */
        while (pos + 2 < length) {
            if (text[pos] == '-' && text[pos + 1] == '-' &&
                text[pos + 2] == '>') {
                return pos + 3; /* past '-->' */
            }
            pos++;
        }
        return 0; /* not found on this line */
    }

    /* ---- Processing instruction: <? ... ?> ---- */
    if (text[pos] == '?') {
        pos++; /* past '?' */
        while (pos + 1 < length) {
            if (text[pos] == '?' && text[pos + 1] == '>') {
                return pos + 2; /* past '?>' */
            }
            pos++;
        }
        return 0;
    }

    /* ---- CDATA: <![CDATA[ ... ]]> ---- */
    if (pos + 7 < length && text[pos] == '!' &&
        text[pos + 1] == '[' && text[pos + 2] == 'C' &&
        text[pos + 3] == 'D' && text[pos + 4] == 'A' &&
        text[pos + 5] == 'T' && text[pos + 6] == 'A' &&
        text[pos + 7] == '[') {
        pos += 8; /* past '![CDATA[' */
        while (pos + 2 < length) {
            if (text[pos] == ']' && text[pos + 1] == ']' &&
                text[pos + 2] == '>') {
                return pos + 3; /* past ']]>' */
            }
            pos++;
        }
        return 0;
    }

    /* ---- Declaration: <!LETTER ... > ---- */
    if (text[pos] == '!' && pos + 1 < length &&
        text[pos + 1] >= 'A' && text[pos + 1] <= 'Z') {
        pos += 2; /* past '!' and first letter */
        while (pos < length && text[pos] != '>')
            pos++;
        if (pos >= length)
            return 0;
        return pos + 1; /* past '>' */
    }

    /* ---- Open tag: <tagname attrs> or <tagname attrs /> ---- */
    if (!is_tag_name_start(text[pos]))
        return 0;

    /* Parse tag name. */
    while (pos < length && is_tag_name_char(text[pos]))
        pos++;

    /* Parse attributes. */
    for (;;) {
        /* Skip whitespace. */
        size_t ws_start = pos;
        while (pos < length &&
               (text[pos] == ' ' || text[pos] == '\t'))
            pos++;

        /* Self-closing or close. */
        if (pos < length && text[pos] == '/') {
            if (pos + 1 < length && text[pos + 1] == '>')
                return pos + 2; /* past '/>' */
            return 0; /* '/' not followed by '>' */
        }
        if (pos < length && text[pos] == '>')
            return pos + 1; /* past '>' */

        /* Must have had whitespace before attribute. */
        if (pos == ws_start)
            return 0; /* no whitespace, no '>' -- invalid */

        /* End of input without closing. */
        if (pos >= length)
            return 0;

        /* Parse attribute name. */
        if (!is_attr_name_start(text[pos]))
            return 0;
        while (pos < length && is_attr_name_char(text[pos]))
            pos++;

        /* Optional attribute value. */
        size_t before_eq = pos;
        while (pos < length &&
               (text[pos] == ' ' || text[pos] == '\t'))
            pos++;
        if (pos < length && text[pos] == '=') {
            pos++; /* past '=' */
            while (pos < length &&
                   (text[pos] == ' ' || text[pos] == '\t'))
                pos++;
            if (pos >= length)
                return 0;
            if (text[pos] == '"') {
                pos++;
                while (pos < length && text[pos] != '"')
                    pos++;
                if (pos >= length)
                    return 0;
                pos++; /* past closing '"' */
            } else if (text[pos] == '\'') {
                pos++;
                while (pos < length && text[pos] != '\'')
                    pos++;
                if (pos >= length)
                    return 0;
                pos++; /* past closing '\'' */
            } else {
                /* Unquoted value. */
                if (text[pos] == '"' || text[pos] == '\'' ||
                    text[pos] == '=' || text[pos] == '<' ||
                    text[pos] == '>' || text[pos] == '`' ||
                    text[pos] == ' ')
                    return 0;
                while (pos < length &&
                       text[pos] != '"' && text[pos] != '\'' &&
                       text[pos] != '=' && text[pos] != '<' &&
                       text[pos] != '>' && text[pos] != '`' &&
                       text[pos] != ' ' && text[pos] != '\t')
                    pos++;
            }
        } else {
            /* No '=' -- bare attribute. Restore position to after
             * attribute name (whitespace may be before next attr
             * or before '>'). */
            pos = before_eq;
        }
    }
}
```

### 1.11 MC/DC Test Cases

Each test feeds markdown through the parser and checks for `MD4S_HTML_INLINE` events (or their absence).

#### Open Tags

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T1 | `text <div> more` | HTML_INLINE `<div>` | Basic open tag |
| T2 | `text <br/> more` | HTML_INLINE `<br/>` | Self-closing, no space |
| T3 | `text <br /> more` | HTML_INLINE `<br />` | Self-closing, with space |
| T4 | `text <span class="x"> more` | HTML_INLINE `<span class="x">` | Double-quoted attribute |
| T5 | `text <span class='x'> more` | HTML_INLINE `<span class='x'>` | Single-quoted attribute |
| T6 | `text <input disabled> more` | HTML_INLINE `<input disabled>` | Bare attribute (no value) |
| T7 | `text <input type=text> more` | HTML_INLINE `<input type=text>` | Unquoted attribute value |
| T8 | `text <my-component> more` | HTML_INLINE `<my-component>` | Hyphenated tag name |
| T9 | `text <h1> more` | HTML_INLINE `<h1>` | Tag with digit |
| T10 | `<div class="a" id='b' hidden>` | HTML_INLINE (full tag) | Multiple mixed attributes |

#### Closing Tags

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T11 | `text </div> more` | HTML_INLINE `</div>` | Basic closing tag |
| T12 | `text </div > more` | HTML_INLINE `</div >` | Closing tag with trailing space |
| T13 | `text </my-component> more` | HTML_INLINE `</my-component>` | Hyphenated closing tag |

#### HTML Comments

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T14 | `text <!-- comment --> more` | HTML_INLINE `<!-- comment -->` | Basic comment |
| T15 | `text <!--x--> more` | HTML_INLINE `<!--x-->` | Minimal comment |
| T16 | `text <!-- --> more` | HTML_INLINE `<!-- -->` | Comment with only space |
| T17 | `text <!---> more` | TEXT (literal) | Comment starting with `>` (invalid) |
| T18 | `text <!----> more` | TEXT (literal) | Comment starting with `->` (invalid) |
| T19 | `text <!-- no end` | TEXT (literal) | Unclosed comment (single-line limit) |

#### Processing Instructions

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T20 | `text <?xml version="1.0"?> more` | HTML_INLINE `<?xml version="1.0"?>` | Basic PI |
| T21 | `text <?php echo "hi"; ?> more` | HTML_INLINE `<?php echo "hi"; ?>` | PHP-style PI |
| T22 | `text <?incomplete` | TEXT (literal) | Unclosed PI |

#### Declarations

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T23 | `text <!DOCTYPE html> more` | HTML_INLINE `<!DOCTYPE html>` | Basic declaration |
| T24 | `text <!X> more` | HTML_INLINE `<!X>` | Minimal declaration |
| T25 | `text <!doctype html> more` | TEXT (literal) | Lowercase (invalid -- must be uppercase) |

#### CDATA

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T26 | `text <![CDATA[data]]> more` | HTML_INLINE `<![CDATA[data]]>` | Basic CDATA |
| T27 | `text <![CDATA[]]> more` | HTML_INLINE `<![CDATA[]]>` | Empty CDATA |
| T28 | `text <![CDATA[no end` | TEXT (literal) | Unclosed CDATA |

#### Invalid / Edge Cases

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T29 | `text <123> more` | TEXT (literal) | Digit-start tag name |
| T30 | `text < div> more` | TEXT (literal) | Space after `<` |
| T31 | `text <> more` | TEXT (literal) | Empty tag |
| T32 | `text <div more` | TEXT (literal) | Unclosed open tag |
| T33 | `text <div attr=> more` | TEXT (literal) | Attribute with `=` but no value |
| T34 | `` text `<div>` more `` | CODE_SPAN (no HTML_INLINE) | Code span takes priority |
| T35 | `text \<div> more` | TEXT `<div>` | Backslash-escaped `<` |
| T36 | `text <div/> more` | HTML_INLINE `<div/>` | Self-close without space |
| T37 | `[<em>text</em>](url)` | LINK + HTML_INLINE | Inline HTML inside link text |

#### Autolink vs Inline HTML Disambiguation

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| T38 | `<https://example.com>` | LINK (autolink) | URL autolink, not HTML |
| T39 | `<user@example.com>` | LINK (autolink) | Email autolink, not HTML |
| T40 | `<a>` | HTML_INLINE `<a>` | Single-letter tag, not autolink |
| T41 | `<a href="url">` | HTML_INLINE | Tag with href attr, not autolink |

---

## Feature 2: Leading Space Tolerance

### 2.1 Problem

CommonMark allows 0-3 leading spaces before most block-level constructs. md4s currently requires column 0 for:
- ATX headings: `line[0] == '#'` (line 839)
- Blockquotes: `line[0] == '>'` (line 869)
- Fenced code blocks: `line[0] == '`'` or `line[0] == '~'` (line 803)

Some constructs already handle leading spaces correctly:
- Thematic breaks: `is_thematic_break()` skips spaces in its loop (line 223-246)
- HTML blocks: `is_html_block_start()` checks `line[0] == '<'` -- this also needs fixing
- Setext underlines: `is_setext_underline()` already skips up to 3 leading spaces (line 397-399)
- Lists: already use `count_leading_spaces()` and allow indentation (lines 899-951)
- Indented code: already checks for 4 leading spaces (line 881-888)

### 2.2 The Pattern

For each construct, the fix follows the same pattern:

```
Before: if (line[0] == MARKER) { ... }
After:  skip up to 3 leading spaces, then check for MARKER
```

The critical constraint: **4+ leading spaces must NOT be consumed** because 4+ spaces triggers indented code block (when not in a list).

### 2.3 Shared Helper

Add a helper to skip 0-3 leading spaces and return the offset:

```c
/*
 * Skip up to 3 leading spaces. Returns the number of spaces skipped.
 * This is the CommonMark "optional indent" that precedes most block
 * constructs. 4+ spaces is an indented code block, not an indent.
 */
static size_t skip_optional_indent(const char *line, size_t len)
{
    size_t n = 0;
    while (n < len && n < 3 && line[n] == ' ')
        n++;
    return n;
}
```

Note: this returns at most 3. If there are 4+ spaces, it returns 3 (the first 3 are consumed as optional indent). But wait -- that's wrong. If there are 4 spaces, that should be an indented code block, not "3 spaces of indent + 1 space of content." The correct behavior: `skip_optional_indent` only consumes spaces, and the caller checks whether the remaining content matches a block construct. The indented code block check (4+ spaces) must come BEFORE the optional-indent checks in `classify_line`.

Actually, looking at the current code flow in `classify_line()`, the order matters:

1. Fenced code (currently checks `line[0]`)
2. ATX heading (currently checks `line[0]`)
3. Thematic break (already handles spaces)
4. Blockquote (currently checks `line[0]`)
5. Indented code (checks 4 leading spaces)
6. HTML block (currently checks `line[0]`)
7. Lists (already handles indent)
8. Paragraph (default)

The indented code check is at position 5. If we add leading-space tolerance to items 1-4 and 6, we need to ensure that a line with 4+ leading spaces still matches indented code, not a construct with optional indent.

**Key insight:** The indented code check should happen BEFORE the optional-indent versions of headings, blockquotes, and fences. Currently it's after those checks, which is fine because those checks require `line[0]` to be the marker. But once we allow `line[0..2]` to be spaces, a line like `    # Heading` (4 spaces + `#`) should be indented code, NOT a heading with 4 spaces of indent.

Wait -- CommonMark says up to 3 spaces. So `    # Heading` (4 spaces) is indeed an indented code block, not a heading. The `skip_optional_indent` helper returns at most 3, so if there are 4 spaces, it returns 3, and the remaining content starts with a space, not `#`. So the heading check would fail. Let me trace through:

- Line: `    # Heading` (4 spaces)
- `skip_optional_indent` returns 3
- Remaining: ` # Heading` (1 space + `#`)
- Check `line[indent] == '#'` -- it's a space, not `#` -- heading check fails
- Falls through to... indented code check

Hmm, but the indented code check currently looks at the original line: `line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' '`. This still works because we haven't modified the line, just computed an offset. So the order works:

1. Compute `indent = skip_optional_indent(line, len)`
2. Check fenced code at `line[indent]`
3. Check ATX heading at `line[indent]`
4. Check thematic break (already handles spaces)
5. Check blockquote at `line[indent]`
6. Check indented code at `line[0..3]` (unchanged, uses original line)
7. Check HTML block at `line[indent]`

Actually wait, there's a subtlety. The indented code check must NOT trigger when a construct with optional indent is valid. Consider `   > quote` (3 spaces + `>`). With the new logic, `skip_optional_indent` returns 3, and `line[3] == '>'` matches blockquote. The blockquote check runs before indented code, so it returns correctly.

But consider `   code` (4 spaces -- but wait, that's only 3 spaces). Let me reconsider: `    code` (4 spaces). `skip_optional_indent` returns 3. `line[3] == ' '` -- not a heading, not a fence, not a blockquote. Falls through. Indented code check: `line[0..3]` are all spaces. Matches indented code. Correct.

And `   code` (3 spaces + text). `skip_optional_indent` returns 3. `line[3]` is `c` -- not any block marker. Falls through. Indented code check: only 3 spaces, needs 4. Fails. Falls through to paragraph. Correct.

### 2.4 Changes to classify_line()

Here are the exact changes needed in `classify_line()`:

#### 2.4.1 Fenced Code Block (line 802-836)

**Current code:**
```c
if (len >= 3 && (line[0] == '`' || line[0] == '~')) {
    char ch = line[0];
    int n = count_leading(line, len, ch);
```

**New code:**
```c
{
    size_t indent = skip_optional_indent(line, len);
    if (indent < len && len - indent >= 3 &&
        (line[indent] == '`' || line[indent] == '~')) {
        char ch = line[indent];
        int n = count_leading(line + indent, len - indent, ch);
        if (n >= 3) {
            bool valid = true;
            if (ch == '`') {
                for (size_t i = indent + (size_t)n; i < len; i++) {
                    if (line[i] == '`') {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                cl.type = LINE_FENCE_OPEN;
                cl.fence_char = ch;
                cl.fence_length = n;
                /* Extract info string (trim whitespace). */
                const char *info = line + indent + n;
                size_t info_len = len - indent - (size_t)n;
                while (info_len > 0 && info[0] == ' ') {
                    info++;
                    info_len--;
                }
                while (info_len > 0 &&
                       info[info_len - 1] == ' ')
                    info_len--;
                cl.info_string = info;
                cl.info_length = info_len;
                return cl;
            }
        }
    }
}
```

Also, the **closing fence** check inside `STATE_FENCED_CODE` (line 768) must allow leading spaces:

**Current:**
```c
int n = count_leading(line, len, p->fence_char);
```

**New:**
```c
size_t fence_indent = skip_optional_indent(line, len);
int n = count_leading(line + fence_indent, len - fence_indent,
                      p->fence_char);
if (n >= p->fence_length) {
    bool rest_blank = is_all_whitespace(
        line + fence_indent + n,
        len - fence_indent - (size_t)n);
```

#### 2.4.2 ATX Heading (line 838-860)

**Current code:**
```c
if (line[0] == '#') {
    int level = count_leading(line, len, '#');
    if (level >= 1 && level <= 6 &&
        ((size_t)level == len || line[level] == ' ')) {
```

**New code:**
```c
{
    size_t indent = skip_optional_indent(line, len);
    if (indent < len && line[indent] == '#') {
        int level = count_leading(line + indent, len - indent, '#');
        if (level >= 1 && level <= 6 &&
            (indent + (size_t)level == len ||
             line[indent + level] == ' ')) {
            cl.type = LINE_HEADING;
            cl.heading_level = level;
            cl.content = line + indent + level;
            cl.content_length = len - indent - (size_t)level;
            if (cl.content_length > 0 && cl.content[0] == ' ') {
                cl.content++;
                cl.content_length--;
            }
            /* Strip trailing '#' and spaces. */
            while (cl.content_length > 0 &&
                   cl.content[cl.content_length - 1] == '#')
                cl.content_length--;
            while (cl.content_length > 0 &&
                   cl.content[cl.content_length - 1] == ' ')
                cl.content_length--;
            return cl;
        }
    }
}
```

#### 2.4.3 Blockquote (line 868-878)

**Current code:**
```c
if (line[0] == '>') {
    cl.type = LINE_BLOCKQUOTE;
    cl.content = line + 1;
    cl.content_length = len - 1;
```

**New code:**
```c
{
    size_t indent = skip_optional_indent(line, len);
    if (indent < len && line[indent] == '>') {
        cl.type = LINE_BLOCKQUOTE;
        cl.content = line + indent + 1;
        cl.content_length = len - indent - 1;
        if (cl.content_length > 0 && cl.content[0] == ' ') {
            cl.content++;
            cl.content_length--;
        }
        return cl;
    }
}
```

#### 2.4.4 HTML Block Start (line 890-896)

**Current code:**
```c
if (is_html_block_start(line, len)) {
```

**New code:**
```c
{
    size_t indent = skip_optional_indent(line, len);
    if (indent < len &&
        is_html_block_start(line + indent, len - indent)) {
        cl.type = LINE_HTML_BLOCK;
        cl.content = line + indent;
        cl.content_length = len - indent;
        return cl;
    }
}
```

Note: `is_html_block_start()` already checks `line[0] == '<'`, so passing `line + indent` is correct.

#### 2.4.5 Indented Code Block -- NO CHANGE but reorder

The indented code block check (line 880-888) must remain BEFORE the constructs that now accept optional indent, OR (simpler) the `skip_optional_indent` approach naturally handles this because 4 spaces will yield indent=3, and `line[3]` will be a space, not a block marker. So the existing order works.

However, there is one subtle issue: a line with exactly 4 spaces and then a `#`:
- `    # heading` (4 spaces + `# heading`)
- `skip_optional_indent` returns 3
- `line[3] == ' '`, not `#` -- heading check fails
- Indented code check: `line[0..3]` are all spaces -- matches
- Result: indented code `# heading` -- this is **correct** per CommonMark

But wait, the current code checks indented code AFTER heading. With the new logic, heading is checked first with optional indent. `skip_optional_indent` returns 3, then checks `line[3]`. `line[3]` is a space, not `#`. Heading fails. Then indented code check runs on the original line. `line[0..3]` are spaces. Matches. Correct.

Actually, I need to double-check the order more carefully. Currently in `classify_line()`:

```
fence open (line 802)
ATX heading (line 838)
thematic break (line 862)
blockquote (line 868)
indented code (line 880)
HTML block (line 890)
lists (line 898)
```

After the changes, the order should be:

```
fence open (with optional indent)
ATX heading (with optional indent)
thematic break (already handles spaces)
blockquote (with optional indent)
indented code (UNCHANGED - checks original line[0..3])
HTML block (with optional indent)
lists (already handle indent)
```

This order is correct. The key: for a line with 4+ spaces, `skip_optional_indent` returns 3, and the remaining content starts with a space, so none of the block-marker checks match. Then the indented code check on the original line succeeds.

### 2.5 Tab Handling

CommonMark says a tab character advances to the next tab stop (multiples of 4). So a tab at column 0 is equivalent to 4 spaces. A tab at column 1 is equivalent to 3 spaces (to column 4). This means:

- `\t# heading` -- tab at col 0 = 4 spaces, this is indented code, NOT a heading
- ` \t# heading` -- space at col 0, tab at col 1 advances to col 4, total 4 columns = indented code
- `  \t# heading` -- 2 spaces, tab at col 2 advances to col 4, total 4 columns = indented code
- `   \t# heading` -- 3 spaces, tab at col 3 advances to col 4, total 4 columns = indented code

**Decision: treat tab as equivalent to reaching the next tabstop (multiple of 4).**

The `skip_optional_indent` helper should count columns, not bytes:

```c
/*
 * Skip up to 3 columns of leading whitespace (spaces and tabs).
 * Tabs advance to the next multiple of 4. Returns the byte offset
 * into the line, and sets *columns to the number of columns consumed.
 * Stops consuming at 3 columns (CommonMark optional indent).
 */
static size_t skip_optional_indent_ex(const char *line, size_t len,
                                       int *columns)
{
    size_t pos = 0;
    int col = 0;

    while (pos < len && col < 4) {
        if (line[pos] == ' ') {
            col++;
            pos++;
        } else if (line[pos] == '\t') {
            int next_tab = ((col / 4) + 1) * 4;
            if (next_tab > 3) {
                /* Tab would push us to 4+ columns. Don't consume it.
                 * But if col < 3, we could partially consume...
                 * Actually, tabs are atomic. We either consume the
                 * whole tab or none of it. */
                break;
            }
            col = next_tab;
            pos++;
        } else {
            break;
        }
    }

    if (col > 3)
        col = 3; /* cap at optional indent */

    if (columns)
        *columns = col;
    return pos;
}
```

Actually, this gets complicated. Let me simplify. For md4s's streaming use case (LLM output), tabs in leading whitespace are extremely rare. LLMs produce spaces, not tabs. The pragmatic approach:

**Decision: `skip_optional_indent` counts only spaces, not tabs.** A leading tab immediately disqualifies the line from optional-indent matching. This means `\t# heading` falls through to indented code or paragraph, which is usually correct (a tab = 4 spaces = indented code).

```c
static size_t skip_optional_indent(const char *line, size_t len)
{
    size_t n = 0;
    while (n < len && n < 3 && line[n] == ' ')
        n++;
    return n;
}
```

This is a known deviation from CommonMark's tab handling, documented as acceptable for the streaming use case. Full tab-stop handling can be added later if needed.

### 2.6 Where 4+ Spaces Should NOT Trigger Indented Code

Per CommonMark, 4+ leading spaces trigger indented code blocks ONLY in certain contexts. Currently md4s already handles one exception:

- **Inside lists:** `!p->in_list` guard on line 881. Correct.

Additional contexts where 4+ spaces should NOT trigger indented code (future work, not part of this spec):

- After a blockquote marker: `>     code` -- the spaces after `>` are content indent, not code indent. Currently md4s strips `> ` prefix before processing content, so the content `    code` would be processed as indented code. This is actually correct per CommonMark (indented code inside blockquotes).

No changes needed for the `in_list` guard. The existing check is correct.

### 2.7 Closing Fence with Leading Spaces

A closing fence for a fenced code block also allows 0-3 leading spaces (CommonMark spec). This is handled in section 2.4.1 above. The closing fence check is inside the `STATE_FENCED_CODE` branch of `classify_line()` and needs the same `skip_optional_indent` treatment.

### 2.8 MC/DC Test Cases

Each test verifies the independent effect of leading spaces on classification.

#### ATX Headings

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S1 | `# Heading\n` | HEADING level 1 | 0 spaces (baseline) |
| S2 | ` # Heading\n` | HEADING level 1 | 1 space |
| S3 | `  # Heading\n` | HEADING level 1 | 2 spaces |
| S4 | `   # Heading\n` | HEADING level 1 | 3 spaces (max) |
| S5 | `    # Heading\n` | INDENTED_CODE `# Heading` | 4 spaces (code) |
| S6 | `     # Heading\n` | INDENTED_CODE ` # Heading` | 5 spaces (code) |
| S7 | `   ## Sub\n` | HEADING level 2 | 3 spaces, level 2 |
| S8 | `   ###### H6\n` | HEADING level 6 | 3 spaces, level 6 |

#### Blockquotes

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S9 | `> quote\n` | BLOCKQUOTE | 0 spaces (baseline) |
| S10 | ` > quote\n` | BLOCKQUOTE | 1 space |
| S11 | `  > quote\n` | BLOCKQUOTE | 2 spaces |
| S12 | `   > quote\n` | BLOCKQUOTE | 3 spaces (max) |
| S13 | `    > quote\n` | INDENTED_CODE `> quote` | 4 spaces (code) |

#### Fenced Code Blocks (Opening)

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S14 | ` ```\ncode\n``` \n` | CODE_BLOCK | Opening fence with 0 spaces |
| S15 | ` ` ` ```\ncode\n``` \n` | CODE_BLOCK | Opening fence with 1 space |
| S16 | `  ` ` ```\ncode\n``` \n` | CODE_BLOCK | Opening fence with 2 spaces |
| S17 | `   ` ` ```\ncode\n``` \n` | CODE_BLOCK | Opening fence with 3 spaces |
| S18 | `    ` ` ```\ncode\n``` \n` | INDENTED_CODE (literal backticks) | Opening fence with 4 spaces |

(Note: The backtick escaping in the table above is for readability. Actual test inputs use literal triple backticks.)

#### Fenced Code Blocks (Closing)

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S19 | ` ```\ncode\n```\n` | CODE_BLOCK (closed) | Closing fence with 0 spaces |
| S20 | ` ```\ncode\n ```\n` | CODE_BLOCK (closed) | Closing fence with 1 space |
| S21 | ` ```\ncode\n  ```\n` | CODE_BLOCK (closed) | Closing fence with 2 spaces |
| S22 | ` ```\ncode\n   ```\n` | CODE_BLOCK (closed) | Closing fence with 3 spaces |
| S23 | ` ```\ncode\n    ```\n` | CODE_BLOCK (still open, ` ``` ` is code content) | Closing fence with 4 spaces |

#### Thematic Breaks (already working, verify)

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S24 | `---\n` | THEMATIC_BREAK | 0 spaces |
| S25 | ` ---\n` | THEMATIC_BREAK | 1 space |
| S26 | `  ---\n` | THEMATIC_BREAK | 2 spaces |
| S27 | `   ---\n` | THEMATIC_BREAK | 3 spaces |
| S28 | `    ---\n` | INDENTED_CODE `---` | 4 spaces |

#### HTML Blocks

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S29 | `<div>\n` | HTML_BLOCK | 0 spaces |
| S30 | ` <div>\n` | HTML_BLOCK | 1 space |
| S31 | `  <div>\n` | HTML_BLOCK | 2 spaces |
| S32 | `   <div>\n` | HTML_BLOCK | 3 spaces |
| S33 | `    <div>\n` | INDENTED_CODE `<div>` | 4 spaces |

#### Cross-cutting: Indented Code Block Boundaries

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S34 | `    code line 1\n    code line 2\n` | CODE_BLOCK (two lines) | Indented code continuity |
| S35 | `   not code\n` | PARAGRAPH | 3 spaces is NOT code |
| S36 | `    code\n   not code\n` | CODE_BLOCK then PARAGRAPH | Transition at 4->3 boundary |
| S37 | `- list item\n    continuation\n` | LIST_ITEM (4 spaces inside list is NOT code) | `in_list` guard |

#### Tab Edge Cases (known limitation)

| # | Input | Expected | Tests |
|---|-------|----------|-------|
| S38 | `\t# heading\n` | PARAGRAPH or INDENTED_CODE (not heading) | Tab = not optional indent |
| S39 | ` \t# heading\n` | PARAGRAPH or INDENTED_CODE | Space + tab = not optional indent |

### 2.9 Implementation Order

1. Add `skip_optional_indent()` helper function
2. Modify closing fence check (inside `STATE_FENCED_CODE`)
3. Modify fenced code opening check
4. Modify ATX heading check
5. Modify blockquote check
6. Modify HTML block start check
7. Add test cases, run full test suite

Each change is independent and can be tested individually.

---

## Appendix A: Summary of md4s.h Changes

```c
/* Add to enum md4s_event, after MD4S_IMAGE_LEAVE: */
MD4S_HTML_INLINE,

/* Add to struct md4s_detail comment block: */
/* HTML_INLINE: raw HTML text (including angle brackets). */
/* Uses text/text_length fields. */
```

## Appendix B: Summary of md4s.c Changes

### New functions (Feature 1):
- `is_tag_name_start(char ch)` -- static inline
- `is_tag_name_char(char ch)` -- static inline
- `is_attr_name_start(char ch)` -- static inline
- `is_attr_name_char(char ch)` -- static inline
- `try_parse_inline_html(const char *text, size_t length, size_t pos)` -- ~120 lines

### Modified functions (Feature 1):
- `parse_inline_depth()` -- insert inline HTML check before autolink check at the `text[pos] == '<'` case (around line 1322)

### New functions (Feature 2):
- `skip_optional_indent(const char *line, size_t len)` -- 6 lines

### Modified functions (Feature 2):
- `classify_line()` -- six modifications:
  1. Closing fence check: add `skip_optional_indent` before `count_leading` (line 768)
  2. Fence open: add `skip_optional_indent` before `line[0]` check (line 803)
  3. ATX heading: add `skip_optional_indent` before `line[0]` check (line 839)
  4. Blockquote: add `skip_optional_indent` before `line[0]` check (line 869)
  5. HTML block: add `skip_optional_indent` before `is_html_block_start` (line 891)
  6. No change to indented code, lists, thematic breaks, or paragraph

### Estimated total new/modified lines: ~200

## Appendix C: Risks and Open Questions

1. **Inline HTML inside emphasis spans:** When inline HTML contains `*` or `_` in attribute values (e.g., `<span data-x="*">bold*`), the emphasis parser may incorrectly match. This is a pre-existing issue with the emphasis parser not respecting HTML context, and is out of scope for this spec.

2. **Nested HTML tags:** md4s does not track HTML tag nesting. `<div><span></span></div>` emits four separate `MD4S_HTML_INLINE` events. This is correct -- inline HTML is opaque raw content, not a parsed DOM.

3. **Performance:** `try_parse_inline_html` is called for every `<` in inline text. Most `<` characters in LLM output are either autolinks, HTML entities (`&lt;`), or comparison operators. The function fails fast (first character check), so performance impact is minimal.

4. **Leading spaces in list continuation:** A list item's continuation lines may have leading spaces that are part of the list indent, not optional block indent. The current list handling strips indent before processing content, so `skip_optional_indent` inside `classify_line` only applies to the top-level classification, not to content within list items. This is correct.

5. **Leading spaces before setext underlines:** Already handled by `is_setext_underline()` (line 397-399). No change needed.
