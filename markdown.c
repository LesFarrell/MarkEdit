#include "markdown.h"

#include <stddef.h>
#include <string.h>

#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"

static int attach_extension(cmark_parser *parser, cmark_llist **render_extensions,
                            cmark_mem *mem, const char *name) {
    cmark_syntax_extension *extension = cmark_find_syntax_extension(name);
    if (!extension || !cmark_parser_attach_syntax_extension(parser, extension)) return 0;
    *render_extensions = cmark_llist_append(mem, *render_extensions, extension);
    return 1;
}

static const char *next_line(const char *line_end) {
    if (*line_end == '\r' && line_end[1] == '\n') return line_end + 2;
    if (*line_end == '\r' || *line_end == '\n') return line_end + 1;
    return line_end;
}

static const char *skip_yaml_front_matter(const char *text) {
    const char *start = text;
    const char *line_end;
    size_t text_length = strlen(text);

    if (text_length >= 3 &&
        (unsigned char)start[0] == 0xEF &&
        (unsigned char)start[1] == 0xBB &&
        (unsigned char)start[2] == 0xBF) {
        start += 3;
    }

    line_end = strpbrk(start, "\r\n");
    if (!line_end || line_end - start != 3 || memcmp(start, "---", 3) != 0) {
        return text;
    }

    start = next_line(line_end);
    while (*start) {
        size_t length;
        line_end = strpbrk(start, "\r\n");
        length = line_end ? (size_t)(line_end - start) : strlen(start);
        if (length == 3 &&
            (memcmp(start, "---", 3) == 0 || memcmp(start, "...", 3) == 0)) {
            return line_end ? next_line(line_end) : start + length;
        }
        if (!line_end) break;
        start = next_line(line_end);
    }

    /* An unterminated opening marker is ordinary Markdown, not metadata. */
    return text;
}

char *markdown_to_html(const char *text) {
    static const char *const extension_names[] = {
        "table",
        "strikethrough",
        "autolink",
        "tagfilter",
        "tasklist"
    };
    const int options = CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8 |
                        CMARK_OPT_FOOTNOTES | CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
    cmark_parser *parser;
    cmark_node *document;
    cmark_mem *mem = cmark_get_default_mem_allocator();
    cmark_llist *render_extensions = NULL;
    char *html;

    if (!text) text = "";

    cmark_gfm_core_extensions_ensure_registered();
    parser = cmark_parser_new(options);
    if (!parser) return NULL;

    for (size_t i = 0; i < sizeof(extension_names) / sizeof(extension_names[0]); i++) {
        if (!attach_extension(parser, &render_extensions, mem, extension_names[i])) {
            cmark_llist_free(mem, render_extensions);
            cmark_parser_free(parser);
            return NULL;
        }
    }

    text = skip_yaml_front_matter(text);
    cmark_parser_feed(parser, text, strlen(text));
    document = cmark_parser_finish(parser);
    if (!document) {
        cmark_llist_free(mem, render_extensions);
        cmark_parser_free(parser);
        return NULL;
    }

    /* CMARK_OPT_UNSAFE is deliberately absent: raw HTML and unsafe URLs stay inert. */
    html = cmark_render_html(document, options, render_extensions);
    cmark_node_free(document);
    cmark_llist_free(mem, render_extensions);
    cmark_parser_free(parser);
    return html;
}
