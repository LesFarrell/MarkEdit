#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/markdown.h"

static int failures = 0;

/* markdown_to_html() tags every block with data-sourcepos="..." for the
   editor/preview scroll sync. Most expectations are about structure and
   content, not positions, so they match against the HTML with those
   attributes removed; expect_contains_raw() checks the positions themselves. */
static void strip_sourcepos(char *html) {
    static const char attribute[] = " data-sourcepos=\"";
    char *out = html;
    for (char *in = html; *in;) {
        if (strncmp(in, attribute, sizeof(attribute) - 1) == 0) {
            char *close = strchr(in + sizeof(attribute) - 1, '"');
            if (close) { in = close + 1; continue; }
        }
        *out++ = *in++;
    }
    *out = 0;
}

static void expect_contains_raw(const char *name, const char *markdown, const char *expected) {
    char *html = markdown_to_html(markdown);
    if (!html || !strstr(html, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

static void expect_contains(const char *name, const char *markdown, const char *expected) {
    char *html = markdown_to_html(markdown);
    if (html) strip_sourcepos(html);
    if (!html || !strstr(html, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

static void expect_not_contains(const char *name, const char *markdown, const char *unexpected) {
    char *html = markdown_to_html(markdown);
    if (html) strip_sourcepos(html);
    if (!html || strstr(html, unexpected)) {
        fprintf(stderr, "FAIL %s\nunexpected substring: %s\nactual: %s\n", name, unexpected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

static void test_length_aware_render(void) {
    static const char markdown[] = "# Kept\n# Ignored\n";
    char *html = markdown_to_html_n(markdown, sizeof("# Kept\n") - 1);
    if (!html || !strstr(html, "Kept") || strstr(html, "Ignored")) {
        fprintf(stderr, "FAIL length-aware render\nactual: %s\n", html ? html : "(null)");
        failures++;
    }
    free(html);
}

int main(void) {
    expect_contains("heading", "# Hello *world*\n", "<h1>Hello <em>world</em></h1>");
    expect_contains_raw("heading sourcepos", "# Hello *world*\n", "<h1 data-sourcepos=\"1:1-1:15\">");
    expect_contains("hard break spaces", "first  \nsecond\n", "first<br />\nsecond");
    expect_contains("hard break slash", "first\\\nsecond\n", "first<br />\nsecond");
    expect_contains("loose list", "- one\n\n- two\n", "<li>\n<p>one</p>");
    expect_contains("table", "A | B\n---|:---:\n1|2\n", "<table>");
    expect_contains("strikethrough", "~~gone~~\n", "<del>gone</del>");
    expect_contains("task list", "- [x] done\n", "<input type=\"checkbox\" checked=\"\" disabled=\"\" />");
    expect_contains("bare URL autolink", "Visit https://example.com now.\n", "href=\"https://example.com\"");
    expect_contains("footnote", "A note.[^1]\n\n[^1]: Footnote text.\n", "class=\"footnotes\"");
    expect_contains("alert marker preserved", "> [!NOTE]\n> Useful information.\n", "[!NOTE]\nUseful information.");
    expect_contains("code language", "```c\nint main(void) {}\n```\n", "class=\"language-c\"");
    expect_contains("inline math delimiters preserved", "Euler: $e^{i\\pi} + 1 = 0$\n", "$e^{i\\pi} + 1 = 0$");
    expect_contains("math language", "```math\n\\int_0^1 x^2 \\, dx\n```\n", "class=\"language-math\"");
    expect_contains("yaml front matter body", "---\ntitle: Example\ntags: [demo]\n---\n# Visible\n", "<h1>Visible</h1>");
    expect_not_contains("yaml front matter hidden", "---\ntitle: Example\ntags: [demo]\n---\n# Visible\n", "title: Example");
    expect_contains("yaml front matter CRLF and BOM", "\xEF\xBB\xBF---\r\ntitle: Example\r\n...\r\nBody\r\n", "<p>Body</p>");
    expect_contains("unterminated yaml marker retained", "---\ntitle: Example\n", "title: Example");
    expect_contains("raw html omitted", "<img src=x onerror=alert(1)>\n", "raw HTML omitted");
    expect_not_contains("raw html inactive", "<img src=x onerror=alert(1)>\n", "<img src=x");
    expect_not_contains("unsafe URL inactive", "[click](javascript:alert(1))\n", "href=\"javascript:");
    expect_not_contains("data URL inactive", "![x](data:text/html,boom)\n", "src=\"data:");
    expect_contains("mermaid language", "```mermaid\ngraph TD\n```\n", "class=\"language-mermaid\"");
    expect_contains("mermaid breakout escaped", "```mermaid\n</pre><img onerror=alert(1)>\n```\n", "&lt;/pre&gt;&lt;img onerror=alert(1)&gt;");
    expect_contains("unicode", "# Caf\xC3\xA9 \xE2\x98\x95\n", "Caf\xC3\xA9 \xE2\x98\x95");

    expect_contains_raw("front matter source positions", "---\ntitle: Example\n---\n# Body\n",
                        "data-sourcepos=\"4:1-4:6\"");
    expect_contains_raw("CRLF front matter source positions", "---\r\ntitle: Example\r\n---\r\n# Body\r\n",
                        "data-sourcepos=\"4:1-4:6\"");
    expect_contains_raw("CR front matter source positions", "---\rtitle: Example\r---\r# Body\r",
                        "data-sourcepos=\"4:1-4:6\"");
    test_length_aware_render();
    if (failures) return 1;
    puts("All parser tests passed.");
    return 0;
}
