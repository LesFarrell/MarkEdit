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
