# md4s vs md4c: Competitive Analysis

*Prepared by the md4s engineering team — March 2026*

md4c is a mature, battle-tested CommonMark parser (6,492 lines, MIT, by Martin Mitas).
md4s is a purpose-built streaming parser (2,225 lines, MIT) for real-time markdown rendering.
This report catalogs every feature, edge case, and robustness gap where md4c exceeds md4s.

---

## Executive Summary

md4s covers the common cases well but has significant gaps in five areas:

| Priority | Gap Area | Impact |
|----------|----------|--------|
| **P0** | **Security/robustness** — unchecked realloc, unbounded recursion, no fuzzing | Crash on adversarial input |
| **P1** | **Emphasis algorithm** — no delimiter stack, no rule-of-three, ASCII-only flanking | Wrong output on complex emphasis |
| **P2** | **Entity/character references** — 6 of 2,231 named entities, no numeric refs | Missing characters in output |
| **P2** | **Inline raw HTML** — completely absent | HTML in flowing text ignored |
| **P3** | **Configuration** — no feature flags, everything always on | Can't disable features for security |

---

## 1. Security & Robustness (P0 — Fix Before Production)

### Critical Bugs

| Bug | Location | Impact |
|-----|----------|--------|
| `realloc` return not checked | `buf_ensure` line 164 | **Crash on OOM** — old pointer lost, NULL deref |
| Unbounded `parse_inline` recursion | All emphasis/link handlers | **Stack overflow** on `*a*a*a*...` × 1000 |
| Signed integer overflow in ordered list number | line 883 | **Undefined behavior** on `99999999999999.` |
| `buf_append` size overflow | line 171 | `size_t` wrap on 32-bit → buffer overwrite |
| `buf_ensure` capacity overflow | line 163 | `new_cap *= 2` wraps to 0 → `realloc(0)` frees buffer |

### Missing Protections (md4c has these)

| Protection | md4c | md4s |
|------------|------|------|
| Code span backtick length cap | 32 (`CODESPAN_MARK_MAXLEN`) | None |
| Table column cap | 128 | 64 |
| Link paren nesting cap | 32 | None |
| Ref def output amplification cap | `min(16*size, 1MB)` | None |
| Inline nesting depth limit | N/A (iterative) | None (recursive) |
| NULL byte replacement | U+FFFD via `MD_TEXT_NULLCHAR` | Passes through, breaks `strlen` |
| Fuzzing | OSS-Fuzz continuous | Never fuzzed |

### Recommended Fixes

1. Check `realloc` return in `buf_ensure` — handle NULL gracefully
2. Add `depth` parameter to `parse_inline` — bail at depth 32
3. Cap ordered list digit parsing to 9 digits
4. Add overflow checks to `buf_append` and `buf_ensure`
5. Add a libFuzzer harness (~50 lines)
6. Replace NULL bytes with U+FFFD

---

## 2. Emphasis / Delimiter Algorithm (P1 — Architectural Gap)

The most significant correctness gap. md4c implements the full CommonMark delimiter stack algorithm. md4s uses greedy left-to-right recursive scanning.

### What md4c Has That md4s Doesn't

| Feature | Impact |
|---------|--------|
| **Rule of three** — 12 mod3-partitioned opener stacks | `*foo**bar**baz*` parsed incorrectly |
| **Delimiter run splitting** — consume N chars from M-char run | `***foo**` can't produce `<em><strong>foo</strong></em>` |
| **Opener/closer/both classification** | Interleaved emphasis breaks |
| **Unicode-aware flanking** — full UTF-8 decode + Unicode Zs/P/S tables | CJK/accented text near delimiters mishandled |
| **Proper underscore rules** — both sides checked, intra-word suppression | `foo_bar_baz` edge cases wrong |
| **Runs of 4+ delimiters** | `****text****` unhandled |

### The Core Problem

md4s processes `***` → `**` → `~~` → `*` → `_` in fixed priority order with greedy forward scanning. The CommonMark spec requires a delimiter stack that resolves pairs dynamically based on flanking status, length parity, and the rule of three. Bringing md4s to full compliance would require replacing the greedy scan with a delimiter stack — a rewrite of the inline parsing core (~300 lines).

### What Works Anyway

Simple, cleanly nested emphasis works correctly via recursion: `**bold**`, `*italic*`, `***bold italic***`, `**nested *italic* inside**`. These cover >95% of real-world LLM output.

---

## 3. Entity & Character References (P2)

| Feature | md4c | md4s |
|---------|------|------|
| Named entities | ~2,231 (structural pattern match, delegates to renderer) | **6 hardcoded** (`&amp;` `&lt;` `&gt;` `&quot;` `&apos;` `&nbsp;`) |
| Decimal numeric `&#1234;` | 1–7 digits | None |
| Hex numeric `&#x1F4A9;` | 1–6 hex digits | None |
| Entities in URLs/titles | Yes | No |
| `&nbsp;` mapping | U+00A0 (non-breaking space) | `' '` (regular space — **incorrect**) |

**Coverage: md4s handles 0.3% of named entities and 0% of numeric entities.**

### Recommended Approach

Don't hardcode 2,231 entities. Follow md4c's approach: structurally recognize `/&[A-Za-z][A-Za-z0-9]{1,47};/` and `/&#[0-9]{1,7};/` and `/&#[xX][0-9a-fA-F]{1,6};/`, then emit an `MD4S_ENTITY` event (new) with the raw entity text. Let the renderer decode. This is ~30 lines of code.

---

## 4. Inline Raw HTML (P2)

md4s has **zero inline HTML support**. md4c recognizes six types:

1. Open tags with attribute parsing (`<tag attr="val">`)
2. Closing tags (`</tag>`)
3. HTML comments (`<!-- ... -->`)
4. Processing instructions (`<? ... ?>`)
5. Declarations (`<!DOCTYPE ...>`)
6. CDATA sections (`<![CDATA[ ... ]]>`)

All emit `MD_TEXT_HTML` events for pass-through. When `<` appears in md4s inline text and isn't an autolink, it's emitted as literal text.

### Impact

Any markdown containing inline HTML (`<br>`, `<kbd>Ctrl</kbd>`, `<sup>2</sup>`, `<span style="...">`) renders the tags as visible text instead of passing them through.

---

## 5. Configuration Flags (P3)

md4c has 16 `MD_FLAG_*` bitflags:

| Flag | Purpose | md4s |
|------|---------|------|
| `TABLES` | Enable GFM tables | Always on |
| `STRIKETHROUGH` | Enable `~~` | Always on |
| `TASKLISTS` | Enable `- [x]` | Always on |
| `NOHTMLBLOCKS` | Disable raw HTML blocks | Not available |
| `NOHTMLSPANS` | Disable inline raw HTML | N/A (not implemented) |
| `PERMISSIVEURLAUTOLINKS` | Bare URL autolinks | Not implemented |
| `PERMISSIVEEMAILAUTOLINKS` | Bare email autolinks | Not implemented |
| `PERMISSIVEWWWAUTOLINKS` | `www.` autolinks | Not implemented |
| `LATEXMATHSPANS` | `$...$` and `$$...$$` | Not implemented |
| `WIKILINKS` | `[[target\|label]]` | Not implemented |
| `UNDERLINE` | `_` as underline, not emphasis | Not implemented |
| `PERMISSIVEATXHEADERS` | `#heading` without space | Not implemented |
| `NOINDENTEDCODEBLOCKS` | Disable 4-space code | Effectively always on |
| `COLLAPSEWHITESPACE` | Normalize whitespace | Not implemented |
| `HARD_SOFT_BREAKS` | All breaks are hard | Not implemented |

md4s has no configuration API. Everything it supports is always enabled.

---

## 6. Block-Level Gaps

### Leading Space Tolerance

CommonMark allows up to 3 leading spaces before most block constructs. md4c enforces this via `indent < code_indent_offset`. md4s checks `line[0]` directly for most constructs, **requiring zero leading spaces** for:
- ATX headings (`line[0] == '#'`)
- Blockquotes (`line[0] == '>'`)
- Fenced code blocks (`line[0] == '`'` or `'~'`)

### Tab Handling

md4c expands tabs to spaces using tabstop-4 alignment (`indent = (indent + 4) & ~3`). md4s **ignores tabs entirely** in indentation — `count_leading_spaces` only counts `' '`.

### Lists

| Feature | md4c | md4s |
|---------|------|------|
| Loose vs tight distinction | Yes (`MD_BLOCK_LOOSE_LIST`) | None |
| Multi-line list items | Yes (continuation at `contents_indent`) | Single-line only |
| Nesting depth | Unlimited (container stack) | 0 or 1 (`list_depth`) |
| Paragraph interruption rules | Full (ordered start≠1, empty items) | None |
| Two-blank-line termination | Yes | No |
| Sub-blocks in list items | Yes (code, quotes, sub-lists) | No |
| Empty list items | Yes (marker + newline) | No |
| Mark delimiter consistency | Yes (`.` vs `)` must match) | No |

### Block Quotes

| Feature | md4c | md4s |
|---------|------|------|
| Nesting | Unlimited (container stack) | Flat (per-line enter/leave) |
| Leading spaces before `>` | Up to 3 | None (must be column 0) |
| Multi-block content inside | Yes (headings, code, lists) | No |
| Lazy continuation | Full (with nesting awareness) | Single-level paragraph only |

### HTML Blocks

| Feature | md4c | md4s |
|---------|------|------|
| Type 1 (`<script>`, `<pre>`, `<style>`) | Ends at closing tag | Ends at blank line **(wrong)** |
| Type 3 (`<?...?>`) | Yes | No |
| Type 4 (`<!DECL>`) | Yes | No |
| Type 5 (`<![CDATA[...]]>`) | Yes | No |
| Type 7 (complete tag) | Yes, can't interrupt paragraph | No |

### Setext Headings

| Feature | md4c | md4s |
|---------|------|------|
| Multi-line content | Yes (all paragraph lines become heading) | First deferred line only |
| Ref-def body interaction | Underline degrades to paragraph | Not handled |
| Proper event semantics | Content emitted as heading | Content emitted as paragraph, then heading marker |

### Indented Code Blocks

| Feature | md4c | md4s |
|---------|------|------|
| Tab expansion | Yes (tabstop-4) | No |
| Indent offset in list items | Proper `contents_indent` tracking | Blunt `!in_list` guard |
| Leading/trailing blank stripping | Yes | No |

### Link Reference Definitions

| Feature | md4c | md4s |
|---------|------|------|
| Multi-line labels | Yes | No |
| Multi-line URLs | Yes | No |
| Title support | Yes (three delimiters, multi-line) | None |
| Label length limit (999 chars) | Yes | No |
| Efficient lookup | Hash table O(1) | Linear scan O(n) |
| Definition limit | Dynamic (unlimited) | Fixed 256 |
| Output amplification cap | `min(16*size, 1MB)` | None |

---

## 7. Inline-Level Gaps

### Links

| Feature | md4c | md4s |
|---------|------|------|
| Shortcut references `[text]` | Yes | No |
| `(...)` title delimiter | Yes | No |
| Angle-bracket URLs in inline links `[t](<url>)` | Yes | No |
| Backslash escapes in URLs/titles | Yes | No |
| Multi-line inline links | Yes | No |
| Nested link prevention | Yes | No (links can nest in links) |
| Unicode case folding in labels | Full | ASCII only |
| Whitespace collapsing in labels | Yes | No |

### Autolinks

| Feature | md4c | md4s |
|---------|------|------|
| Angle-bracket `<url>` | Full validation | Heuristic (`://` or `@`) |
| GFM extended bare URL | Yes (`http://`, `https://`, `ftp://`) | None |
| GFM extended email | Yes (`user@host`) | None |
| GFM extended `www.` | Yes | None |

### Code Spans

| Feature | md4c | md4s |
|---------|------|------|
| Multi-line code spans | Yes | No (line-at-a-time) |
| Space stripping (spec rules 2-3) | Yes | No |
| Backtick length cap | 32 | None |
| Line ending → space conversion | Yes | N/A |

### Hard Line Breaks

| Feature | md4c | md4s |
|---------|------|------|
| Trailing spaces (2+) | Yes | **None** |
| Backslash + newline | Yes | **None** |

### Strikethrough

| Feature | md4c | md4s |
|---------|------|------|
| Single `~text~` | Yes | No (double only) |
| Unicode flanking rules | Yes | None |

---

## 8. Testing & Spec Compliance

| Dimension | md4c | md4s |
|-----------|------|------|
| CommonMark spec tests (652 examples) | Passes | **Not run** |
| GFM spec tests | Passes | **Not run** |
| Fuzz testing | OSS-Fuzz continuous | **Never fuzzed** |
| Unit tests | External (spec-driven) | 250 MC/DC tests |
| Adversarial input tests | Via fuzzing | None |
| Estimated spec pass rate | ~98% | ~60-70% (estimated) |

### Recommended Testing Steps

1. **Add libFuzzer harness** — ~50 lines, immediate crash discovery
2. **Build HTML renderer + spec test driver** — run 652 CommonMark examples
3. **Add adversarial tests** — NUL bytes, CR/CRLF, huge inputs, deep nesting

---

## 9. API & Architecture Differences

### What md4s Has That md4c Doesn't

| Feature | Benefit |
|---------|---------|
| **Streaming feed API** | Process arbitrarily chunked input without buffering |
| **Partial line preview** | `PARTIAL_LINE`/`PARTIAL_CLEAR` for real-time display |
| **Cancellation** | `md4s_cancel()` returns raw text without closing |
| **Raw text accumulation** | `md4s_finalize()` returns complete markdown |
| **3x less code** | 2,225 vs 6,492 lines — easier to audit |
| **Simpler API** | 5 functions, 1 callback, 1 detail struct |

### What md4c Has That md4s Doesn't

| Feature | Impact |
|---------|--------|
| **Callback abort** | Callbacks return `int`; non-zero aborts parsing |
| **Debug logging** | `debug_log` callback for diagnostics |
| **HTML output layer** | `md4c-html.h` — batteries-included HTML renderer |
| **C89 portability** | Compiles on older MSVC, embedded |
| **Memory efficiency** | Borrows input (no copy); md4s copies 100% |
| **Custom allocator** | Not in md4c either, but smaller working set |

---

## 10. Prioritized Roadmap

### Phase 1: Security Hardening (1-2 days)
- [ ] Check `realloc` return in `buf_ensure`
- [ ] Add recursion depth limit to `parse_inline`
- [ ] Cap ordered list digit parsing
- [ ] Add `buf_append`/`buf_ensure` overflow checks
- [ ] Add libFuzzer harness
- [ ] Handle NUL bytes (replace with U+FFFD)

### Phase 2: High-Impact Correctness (3-5 days)
- [ ] Entity/character references — structural recognition + new event type
- [ ] Hard line breaks (trailing spaces + backslash)
- [ ] Inline raw HTML (at minimum: open/close tags, comments)
- [ ] Leading space tolerance (0-3 spaces before block constructs)
- [ ] Tab expansion in indentation
- [ ] Shortcut reference links `[text]`
- [ ] Code span space stripping

### Phase 3: Spec Compliance (5-10 days)
- [ ] Build HTML renderer for spec testing
- [ ] Run CommonMark 652-example spec suite
- [ ] Delimiter stack algorithm (replace greedy scan)
- [ ] Unicode-aware flanking (UTF-8 decode + Unicode tables)
- [ ] Multi-line constructs (code spans, link defs, setext)
- [ ] Proper list model (loose/tight, continuation, nesting)
- [ ] HTML block types 1, 3, 4, 5, 7

### Phase 4: Extensions & Polish (ongoing)
- [ ] Configuration flags API
- [ ] GFM extended autolinks (bare URLs)
- [ ] LaTeX math spans
- [ ] Wiki links
- [ ] Single-tilde strikethrough
- [ ] Callback abort support
- [ ] Bare `\r` line ending support

---

## Conclusion

md4s and md4c serve fundamentally different use cases. md4c is a spec-compliant batch parser for general-purpose markdown rendering. md4s is a streaming parser optimized for real-time display of LLM output. md4s's streaming capability cannot be replicated with md4c without buffering the entire document.

For md4s's target audience (AI CLI tools), the security hardening (Phase 1) and high-impact correctness fixes (Phase 2) would close the most visible gaps. Full CommonMark compliance (Phase 3) would make md4s competitive for general-purpose use. The streaming architecture is a genuine differentiator that no other C markdown parser offers.
