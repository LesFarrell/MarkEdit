#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../markdown.h"

static int failures = 0;

static void expect_contains(const char *name, const char *markdown, const char *expected) {
    char *html = markdown_to_html(markdown);
    if (!html || !strstr(html, expected)) {
        fprintf(stderr, "FAIL %s\nexpected substring: %s\nactual: %s\n", name, expected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

static void expect_not_contains(const char *name, const char *markdown, const char *unexpected) {
    char *html = markdown_to_html(markdown);
    if (!html || strstr(html, unexpected)) {
        fprintf(stderr, "FAIL %s\nunexpected substring: %s\nactual: %s\n", name, unexpected, html ? html : "(null)");
        failures++;
    }
    free(html);
}

int main(void) {
    expect_contains("heading", "# Hello *world*\n", "<h1>Hello <em>world</em></h1>");
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
    expect_contains("raw html omitted", "<img src=x onerror=alert(1)>\n", "raw HTML omitted");
    expect_not_contains("raw html inactive", "<img src=x onerror=alert(1)>\n", "<img src=x");
    expect_not_contains("unsafe URL inactive", "[click](javascript:alert(1))\n", "href=\"javascript:");
    expect_not_contains("data URL inactive", "![x](data:text/html,boom)\n", "src=\"data:");
    expect_contains("mermaid language", "```mermaid\ngraph TD\n```\n", "class=\"language-mermaid\"");
    expect_contains("mermaid breakout escaped", "```mermaid\n</pre><img onerror=alert(1)>\n```\n", "&lt;/pre&gt;&lt;img onerror=alert(1)&gt;");
    expect_contains("unicode", "# Caf\xC3\xA9 \xE2\x98\x95\n", "Caf\xC3\xA9 \xE2\x98\x95");

    if (failures) return 1;
    puts("All parser tests passed.");
    return 0;
}
