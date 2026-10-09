/* Text operations on a supplied Scintilla view; no application state. */
#include "editor_commands.h"

#include <stdlib.h>
#include <string.h>

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
