#ifndef EDITOR_COMMANDS_H
#define EDITOR_COMMANDS_H

#include <windows.h>
#include "third_party/scintilla/include/Scintilla.h"

/* Excludes a final line when the selection ends exactly at its start. */
void editor_selected_line_range(HWND hEdit, Sci_Position *firstLine, Sci_Position *lastLine);

/* Returns -1 when the caret is not adjacent to a brace. */
Sci_Position brace_position_near_caret(HWND hEdit);
void jump_to_matching_brace(HWND hEdit);

void apply_title_case(HWND hEdit);
/* mode: 0 = ascending, 1 = descending, 2 = remove duplicates in original order.
   eol is the line separator to use when joining the resulting lines. */
void transform_selected_lines(HWND hEdit, int mode, const char *eol);
void convert_indentation(HWND hEdit, int toSpaces);
void trim_trailing_whitespace(HWND hEdit);

#endif
