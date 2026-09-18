#include "text_codec.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

WCHAR *to_wide(const char *utf8) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (length <= 0) return NULL;
    WCHAR *wide = (WCHAR *)malloc((size_t)length * sizeof(WCHAR));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

char *to_utf8(const WCHAR *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) return NULL;
    char *utf8 = (char *)malloc((size_t)length);
    if (!utf8) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, length, NULL, NULL)) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

int has_utf8_bom(const char *bytes, int length) {
    return length >= 3 && (unsigned char)bytes[0] == 0xEF &&
           (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF;
}

int hex_digit_value(WCHAR character) {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
}

WCHAR *decode_json_string(const WCHAR *json) {
    if (!json || json[0] != L'"') return NULL;
    size_t length = wcslen(json);
    WCHAR *output = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
    if (!output) return NULL;
    size_t outputLength = 0;
    size_t i = 1;
    for (; i < length && json[i] != L'"'; i++) {
        WCHAR character = json[i];
        if (character != L'\\') {
            output[outputLength++] = character;
            continue;
        }
        if (++i >= length) {
            free(output);
            return NULL;
        }
        character = json[i];
        if (character == L'"' || character == L'\\' || character == L'/')
            output[outputLength++] = character;
        else if (character == L'b') output[outputLength++] = L'\b';
        else if (character == L'f') output[outputLength++] = L'\f';
        else if (character == L'n') output[outputLength++] = L'\n';
        else if (character == L'r') output[outputLength++] = L'\r';
        else if (character == L't') output[outputLength++] = L'\t';
        else if (character == L'u' && i + 4 < length) {
            int value = 0;
            for (int digitIndex = 0; digitIndex < 4; digitIndex++) {
                int digit = hex_digit_value(json[++i]);
                if (digit < 0) {
                    free(output);
                    return NULL;
                }
                value = value * 16 + digit;
            }
            output[outputLength++] = (WCHAR)value;
        } else {
            free(output);
            return NULL;
        }
    }
    /* The loop above also exits at end-of-string with no closing quote found -
       that's truncated/malformed input, not a valid (possibly empty) string,
       and returning the partial content instead of NULL would let a caller
       treat garbage as a successful decode. */
    if (i >= length) {
        free(output);
        return NULL;
    }
    output[outputLength] = 0;
    return output;
}

char *json_escape(const char *utf8) {
    size_t length = strlen(utf8);
    char *output = (char *)malloc(length * 6 + 1);
    if (!output) return NULL;
    size_t outputLength = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)utf8[i];
        switch (character) {
            case '\\': output[outputLength++] = '\\'; output[outputLength++] = '\\'; break;
            case '\'': output[outputLength++] = '\\'; output[outputLength++] = '\''; break;
            case '<': memcpy(output + outputLength, "\\u003C", 6); outputLength += 6; break;
            case '\n': output[outputLength++] = '\\'; output[outputLength++] = 'n'; break;
            case '\r': output[outputLength++] = '\\'; output[outputLength++] = 'r'; break;
            case '\t': output[outputLength++] = '\\'; output[outputLength++] = 't'; break;
            case '\b': output[outputLength++] = '\\'; output[outputLength++] = 'b'; break;
            case '\f': output[outputLength++] = '\\'; output[outputLength++] = 'f'; break;
            default:
                if (character < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    output[outputLength++] = '\\'; output[outputLength++] = 'u';
                    output[outputLength++] = '0'; output[outputLength++] = '0';
                    output[outputLength++] = hex[character >> 4];
                    output[outputLength++] = hex[character & 15];
                } else if (character == 0xE2 && i + 2 < length &&
                           (unsigned char)utf8[i + 1] == 0x80 &&
                           ((unsigned char)utf8[i + 2] == 0xA8 ||
                            (unsigned char)utf8[i + 2] == 0xA9)) {
                    memcpy(output + outputLength,
                           (unsigned char)utf8[i + 2] == 0xA8 ? "\\u2028" : "\\u2029", 6);
                    outputLength += 6;
                    i += 2;
                } else output[outputLength++] = (char)character;
                break;
        }
    }
    output[outputLength] = 0;
    return output;
}
