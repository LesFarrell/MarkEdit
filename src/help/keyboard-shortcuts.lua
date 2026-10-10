-- Help > Keyboard Shortcuts: builds the page as Markdown each time it opens.
--
-- The page is generated rather than written out by hand, so it always
-- matches the shortcuts MarkEdit really has and the menus the user really
-- sees, including a customised %APPDATA%\markedit\menus.lua.
--
-- How it's run: markedit.rc builds this file into markedit.exe, and
-- plugin_host_render_page() runs it in a Lua state of its own (not the one
-- plugins share), with the base, string, table and utf8 libraries only. It
-- gets one argument, a table:
--
--   menus      the loaded menu definition, as a list of top-level menus.
--              Each item is a table with a `kind`:
--                "submenu"    label, items
--                "command"    label, command (a built-in name, "file.new")
--                "function"   label (Lua code in the definition)
--                "list"       list ("recent_files", "plugins", ...)
--                "separator"
--   shortcuts  built-in command name -> shortcut text, for every command
--              that has one, e.g. shortcuts["file.new"] == "Ctrl+N"
--              (from src/menu_commands.c)
--
-- and returns the page. Changes here need a rebuild; tests/test_menus.c
-- checks the page lists every shortcut.

local data = ...
local menus, shortcuts = data.menus, data.shortcuts

-- Keys that aren't built-in commands, so neither the menus nor the command
-- table know about them.
local OTHER_KEYS = {
  { "Tab / Shift+Tab", "Smart indent / outdent" },
  { "Ctrl+Shift+F6", "Cycle panes backwards" },
  { "Ctrl+Numpad + / Ctrl+Numpad -", "Zoom in / out" },
}

-- A menu label as shown: "&&" is a literal "&", any other "&" only marks
-- the access key.
local function plain(label)
  return (label:gsub("&(.?)", "%1"))
end

-- Text safe inside a Markdown table cell.
local function cell(text)
  return (text:gsub("[\\|`*_]", "\\%0"))
end

local out = {}
local function line(text) out[#out + 1] = text or "" end

-- A "## heading" and a two-column table of rows ({ keys, action } pairs),
-- or nothing when there are no rows.
local function section(heading, rows)
  if #rows == 0 then return end
  line("## " .. heading)
  line()
  line("| Shortcut | Action |")
  line("|---|---|")
  for _, row in ipairs(rows) do line("| " .. cell(row[1]) .. " | " .. cell(row[2]) .. " |") end
  line()
end

-- The rows for every command with a shortcut in items, depth first. An
-- item in a submenu is named by its path: "Bookmarks > Next Bookmark".
local listed = {}
local function collect(items, path, rows)
  for _, item in ipairs(items) do
    if item.kind == "submenu" then
      collect(item.items, path .. plain(item.label) .. " > ", rows)
    elseif item.kind == "command" and shortcuts[item.command] and not listed[item.command] then
      listed[item.command] = true
      rows[#rows + 1] = { shortcuts[item.command], path .. plain(item.label) }
    end
  end
  return rows
end

line("# MarkEdit Keyboard Shortcuts")
line()
line("Every command is also in the menus.")
line("This page is built from MarkEdit's current menus each time it opens. It's an")
line("unsaved document, so editing it changes nothing.")
line()

for _, menu in ipairs(menus) do
  section(plain(menu.label), collect(menu.items, "", {}))
end

-- Commands left out of the menus keep their shortcuts.
local unlisted = {}
for name, keys in pairs(shortcuts) do
  if not listed[name] then unlisted[#unlisted + 1] = { keys, name } end
end
table.sort(unlisted, function(a, b) return a[2] < b[2] end)
section("Not in the menus", unlisted)

section("Other keys", OTHER_KEYS)

return table.concat(out, "\n")
