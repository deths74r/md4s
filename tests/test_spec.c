/*
 * CommonMark spec test suite for md4s.
 *
 * Embeds ~50 representative spec examples from CommonMark 0.31.2
 * and compares md4s HTML output against the expected result.
 *
 * Build:
 *   cc -std=c11 -Wall -I. -g3 -O0 -o tests/test_spec tests/test_spec.c md4s.c
 *
 * Run:
 *   ./tests/test_spec          # run all examples
 *   ./tests/test_spec -v       # verbose: show all pass/fail details
 */
#include "md4s_html.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* Spec example structure                                              */
/* ================================================================== */

struct spec_example {
	int number;
	const char *section;
	const char *markdown;
	const char *expected_html;
};

/* ================================================================== */
/* Embedded CommonMark 0.31.2 spec examples                            */
/* ================================================================== */

static const struct spec_example examples[] = {

/* ------------------------------------------------------------------ */
/* Thematic breaks (examples 43-61)                                    */
/* ------------------------------------------------------------------ */

{ 43, "Thematic breaks",
  "***\n---\n___\n",
  "<hr />\n<hr />\n<hr />\n" },

{ 44, "Thematic breaks",
  "+++\n",
  "<p>+++</p>\n" },

{ 45, "Thematic breaks",
  "===\n",
  "<p>===</p>\n" },

{ 46, "Thematic breaks",
  "--\n**\n__\n",
  "<p>--\n**\n__</p>\n" },

{ 47, "Thematic breaks",
  " ***\n  ***\n   ***\n",
  "<hr />\n<hr />\n<hr />\n" },

{ 48, "Thematic breaks",
  "    ***\n",
  "<pre><code>***\n</code></pre>\n" },

{ 50, "Thematic breaks",
  "_____________________________________\n",
  "<hr />\n" },

{ 51, "Thematic breaks",
  " - - -\n",
  "<hr />\n" },

{ 52, "Thematic breaks",
  " **  * ** * ** * **\n",
  "<hr />\n" },

{ 53, "Thematic breaks",
  "-     -      -      -\n",
  "<hr />\n" },

{ 54, "Thematic breaks",
  "- - - -    \n",
  "<hr />\n" },

{ 55, "Thematic breaks",
  "_ _ _ _ a\n\na------\n\n---a---\n",
  "<p>_ _ _ _ a</p>\n<p>a------</p>\n<p>---a---</p>\n" },

{ 56, "Thematic breaks",
  " *-*\n",
  "<p><em>-</em></p>\n" },

/* ------------------------------------------------------------------ */
/* ATX headings (examples 62-78)                                       */
/* ------------------------------------------------------------------ */

{ 62, "ATX headings",
  "# foo\n## foo\n### foo\n#### foo\n##### foo\n###### foo\n",
  "<h1>foo</h1>\n<h2>foo</h2>\n<h3>foo</h3>\n<h4>foo</h4>\n<h5>foo</h5>\n<h6>foo</h6>\n" },

{ 63, "ATX headings",
  "####### foo\n",
  "<p>####### foo</p>\n" },

{ 64, "ATX headings",
  "#5 bolt\n\n#hashtag\n",
  "<p>#5 bolt</p>\n<p>#hashtag</p>\n" },

{ 65, "ATX headings",
  "\\## foo\n",
  "<p>## foo</p>\n" },

{ 66, "ATX headings",
  "# foo *bar* \\*baz\\*\n",
  "<h1>foo <em>bar</em> *baz*</h1>\n" },

{ 67, "ATX headings",
  "#                  foo                     \n",
  "<h1>foo</h1>\n" },

{ 68, "ATX headings",
  " ### foo\n  ## foo\n   # foo\n",
  "<h3>foo</h3>\n<h2>foo</h2>\n<h1>foo</h1>\n" },

{ 69, "ATX headings",
  "    # foo\n",
  "<pre><code># foo\n</code></pre>\n" },

{ 70, "ATX headings",
  "foo\n    # bar\n",
  "<p>foo\n# bar</p>\n" },

{ 71, "ATX headings",
  "## foo ##\n  ###   bar    ###\n",
  "<h2>foo</h2>\n<h3>bar</h3>\n" },

{ 72, "ATX headings",
  "# foo ##################################\n##### foo ##\n",
  "<h1>foo</h1>\n<h5>foo</h5>\n" },

{ 73, "ATX headings",
  "### foo ###     \n",
  "<h3>foo</h3>\n" },

{ 74, "ATX headings",
  "### foo ### b\n",
  "<h3>foo ### b</h3>\n" },

{ 75, "ATX headings",
  "# foo#\n",
  "<h1>foo#</h1>\n" },

{ 76, "ATX headings",
  "### foo \\###\n## foo #\\##\n# foo \\#\n",
  "<h3>foo ###</h3>\n<h2>foo ###</h2>\n<h1>foo #</h1>\n" },

{ 77, "ATX headings",
  "****\n## foo\n****\n",
  "<hr />\n<h2>foo</h2>\n<hr />\n" },

{ 78, "ATX headings",
  "Foo bar\n# baz\nBar foo\n",
  "<p>Foo bar</p>\n<h1>baz</h1>\n<p>Bar foo</p>\n" },

/* ------------------------------------------------------------------ */
/* Setext headings (examples 80-102)                                   */
/* ------------------------------------------------------------------ */

{ 80, "Setext headings",
  "Foo *bar*\n=========\n\nFoo *bar*\n---------\n",
  "<h1>Foo <em>bar</em></h1>\n<h2>Foo <em>bar</em></h2>\n" },

{ 81, "Setext headings",
  "Foo *bar\nbaz*\n====\n",
  "<h1>Foo <em>bar\nbaz</em></h1>\n" },

{ 83, "Setext headings",
  "  Foo *bar\nbaz*\t\n====\n",
  "<h1>Foo <em>bar\nbaz</em></h1>\n" },

{ 86, "Setext headings",
  "Foo\n-------------------------\nFoo\n=\n",
  "<h2>Foo</h2>\n<h1>Foo</h1>\n" },

{ 87, "Setext headings",
  "   Foo\n---\n\n  Foo\n-----\n\n  Foo\n  ===\n",
  "<h2>Foo</h2>\n<h2>Foo</h2>\n<h1>Foo</h1>\n" },

{ 88, "Setext headings",
  "    Foo\n    ---\n\n    Foo\n---\n",
  "<pre><code>Foo\n---\n\nFoo\n</code></pre>\n<hr />\n" },

{ 93, "Setext headings",
  "---\nFoo\n---\nBar\n---\nBaz\n",
  "<hr />\n<h2>Foo</h2>\n<h2>Bar</h2>\n<p>Baz</p>\n" },

/* ------------------------------------------------------------------ */
/* Indented code blocks (examples 107-116)                             */
/* ------------------------------------------------------------------ */

{ 107, "Indented code blocks",
  "    a simple\n      indented code block\n",
  "<pre><code>a simple\n  indented code block\n</code></pre>\n" },

{ 110, "Indented code blocks",
  "    <a/>\n    *hi*\n\n    - one\n",
  "<pre><code>&lt;a/&gt;\n*hi*\n\n- one\n</code></pre>\n" },

{ 111, "Indented code blocks",
  "    chunk1\n\n    chunk2\n  \n \n \n    chunk3\n",
  "<pre><code>chunk1\n\nchunk2\n\n\n\nchunk3\n</code></pre>\n" },

{ 112, "Indented code blocks",
  "    chunk1\n      \n      chunk2\n",
  "<pre><code>chunk1\n  \n  chunk2\n</code></pre>\n" },

/* ------------------------------------------------------------------ */
/* Fenced code blocks (examples 119-148)                               */
/* ------------------------------------------------------------------ */

{ 119, "Fenced code blocks",
  "```\n<\n >\n```\n",
  "<pre><code>&lt;\n &gt;\n</code></pre>\n" },

{ 120, "Fenced code blocks",
  "~~~\n<\n >\n~~~\n",
  "<pre><code>&lt;\n &gt;\n</code></pre>\n" },

{ 122, "Fenced code blocks",
  "```\naaa\n~~~\n```\n",
  "<pre><code>aaa\n~~~\n</code></pre>\n" },

{ 123, "Fenced code blocks",
  "~~~\naaa\n```\n~~~\n",
  "<pre><code>aaa\n```\n</code></pre>\n" },

{ 124, "Fenced code blocks",
  "````\naaa\n```\n``````\n",
  "<pre><code>aaa\n```\n</code></pre>\n" },

{ 126, "Fenced code blocks",
  "```\n\n  \n```\n",
  "<pre><code>\n  \n</code></pre>\n" },

{ 127, "Fenced code blocks",
  "```\n```\n",
  "<pre><code></code></pre>\n" },

{ 139, "Fenced code blocks",
  "```ruby\ndef foo(x)\n  return 3\nend\n```\n",
  "<pre><code class=\"language-ruby\">def foo(x)\n  return 3\nend\n</code></pre>\n" },

{ 140, "Fenced code blocks",
  "~~~~    ruby startance\ndef foo(x)\n  return 3\nend\n~~~~~~~\n",
  "<pre><code class=\"language-ruby\">def foo(x)\n  return 3\nend\n</code></pre>\n" },

{ 141, "Fenced code blocks",
  "````;\n````\n",
  "<pre><code class=\"language-;\"></code></pre>\n" },

{ 143, "Fenced code blocks",
  "``` aa ```\nfoo\n",
  "<p><code>aa</code>\nfoo</p>\n" },

/* ------------------------------------------------------------------ */
/* Paragraphs (examples 219-227)                                       */
/* ------------------------------------------------------------------ */

{ 219, "Paragraphs",
  "aaa\n\nbbb\n",
  "<p>aaa</p>\n<p>bbb</p>\n" },

{ 220, "Paragraphs",
  "aaa\nbbb\n\nccc\nddd\n",
  "<p>aaa\nbbb</p>\n<p>ccc\nddd</p>\n" },

{ 221, "Paragraphs",
  "aaa\n\n\nbbb\n",
  "<p>aaa</p>\n<p>bbb</p>\n" },

{ 222, "Paragraphs",
  "  aaa\n bbb\n",
  "<p>aaa\nbbb</p>\n" },

{ 223, "Paragraphs",
  "aaa\n             bbb\n                       ccc\n",
  "<p>aaa\nbbb\nccc</p>\n" },

{ 224, "Paragraphs",
  "   aaa\nbbb\n",
  "<p>aaa\nbbb</p>\n" },

{ 225, "Paragraphs",
  "    aaa\nbbb\n",
  "<pre><code>aaa\n</code></pre>\n<p>bbb</p>\n" },

/* ------------------------------------------------------------------ */
/* Block quotes (examples 228-252)                                     */
/* ------------------------------------------------------------------ */

{ 228, "Block quotes",
  "> # Foo\n> bar\n> baz\n",
  "<blockquote>\n<h1>Foo</h1>\n<p>bar\nbaz</p>\n</blockquote>\n" },

{ 229, "Block quotes",
  "># Foo\n>bar\n> baz\n",
  "<blockquote>\n<h1>Foo</h1>\n<p>bar\nbaz</p>\n</blockquote>\n" },

{ 230, "Block quotes",
  "   > # Foo\n   > bar\n > baz\n",
  "<blockquote>\n<h1>Foo</h1>\n<p>bar\nbaz</p>\n</blockquote>\n" },

{ 231, "Block quotes",
  "    > # Foo\n    > bar\n    > baz\n",
  "<pre><code>&gt; # Foo\n&gt; bar\n&gt; baz\n</code></pre>\n" },

{ 232, "Block quotes",
  "> # Foo\n> bar\nbaz\n",
  "<blockquote>\n<h1>Foo</h1>\n<p>bar\nbaz</p>\n</blockquote>\n" },

{ 233, "Block quotes",
  "> bar\nbaz\n> foo\n",
  "<blockquote>\n<p>bar\nbaz\nfoo</p>\n</blockquote>\n" },

{ 236, "Block quotes",
  "> foo\n\n> bar\n",
  "<blockquote>\n<p>foo</p>\n</blockquote>\n<blockquote>\n<p>bar</p>\n</blockquote>\n" },

{ 237, "Block quotes",
  "> foo\n> bar\n",
  "<blockquote>\n<p>foo\nbar</p>\n</blockquote>\n" },

{ 241, "Block quotes",
  ">\n",
  "<blockquote>\n</blockquote>\n" },

{ 242, "Block quotes",
  ">\n>  \n> \n",
  "<blockquote>\n</blockquote>\n" },

/* ------------------------------------------------------------------ */
/* Lists (examples 264-311) — selected                                 */
/* ------------------------------------------------------------------ */

{ 264, "Lists",
  "A list item.\n\n    indented code\n\n> A block quote.\n",
  "<p>A list item.</p>\n<pre><code>indented code\n</code></pre>\n<blockquote>\n<p>A block quote.</p>\n</blockquote>\n" },

{ 281, "Lists",
  "- one\n\n two\n",
  "<ul>\n<li>one</li>\n</ul>\n<p>two</p>\n" },

{ 283, "Lists",
  "- a\n- b\n\n  c\n- d\n",
  "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n<p>c</p>\n</li>\n<li>\n<p>d</p>\n</li>\n</ul>\n" },

{ 301, "Lists",
  "- foo\n- bar\n+ baz\n",
  "<ul>\n<li>foo</li>\n<li>bar</li>\n</ul>\n<ul>\n<li>baz</li>\n</ul>\n" },

{ 302, "Lists",
  "1. foo\n2. bar\n",
  "<ol>\n<li>foo</li>\n<li>bar</li>\n</ol>\n" },

{ 304, "Lists",
  "- foo\n\n- bar\n\n\n- baz\n",
  "<ul>\n<li>\n<p>foo</p>\n</li>\n<li>\n<p>bar</p>\n</li>\n<li>\n<p>baz</p>\n</li>\n</ul>\n" },

{ 305, "Lists",
  "- foo\n  - bar\n    - baz\n\n\n      bim\n",
  "<ul>\n<li>foo\n<ul>\n<li>bar\n<ul>\n<li>\n<p>baz</p>\n<p>bim</p>\n</li>\n</ul>\n</li>\n</ul>\n</li>\n</ul>\n" },

/* ------------------------------------------------------------------ */
/* Code spans (examples 328-349)                                       */
/* ------------------------------------------------------------------ */

{ 328, "Code spans",
  "`foo`\n",
  "<p><code>foo</code></p>\n" },

{ 329, "Code spans",
  "`` foo ` bar ``\n",
  "<p><code>foo ` bar</code></p>\n" },

{ 330, "Code spans",
  "` `` `\n",
  "<p><code>``</code></p>\n" },

{ 331, "Code spans",
  "`  ``  `\n",
  "<p><code> `` </code></p>\n" },

{ 332, "Code spans",
  "` a`\n",
  "<p><code> a</code></p>\n" },

{ 334, "Code spans",
  "` b `\n",
  "<p><code>b</code></p>\n" },

{ 335, "Code spans",
  "` `\n` `\n",
  "<p><code> </code>\n<code> </code></p>\n" },

{ 349, "Code spans",
  "`foo   bar \nbaz`\n",
  "<p><code>foo   bar  baz</code></p>\n" },

/* ------------------------------------------------------------------ */
/* Emphasis (examples 350-480) — selected representative               */
/* ------------------------------------------------------------------ */

{ 350, "Emphasis and strong emphasis",
  "*foo bar*\n",
  "<p><em>foo bar</em></p>\n" },

{ 360, "Emphasis and strong emphasis",
  "_foo bar_\n",
  "<p><em>foo bar</em></p>\n" },

{ 369, "Emphasis and strong emphasis",
  "**foo bar**\n",
  "<p><strong>foo bar</strong></p>\n" },

{ 381, "Emphasis and strong emphasis",
  "__foo bar__\n",
  "<p><strong>foo bar</strong></p>\n" },

{ 393, "Emphasis and strong emphasis",
  "*foo**bar**baz*\n",
  "<p><em>foo<strong>bar</strong>baz</em></p>\n" },

{ 403, "Emphasis and strong emphasis",
  "***foo***\n",
  "<p><em><strong>foo</strong></em></p>\n" },

{ 412, "Emphasis and strong emphasis",
  "*foo **bar** baz*\n",
  "<p><em>foo <strong>bar</strong> baz</em></p>\n" },

{ 417, "Emphasis and strong emphasis",
  "*foo**bar***\n",
  "<p><em>foo<strong>bar</strong></em></p>\n" },

/* ------------------------------------------------------------------ */
/* Links (examples 481-530) — selected                                 */
/* ------------------------------------------------------------------ */

{ 481, "Links",
  "[link](/uri)\n",
  "<p><a href=\"/uri\">link</a></p>\n" },

{ 482, "Links",
  "[link]()\n",
  "<p><a href=\"\">link</a></p>\n" },

{ 483, "Links",
  "[]()\n",
  "<p><a href=\"\"></a></p>\n" },

{ 485, "Links",
  "[link](/url \"title\")\n",
  "<p><a href=\"/url\" title=\"title\">link</a></p>\n" },

{ 486, "Links",
  "[link](/url 'title')\n",
  "<p><a href=\"/url\" title=\"title\">link</a></p>\n" },

{ 487, "Links",
  "[link](/url (title))\n",
  "<p><a href=\"/url\" title=\"title\">link</a></p>\n" },

{ 492, "Links",
  "[link](foo%20b&auml;)\n",
  "<p><a href=\"foo%20b&auml;\">link</a></p>\n" },

{ 497, "Links",
  "[a](<b)c>)\n",
  "<p><a href=\"b)c\">a</a></p>\n" },

{ 504, "Links",
  "[a](url)\n",
  "<p><a href=\"url\">a</a></p>\n" },

{ 506, "Links",
  "[a](b \"c\")\n",
  "<p><a href=\"b\" title=\"c\">a</a></p>\n" },

/* ------------------------------------------------------------------ */
/* Images (examples 571-588) — selected                                */
/* ------------------------------------------------------------------ */

{ 571, "Images",
  "![foo](/url \"title\")\n",
  "<p><img src=\"/url\" alt=\"foo\" title=\"title\" /></p>\n" },

{ 572, "Images",
  "![foo ![bar](/url)](/url2)\n",
  "<p><img src=\"/url2\" alt=\"foo bar\" /></p>\n" },

{ 575, "Images",
  "![foo](/url)\n",
  "<p><img src=\"/url\" alt=\"foo\" /></p>\n" },

{ 580, "Images",
  "![](/url)\n",
  "<p><img src=\"/url\" alt=\"\" /></p>\n" },

/* ------------------------------------------------------------------ */
/* HTML blocks (examples 148-160) — selected                           */
/* ------------------------------------------------------------------ */

{ 148, "HTML blocks",
  "<table><tr><td>\n<pre>\n**Hello**,\n\n_world_.\n</pre>\n</td></tr></table>\n",
  "<table><tr><td>\n<pre>\n**Hello**,\n<p><em>world</em>.\n</pre></p>\n</td></tr></table>\n" },

{ 153, "HTML blocks",
  "<DIV CLASS=\"foo\">\n\n*Markdown*\n\n</DIV>\n",
  "<DIV CLASS=\"foo\">\n<p><em>Markdown</em></p>\n</DIV>\n" },

};

static const int num_examples = sizeof(examples) / sizeof(examples[0]);

/* ================================================================== */
/* Helper: escape control characters for display                       */
/* ================================================================== */

static void print_escaped(const char *s)
{
	if (!s) {
		printf("(null)");
		return;
	}
	for (; *s; s++) {
		switch (*s) {
		case '\n': printf("\\n"); break;
		case '\t': printf("\\t"); break;
		default:   putchar(*s);   break;
		}
	}
}

/* ================================================================== */
/* Main test driver                                                    */
/* ================================================================== */

int main(int argc, char **argv)
{
	bool verbose = false;
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		verbose = true;

	int passed = 0, failed = 0;
	const char *last_section = "";

	printf("CommonMark spec tests for md4s\n");
	printf("==============================\n\n");

	for (int i = 0; i < num_examples; i++) {
		const struct spec_example *ex = &examples[i];

		/* Print section header when it changes. */
		if (strcmp(ex->section, last_section) != 0) {
			printf("  %s:\n", ex->section);
			last_section = ex->section;
		}

		/* Feed markdown through md4s HTML renderer. */
		char *got = md4s_to_html(ex->markdown, strlen(ex->markdown));
		if (!got) {
			printf("    example %-4d  FAIL (renderer returned NULL)\n",
			       ex->number);
			failed++;
			continue;
		}

		bool pass = (strcmp(got, ex->expected_html) == 0);

		if (pass) {
			passed++;
			if (verbose)
				printf("    example %-4d  ok\n", ex->number);
		} else {
			failed++;
			printf("    example %-4d  FAIL\n", ex->number);
			printf("      markdown: ");
			print_escaped(ex->markdown);
			printf("\n");
			printf("      expected: ");
			print_escaped(ex->expected_html);
			printf("\n");
			printf("      got:      ");
			print_escaped(got);
			printf("\n");
		}

		free(got);
	}

	printf("\n------------------------------\n");
	printf("%d passed, %d failed, %d total\n",
	       passed, failed, num_examples);

	if (passed == num_examples)
		printf("All spec examples passed!\n");
	else
		printf("Pass rate: %d%%\n",
		       (int)(100.0 * passed / num_examples));

	return failed > 0 ? 1 : 0;
}
