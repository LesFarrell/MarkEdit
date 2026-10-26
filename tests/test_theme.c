#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plugin_host.h"
#include "../src/theme.h"

/* theme.h's inline helpers read these settings; the tests use the explicit
   theme_palette_for() instead. */
int g_editorDarkMode, g_previewDarkMode;

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

/* The app's set_theme_colour(), as plugin_bridge.inc defines it. */
static const char *set_colour(void *context, int dark, const char *name, const char *value) {
    (void)context;
    COLORREF colour;
    if (!theme_parse_colour(value, &colour)) return "the colour must be written \"#RRGGBB\"";
    if (!theme_set_colour(dark, name, colour)) return "there is no colour with this name";
    return NULL;
}

static char *load(const char *code) {
    return plugin_host_load_theme("themes.lua", code, strlen(code), set_colour, NULL);
}

static void test_built_in_palettes(void) {
    theme_reset_palettes();
    CHECK(theme_palette_for(0)->bg == RGB(0xFF, 0xFF, 0xFF));
    CHECK(theme_palette_for(1)->bg == RGB(0x16, 0x1B, 0x22));
    CHECK(theme_palette_for(0)->selectionBg == RGB(0xB6, 0xD7, 0xFF));
    CHECK(theme_palette_for(1)->keyword == RGB(0xFF, 0x7B, 0x72));
    CHECK(theme_palette_for(1)->operatorColour == RGB(0xE6, 0xED, 0xF3));
}

static void test_parse_colour(void) {
    COLORREF colour = 0;
    CHECK(theme_parse_colour("#A1b2C3", &colour) && colour == RGB(0xA1, 0xB2, 0xC3));
    CHECK(!theme_parse_colour("A1B2C3", &colour));
    CHECK(!theme_parse_colour("#A1B2C", &colour));
    CHECK(!theme_parse_colour("#A1B2C3D", &colour));
    CHECK(!theme_parse_colour("#GG0000", &colour));
    CHECK(!theme_parse_colour(NULL, &colour));
}

/* Loading the generated template changes nothing: it lists every colour at
   its built-in value. */
static void test_template_round_trip(void) {
    theme_reset_palettes();
    ThemePalette light = *theme_palette_for(0), dark = *theme_palette_for(1);
    char *template = theme_template_lua();
    CHECK(template && strstr(template, "selectionBg") && strstr(template, "\"#264F78\""));
    char *report = template ? load(template) : NULL;
    if (report) fprintf(stderr, "%s\n", report);
    CHECK(report == NULL);
    CHECK(memcmp(&light, theme_palette_for(0), sizeof(light)) == 0);
    CHECK(memcmp(&dark, theme_palette_for(1), sizeof(dark)) == 0);
    free(report);
    free(template);
}

static void test_partial_override(void) {
    theme_reset_palettes();
    COLORREF lightKeyword = theme_palette_for(0)->keyword;
    COLORREF darkBg = theme_palette_for(1)->bg;
    char *report = load("return { dark = { keyword = \"#010203\" } }");
    CHECK(report == NULL);
    CHECK(theme_palette_for(1)->keyword == RGB(0x01, 0x02, 0x03));
    CHECK(theme_palette_for(1)->bg == darkBg);
    CHECK(theme_palette_for(0)->keyword == lightKeyword);
    free(report);
    theme_reset_palettes();
    CHECK(theme_palette_for(1)->keyword == RGB(0xFF, 0x7B, 0x72));
}

/* Every mistake is reported, and the colours that were fine still apply. */
static void test_problems(void) {
    theme_reset_palettes();
    char *report = load(
        "return {\n"
        "  light = { bg = \"#101010\", nope = \"#000000\", fg = \"red\", border = 5 },\n"
        "  blue = { bg = \"#000000\" },\n"
        "  dark = \"#000000\",\n"
        "}");
    CHECK(report && strstr(report, "themes.lua: light.nope: there is no colour with this name"));
    CHECK(report && strstr(report, "light.fg: the colour must be written"));
    CHECK(report && strstr(report, "light.border: the colour must be a string"));
    CHECK(report && strstr(report, "themes.lua: blue: only \"light\" and \"dark\" themes exist"));
    CHECK(report && strstr(report, "themes.lua: dark: must be a table of colours"));
    CHECK(theme_palette_for(0)->bg == RGB(0x10, 0x10, 0x10));
    free(report);

    report = load("return 5");
    CHECK(report && strstr(report, "must return a table of themes"));
    free(report);

    report = load("return {");
    CHECK(report && strstr(report, "themes.lua:"));
    free(report);

    report = load("while true do end");
    CHECK(report && strstr(report, "did not finish"));
    free(report);
    /* Catching the limit's error doesn't let the file carry on. */
    report = load("while true do pcall(function() while true do end end) end");
    CHECK(report && strstr(report, "did not finish"));
    free(report);

    /* It has the plugins' memory limit, so a huge allocation is reported. */
    report = load("local s = string.rep('x', 1 << 30) return {}");
    CHECK(report && strstr(report, "memory"));
    free(report);

    /* The file sees only the base and string libraries. */
    report = load("return { light = { bg = os.getenv('X') } }");
    CHECK(report != NULL);
    free(report);

    /* ...with the same file and bytecode restrictions plugins have. */
    report = load(
        "assert(dofile == nil and loadfile == nil, 'file loaders removed')\n"
        "assert(load(string.dump(function() end)) == nil, 'binary chunks refused')\n"
        "return {}");
    if (report) fprintf(stderr, "%s\n", report);
    CHECK(report == NULL);
    free(report);
    theme_reset_palettes();
}

/* The shipped example loads without a single problem and really applies. */
static void test_example_theme(void) {
    FILE *file = fopen("plugins/examples/themes.lua", "rb");
    CHECK(file != NULL);
    if (!file) return;
    char code[16384];
    size_t length = fread(code, 1, sizeof(code) - 1, file);
    fclose(file);
    code[length] = 0;
    theme_reset_palettes();
    char *report = load(code);
    if (report) fprintf(stderr, "%s\n", report);
    CHECK(report == NULL);
    CHECK(theme_palette_for(1)->bg == RGB(0x00, 0x2B, 0x36));
    CHECK(theme_palette_for(0)->bg == RGB(0xFD, 0xF6, 0xE3));
    CHECK(theme_palette_for(0)->keyword == RGB(0x85, 0x99, 0x00));
    /* mix(): a quarter of the way from base3 to base00. */
    CHECK(theme_palette_for(0)->border == RGB(0xD7, 0xD7, 0xCB));
    free(report);
    theme_reset_palettes();
}

int main(void) {
    test_built_in_palettes();
    test_example_theme();
    test_parse_colour();
    test_template_round_trip();
    test_partial_override();
    test_problems();
    if (failures) return 1;
    puts("All theme tests passed.");
    return 0;
}
