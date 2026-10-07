#ifndef FILE_IO_H
#define FILE_IO_H

#include <stddef.h>
#include <windows.h>
#include "text_codec.h"

/* Creates or truncates path. Returns nonzero only when all bytes and the
   final handle close succeed; a failure may leave a partial file. */
int write_bytes_file(const WCHAR *path, const char *bytes, size_t length);

/* Readers return heap-allocated, NUL-terminated buffers (caller frees), or
   NULL on failure. Optional output arguments are set only on success. */
char *read_bytes_file(const WCHAR *path, int *outLength);
/* Detects the source encoding and returns UTF-8; outEncoding is TextEncoding. */
char *read_text_file(const WCHAR *path, int *outLength, int *outEncoding, int *outHasBom);
/* Validates UTF-8, strips its BOM, and rejects files larger than maximumSize.
   A NULL path returns an allocated empty string. Intended for small assets. */
char *read_utf8_text_file(const WCHAR *path, size_t maximumSize);

/* Atomically publishes encoded UTF-8 input. target is an I/O-ready path. */
int write_file_atomic(const WCHAR *target, const char *text, size_t length,
                      TextEncoding encoding, int hasBom);

#endif
