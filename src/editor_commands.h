#ifndef EDITOR_COMMANDS_H
#define EDITOR_COMMANDS_H

#include <windows.h>
#include "../third_party/scintilla/include/Scintilla.h"

/* Returns -1 when the caret is not adjacent to a brace. */
Sci_Position brace_position_near_caret(HWND hEdit);
void jump_to_matching_brace(HWND hEdit);

void convert_indentation(HWND hEdit, int toSpaces);

#endif
