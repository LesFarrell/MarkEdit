/* Lua extension host: extension discovery (*.lua files and folders with
   init.lua) and the folder-local module loader behind require(). */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "file_io.h"
#include "path_utils.h"
#include "plugin_host_internal.h"
#include "text_codec.h"

#include "lauxlib.h"
#include "lualib.h"

PluginExtension s_extensions[256];
int s_extensionCount = 0;
PluginContext s_contexts[256];
int s_contextCount = 0;
WCHAR *s_firstErrorPath = NULL;
int s_firstErrorLine = 0;

static PluginContext *find_context(const char *owner) {
    if (!owner) return NULL;
    for (int i = 0; i < s_contextCount; i++)
        if (strcmp(s_contexts[i].owner, owner) == 0) return &s_contexts[i];
    return NULL;
}

int remember_context(const char *owner, const WCHAR *moduleRoot) {
    PluginContext *context = find_context(owner);
    if (context) {
        if (moduleRoot && !context->moduleRoot) context->moduleRoot = wcsdup_heap(moduleRoot);
        return !moduleRoot || context->moduleRoot != NULL;
    }
    if (s_contextCount >= (int)(sizeof(s_contexts) / sizeof(s_contexts[0]))) return 0;
    context = &s_contexts[s_contextCount];
    context->owner = dup_string(owner);
    context->moduleRoot = moduleRoot ? wcsdup_heap(moduleRoot) : NULL;
    if (!context->owner || (moduleRoot && !context->moduleRoot)) {
        free(context->owner); free(context->moduleRoot);
        memset(context, 0, sizeof(*context));
        return 0;
    }
    s_contextCount++;
    return 1;
}

static int valid_module_name(const char *module) {
    size_t length = strlen(module);
    if (!length || length > 128 || module[0] == '.' || module[length - 1] == '.') return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)module[i];
        if (!(isalnum(c) || c == '_' || c == '.') ||
            (c == '.' && i && module[i - 1] == '.')) return 0;
    }
    return 1;
}

static char s_moduleLoadingSentinel;

/* Leaves the module cache table on the stack, and a cached value above it
   when one exists. */
static int push_module_cache(lua_State *L, const char *cacheKey) {
    lua_getfield(L, LUA_REGISTRYINDEX, "markedit.modules");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "markedit.modules");
    }
    lua_getfield(L, -1, cacheKey);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    return 0;
}

static WCHAR *module_source_path(const WCHAR *moduleRoot, const char *module, char relative[260]) {
    snprintf(relative, 260, "%s.lua", module);
    for (char *p = relative; *p; p++) if (*p == '.') *p = '/';
    /* Restore the extension dot replaced by the loop above. */
    memcpy(relative + strlen(relative) - 4, ".lua", 5);
    WCHAR *wide = to_wide(relative);
    WCHAR *path = wide ? path_join(moduleRoot, wide) : NULL;
    free(wide);
    return path;
}

/* A small replacement for Lua's package loader. It reads source modules only
   below the folder extension whose init.lua is currently loading. */
int plugin_require(lua_State *L) {
    const char *module = checked_cstring(L, 1, "module name");
    const char *owner = s_loadingName ? s_loadingName : s_runningOwner;
    PluginContext *context = find_context(owner);
    if (!context || !context->moduleRoot)
        return luaL_error(L, "require is available only from a folder extension");
    if (!valid_module_name(module))
        return luaL_error(L, "module names may contain letters, digits, '_' and single dots");
    char cacheKey[520];
    snprintf(cacheKey, sizeof(cacheKey), "%s\n%s", owner, module);
    if (push_module_cache(L, cacheKey)) {
        if (lua_touserdata(L, -1) == &s_moduleLoadingSentinel)
            return luaL_error(L, "circular require of module '%s'", module);
        return 1;
    }
    lua_pushlightuserdata(L, &s_moduleLoadingSentinel);
    lua_setfield(L, -2, cacheKey);

    char relative[260];
    WCHAR *path = module_source_path(context->moduleRoot, module, relative);
    size_t codeLength = 0;
    char *code = path ? read_utf8_text_file_length(path, PLUGIN_MAX_SCRIPT_BYTES, &codeLength) : NULL;
    free(path);
    if (!code) {
        lua_pushnil(L); lua_setfield(L, -2, cacheKey);
        return luaL_error(L, "module '%s' was not found or is not valid UTF-8", module);
    }
    char chunkName[300];
    snprintf(chunkName, sizeof(chunkName), "=%s/%s", owner, relative);
    int status = luaL_loadbufferx(L, code, codeLength, chunkName, "t");
    free(code);
    if (status != LUA_OK) {
        lua_pushnil(L); lua_setfield(L, -3, cacheKey);
        return lua_error(L);
    }
    set_chunk_environment(L, -1, owner);
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
        lua_pushnil(L); lua_setfield(L, -3, cacheKey);
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, cacheKey);
    return 1;
}

typedef struct {
    WCHAR *name;
    int folder;
} ExtensionCandidate;

static int compare_candidates(const void *a, const void *b) {
    return _wcsicmp(((const ExtensionCandidate *)a)->name,
                    ((const ExtensionCandidate *)b)->name);
}

/* FindFirstFileW's "*.lua" also matches longer extensions such as ".luac"
   through 8.3 short names, so the extension is checked exactly. */
static int is_lua_script(const WIN32_FIND_DATAW *found) {
    if (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    const WCHAR *dot = wcsrchr(found->cFileName, L'.');
    return dot && _wcsicmp(dot, L".lua") == 0;
}

static int is_folder_extension(const WCHAR *directory, const WCHAR *name) {
    WCHAR *folder = path_join(directory, name);
    WCHAR *entry = folder ? path_join(folder, L"init.lua") : NULL;
    DWORD attributes = entry ? GetFileAttributesW(entry) : INVALID_FILE_ATTRIBUTES;
    free(entry);
    free(folder);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void remember_load_error(const WCHAR *path, const WCHAR *moduleRoot,
                                const char *error, const char *shownName) {
    if (s_firstErrorPath || !path || !error) return;
    const char *at = shownName ? strstr(error, shownName) : NULL;
    if (at) {
        at += strlen(shownName);
        if (*at == '/' && moduleRoot) {
            const char *colon = strchr(at + 1, ':');
            if (colon && colon > at + 1 && (size_t)(colon - at) < 260) {
                char relative[260];
                memcpy(relative, at + 1, (size_t)(colon - at - 1));
                relative[colon - at - 1] = 0;
                WCHAR *wide = to_wide(relative);
                if (wide) {
                    for (WCHAR *p = wide; *p; p++) if (*p == L'/') *p = L'\\';
                    s_firstErrorPath = path_join(moduleRoot, wide);
                }
                free(wide);
                at = colon;
            }
        }
        if (*at == ':') {
            char *end = NULL;
            long line = strtol(at + 1, &end, 10);
            if (end != at + 1 && line > 0 && line <= INT_MAX) s_firstErrorLine = (int)line;
        }
    }
    if (!s_firstErrorPath) s_firstErrorPath = wcsdup_heap(path);
}

static char *scoped_owner(const char *scope, const char *name) {
    if (!scope || !*scope || _stricmp(scope, "bundled") == 0) return dup_string(name);
    size_t size = strlen(scope) + strlen(name) + 2;
    char *owner = (char *)malloc(size);
    if (owner) snprintf(owner, size, "%s/%s", scope, name);
    return owner;
}

static char *load_script_file(const WCHAR *directory, const WCHAR *fileName,
                              const char *scope, int folder) {
    char *plainName = to_utf8(fileName);
    char *name = plainName ? scoped_owner(scope, plainName) : NULL;
    WCHAR *root = folder ? path_join(directory, fileName) : NULL;
    WCHAR *path = folder ? path_join(root, L"init.lua") : path_join(directory, fileName);
    size_t codeLength = 0;
    char *code = path ? read_utf8_text_file_length(path, PLUGIN_MAX_SCRIPT_BYTES, &codeLength) : NULL;
    char *error;
    if (code) {
        error = run_script(name ? name : "plugin", code, codeLength, NULL, root);
    } else {
        const char *shown = name ? name : "A plugin";
        error = (char *)malloc(strlen(shown) + 64);
        if (error) sprintf(error, "%s: could not be read as UTF-8 text (1 MiB at most)", shown);
    }
    remember_load_error(path, root, error, name);
    free(code);
    free(path);
    free(root);
    free(name);
    free(plainName);
    return error;
}

static int remember_extension(const char *scope, const char *name, int enabled) {
    if (s_extensionCount >= (int)(sizeof(s_extensions) / sizeof(s_extensions[0]))) return 0;
    PluginExtension *extension = &s_extensions[s_extensionCount];
    extension->name = dup_string(name);
    extension->scope = dup_string(scope ? scope : "");
    if (!extension->name || !extension->scope) {
        free(extension->name);
        free(extension->scope);
        memset(extension, 0, sizeof(*extension));
        return 0;
    }
    extension->enabled = enabled;
    s_extensionCount++;
    return 1;
}

char *plugin_host_load_directory(const WCHAR *directory, const char *scope) {
    WCHAR *pattern = path_join(directory, L"*");
    if (!pattern) return dup_string("out of memory");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return NULL;

    ExtensionCandidate *candidates = NULL;
    size_t count = 0, capacity = 0;
    char *report = NULL;
    do {
        int folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (folder && (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)) continue;
        if (folder ? !is_folder_extension(directory, found.cFileName) : !is_lua_script(&found)) continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            ExtensionCandidate *larger = (ExtensionCandidate *)realloc(
                candidates, grown * sizeof(*candidates));
            if (!larger) { report = dup_string("out of memory listing plugins"); break; }
            candidates = larger;
            capacity = grown;
        }
        WCHAR *name = wcsdup_heap(found.cFileName);
        if (name) candidates[count++] = (ExtensionCandidate){ name, folder };
    } while (FindNextFileW(find, &found));
    FindClose(find);

    /* Name order, so scripts load (and their menu items appear) predictably. */
    if (count) qsort(candidates, count, sizeof(*candidates), compare_candidates);
    for (size_t i = 0; i < count; i++) {
        char *name = to_utf8(candidates[i].name);
        int enabled = !s_callbacks.plugin_enabled ||
            s_callbacks.plugin_enabled(scope ? scope : "", name ? name : "plugin");
        remember_extension(scope, name ? name : "plugin", enabled);
        if (enabled) report = append_line(report,
            load_script_file(directory, candidates[i].name, scope, candidates[i].folder));
        free(name);
        free(candidates[i].name);
    }
    free(candidates);
    return report;
}

int plugin_host_extension_count(void) { return s_extensionCount; }
const char *plugin_host_extension_name(int index) {
    return index >= 0 && index < s_extensionCount ? s_extensions[index].name : NULL;
}
const char *plugin_host_extension_scope(int index) {
    return index >= 0 && index < s_extensionCount ? s_extensions[index].scope : NULL;
}
int plugin_host_extension_is_enabled(int index) {
    return index >= 0 && index < s_extensionCount && s_extensions[index].enabled;
}
const WCHAR *plugin_host_error_path(void) { return s_firstErrorPath; }
int plugin_host_error_line(void) { return s_firstErrorLine; }