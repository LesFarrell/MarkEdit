#include "terminal_input.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static size_t copy_sequence(char *output, size_t capacity, const char *sequence) {
    size_t length = strlen(sequence);
    if (!output || length >= capacity) return 0;
    memcpy(output, sequence, length + 1);
    return length;
}

static size_t format_csi(char *output, size_t capacity, int code,
                         unsigned int modifier, char final) {
    int length;
    if (code > 0)
        length = snprintf(output, capacity, "\x1b[%d;%u%c", code, modifier, final);
    else
        length = snprintf(output, capacity, "\x1b[1;%u%c", modifier, final);
    return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}

size_t terminal_input_sequence(unsigned int key, unsigned int modifiers,
                               int applicationCursorKeys,
                               char *sequence, size_t capacity) {
    modifiers &= TERMINAL_MOD_SHIFT | TERMINAL_MOD_ALT | TERMINAL_MOD_CONTROL;
    unsigned int xtermModifier = modifiers + 1;
    char final = 0;

    switch (key) {
        case VK_BACK: {
            char bytes[3];
            size_t length = 0;
            if (modifiers & TERMINAL_MOD_ALT) bytes[length++] = '\x1b';
            bytes[length++] = (modifiers & TERMINAL_MOD_CONTROL) ? '\x17' : '\x7f';
            bytes[length] = 0;
            return copy_sequence(sequence, capacity, bytes);
        }
        case VK_CANCEL:
            return copy_sequence(sequence, capacity, "\x03");
        case VK_TAB:
            if (!(modifiers & TERMINAL_MOD_SHIFT)) return 0;
            if (modifiers == TERMINAL_MOD_SHIFT)
                return copy_sequence(sequence, capacity, "\x1b[Z");
            return format_csi(sequence, capacity, 0, xtermModifier, 'Z');
        case VK_UP: final = 'A'; break;
        case VK_DOWN: final = 'B'; break;
        case VK_RIGHT: final = 'C'; break;
        case VK_LEFT: final = 'D'; break;
        case VK_HOME: final = 'H'; break;
        case VK_END: final = 'F'; break;
        default: break;
    }

    if (final) {
        if (modifiers) return format_csi(sequence, capacity, 0, xtermModifier, final);
        if (applicationCursorKeys && key >= VK_LEFT && key <= VK_DOWN) {
            char application[] = { '\x1b', 'O', final, 0 };
            return copy_sequence(sequence, capacity, application);
        }
        char cursor[] = { '\x1b', '[', final, 0 };
        return copy_sequence(sequence, capacity, cursor);
    }

    int code = 0;
    switch (key) {
        case VK_INSERT: code = 2; break;
        case VK_DELETE: code = 3; break;
        case VK_PRIOR: code = 5; break;
        case VK_NEXT: code = 6; break;
        case VK_F5: code = 15; break;
        case VK_F6: code = 17; break;
        case VK_F7: code = 18; break;
        case VK_F8: code = 19; break;
        case VK_F9: code = 20; break;
        case VK_F10: code = 21; break;
        case VK_F11: code = 23; break;
        case VK_F12: code = 24; break;
        default: break;
    }
    if (code) {
        if (modifiers) return format_csi(sequence, capacity, code, xtermModifier, '~');
        int length = snprintf(sequence, capacity, "\x1b[%d~", code);
        return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
    }

    if (key >= VK_F1 && key <= VK_F4) {
        final = (char)('P' + key - VK_F1);
        if (modifiers) return format_csi(sequence, capacity, 0, xtermModifier, final);
        char function[] = { '\x1b', 'O', final, 0 };
        return copy_sequence(sequence, capacity, function);
    }
    return 0;
}
