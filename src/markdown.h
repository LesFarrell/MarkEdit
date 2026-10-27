#ifndef MARKEDIT_MARKDOWN_H
#define MARKEDIT_MARKDOWN_H

#include <stddef.h>

/* Convert UTF-8 Markdown to a newly allocated UTF-8 HTML fragment. */
char *markdown_to_html(const char *text);
/* Length-aware form for callers that already know the buffer size. */
char *markdown_to_html_n(const char *text, size_t length);

#endif
