#ifndef MARKEDIT_TERMINAL_INPUT_H
#define MARKEDIT_TERMINAL_INPUT_H

#include <stddef.h>

/* These bit values deliberately match xterm's modifier parameter: adding one
   gives 2 for Shift, 3 for Alt, 5 for Control, and 2-8 for combinations. */
enum {
    TERMINAL_MOD_SHIFT = 1,
    TERMINAL_MOD_ALT = 2,
    TERMINAL_MOD_CONTROL = 4
};

/* Builds the VT input sequence for a non-text Win32 virtual key. This covers
   cursor/editing keys, Backspace, backtab, Ctrl+Break, and F1-F12, including
   xterm modifier parameters and DECCKM application-cursor mode. Returns the
   byte length, or zero if the buffer is too small or the key must be decoded
   by TranslateMessage/WM_(SYS)CHAR to preserve the active keyboard layout. */
size_t terminal_input_sequence(unsigned int key, unsigned int modifiers,
                               int applicationCursorKeys,
                               char *sequence, size_t capacity);

#endif
