#ifndef EDITOR_COMMANDS_H
#define EDITOR_COMMANDS_H

#include <windows.h>
#include "../third_party/scintilla/include/Scintilla.h"

/* The app's Scintilla margin indices, in left-to-right order. Shared by the
   main editor's setup in markedit.c and the minimap's in outline_minimap.c. */
#define MARGIN_LINENUMBERS 0
#define MARGIN_BOOKMARKS 1
#define MARGIN_FOLD 2

/* Returns -1 when the caret is not adjacent to a brace. */
Sci_Position brace_position_near_caret(HWND hEdit);
void jump_to_matching_brace(HWND hEdit);

void convert_indentation(HWND hEdit, int toSpaces);

#endif
