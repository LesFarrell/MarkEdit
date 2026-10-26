#ifndef TEXT_SEARCH_H
#define TEXT_SEARCH_H

#include <windows.h>
#include "../third_party/scintilla/include/Scintilla.h"

/* Find/Replace in a Scintilla window. Positions are byte offsets; text is
   UTF-8. No application state: the Find dialog keeps the options. */

/* Search options, combined with |. */
#define SEARCH_MATCH_CASE 1
#define SEARCH_WHOLE_WORD 2
#define SEARCH_REGEX 4        /* ECMAScript syntax; replacements use \0-\9 */

/* Results of search_find(). */
#define SEARCH_BAD_PATTERN -1 /* also returned by the other functions */
#define SEARCH_NOT_FOUND 0
#define SEARCH_FOUND 1
#define SEARCH_WRAPPED 2      /* found after wrapping round the document */

/* Selects the next match after the main selection, or the previous one
   before it, wrapping round the document, and scrolls it into view. */
int search_find(HWND hEdit, const char *needle, int options, int forward);

/* The number of matches between start and end, or SEARCH_BAD_PATTERN. */
long search_count(HWND hEdit, const char *needle, int options, Sci_Position start, Sci_Position end);

/* Replaces the main selection with replacement if the selection is exactly a
   match, leaving the caret after it. Returns 1 if it replaced, 0 if the
   selection isn't a match, or SEARCH_BAD_PATTERN. */
int search_replace_selection(HWND hEdit, const char *needle, const char *replacement, int options);

/* Replaces every match between start and *end as one undo step, updating
   *end to where that text now ends. Returns the number replaced, or
   SEARCH_BAD_PATTERN. */
long search_replace_all(HWND hEdit, const char *needle, const char *replacement, int options,
                        Sci_Position start, Sci_Position *end);

#endif
