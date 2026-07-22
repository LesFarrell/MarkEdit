#define MARKEDIT_PARSER_ONLY
#include "../markedit.c"

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
    expect_contains("hard break spaces", "first  \nsecond\n", "first<br>\nsecond");
    expect_contains("hard break slash", "first\\\nsecond\n", "first<br>\nsecond");
    expect_contains("loose list", "- one\n\n- two\n", "<li><p>one</p>");
    expect_contains("table", "A | B\n---|:---:\n1|2\n", "<table>");
    expect_contains("raw html escaped", "<img src=x onerror=alert(1)>\n", "&lt;img src=x onerror=alert(1)&gt;");
    expect_not_contains("raw html inactive", "<img src=x onerror=alert(1)>\n", "<img src=x");
    expect_contains("mermaid breakout escaped", "```mermaid\n</pre><img onerror=alert(1)>\n```\n", "&lt;/pre&gt;&lt;img onerror=alert(1)&gt;");
    expect_contains("unicode", "# Caf\xC3\xA9 \xE2\x98\x95\n", "Caf\xC3\xA9 \xE2\x98\x95");

    if (failures) return 1;
    puts("All parser tests passed.");
    return 0;
}
