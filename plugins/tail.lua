-- Follow a growing file like `tail -f`. MarkEdit's native watcher appends
-- decoded growth and handles rotation; this plugin owns the opt-in state,
-- checkbox policy and caret-at-EOF behaviour.

local function go_to_end()
  editor.set_caret(editor.length() + 1)
end

local function stop(message)
  if documents.follow() then documents.follow(false) end
  config.set("following", false)
  if message then ui.status(message) end
end

-- Follow state belongs to an open document and cannot survive a plugin reload.
config.set("following", false)

menu.checkbox("/Tools/Logs/Tail/Follow Current File", "following", false, function(checked)
  if not checked then
    stop("Stopped following file updates")
    return
  end
  if not editor.file_path() then
    config.set("following", false)
    ui.message("Save the document before following it.", "Tail", "warning")
    return
  end
  if documents.modified() then
    config.set("following", false)
    ui.message("Save or discard your changes before following this file.", "Tail", "warning")
    return
  end
  if not documents.follow(true) then
    config.set("following", false)
    ui.message("The file could not be followed.", "Tail", "error")
    return
  end
  go_to_end()
  ui.status("Following file updates")
end)

local function follow_end()
  if documents.follow() then go_to_end() end
end

events.on("open", follow_end)
events.on("tab_changed", function()
  config.set("following", documents.follow())
  follow_end()
end)

events.on("text_changed", function()
  if not documents.follow() then config.set("following", false) end
end)

events.on("selection_changed", function()
  if not documents.follow() then return end
  local first, last = editor.get_selection_range()
  if first <= last or editor.get_caret() ~= editor.length() + 1 then
    stop("Stopped following because the caret moved")
  end
end)
