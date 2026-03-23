# Delimiter Stack Implementation Specification for md4s

## Replacing the Greedy Emphasis Scanner with CommonMark-Compliant Delimiter Resolution

### Scope

This specification covers the replacement of `parse_inline_depth()` in md4s.c. The current implementation uses a greedy, recursive, left-to-right scanner that handles emphasis in three hardcoded tiers: triple (`***`/`___`), double (`**`/`__`/`~~`), then single (`*`/`_`). It fails on inputs requiring the CommonMark process_emphasis algorithm.

---

## 1. Data Structures

### 1a. Mark Entry

```c
struct md4s_mark {
    size_t pos;       /* Byte offset in text where this run starts.            */
    size_t len;       /* Current (remaining) run length in characters.         */
    size_t orig_len;  /* Original run length before any splitting.             */
    char ch;          /* Delimiter character: '*', '_', or '~'.                */
    uint8_t flags;    /* Bitfield: MARK_OPENER | MARK_CLOSER | MARK_RESOLVED. */
    uint8_t mod3;     /* orig_len % 3, cached for rule-of-three checks.       */
};

#define MARK_OPENER   0x01
#define MARK_CLOSER   0x02
#define MARK_RESOLVED 0x04
#define MARK_BOTH     (MARK_OPENER | MARK_CLOSER)
```

**Why `orig_len` and `len` are separate:** When a run of 3 `*` opens against a closer of 2, the opener splits: 2 characters become a resolved bold opener, 1 character remains. The `mod3` field records `orig_len % 3` at creation time for rule-of-three checks.

### 1b. Mark Array

```c
#define MARK_INITIAL_CAP  32
#define MARK_MAX_CAP      512

struct md4s_mark_array {
    struct md4s_mark *marks;
    int count;
    int cap;
};
```

Pre-allocate 32 (covers typical inline content). Double to max 512. Beyond that, remaining delimiters become literal text.

Storage: heap-allocated, local to `parse_inline_delim()`. Freed on return — no parser struct changes.

### 1c. Match Entry

```c
struct md4s_emph_match {
    size_t open_pos;   /* Position of opening delimiter (inner edge). */
    size_t close_pos;  /* Position of closing delimiter.              */
    uint8_t len;       /* 1 = italic, 2 = bold.                       */
    char ch;           /* '*', '_', or '~'.                            */
};
```

---

## 2. Collection Phase

### 2a. Scanning

Walk the text left-to-right. Handle highest-priority constructs eagerly; record delimiter marks for deferred resolution:

1. Backslash escape → record as segment
2. HTML entity → record as segment
3. Backtick code span → record as segment (code spans have highest priority)
4. `<` autolink → record as segment
5. `[` or `![` link/image → record as segment (link text recursed later)
6. `*`, `_`, `~` → measure run, classify flanking, record mark
7. Anything else → plain text

### 2b. Flanking Classification

Three-level character classification using gstr:

```c
static int char_class(const char *text, size_t len, size_t pos, bool before)
{
    /* 0 = whitespace/boundary, 1 = punctuation, 2 = other */
    if (before && pos == 0) return 0;
    if (!before && pos >= len) return 0;
    uint32_t cp;
    if (before) {
        size_t prev = utf8_prev(text, len, pos);
        utf8_decode(text + prev, pos - prev, &cp);
    } else {
        utf8_decode(text + pos, len - pos, &cp);
    }
    if (cp == 0) return 0;
    if (gstr_is_whitespace_cp(cp)) return 0;
    if (gstr_is_unicode_punctuation(cp)) return 1;
    return 2;
}
```

Flanking rules per CommonMark 0.31.2:

```c
int before = char_class(text, length, run_start, true);
int after  = char_class(text, length, run_start + run_len, false);

bool left_flanking  = (after != 0) && (after != 1 || before <= 1);
bool right_flanking = (before != 0) && (before != 1 || after <= 1);
```

### 2c. Opener/Closer Flags

For `*`:
- Opener if left-flanking
- Closer if right-flanking

For `_` (additional restrictions):
- Opener if left-flanking AND (not right-flanking OR preceded by punctuation)
- Closer if right-flanking AND (not left-flanking OR followed by punctuation)

For `~` (only runs of exactly 2):
- Same as `*`

If flags == 0, emit as literal text — do not record a mark.

---

## 3. Resolution Phase (process_emphasis)

### 3a. Algorithm

```
for current_idx = 0 to count-1:
    mark = marks[current_idx]
    if mark has no CLOSER flag or is resolved, skip

    search backward for compatible opener:
        - same character
        - has OPENER flag
        - not resolved
        - passes rule-of-three check

    if found:
        use_len = (min(opener.len, closer.len) >= 2) ? 2 : 1
        record match(opener, closer, use_len)
        update lengths, mark resolved if consumed
        deactivate all unresolved marks between opener and closer
    else:
        update opener_bottom for this category
```

### 3b. Rule of Three

When either opener or closer has MARK_BOTH (can function as both opener and closer):

```c
if ((opener.flags & MARK_BOTH) == MARK_BOTH ||
    (closer.flags & MARK_BOTH) == MARK_BOTH) {
    if ((opener.orig_len + closer.orig_len) % 3 == 0 &&
        opener.orig_len % 3 != 0 && closer.orig_len % 3 != 0) {
        // Rule of three BLOCKS this match — skip this opener
    }
}
```

### 3c. Opener Bottom Arrays

13 slots: 2 characters (`*`, `_`) x 2 closer types (closer-only vs both) x 3 mod3 values, plus 1 for `~`:

```c
#define OPENER_BOTTOM_COUNT 13
int opener_bottom[OPENER_BOTTOM_COUNT];
// Initialize all to -1

static int opener_bottom_idx(char ch, uint8_t flags, uint8_t mod3) {
    if (ch == '~') return 12;
    int ch_idx = (ch == '*') ? 0 : 1;
    int both = (flags & MARK_BOTH) == MARK_BOTH ? 1 : 0;
    return ch_idx * 6 + both * 3 + mod3;
}
```

Required for O(n) amortized complexity.

### 3d. Delimiter Splitting

When opener has more characters than needed:

```c
// Opener gives rightmost use_len characters
size_t open_pos = opener->pos + opener->len - use_len;
opener->len -= use_len;

// Closer gives leftmost use_len characters
size_t close_pos = closer->pos;
closer->pos += use_len;
closer->len -= use_len;

matches[count++] = {open_pos, close_pos, use_len, ch};
```

### 3e. Deactivation

After matching opener at index `oi` with closer at index `ci`, deactivate all unresolved marks between them:

```c
for (int k = oi + 1; k < ci; k++) {
    if (!(marks[k].flags & MARK_RESOLVED))
        marks[k].flags |= MARK_RESOLVED;
}
```

---

## 4. Emission Phase

### 4a. Architecture (Hybrid Eager/Deferred)

Build a sorted event list from matches, then walk left-to-right emitting everything:

```c
struct pos_event {
    size_t pos;
    uint8_t skip;          /* Bytes to skip (delimiter length). */
    enum md4s_event event;
    int order;             /* 0=LEAVE (first), 1=ENTER (second). */
};
```

For each match, produce two events:
- `{open_pos, len, BOLD/ITALIC/STRIKE_ENTER, order=1}`
- `{close_pos, len, BOLD/ITALIC/STRIKE_LEAVE, order=0}`

Sort by `(pos ASC, order ASC, skip DESC for enters, skip ASC for leaves)`.

Walk text left-to-right. At each event position, flush pending text and emit the event.

### 4b. Non-Delimiter Segments

Code spans, escapes, entities, links, images, and autolinks are handled through a segment array recorded during collection. During emission, these segments are interleaved with the sorted delimiter events.

For links/images, the link text is recursively parsed via `parse_inline_delim(parser, link_text, link_text_len, depth + 1)`.

---

## 5. Integration

### 5a. Keep Unchanged
- `find_backtick_close` — code span matching
- Backslash escape handling
- Entity handling
- Link/image parsing (with recursive `parse_inline_delim` for text)
- Autolink parsing
- `emit`, `emit_simple`, `emit_text` helpers

### 5b. Delete
- `find_double_close` (~30 lines)
- `find_single_close` (~43 lines)
- `is_word_boundary` (~26 lines) — replaced by `char_class`
- `parse_inline_depth` (~452 lines) — replaced by `parse_inline_delim`

### 5c. Add
- `char_class` (~25 lines)
- Mark/match/event structures (~40 lines)
- Mark array management (~30 lines)
- Flanking classification + mark recording (~50 lines)
- `process_emphasis` (~80 lines)
- `parse_inline_delim` collection phase (~150 lines)
- `parse_inline_delim` emission phase (~60 lines)

**Net: ~551 lines deleted, ~438 lines added. Net reduction of ~113 lines.**

### 5d. Recursion

Recursion eliminated for emphasis content. Remains only for link/image text (link brackets delimit emphasis scope). `MAX_INLINE_DEPTH` still applies for link nesting only.

---

## 6. Strikethrough

- `~~` uses the same delimiter stack, opener-bottom slot 12
- Only runs of exactly 2 are valid (runs of 1 or 3+ are literal)
- No rule-of-three check for `~`
- `use_len` always 2
- Single-tilde `~` NOT added in this change

---

## 7. Performance

- **Collection:** O(n)
- **Resolution:** O(n) amortized (opener-bottom prevents rescanning)
- **Emission:** O(n + m log m) where m = matches
- **Memory:** ~4KB typical, ~75KB worst case (all freed per-call)

---

## 8. Test Plan

### 8a. Rule-of-Three Tests

| Test | Input | Expected | Condition |
|------|-------|----------|-----------|
| `rule3_blocks` | `*foo**bar*` | `<em>foo**bar</em>` | 1+2=3, both are "both" type → blocked |
| `rule3_allows_not_3` | `**foo**` | `<strong>foo</strong>` | 2+2=4, not multiple of 3 |
| `rule3_allows_both_mod3_0` | `***foo***` | `<em><strong>foo</strong></em>` | 3+3=6, but both are multiples of 3 → allowed |
| `rule3_opener_only` | `**foo** **bar**` | `<strong>foo</strong> <strong>bar</strong>` | Opener-only, closer-only → no rule-of-three |
| `rule3_inner_outer` | `*foo**bar**baz*` | `<em>foo<strong>bar</strong>baz</em>` | Inner `**` match, outer `*` match |

### 8b. Delimiter Splitting Tests

| Test | Input | Expected |
|------|-------|----------|
| `split_3_2` | `***foo**` | `*<strong>foo</strong>` |
| `split_2_3` | `**foo***` | `<strong>foo</strong>*` |
| `split_4_4` | `****foo****` | `<strong><strong>foo</strong></strong>` |
| `split_5_5` | `*****foo*****` | `<strong><strong><em>foo</em></strong></strong>` |

### 8c. Flanking Classification Tests

| Test | Input | Before | After | Flags |
|------|-------|--------|-------|-------|
| `star_opener_start` | `*foo` | boundary(0) | other(2) | OPENER |
| `star_closer_end` | `foo*` | other(2) | boundary(0) | CLOSER |
| `star_both_mid` | `foo*bar` | other(2) | other(2) | BOTH |
| `under_blocked_intra` | `foo_bar` | other(2) | other(2) | NONE (intra-word) |
| `under_open_after_punct` | `._foo_` | punct(1) | other(2) | OPENER |

### 8d. Interleaved Emphasis Tests

| Test | Input | Expected |
|------|-------|----------|
| `interleave_star_under` | `*foo _bar* baz_` | `<em>foo _bar</em> baz_` |
| `interleave_nested` | `*foo _bar_ baz*` | `<em>foo <em>bar</em> baz</em>` |

### 8e. 4+ Character Run Tests

| Test | Input | Expected |
|------|-------|----------|
| `run4_star` | `****foo*` | `***<em>foo</em>` |
| `run4_both` | `****foo****` | `<strong><strong>foo</strong></strong>` |

### 8f. Regression Verification

All 264 existing md4s tests must pass. The ~30 emphasis-related tests should produce identical results for basic cases. New tests added BEFORE the switch to establish baseline failures with the greedy scanner.

---

## 9. Migration

### Big-bang replacement in a single commit:

1. Add `parse_inline_delim`, `char_class`, `process_emphasis`, structures
2. Switch `parse_inline` to call `parse_inline_delim`
3. Delete `find_double_close`, `find_single_close`, `is_word_boundary`, `parse_inline_depth`

### Verification:
1. Run all 264 existing tests
2. Run new MC/DC tests (~40 cases)
3. Run CommonMark spec examples 350-480
4. Fuzz test with random delimiter combinations

---

## 10. Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Mark array | Heap, local to function | No parser struct changes; freed on return |
| Opener-bottom | 13 slots (12 emphasis + 1 strikethrough) | Required for O(n) amortized |
| Match recording | Separate array | Cleaner than inline mark splitting |
| Emission | Sorted event list from matches | Handles all ordering edge cases |
| Link handling | Recursive call | Matches CommonMark scoping |
| Single `~` | Not added | Separate feature |
| Migration | Big-bang | No meaningful intermediate state |
