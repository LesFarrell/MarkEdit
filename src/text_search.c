/* Find/Replace on a supplied Scintilla view; no application state. */
#include "text_search.h"

#include <stdlib.h>
#include <string.h>

/* search_range()'s result for a regular expression Scintilla can't compile. */
#define SCINTILLA_BAD_REGEX -2

/* The pattern Scintilla is given, and its flags. A whole-word regular
   expression is wrapped in \b...\b, as Scintilla's whole-word flag only
   applies to plain text. Caller frees *pattern. */
static int prepare(const char *needle, int options, char **pattern, int *flags) {
    *flags = (options & SEARCH_MATCH_CASE) ? SCFIND_MATCHCASE : 0;
    if (!(options & SEARCH_REGEX)) {
        if (options & SEARCH_WHOLE_WORD) *flags |= SCFIND_WHOLEWORD;
        *pattern = _strdup(needle);
        return *pattern != NULL;
    }
    *flags |= SCFIND_REGEXP | SCFIND_CXX11REGEX;
    if (!(options & SEARCH_WHOLE_WORD)) {
        *pattern = _strdup(needle);
        return *pattern != NULL;
    }
    size_t length = strlen(needle);
    *pattern = (char *)malloc(length + 10);
    if (!*pattern) return 0;
    memcpy(*pattern, "\\b(?:", 5);
    memcpy(*pattern + 5, needle, length);
    memcpy(*pattern + 5 + length, ")\\b", 4);
    return 1;
}

/* Searches from `from` towards `to` (backwards when to < from), leaving the
   match as the target. Returns its start, -1, or SCINTILLA_BAD_REGEX.
   Scintilla reports a regular expression it can't compile as not found,
   with a warning status, which is cleared again afterwards. */
static Sci_Position search_range(HWND hEdit, const char *pattern, size_t patternLength,
                                 Sci_Position from, Sci_Position to) {
    SendMessageA(hEdit, SCI_SETTARGETRANGE, (WPARAM)from, (LPARAM)to);
    SendMessageA(hEdit, SCI_SETSTATUS, SC_STATUS_OK, 0);
    Sci_Position found = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET,
                                                    patternLength, (LPARAM)pattern);
    if (found < 0 && SendMessageA(hEdit, SCI_GETSTATUS, 0, 0) == SC_STATUS_WARN_REGEX) {
        SendMessageA(hEdit, SCI_SETSTATUS, SC_STATUS_OK, 0);
        return SCINTILLA_BAD_REGEX;
    }
    return found;
}

static Sci_Position target_end(HWND hEdit) {
    return (Sci_Position)SendMessageA(hEdit, SCI_GETTARGETEND, 0, 0);
}

/* Replaces the target, expanding \0-\9 for a regular expression. Returns the
   replacement's length. */
static Sci_Position replace_target(HWND hEdit, const char *replacement, int flags) {
    UINT message = (flags & SCFIND_REGEXP) ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
    return (Sci_Position)SendMessageA(hEdit, message, (WPARAM)-1, (LPARAM)replacement);
}

/* Selects the match left as the target and scrolls it into view, unfolding
   its line if needed. */
static void select_target(HWND hEdit, Sci_Position start) {
    Sci_Position end = target_end(hEdit);
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    SendMessageA(hEdit, SCI_ENSUREVISIBLEENFORCEPOLICY, (WPARAM)line, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageA(hEdit, SCI_SCROLLRANGE, (WPARAM)start, (LPARAM)end);
}

int search_find(HWND hEdit, const char *needle, int options, int forward) {
    char *pattern;
    int flags;
    if (!needle || !needle[0]) return SEARCH_NOT_FOUND;
    if (!prepare(needle, options, &pattern, &flags)) return SEARCH_NOT_FOUND;
    size_t patternLength = strlen(pattern);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, (WPARAM)flags, 0);
    Sci_Position length = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position selStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    int result = SEARCH_FOUND;
    Sci_Position found = forward ? search_range(hEdit, pattern, patternLength, selEnd, length)
                                 : search_range(hEdit, pattern, patternLength, selStart, 0);
    /* An empty match where the caret already is (a regex such as "^") would
       be found again forever, so step past it. */
    if (found >= 0 && found == target_end(hEdit) && found == selStart && selStart == selEnd) {
        Sci_Position next = (Sci_Position)SendMessageA(hEdit,
            forward ? SCI_POSITIONAFTER : SCI_POSITIONBEFORE, (WPARAM)found, 0);
        found = next == found ? -1
              : forward ? search_range(hEdit, pattern, patternLength, next, length)
                        : search_range(hEdit, pattern, patternLength, next, 0);
    }
    if (found == -1) {
        result = SEARCH_WRAPPED;
        found = forward ? search_range(hEdit, pattern, patternLength, 0, selStart)
                        : search_range(hEdit, pattern, patternLength, length, selEnd);
    }
    free(pattern);
    if (found == SCINTILLA_BAD_REGEX) return SEARCH_BAD_PATTERN;
    if (found < 0) return SEARCH_NOT_FOUND;
    select_target(hEdit, found);
    return result;
}

long search_count(HWND hEdit, const char *needle, int options, Sci_Position start, Sci_Position end) {
    char *pattern;
    int flags;
    if (!needle || !needle[0]) return 0;
    if (!prepare(needle, options, &pattern, &flags)) return 0;
    size_t patternLength = strlen(pattern);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, (WPARAM)flags, 0);
    long count = 0;
    Sci_Position position = start;
    while (position <= end) {
        Sci_Position found = search_range(hEdit, pattern, patternLength, position, end);
        if (found == SCINTILLA_BAD_REGEX) { count = SEARCH_BAD_PATTERN; break; }
        if (found < 0) break;
        count++;
        Sci_Position matchEnd = target_end(hEdit);
        if (matchEnd > found) {
            position = matchEnd;
        } else {
            /* An empty match: move on a character so it isn't counted again. */
            Sci_Position next = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONAFTER, (WPARAM)found, 0);
            if (next <= found) break;
            position = next;
        }
    }
    free(pattern);
    return count;
}

int search_replace_selection(HWND hEdit, const char *needle, const char *replacement, int options) {
    char *pattern;
    int flags;
    if (!needle || !needle[0]) return 0;
    if (!prepare(needle, options, &pattern, &flags)) return 0;
    size_t patternLength = strlen(pattern);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, (WPARAM)flags, 0);
    Sci_Position selStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    Sci_Position found = search_range(hEdit, pattern, patternLength, selStart, selEnd);
    free(pattern);
    if (found == SCINTILLA_BAD_REGEX) return SEARCH_BAD_PATTERN;
    if (found != selStart || target_end(hEdit) != selEnd) return 0;
    Sci_Position replaced = replace_target(hEdit, replacement, flags);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(selStart + replaced), (LPARAM)(selStart + replaced));
    return 1;
}

long search_replace_all(HWND hEdit, const char *needle, const char *replacement, int options,
                        Sci_Position start, Sci_Position *end) {
    char *pattern;
    int flags;
    if (!needle || !needle[0]) return 0;
    if (!prepare(needle, options, &pattern, &flags)) return 0;
    size_t patternLength = strlen(pattern);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, (WPARAM)flags, 0);
    long count = 0;
    Sci_Position position = start;
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    while (position <= *end) {
        Sci_Position found = search_range(hEdit, pattern, patternLength, position, *end);
        if (found == SCINTILLA_BAD_REGEX) { count = SEARCH_BAD_PATTERN; break; }
        if (found < 0) break;
        Sci_Position matched = target_end(hEdit) - found;
        Sci_Position replaced = replace_target(hEdit, replacement, flags);
        /* Searching resumes after the new text: re-checking it could find
           the match again, as when "a" is replaced with "aa". */
        *end += replaced - matched;
        position = found + replaced;
        count++;
        if (matched == 0) {
            /* An empty match: move on a character so it isn't replaced again. */
            Sci_Position next = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONAFTER, (WPARAM)position, 0);
            if (next <= position) break;
            position = next;
        }
    }
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(pattern);
    return count;
}
