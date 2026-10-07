#include "text_codec.h"

#include <limits.h>
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

/* ---- text file encodings ----
   The editor always holds UTF-8; these convert a file's bytes to and from
   that. Both directions stream, so a large file can be converted a chunk at
   a time: a sequence split across two chunks is carried into the next call
   through TextCodecState. */

UINT text_ansi_code_page(void) {
    UINT codePage = GetACP();
    /* With Windows' "use Unicode UTF-8 for worldwide language support"
       option the ANSI code page *is* UTF-8 - the very interpretation an
       ANSI-detected file just failed - so fall back to Western European. */
    return codePage == CP_UTF8 ? 1252 : codePage;
}

size_t text_bom_length(const char *bytes, size_t length, TextEncoding encoding) {
    const unsigned char *b = (const unsigned char *)bytes;
    switch (encoding) {
        case TEXT_ENCODING_UTF8:
            return length >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
        case TEXT_ENCODING_UTF16LE: return length >= 2 && b[0] == 0xFF && b[1] == 0xFE ? 2 : 0;
        case TEXT_ENCODING_UTF16BE: return length >= 2 && b[0] == 0xFE && b[1] == 0xFF ? 2 : 0;
        default: return 0;
    }
}

const char *text_encoding_bom(TextEncoding encoding, size_t *outLength) {
    switch (encoding) {
        case TEXT_ENCODING_UTF8: *outLength = 3; return "\xEF\xBB\xBF";
        case TEXT_ENCODING_UTF16LE: *outLength = 2; return "\xFF\xFE";
        case TEXT_ENCODING_UTF16BE: *outLength = 2; return "\xFE\xFF";
        default: *outLength = 0; return "";
    }
}

/* Strict UTF-8 (RFC 3629): overlong forms, surrogates, and code points past
   U+10FFFF are all invalid, which is what lets a legacy 8-bit file be told
   apart from UTF-8 - its accented letters almost never form valid sequences. */
void utf8_validator_init(Utf8Validator *validator) {
    memset(validator, 0, sizeof(*validator));
}

int utf8_validator_feed(Utf8Validator *validator, const char *bytes, size_t length) {
    if (validator->invalid) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (validator->pending) {
            if (c < validator->low || c > validator->high) { validator->invalid = 1; return 0; }
            validator->pending--;
            validator->low = 0x80;
            validator->high = 0xBF;
            continue;
        }
        if (c < 0x80) continue;
        validator->low = 0x80;
        validator->high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) validator->pending = 1;
        else if (c == 0xE0) { validator->pending = 2; validator->low = 0xA0; }
        else if (c == 0xED) { validator->pending = 2; validator->high = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) validator->pending = 2;
        else if (c == 0xF0) { validator->pending = 3; validator->low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) validator->pending = 3;
        else if (c == 0xF4) { validator->pending = 3; validator->high = 0x8F; }
        else { validator->invalid = 1; return 0; }
    }
    return 1;
}

int utf8_validator_finish(const Utf8Validator *validator) {
    return !validator->invalid && validator->pending == 0;
}

TextEncoding detect_text_encoding(const char *bytes, size_t length, int complete,
                                  size_t *outBomLength) {
    static const TextEncoding withBoms[] = { TEXT_ENCODING_UTF8, TEXT_ENCODING_UTF16LE, TEXT_ENCODING_UTF16BE };
    for (int i = 0; i < 3; i++) {
        size_t bom = text_bom_length(bytes, length, withBoms[i]);
        if (bom) { *outBomLength = bom; return withBoms[i]; }
    }
    *outBomLength = 0;

    /* UTF-16 without a BOM: text that's mostly Latin script has a NUL in
       every other byte - odd offsets for little-endian, even for big. Real
       UTF-8 and 8-bit text essentially never contain NULs at all. */
    size_t sample = (length < 4096 ? length : 4096) & ~(size_t)1;
    if (sample >= 4) {
        size_t evenZeros = 0, oddZeros = 0, pairs = sample / 2;
        for (size_t i = 0; i < sample; i += 2) {
            if (!bytes[i]) evenZeros++;
            if (!bytes[i + 1]) oddZeros++;
        }
        if (oddZeros * 10 >= pairs * 4 && evenZeros * 20 < pairs) return TEXT_ENCODING_UTF16LE;
        if (evenZeros * 10 >= pairs * 4 && oddZeros * 20 < pairs) return TEXT_ENCODING_UTF16BE;
    }

    Utf8Validator validator;
    utf8_validator_init(&validator);
    if (!utf8_validator_feed(&validator, bytes, length)) return TEXT_ENCODING_ANSI;
    /* A prefix of a larger file may legitimately end mid-sequence. */
    if (complete && !utf8_validator_finish(&validator)) return TEXT_ENCODING_ANSI;
    return TEXT_ENCODING_UTF8;
}

size_t text_decode_capacity(size_t inputLength) {
    /* An ANSI byte can become up to 3 UTF-8 bytes, a UTF-16 unit likewise
       (from 2 input bytes); the slack covers bytes carried from the last call. */
    return inputLength * 3 + 16;
}

size_t text_encode_capacity(size_t inputLength) {
    /* Each UTF-8 byte becomes at most 2 bytes of UTF-16 or of a DBCS code page. */
    return inputLength * 2 + 16;
}

/* Joins the bytes carried over from the previous call with this call's
   input. Callers stay well under INT_MAX per call, which is what the Win32
   conversion functions below take. */
static char *join_carry(TextCodecState *state, const char *input, size_t inputLength, size_t *outLength) {
    char *work = (char *)malloc(state->carryLength + inputLength + 1);
    if (!work) return NULL;
    memcpy(work, state->carry, state->carryLength);
    if (inputLength) memcpy(work + state->carryLength, input, inputLength);
    *outLength = state->carryLength + inputLength;
    state->carryLength = 0;
    return work;
}

static void keep_carry(TextCodecState *state, const char *bytes, size_t count) {
    memcpy(state->carry, bytes, count);
    state->carryLength = count;
}

int text_decode(TextCodecState *state, const char *input, size_t inputLength, int final,
                char *output, size_t outputCapacity, size_t *outLength) {
    *outLength = 0;
    if (state->encoding == TEXT_ENCODING_UTF8) {
        if (inputLength > outputCapacity) return 0;
        if (inputLength) memcpy(output, input, inputLength);
        *outLength = inputLength;
        return 1;
    }
    size_t length = 0;
    char *work = join_carry(state, input, inputLength, &length);
    if (!work) return 0;
    const unsigned char *b = (const unsigned char *)work;
    WCHAR *wide = NULL;
    int wideLength = 0, ok = 1;

    if (state->encoding == TEXT_ENCODING_UTF16LE || state->encoding == TEXT_ENCODING_UTF16BE) {
        int bigEndian = state->encoding == TEXT_ENCODING_UTF16BE;
        size_t units = length / 2;
        /* A high surrogate at the end pairs with the next call's first unit. */
        if (!final && units) {
            size_t last = (units - 1) * 2;
            unsigned unit = bigEndian ? (unsigned)(b[last] << 8 | b[last + 1]) : (unsigned)(b[last + 1] << 8 | b[last]);
            if (unit >= 0xD800 && unit <= 0xDBFF) units--;
        }
        wide = (WCHAR *)malloc((units ? units : 1) * sizeof(WCHAR));
        if (!wide) { free(work); return 0; }
        for (size_t i = 0; i < units; i++)
            wide[i] = bigEndian ? (WCHAR)(b[2 * i] << 8 | b[2 * i + 1]) : (WCHAR)(b[2 * i + 1] << 8 | b[2 * i]);
        wideLength = (int)units;
        size_t rest = length - units * 2;
        if (!final) keep_carry(state, work + units * 2, rest);
        else if (rest) {
            /* A dangling odd byte at the very end can't be a character. */
            WCHAR *grown = (WCHAR *)realloc(wide, (units + 1) * sizeof(WCHAR));
            if (!grown) { free(wide); free(work); return 0; }
            wide = grown;
            wide[wideLength++] = 0xFFFD;
        }
    } else {
        UINT codePage = text_ansi_code_page();
        size_t usable = length;
        if (!final) {
            /* Stop short of a double-byte character's lead byte when its
               trail byte is still to come. */
            size_t i = 0;
            while (i < length) {
                if (IsDBCSLeadByteEx(codePage, b[i])) {
                    if (i + 1 >= length) break;
                    i += 2;
                } else i++;
            }
            usable = i;
            keep_carry(state, work + usable, length - usable);
        }
        if (usable) {
            wideLength = MultiByteToWideChar(codePage, 0, work, (int)usable, NULL, 0);
            wide = wideLength > 0 ? (WCHAR *)malloc((size_t)wideLength * sizeof(WCHAR)) : NULL;
            if (!wide || !MultiByteToWideChar(codePage, 0, work, (int)usable, wide, wideLength)) ok = 0;
        }
    }

    if (ok && wideLength > 0) {
        int written = WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, output,
            outputCapacity > INT_MAX ? INT_MAX : (int)outputCapacity, NULL, NULL);
        if (written <= 0) ok = 0;
        else *outLength = (size_t)written;
    }
    free(wide);
    free(work);
    return ok;
}

int text_encode(TextCodecState *state, const char *input, size_t inputLength, int final,
                char *output, size_t outputCapacity, size_t *outLength, int *outLossy) {
    *outLength = 0;
    if (state->encoding == TEXT_ENCODING_UTF8) {
        if (inputLength > outputCapacity) return 0;
        if (inputLength) memcpy(output, input, inputLength);
        *outLength = inputLength;
        return 1;
    }
    size_t length = 0;
    char *work = join_carry(state, input, inputLength, &length);
    if (!work) return 0;
    size_t usable = length;
    if (!final) {
        /* Hold back a UTF-8 sequence that the next call completes. */
        const unsigned char *b = (const unsigned char *)work;
        size_t start = length;
        while (start > 0 && length - start < 4 && (b[start - 1] & 0xC0) == 0x80) start--;
        if (start > 0) {
            unsigned char lead = b[start - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (start - 1 + need > length) usable = start - 1;
        }
        keep_carry(state, work + usable, length - usable);
    }
    int ok = 1;
    if (usable) {
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, work, (int)usable, NULL, 0);
        WCHAR *wide = wideLength > 0 ? (WCHAR *)malloc((size_t)wideLength * sizeof(WCHAR)) : NULL;
        if (!wide || !MultiByteToWideChar(CP_UTF8, 0, work, (int)usable, wide, wideLength)) ok = 0;
        if (ok && state->encoding == TEXT_ENCODING_ANSI) {
            BOOL usedDefault = FALSE;
            int written = WideCharToMultiByte(text_ansi_code_page(), WC_NO_BEST_FIT_CHARS, wide, wideLength,
                output, outputCapacity > INT_MAX ? INT_MAX : (int)outputCapacity, NULL, &usedDefault);
            if (written <= 0) ok = 0;
            else *outLength = (size_t)written;
            if (usedDefault && outLossy) *outLossy = 1;
        } else if (ok) {
            if ((size_t)wideLength * 2 > outputCapacity) ok = 0;
            int bigEndian = state->encoding == TEXT_ENCODING_UTF16BE;
            unsigned char *o = (unsigned char *)output;
            for (int i = 0; ok && i < wideLength; i++) {
                o[2 * i + (bigEndian ? 0 : 1)] = (unsigned char)(wide[i] >> 8);
                o[2 * i + (bigEndian ? 1 : 0)] = (unsigned char)(wide[i] & 0xFF);
            }
            if (ok) *outLength = (size_t)wideLength * 2;
        }
        free(wide);
    }
    free(work);
    return ok;
}

#define TEXT_CODEC_PIECE (1024 * 1024)

char *text_decode_all(TextEncoding encoding, const char *bytes, size_t length, size_t *outLength) {
    TextCodecState state = { encoding, {0}, 0 };
    size_t capacity = length + length / 2 + TEXT_CODEC_PIECE, used = 0;
    char *output = (char *)malloc(capacity);
    if (!output) return NULL;
    size_t offset = 0;
    do {
        size_t piece = length - offset > TEXT_CODEC_PIECE ? TEXT_CODEC_PIECE : length - offset;
        size_t room = text_decode_capacity(piece) + 1;
        if (capacity - used < room) {
            size_t next = capacity * 2 > used + room ? capacity * 2 : used + room;
            char *grown = (char *)realloc(output, next);
            if (!grown) { free(output); return NULL; }
            output = grown;
            capacity = next;
        }
        size_t produced = 0;
        if (!text_decode(&state, bytes + offset, piece, offset + piece == length,
                         output + used, capacity - used - 1, &produced)) {
            free(output);
            return NULL;
        }
        used += produced;
        offset += piece;
    } while (offset < length);
    output[used] = 0;
    if (outLength) *outLength = used;
    return output;
}

int text_encoding_is_lossless(const char *utf8, size_t length, TextEncoding encoding) {
    /* UTF-8 output is the buffer's bytes as they are, so nothing is lost. */
    if (encoding == TEXT_ENCODING_UTF8) return 1;
    /* UTF-16 holds every character, but bytes that aren't valid UTF-8 (a
       file reopened with the wrong encoding) would become U+FFFD. */
    if (encoding != TEXT_ENCODING_ANSI) {
        Utf8Validator validator;
        utf8_validator_init(&validator);
        return utf8_validator_feed(&validator, utf8, length) && utf8_validator_finish(&validator);
    }
    TextCodecState state = { encoding, {0}, 0 };
    char *scratch = (char *)malloc(text_encode_capacity(TEXT_CODEC_PIECE));
    if (!scratch) return -1;
    int lossy = 0;
    size_t offset = 0;
    do {
        size_t piece = length - offset > TEXT_CODEC_PIECE ? TEXT_CODEC_PIECE : length - offset;
        size_t produced = 0;
        if (!text_encode(&state, utf8 + offset, piece, offset + piece == length, scratch,
                         text_encode_capacity(TEXT_CODEC_PIECE), &produced, &lossy)) lossy = 1;
        offset += piece;
    } while (offset < length && !lossy);
    free(scratch);
    return !lossy;
}
