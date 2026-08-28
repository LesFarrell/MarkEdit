#ifndef MARKEDIT_TEXT_CODEC_H
#define MARKEDIT_TEXT_CODEC_H

#include <windows.h>

WCHAR *to_wide(const char *utf8);
char *to_utf8(const WCHAR *wide);
WCHAR *decode_json_string(const WCHAR *json);
char *json_escape(const char *utf8);
int has_utf8_bom(const char *bytes, int length);
int hex_digit_value(WCHAR character);

#endif
