#ifndef MARKEDIT_WORKSPACE_TEXT_H
#define MARKEDIT_WORKSPACE_TEXT_H

#include <stddef.h>

/* Unicode-aware, invariant, case-insensitive search over UTF-8 strings. */
const char *utf8_case_find(const char *haystack, const char *needle);

/* As utf8_case_find(), also storing the length in bytes of the text that
   matched, which can differ from strlen(needle) for non-ASCII needles. */
const char *utf8_case_find_length(const char *haystack, const char *needle, size_t *matchBytes);

/* Returns the first CR, LF, or CRLF terminator in a mutable UTF-8 buffer. */
char *text_line_break(char *start, size_t *terminatorLength);

/* Rewrites exact Markdown inline/reference link destinations. The optional
   query/fragment suffix is retained. Returns a heap string only when at
   least one destination changed; otherwise returns NULL. */
char *markdown_rewrite_link_targets(const char *text, const char *oldTarget,
                                    const char *newTarget, int *changeCount);

/* As markdown_rewrite_link_targets() for a renamed folder: also rewrites
   destinations inside it, so "docs/intro.md" becomes "guide/intro.md". */
char *markdown_rewrite_link_directory(const char *text, const char *oldDirectory,
                                      const char *newDirectory, int *changeCount);

#endif
