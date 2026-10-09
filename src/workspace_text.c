#include "workspace_text.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "text_codec.h"

const char *utf8_case_find(const char *haystack, const char *needle) {
    return utf8_case_find_length(haystack, needle, NULL);
}

const char *utf8_case_find_length(const char *haystack, const char *needle, size_t *matchBytes) {
    if (matchBytes) *matchBytes = 0;
    if (!haystack || !needle) return NULL;
    if (!needle[0]) return haystack;
    int asciiNeedle = 1;
    for (const unsigned char *scan = (const unsigned char *)needle; *scan; scan++)
        if (*scan >= 0x80) { asciiNeedle = 0; break; }
    if (asciiNeedle) {
        size_t needleBytes = strlen(needle);
        for (const unsigned char *candidate = (const unsigned char *)haystack; *candidate; candidate++) {
            size_t i = 0;
            while (i < needleBytes && candidate[i]) {
                unsigned char left = candidate[i], right = (unsigned char)needle[i];
                if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + ('a' - 'A'));
                if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + ('a' - 'A'));
                if (left != right) break;
                i++;
            }
            if (i == needleBytes) {
                if (matchBytes) *matchBytes = needleBytes;
                return (const char *)candidate;
            }
        }
        return NULL;
    }
    int hayLength = 0, needleLength = 0;
    WCHAR *wideHaystack = to_wide_n(haystack, strlen(haystack), &hayLength);
    WCHAR *wideNeedle = to_wide_n(needle, strlen(needle), &needleLength);
    if (!wideHaystack || !wideNeedle) {
        free(wideHaystack); free(wideNeedle);
        return NULL;
    }
    int foundLength = 0;
    int found = FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE,
                                wideHaystack, hayLength, wideNeedle, needleLength,
                                &foundLength, NULL, NULL, 0);
    /* A linguistic match need not be the needle's length: a precomposed
       character can match its decomposed form, and ignorable code points
       can sit inside the match. Report the bytes actually matched. */
    const char *result = NULL;
    if (found >= 0 && foundLength > 0) {
        int byteOffset = found ? WideCharToMultiByte(CP_UTF8, 0, wideHaystack, found,
                                                     NULL, 0, NULL, NULL) : 0;
        int byteLength = WideCharToMultiByte(CP_UTF8, 0, wideHaystack + found, foundLength,
                                             NULL, 0, NULL, NULL);
        if ((byteOffset > 0 || !found) && byteLength > 0) {
            result = haystack + byteOffset;
            if (matchBytes) *matchBytes = (size_t)byteLength;
        }
    }
    free(wideHaystack); free(wideNeedle);
    return result;
}

char *text_line_break(char *start, size_t *terminatorLength) {
    if (terminatorLength) *terminatorLength = 0;
    if (!start) return NULL;
    char *cursor = start;
    while (*cursor && *cursor != '\r' && *cursor != '\n') cursor++;
    if (!*cursor) return cursor;
    size_t length = (*cursor == '\r' && cursor[1] == '\n') ? 2 : 1;
    if (terminatorLength) *terminatorLength = length;
    return cursor;
}

static int utf8_case_equal_n(const char *left, size_t leftLength, const char *right) {
    int leftWideLength = 0, rightWideLength = 0;
    WCHAR *leftWide = to_wide_n(left, leftLength, &leftWideLength);
    WCHAR *rightWide = to_wide_n(right, strlen(right), &rightWideLength);
    int equal = leftWide && rightWide && leftWideLength == rightWideLength &&
        CompareStringOrdinal(leftWide, leftWideLength, rightWide, rightWideLength, TRUE) == CSTR_EQUAL;
    free(leftWide); free(rightWide);
    return equal;
}

static int append_bytes(char **output, size_t *length, size_t *capacity,
                        const char *bytes, size_t count) {
    if (count > SIZE_MAX - *length - 1) return 0;
    size_t wanted = *length + count + 1;
    if (wanted > *capacity) {
        size_t next = *capacity ? *capacity : 256;
        while (next < wanted) {
            if (next > SIZE_MAX / 2) { next = wanted; break; }
            next *= 2;
        }
        char *grown = (char *)realloc(*output, next);
        if (!grown) return 0;
        *output = grown;
        *capacity = next;
    }
    if (count) memcpy(*output + *length, bytes, count);
    *length += count;
    (*output)[*length] = 0;
    return 1;
}

static const char *destination_start(const char *text, const char *position) {
    if (position > text && position[-1] == ']' && position[0] == '(') {
        position++;
    } else {
        const char *line = position;
        while (line > text && line[-1] != '\r' && line[-1] != '\n') line--;
        const char *scan = line;
        int spaces = 0;
        while ((*scan == ' ' || *scan == '\t') && spaces < 4) { scan++; spaces++; }
        if (*scan != '[') return NULL;
        const char *close = strchr(scan + 1, ']');
        if (!close || close + 1 != position || *position != ':') return NULL;
        position++;
    }
    while (*position == ' ' || *position == '\t') position++;
    return position;
}

/* Length of the part of [start, pathEnd) that `oldTarget` names: all of it
   for an exact match, or - with matchDirectory - the leading folder of a
   path inside it ("docs" in "docs/intro.md"). 0 when it doesn't match. */
static size_t matched_target_length(const char *start, const char *pathEnd,
                                    const char *oldTarget, int matchDirectory) {
    size_t pathLength = (size_t)(pathEnd - start);
    if (utf8_case_equal_n(start, pathLength, oldTarget)) return pathLength;
    if (!matchDirectory) return 0;
    size_t oldLength = strlen(oldTarget);
    if (oldLength >= pathLength || (start[oldLength] != '/' && start[oldLength] != '\\'))
        return 0;
    return utf8_case_equal_n(start, oldLength, oldTarget) ? oldLength : 0;
}

static const char *next_text_line(const char *line) {
    while (*line && *line != '\r' && *line != '\n') line++;
    if (*line == '\r' && line[1] == '\n') return line + 2;
    return *line ? line + 1 : line;
}

/* A CommonMark fenced code marker may be indented by at most three spaces.
   Returns its repeated marker length, or 0 for an ordinary line. */
static size_t fence_marker(const char *line, char *marker) {
    int spaces = 0;
    while (*line == ' ' && spaces < 4) { line++; spaces++; }
    if (spaces > 3 || (*line != '`' && *line != '~')) return 0;
    *marker = *line;
    const char *start = line;
    while (*line == *marker) line++;
    size_t length = (size_t)(line - start);
    return length >= 3 ? length : 0;
}

static int closes_fence(const char *line, char marker, size_t minimumLength) {
    char candidate = 0;
    size_t length = fence_marker(line, &candidate);
    if (candidate != marker || length < minimumLength) return 0;
    while (*line == ' ') line++;
    line += length;
    while (*line == ' ' || *line == '\t') line++;
    return !*line || *line == '\r' || *line == '\n';
}

/* An unmatched backtick run is ordinary text, not the start of a code span.
   Code spans may cross a soft line break, but not the blank line that ends
   their paragraph. */
static int code_span_closes(const char *cursor, size_t openingTicks) {
    while (*cursor) {
        if (*cursor == '`') {
            const char *run = cursor;
            while (*cursor == '`') cursor++;
            if ((size_t)(cursor - run) == openingTicks) return 1;
            continue;
        }
        if (*cursor == '\r' || *cursor == '\n') {
            const char *next = next_text_line(cursor);
            const char *content = next;
            while (*content == ' ' || *content == '\t') content++;
            if (*content == '\r' || *content == '\n' || !*content) return 0;
            cursor = next;
            continue;
        }
        cursor++;
    }
    return 0;
}

static char *rewrite_link_targets(const char *text, const char *oldTarget,
                                  const char *newTarget, int matchDirectory,
                                  int *changeCount);

char *markdown_rewrite_link_targets(const char *text, const char *oldTarget,
                                    const char *newTarget, int *changeCount) {
    return rewrite_link_targets(text, oldTarget, newTarget, 0, changeCount);
}

char *markdown_rewrite_link_directory(const char *text, const char *oldDirectory,
                                      const char *newDirectory, int *changeCount) {
    return rewrite_link_targets(text, oldDirectory, newDirectory, 1, changeCount);
}

static char *rewrite_link_targets(const char *text, const char *oldTarget,
                                  const char *newTarget, int matchDirectory,
                                  int *changeCount) {
    if (changeCount) *changeCount = 0;
    if (!text || !oldTarget || !oldTarget[0] || !newTarget) return NULL;
    char *output = NULL;
    size_t outputLength = 0, outputCapacity = 0;
    const char *copiedThrough = text;
    const char *cursor = text;
    char fence = 0;
    size_t fenceLength = 0, codeTicks = 0;
    int changes = 0;
    while (*cursor) {
        if (!codeTicks && (cursor == text || cursor[-1] == '\r' || cursor[-1] == '\n')) {
            if (fence) {
                if (closes_fence(cursor, fence, fenceLength)) {
                    fence = 0;
                    fenceLength = 0;
                }
                cursor = next_text_line(cursor);
                continue;
            }
            char marker = 0;
            size_t markerLength = fence_marker(cursor, &marker);
            if (markerLength) {
                fence = marker;
                fenceLength = markerLength;
                cursor = next_text_line(cursor);
                continue;
            }
        }
        if (*cursor == '`') {
            const char *run = cursor;
            while (*cursor == '`') cursor++;
            size_t ticks = (size_t)(cursor - run);
            if (!codeTicks && code_span_closes(cursor, ticks)) codeTicks = ticks;
            else if (ticks == codeTicks) codeTicks = 0;
            continue;
        }
        if (codeTicks) { cursor++; continue; }
        if (*cursor != '(' && *cursor != ':') { cursor++; continue; }
        const char *start = destination_start(text, cursor);
        if (!start) { cursor++; continue; }
        int angled = *start == '<';
        if (angled) start++;
        const char *end = start;
        while (*end && (angled ? *end != '>' : *end != ')' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')) end++;
        const char *pathEnd = start;
        while (pathEnd < end && *pathEnd != '#' && *pathEnd != '?') pathEnd++;
        size_t matched = pathEnd > start
            ? matched_target_length(start, pathEnd, oldTarget, matchDirectory) : 0;
        if (matched) {
            /* A bare destination ends at whitespace and can't hold
               parentheses safely, so a new name with either is written in
               the <...> form, with the rest of the destination (the path
               inside a renamed folder, #fragment, ?query) kept inside it. */
            int wrap = !angled && strpbrk(newTarget, " \t()") != NULL;
            const char *rest = start + matched;
            const char *restEnd = wrap ? end : rest;
            if (!append_bytes(&output, &outputLength, &outputCapacity,
                              copiedThrough, (size_t)(start - copiedThrough)) ||
                (wrap && !append_bytes(&output, &outputLength, &outputCapacity, "<", 1)) ||
                !append_bytes(&output, &outputLength, &outputCapacity,
                              newTarget, strlen(newTarget)) ||
                !append_bytes(&output, &outputLength, &outputCapacity,
                              rest, (size_t)(restEnd - rest)) ||
                (wrap && !append_bytes(&output, &outputLength, &outputCapacity, ">", 1))) {
                free(output);
                return NULL;
            }
            copiedThrough = restEnd;
            changes++;
        }
        cursor = end > cursor ? end : cursor + 1;
    }
    if (!changes) { free(output); return NULL; }
    if (!append_bytes(&output, &outputLength, &outputCapacity,
                      copiedThrough, strlen(copiedThrough))) {
        free(output);
        return NULL;
    }
    if (changeCount) *changeCount = changes;
    return output;
}
