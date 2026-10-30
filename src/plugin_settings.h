#ifndef TINKEREDIT_PLUGIN_SETTINGS_H
#define TINKEREDIT_PLUGIN_SETTINGS_H

#include <windows.h>

/* The sectioned JSON store shared by application preferences and Lua plugins.
   Values stay as strings so existing settings readers and the Lua config API
   keep their established conversion rules. Returned strings are heap allocated. */
char *plugin_settings_get(const WCHAR *path, const char *plugin, const char *key);
int plugin_settings_set(const WCHAR *path, const char *plugin, const char *key,
                        const char *value);
int plugin_settings_clear_section(const WCHAR *path, const char *plugin);

typedef struct PluginSettingEntry {
    char *section;
    char *key;
    char *value;
} PluginSettingEntry;

/* Returns copies of every setting whose section starts with prefix. The caller
   owns the returned array and releases it with plugin_settings_free_entries(). */
int plugin_settings_list(const WCHAR *path, const char *prefix,
                         PluginSettingEntry **entries, int *count);
void plugin_settings_free_entries(PluginSettingEntry *entries, int count);

#endif
