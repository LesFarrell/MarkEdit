#include "markdown_links.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FenceState {
    char marker;
    int length;
} FenceState;

static const char *line_end(const char *line) {
    const char *end = line;
    while (*end && *end != '\r' && *end != '\n') end++;
    return end;
}

static const char *next_line(const char *end) {
    if (*end == '\r' && end[1] == '\n') return end + 2;
    return *end ? end + 1 : end;
}

static const char *skip_indent(const char *line, const char *end) {
    int spaces = 0;
    while (line < end && spaces < 3 && *line == ' ') { line++; spaces++; }
    return line;
}

static int fence_run(const char *line, const char *end, char *marker) {
    const char *p = skip_indent(line, end);
    if (p == end || (*p != '`' && *p != '~')) return 0;
    *marker = *p;
    const char *start = p;
    while (p < end && *p == *marker) p++;
    return (int)(p - start);
}

static int update_fence(FenceState *fence, const char *line, const char *end) {
    char marker = 0;
    int run = fence_run(line, end, &marker);
    if (!fence->marker) {
        if (run >= 3) { fence->marker = marker; fence->length = run; return 1; }
        return 0;
    }
    if (marker == fence->marker && run >= fence->length) {
        fence->marker = 0;
        fence->length = 0;
    }
    return 1;
}

static const char *trim_start(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    return start;
}

static const char *trim_end(const char *start, const char *end) {
    while (end > start && isspace((unsigned char)end[-1])) end--;
    return end;
}

static void visit_target(MarkdownLinkVisitor visitor, MarkdownLinkKind kind, int line,
                         const char *start, const char *end, void *context) {
    start = trim_start(start, end);
    end = trim_end(start, end);
    if (start < end) visitor(kind, line, start, (size_t)(end - start), context);
}

/* Returns the byte after the matching inline-code delimiter. An unmatched
   run is ordinary Markdown text, so only its first backtick is skipped. */
static const char *skip_inline_code(const char *p, const char *end) {
    const char *runEnd = p;
    while (runEnd < end && *runEnd == '`') runEnd++;
    size_t run = (size_t)(runEnd - p);
    for (const char *q = runEnd; q < end; q++) {
        if (*q != '`') continue;
        const char *candidate = q;
        while (q < end && *q == '`') q++;
        if ((size_t)(q - candidate) == run) return q;
        q--;
    }
    return p + 1;
}

static const char *markdown_destination(const char *start, const char *end,
                                        const char **targetEnd) {
    const char *p = trim_start(start, end);
    if (p >= end) return NULL;
    if (*p == '<') {
        const char *close = p + 1;
        while (close < end && *close != '>') close++;
        if (close == end) return NULL;
        *targetEnd = close;
        return p + 1;
    }
    const char *target = p;
    int nesting = 0;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == '(') { nesting++; p++; continue; }
        if (*p == ')') {
            if (!nesting) break;
            nesting--; p++; continue;
        }
        if (!nesting && isspace((unsigned char)*p)) break;
        p++;
    }
    *targetEnd = p;
    return target;
}

static void visit_reference_definition(const char *line, const char *end, int lineNumber,
                                       MarkdownLinkVisitor visitor, void *context) {
    const char *p = skip_indent(line, end);
    if (p >= end || *p != '[' || (p > line && p[-1] == '!')) return;
    const char *close = memchr(p + 1, ']', (size_t)(end - p - 1));
    if (!close || close + 1 >= end || close[1] != ':') return;
    const char *targetEnd = NULL;
    const char *target = markdown_destination(close + 2, end, &targetEnd);
    if (target) visit_target(visitor, MARKDOWN_LINK_FILE, lineNumber,
                             target, targetEnd, context);
}

static void visit_line_links(const char *line, const char *end, int lineNumber,
                             MarkdownLinkVisitor visitor, void *context) {
    visit_reference_definition(line, end, lineNumber, visitor, context);
    for (const char *p = line; p < end;) {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == '`') { p = skip_inline_code(p, end); continue; }
        if (p + 1 < end && p[0] == '[' && p[1] == '[') {
            const char *close = p + 2;
            while (close + 1 < end && !(close[0] == ']' && close[1] == ']')) close++;
            if (close + 1 < end) {
                const char *separator = memchr(p + 2, '|', (size_t)(close - (p + 2)));
                visit_target(visitor, MARKDOWN_LINK_WIKI, lineNumber, p + 2,
                             separator ? separator : close, context);
                p = close + 2;
                continue;
            }
        }
        if (*p == '[' || (*p == '!' && p + 1 < end && p[1] == '[')) {
            MarkdownLinkKind kind = *p == '!' ? MARKDOWN_LINK_IMAGE : MARKDOWN_LINK_FILE;
            const char *label = p + (kind == MARKDOWN_LINK_IMAGE ? 2 : 1);
            const char *close = memchr(label, ']', (size_t)(end - label));
            if (close && close + 1 < end && close[1] == '(') {
                const char *targetEnd = NULL;
                const char *target = markdown_destination(close + 2, end, &targetEnd);
                if (target) visit_target(visitor, kind, lineNumber, target, targetEnd, context);
                const char *finish = targetEnd;
                while (finish < end && *finish != ')') finish++;
                p = finish < end ? finish + 1 : end;
                continue;
            }
        }
        p++;
    }
}

void markdown_links_visit(const char *text, MarkdownLinkVisitor visitor, void *context) {
    if (!text || !visitor) return;
    FenceState fence = {0};
    int lineNumber = 1;
    for (const char *line = text; *line;) {
        const char *end = line_end(line);
        if (!update_fence(&fence, line, end) && !fence.marker)
            visit_line_links(line, end, lineNumber, visitor, context);
        line = next_line(end);
        lineNumber++;
    }
}

static char *plain_heading(const char *start, const char *end) {
    while (end > start && (* (end - 1) == '#' || isspace((unsigned char)end[-1]))) end--;
    while (start < end && isspace((unsigned char)*start)) start++;
    size_t capacity = (size_t)(end - start) + 1;
    char *plain = (char *)malloc(capacity);
    if (!plain) return NULL;
    size_t out = 0;
    for (const char *p = start; p < end; p++) {
        if (*p == '\\' && p + 1 < end) { plain[out++] = *++p; continue; }
        if (*p == '*' || *p == '`' || *p == '~') continue;
        if (*p == '!' && p + 1 < end && p[1] == '[') p++;
        if (*p == '[') {
            const char *close = memchr(p + 1, ']', (size_t)(end - p - 1));
            if (close) {
                const char *q = p + 1;
                while (q < close) plain[out++] = *q++;
                p = close;
                if (p + 1 < end && p[1] == '(') {
                    p += 2;
                    int nesting = 0;
                    while (p < end) {
                        if (*p == '(') nesting++;
                        else if (*p == ')' && nesting-- == 0) break;
                        p++;
                    }
                }
                continue;
            }
        }
        plain[out++] = *p;
    }
    plain[out] = 0;
    return plain;
}

/* A compact UTF-8 approximation of preview_shell.html's headingSlug(): ASCII
   punctuation becomes one '-', apostrophes disappear, and non-ASCII UTF-8
   bytes are retained. */
static char *heading_slug(const char *heading) {
    size_t length = strlen(heading);
    char *slug = (char *)malloc(length + 8);
    if (!slug) return NULL;
    size_t out = 0;
    int pendingDash = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)heading[i];
        if (c >= 0x80) {
            if (pendingDash && out) slug[out++] = '-';
            pendingDash = 0;
            slug[out++] = (char)c;
        } else if (isalnum(c)) {
            if (pendingDash && out) slug[out++] = '-';
            pendingDash = 0;
            slug[out++] = (char)tolower(c);
        } else if (c == '\'' || c == '`') {
            /* Removed rather than treated as a word separator. */
        } else {
            pendingDash = out != 0;
        }
    }
    if (!out) memcpy(slug, "section", (out = 7));
    slug[out] = 0;
    return slug;
}

typedef struct SlugCount {
    char *slug;
    int count;
} SlugCount;

static int anchor_matches(char *plain, const char *fragment,
                          SlugCount **counts, int *count, int *capacity) {
    char *slug = heading_slug(plain);
    free(plain);
    if (!slug) return 0;
    int index = -1;
    for (int i = 0; i < *count; i++)
        if (strcmp((*counts)[i].slug, slug) == 0) { index = i; break; }
    if (index < 0) {
        if (*count >= *capacity) {
            int next = *capacity ? *capacity * 2 : 16;
            SlugCount *grown = (SlugCount *)realloc(*counts, (size_t)next * sizeof(**counts));
            if (!grown) { free(slug); return 0; }
            *counts = grown; *capacity = next;
        }
        index = (*count)++;
        (*counts)[index].slug = slug;
        (*counts)[index].count = 0;
    } else free(slug);
    int suffix = (*counts)[index].count++;
    const char *base = (*counts)[index].slug;
    if (!suffix) return strcmp(base, fragment) == 0;
    size_t needed = strlen(base) + 24;
    char *full = (char *)malloc(needed);
    if (!full) return 0;
    snprintf(full, needed, "%s-%d", base, suffix);
    int matches = strcmp(full, fragment) == 0;
    free(full);
    return matches;
}

int markdown_heading_anchor_exists(const char *text, const char *fragment) {
    if (!text || !fragment || !*fragment) return 0;
    FenceState fence = {0};
    SlugCount *counts = NULL;
    int count = 0, capacity = 0, found = 0;
    const char *previousStart = NULL, *previousEnd = NULL;
    for (const char *line = text; *line && !found;) {
        const char *end = line_end(line);
        int wasFenced = fence.marker != 0;
        int fenceLine = update_fence(&fence, line, end);
        if (!fenceLine && !wasFenced && !fence.marker) {
            const char *p = skip_indent(line, end);
            if (p < end && *p == '#') {
                const char *hashes = p;
                while (p < end && *p == '#') p++;
                if (p - hashes <= 6 && p < end && isspace((unsigned char)*p)) {
                    char *plain = plain_heading(p, end);
                    if (plain) found = anchor_matches(plain, fragment, &counts, &count, &capacity);
                }
            } else if (previousStart && p < end) {
                char marker = *p;
                const char *q = p;
                if (marker == '=' || marker == '-') {
                    while (q < end && *q == marker) q++;
                    if (q == end && q - p >= 2) {
                        char *plain = plain_heading(previousStart, previousEnd);
                        if (plain) found = anchor_matches(plain, fragment, &counts, &count, &capacity);
                    }
                }
            }
            previousStart = p < end ? line : NULL;
            previousEnd = end;
        } else {
            previousStart = previousEnd = NULL;
        }
        line = next_line(end);
    }
    for (int i = 0; i < count; i++) free(counts[i].slug);
    free(counts);
    return found;
}
