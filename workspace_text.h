#ifndef MARKEDIT_WORKSPACE_TEXT_H
#define MARKEDIT_WORKSPACE_TEXT_H

#include <stddef.h>

/* Unicode-aware, invariant, case-insensitive search over UTF-8 strings. */
const char *utf8_case_find(const char *haystack, const char *needle);

/* Returns the first CR, LF, or CRLF terminator in a mutable UTF-8 buffer. */
char *text_line_break(char *start, size_t *terminatorLength);

/* Rewrites exact Markdown inline/reference link destinations. The optional
   query/fragment suffix is retained. Returns a heap string only when at
   least one destination changed; otherwise returns NULL. */
char *markdown_rewrite_link_targets(const char *text, const char *oldTarget,
                                    const char *newTarget, int *changeCount);

#endif
