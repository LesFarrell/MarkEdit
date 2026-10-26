#ifndef MARKEDIT_TERMINAL_INPUT_H
#define MARKEDIT_TERMINAL_INPUT_H

#include <stddef.h>

enum {
    TERMINAL_MOD_SHIFT = 1,
    TERMINAL_MOD_ALT = 2,
    TERMINAL_MOD_CONTROL = 4
};

/* Builds the VT input sequence for a non-text Win32 virtual key. Returns its
   byte length, or zero when the key should be left to WM_(SYS)CHAR. */
size_t terminal_input_sequence(unsigned int key, unsigned int modifiers,
                               int applicationCursorKeys,
                               char *sequence, size_t capacity);

#endif
