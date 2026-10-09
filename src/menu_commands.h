#ifndef MARKEDIT_MENU_COMMANDS_H
#define MARKEDIT_MENU_COMMANDS_H

#include <windows.h>

#include "plugin_host.h"

/* A built-in command, as menus.lua names it. */
typedef struct {
    const char *name;     /* e.g. "file.new" */
    WORD id;              /* its IDM_ command ID */
    const char *shortcut; /* shown after its label, or NULL; the keys
                             themselves are handled elsewhere */
} MenuCommand;

const MenuCommand *menu_command_find(const char *name);
const MenuCommand *menu_command_for_id(WORD id);
/* NULL past the end, so callers can loop until it returns NULL. */
const MenuCommand *menu_command_at(int index);

/* Problems in a loaded menu definition that the plugin host can't see:
   unknown command and list names. A heap UTF-8 report (caller frees), or
   NULL when there are none. */
char *menu_definition_problems(const PluginMenuItem *items, int count);

#endif
