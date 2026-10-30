-- View > Follow EOF, like tail -f: the active document keeps showing
-- the end of its file while another program adds to it, which is handy for
-- watching a log. Tinker Editor checks open files for changes every second and a
-- half; a followed one reloads without asking (documents.auto_reload) and
-- moves to its last line. Typing into it pauses that, since Tinker Editor then
-- asks before reloading over unsaved edits. Choose the command again to
-- stop; following also ends when the tab closes. The check mark shows
-- whether the active tab is followed.
--
-- Uses only the plugin API documented in README.md in this folder.

-- Followed files, by lower-cased path, for the documents set to reload.
local followed = {}

-- The menu.checkbox state, which Tinker Editor saves; following never outlives
-- the session, so it starts cleared.
local CHECK_KEY = "following"
config.set(CHECK_KEY, false)

local function key(path)
  return path and path:lower()
end

local function following()
  local path = editor.file_path()
  return path ~= nil and followed[key(path)] == true and documents.auto_reload()
end

local function go_to_end()
  editor.scroll_to(editor.length() + 1)
end

menu.checkbox("/View/Follow EOF", CHECK_KEY, false, function()
  local path = editor.file_path()
  if not path then
    config.set(CHECK_KEY, false)
    ui.message("Save the document first: only a file on disk can be followed.", nil, "warning")
    return
  end
  local on = not following()
  followed[key(path)] = on or nil
  documents.auto_reload(on)
  config.set(CHECK_KEY, on)
  if on then go_to_end() end
  ui.status(on and "Following the end of the file." or "Stopped following the file.")
end)

events.on("reload", function(path)
  if path and followed[key(path)] then go_to_end() end
end)

events.on("tab_changed", function()
  config.set(CHECK_KEY, following())
end)

events.on("close", function(path)
  if path then followed[key(path)] = nil end
end)
