#ifndef MARKEDIT_TEXT_CODEC_H
#define MARKEDIT_TEXT_CODEC_H

#include <windows.h>

WCHAR *to_wide(const char *utf8);
char *to_utf8(const WCHAR *wide);
WCHAR *decode_json_string(const WCHAR *json);
char *json_escape(const char *utf8);
int has_utf8_bom(const char *bytes, int length);
int hex_digit_value(WCHAR character);

/* ---- text file encodings: the editor holds UTF-8; files may be any of these ---- */
typedef enum {
    TEXT_ENCODING_UTF8 = 0,
    TEXT_ENCODING_UTF16LE,
    TEXT_ENCODING_UTF16BE,
    TEXT_ENCODING_ANSI /* the system code page, see text_ansi_code_page() */
} TextEncoding;

/* Carries a sequence split across two calls of text_decode()/text_encode(). */
typedef struct {
    TextEncoding encoding;
    unsigned char carry[4];
    size_t carryLength;
} TextCodecState;

typedef struct {
    int pending, invalid;
    unsigned char low, high;
} Utf8Validator;

UINT text_ansi_code_page(void);
/* Length of `encoding`'s byte-order mark at the start of `bytes`, or 0. */
size_t text_bom_length(const char *bytes, size_t length, TextEncoding encoding);
const char *text_encoding_bom(TextEncoding encoding, size_t *outLength);
void utf8_validator_init(Utf8Validator *validator);
int utf8_validator_feed(Utf8Validator *validator, const char *bytes, size_t length);
int utf8_validator_finish(const Utf8Validator *validator);
/* BOM first, then BOM-less UTF-16, then strict UTF-8, else ANSI. `complete`
   is 0 when `bytes` is only the start of the file. */
TextEncoding detect_text_encoding(const char *bytes, size_t length, int complete,
                                  size_t *outBomLength);
size_t text_decode_capacity(size_t inputLength);
size_t text_encode_capacity(size_t inputLength);
/* Streaming conversion to (decode) and from (encode) UTF-8; `final` marks
   the last call. Inputs are expected in pieces of a few MiB at most. */
int text_decode(TextCodecState *state, const char *input, size_t inputLength, int final,
                char *output, size_t outputCapacity, size_t *outLength);
int text_encode(TextCodecState *state, const char *input, size_t inputLength, int final,
                char *output, size_t outputCapacity, size_t *outLength, int *outLossy);
/* Whole-buffer decode to a NUL-terminated UTF-8 heap string. */
char *text_decode_all(TextEncoding encoding, const char *bytes, size_t length, size_t *outLength);
/* 1 when every character of `utf8` survives being saved as `encoding`,
   0 when some wouldn't, -1 when there wasn't memory to find out. */
int text_encoding_is_lossless(const char *utf8, size_t length, TextEncoding encoding);

#endif
