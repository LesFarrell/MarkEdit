#ifndef TINKEREDIT_MARKDOWN_LINKS_H
#define TINKEREDIT_MARKDOWN_LINKS_H

#include <stddef.h>

typedef enum MarkdownLinkKind {
    MARKDOWN_LINK_WIKI,
    MARKDOWN_LINK_FILE,
    MARKDOWN_LINK_IMAGE
} MarkdownLinkKind;

/* target points into the supplied document and is not NUL-terminated. */
typedef void (*MarkdownLinkVisitor)(MarkdownLinkKind kind, int line,
                                    const char *target, size_t targetLength,
                                    void *context);

/* Visits wiki links, inline links/images, and reference definitions outside
   fenced and inline code. Lines are one-based. */
void markdown_links_visit(const char *text, MarkdownLinkVisitor visitor, void *context);

/* Whether fragment names one of the document's rendered heading IDs. This
   follows preview_shell.html's heading slug and duplicate-suffix rules for
   ordinary UTF-8 Markdown headings. fragment has no leading '#'. */
int markdown_heading_anchor_exists(const char *text, const char *fragment);

#endif
