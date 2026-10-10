#ifndef MARKEDIT_PLUGIN_HOST_H
#define MARKEDIT_PLUGIN_HOST_H

#include <stddef.h>
#include <windows.h>

/* Shared with the built-in formatting Lua chunk, which registers about 50, and
   with functions in the menu definition. */
#define PLUGIN_MAX_COMMANDS 256
#define PLUGIN_MAX_HANDLERS 100
/* A plugin call (or a script's top-level code) is stopped after this long. */
#define PLUGIN_TIME_LIMIT_MS 5000

/* What the plugin API needs from the application. Any member may be NULL,
   in which case the matching Lua function raises an error. All text is UTF-8. */
typedef struct {
    /* Asks for a line of text; returns it (heap, caller frees) or NULL when
       cancelled. */
    char *(*prompt)(const char *title, const char *label, const char *initial);
    /* Returns the 0-based index of the chosen item, or -1 when cancelled. */
    int (*choose)(const char *title, const char *label, const char *const *items, int count);
    /* Path of the document being edited (heap, caller frees), or NULL when
       it has never been saved. */
    char *(*file_path)(void);
    /* Lower-case language name of the document being edited, e.g. "markdown". */
    const char *(*language)(void);
    /* Shows a message; kind is "info", "warning" or "error". */
    void (*message)(const char *title, const char *text, const char *kind);
    /* Reads/writes a value in this plugin's private persistent settings.
       config_get returns a heap string, or NULL when absent. config_set
       receives NULL to delete a value and returns nonzero on success. */
    char *(*config_get)(const char *plugin, const char *key);
    int (*config_set)(const char *plugin, const char *key, const char *value);
} PluginHostCallbacks;

/* Copied; call before running commands. */
void plugin_host_set_callbacks(const PluginHostCallbacks *callbacks);

/* ---- menu definitions (menus.lua) ---- */

typedef enum {
    PLUGIN_MENU_SEPARATOR = 0,
    PLUGIN_MENU_COMMAND,  /* a built-in command, by name */
    PLUGIN_MENU_FUNCTION, /* a Lua function, run as plugin command `command` */
    PLUGIN_MENU_SUBMENU,
    PLUGIN_MENU_LIST      /* generated items, by name, e.g. "recent_files" */
} PluginMenuKind;

typedef struct PluginMenuItem {
    PluginMenuKind kind;
    char *label;  /* UTF-8; NULL for separators and lists */
    char *name;   /* the command or list name */
    int command;
    struct PluginMenuItem *items; /* a submenu's contents */
    int itemCount;
} PluginMenuItem;

/* Runs a menu definition: a script returning the menu bar as a table of
   menus (see src/menus.lua for the format). Its functions become commands
   that plugin_host_command_is_placed() reports as already placed. Command
   and list names are not checked here. Call after plugin_host_shutdown(),
   before loading plugins. */
char *plugin_host_load_menu(const char *name, const char *code, size_t length);
/* The loaded definition's top-level menus, or NULL (count 0) if none. */
const PluginMenuItem *plugin_host_menu(int *count);

/* Functions returning char * give a heap-allocated UTF-8 error report
   (caller frees), or NULL on success. */

/* ---- generated pages (src/help/keyboard-shortcuts.lua) ---- */

/* A built-in command's shortcut text, for a page script. */
typedef struct {
    const char *name;     /* e.g. "file.new" */
    const char *shortcut; /* e.g. "Ctrl+N" */
} PluginShortcut;

/* Runs a built-in page script in a Lua state of its own (so plugins can't
   affect it) and sets *page to the Markdown it returns (heap, caller frees;
   NULL on error). The script's one argument is a table: `menus`, the loaded
   menu definition, and `shortcuts`, name -> shortcut text. */
char *plugin_host_render_page(const char *name, const char *code, size_t length,
                              const PluginShortcut *shortcuts, int shortcutCount, char **page);

/* Runs every *.lua file in directory in name order, adding to whatever is
   loaded (call plugin_host_shutdown() first to start afresh). A missing
   directory is not an error. Files that fail to load are reported and
   skipped; the rest stay loaded. */
char *plugin_host_load_directory(const WCHAR *directory);
/* Runs one plugin script's top-level code, starting the Lua state if needed. */
char *plugin_host_load_chunk(const char *name, const char *code, size_t length);
/* The same, for a script built into MarkEdit: its commands have no time limit. */
char *plugin_host_load_builtin(const char *name, const char *code, size_t length);
/* Discards every command, handler and the menu definition. Does nothing
   while plugin code is running. */
void plugin_host_shutdown(void);
/* Whether plugin code is running (it may be waiting in a dialog), so
   plugin_host_run() and plugin_host_fire() would do nothing now. */
int plugin_host_is_busy(void);

int plugin_host_command_count(void);
/* UTF-8 menu label of a registered command. For one from the menu
   definition it is the item's path, e.g. "/File/Insert Date". */
const char *plugin_host_command_label(int index);
/* Whether the menu definition already placed the command, so it shouldn't
   be placed again by its label. */
int plugin_host_command_is_placed(int index);
/* Whether the entry is a menu.separator(path) rather than a command: its
   label is the menu path and it has nothing to run. Separators share the
   command list so they keep their order
   among the commands placed around them. */
int plugin_host_command_is_separator(int index);
/* Whether a command was registered with menu.checkbox, and its persisted
   state. A missing/unavailable setting uses the checkbox's default. */
int plugin_host_command_is_checkable(int index);
int plugin_host_command_is_checked(int index);
/* The built-in command a command was registered to run for, or NULL. */
const char *plugin_host_command_overrides(int index);
/* The command registered (last) to run for the built-in command called
   name, or -1 when there is none. */
int plugin_host_find_override(const char *name);
/* Runs a command against hEdit as a single undo step; edits it made are
   undone if it fails. */
char *plugin_host_run(int index, HWND hEdit);
/* Runs every handler registered with events.on(event, fn), in registration
   order, each the way a command runs. argument is passed to them (nil when
   NULL). Does nothing while plugin code is already running, since events
   can arrive from a message loop a plugin dialog is running. */
char *plugin_host_fire(const char *event, const char *argument, HWND hEdit);

#endif
