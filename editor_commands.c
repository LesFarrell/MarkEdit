/* Text operations on a supplied Scintilla view; no application state. */
#include "editor_commands.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Returns the 0-based [firstLine, lastLine] range touched by the current
   selection (a single line if there is no selection). */
void editor_selected_line_range(HWND hEdit, Sci_Position *firstLine, Sci_Position *lastLine) {
    Sci_Position selStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    *firstLine = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)selStart, 0);
    *lastLine = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)selEnd, 0);
    /* A selection ending exactly at the start of a line (e.g. after pressing
       Home then Shift+Down once) shouldn't pull that line into the range. */
    if (*lastLine > *firstLine && selEnd == (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)*lastLine, 0))
        (*lastLine)--;
}

static int is_brace_char(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

/* The position of the brace adjacent to the caret - preferring the character
   just before it, so the match follows the caret the way editors expect -
   or -1 if the caret isn't next to one. */
Sci_Position brace_position_near_caret(HWND hEdit) {
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    if (pos > 0 && is_brace_char((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(pos - 1), 0)))
        return pos - 1;
    if (is_brace_char((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)pos, 0)))
        return pos;
    return -1;
}

void jump_to_matching_brace(HWND hEdit) {
    Sci_Position bracePos = brace_position_near_caret(hEdit);
    if (bracePos < 0) return;
    Sci_Position match = (Sci_Position)SendMessageA(hEdit, SCI_BRACEMATCH, (WPARAM)bracePos, 0);
    if (match < 0) return;
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)match, (LPARAM)match);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
}

/* Uppercases the first letter of each word in the selection and lowercases
   the rest. Non-ASCII bytes (including UTF-8 continuation bytes) are left
   untouched rather than mangled. */
void apply_title_case(HWND hEdit) {
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    Sci_Position length = end - start;
    if (length <= 0) return;
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) return;
    SendMessageA(hEdit, SCI_GETSELTEXT, 0, (LPARAM)text);
    int atWordStart = 1;
    for (Sci_Position i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalpha(c)) {
            text[i] = (char)(atWordStart ? toupper(c) : tolower(c));
            atWordStart = 0;
        } else {
            atWordStart = 1;
        }
    }
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)text);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)(start + length));
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(text);
}

static int compare_lines_asc(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int compare_lines_desc(const void *a, const void *b) {
    return strcmp(*(char *const *)b, *(char *const *)a);
}

/* mode: 0 = sort ascending, 1 = sort descending, 2 = remove duplicate lines
   (first occurrence kept, order otherwise preserved). Operates on the lines
   touched by the current selection, as one undo step. */
void transform_selected_lines(HWND hEdit, int mode, const char *eol) {
    Sci_Position firstLine, lastLine;
    editor_selected_line_range(hEdit, &firstLine, &lastLine);
    if (lastLine <= firstLine) return;

    Sci_Position rangeStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    Sci_Position rangeEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);
    Sci_Position totalLength = rangeEnd - rangeStart;
    char *buffer = (char *)malloc((size_t)totalLength + 1);
    if (!buffer) return;
    struct Sci_TextRangeFull tr;
    tr.chrg.cpMin = rangeStart;
    tr.chrg.cpMax = rangeEnd;
    tr.lpstrText = buffer;
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);

    Sci_Position lineCount = lastLine - firstLine + 1;
    char **lines = (char **)malloc((size_t)lineCount * sizeof(char *));
    if (!lines) { free(buffer); return; }
    Sci_Position n = 0;
    char *cursor = buffer;
    for (Sci_Position i = 0; i < lineCount; i++) {
        char *lineEnd = cursor + strcspn(cursor, "\r\n");
        size_t len = (size_t)(lineEnd - cursor);
        char *copy = (char *)malloc(len + 1);
        if (!copy) {
            for (Sci_Position j = 0; j < n; j++) free(lines[j]);
            free(lines);
            free(buffer);
            return;
        }
        memcpy(copy, cursor, len);
        copy[len] = 0;
        lines[n++] = copy;
        cursor = lineEnd;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;
    }

    if (mode == 2) {
        int kept = 0;
        for (Sci_Position i = 0; i < n; i++) {
            int dup = 0;
            for (int j = 0; j < kept; j++) {
                if (strcmp(lines[j], lines[i]) == 0) { dup = 1; break; }
            }
            if (dup) free(lines[i]);
            else lines[kept++] = lines[i];
        }
        n = kept;
    } else {
        qsort(lines, (size_t)n, sizeof(char *), mode == 1 ? compare_lines_desc : compare_lines_asc);
    }

    size_t outCap = 1;
    for (Sci_Position i = 0; i < n; i++) outCap += strlen(lines[i]) + strlen(eol);
    char *out = (char *)malloc(outCap);
    if (out) {
        out[0] = 0;
        for (Sci_Position i = 0; i < n; i++) {
            strcat(out, lines[i]);
            if (i < n - 1) strcat(out, eol);
        }
        SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)rangeStart, (LPARAM)rangeEnd);
        SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)out);
        SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
        free(out);
    }
    for (Sci_Position i = 0; i < n; i++) free(lines[i]);
    free(lines);
    free(buffer);
}

/* Rewrites each line's leading indentation between tabs and spaces, using the
   editor's configured tab width. Only leading whitespace is touched - tabs or
   spaces elsewhere on the line (inside a string, say) are left alone. */
void convert_indentation(HWND hEdit, int toSpaces) {
    int tabWidth = (int)SendMessageA(hEdit, SCI_GETTABWIDTH, 0, 0);
    if (tabWidth <= 0) tabWidth = 4;
    Sci_Position lineCount = (Sci_Position)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0);
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    for (Sci_Position line = lineCount - 1; line >= 0; line--) {
        Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        Sci_Position indentEnd = lineStart;
        int column = 0;
        while (indentEnd < lineEnd) {
            char ch = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)indentEnd, 0);
            if (ch == ' ') { column++; indentEnd++; }
            else if (ch == '\t') { column += tabWidth - (column % tabWidth); indentEnd++; }
            else break;
        }
        if (indentEnd == lineStart) continue;
        int newLen;
        char *newIndent;
        if (toSpaces) {
            newLen = column;
            newIndent = (char *)malloc((size_t)newLen + 1);
            if (!newIndent) continue;
            memset(newIndent, ' ', (size_t)newLen);
        } else {
            int tabs = column / tabWidth;
            int spaces = column % tabWidth;
            newLen = tabs + spaces;
            newIndent = (char *)malloc((size_t)newLen + 1);
            if (!newIndent) continue;
            memset(newIndent, '\t', (size_t)tabs);
            memset(newIndent + tabs, ' ', (size_t)spaces);
        }
        newIndent[newLen] = 0;
        SendMessageA(hEdit, SCI_SETTARGETRANGE, (WPARAM)lineStart, (LPARAM)indentEnd);
        SendMessageA(hEdit, SCI_REPLACETARGET, (WPARAM)newLen, (LPARAM)newIndent);
        free(newIndent);
    }
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
}

void trim_trailing_whitespace(HWND hEdit) {
    Sci_Position lineCount = (Sci_Position)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0);
    Sci_Position caret = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    int trimmed = 0;

    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    for (Sci_Position line = lineCount - 1; line >= 0; line--) {
        Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        Sci_Position cut = lineEnd;
        while (cut > lineStart) {
            char ch = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(cut - 1), 0);
            if (ch != ' ' && ch != '\t') break;
            cut--;
        }
        if (cut == lineEnd) continue;

        SendMessageA(hEdit, SCI_SETTARGETRANGE, (WPARAM)cut, (LPARAM)lineEnd);
        SendMessageA(hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
        if (caret > lineEnd) caret -= lineEnd - cut;
        else if (caret > cut) caret = cut;
        trimmed = 1;
    }
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);

    if (trimmed) SendMessageA(hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
    SetFocus(hEdit);
}

