-- Example bundled MarkEdit plugin. Top-level *.lua files and folders with
-- init.lua load at startup and again through Plugins > Reload Plugins.
--
-- It shows each kind of thing a plugin can do:
--   menu.add            commands on the Plugins menu, in submenus (Lines,
--                       Text), in a built-in submenu (Snippets > Markdown)
--                       and in a built-in top-level menu (Snippets)
--   menu.separator      a line between groups of commands (in Lines)
--   contextmenu.*       an editor right-click command that can be removed
--   ui.*                a message, a text prompt and a list to choose from
--   events.on           a save handler, switched on by a command
--
-- The full API, with every function, its arguments and its limits, is in
-- README.md in this folder (also Help > Plugin Guide).

-- Splits on the document's line ending. The second result says whether the
-- text ended with one, so joining can put it back.
local function split_lines(text, eol)
  local lines, start = {}, 1
  while true do
    local first, last = text:find(eol, start, true)
    if not first then break end
    lines[#lines + 1] = text:sub(start, first - 1)
    start = last + 1
  end
  local rest = text:sub(start)
  if rest ~= "" then lines[#lines + 1] = rest end
  return lines, rest == "" and #lines > 0
end

local function join_lines(lines, eol, trailing)
  return table.concat(lines, eol) .. (trailing and eol or "")
end

-- ---- Plugins > Lines ----

menu.add("Lines/Reverse Selected Lines", function()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select some lines first.")
    return
  end
  local eol = editor.eol()
  local lines, trailing = split_lines(text, eol)
  local reversed = {}
  for i = #lines, 1, -1 do reversed[#reversed + 1] = lines[i] end
  editor.replace_selection(join_lines(reversed, eol, trailing))
end)

-- ---- Plugins > Text ----

menu.add("Text/Wrap Selection...", function()
  local marker = ui.prompt("Wrap the selection in:", "**")
  if not marker or marker == "" then return end
  -- Closing markers mirror opening brackets, so "(" wraps as "(...)".
  -- Reversed by character, not byte, so UTF-8 markers like "“" stay valid.
  local reversed = {}
  for _, code in utf8.codes(marker) do table.insert(reversed, 1, utf8.char(code)) end
  local closing = table.concat(reversed):gsub(utf8.charpattern,
    { ["("] = ")", ["["] = "]", ["{"] = "}", ["<"] = ">", ["“"] = "”", ["‘"] = "’" })
  editor.replace_selection(marker .. editor.get_selection() .. closing)
end)

-- A persistent checkable command. MarkEdit stores its state and passes the
-- new value to the function after each click. Off by default, so installing
-- this example never changes anyone's files unasked.
menu.checkbox("Text/Trim Whitespace on Save", "trim_on_save", false, function(checked)
  ui.message("Trailing spaces and tabs will " .. (checked and "now" or "no longer")
    .. " be removed when a document is saved.")
end)

events.on("save", function()
  if not config.get("trim_on_save", false) then return end
  -- Line by line from the bottom, so the caret and earlier positions stay put.
  for line = editor.line_count(), 1, -1 do
    local text = editor.get_line(line)
    local trimmed = text:gsub("[ \t]+$", "")
    if #trimmed < #text then
      local start, finish = editor.line_range(line)
      editor.replace_range(start + #trimmed, finish, "")
    end
  end
end)

-- ---- editor right-click menu ----

local context_item

local function add_context_item()
  context_item = contextmenu.add("Formatting/Inline/Wrap Selection in Backticks", function()
    local text = editor.get_selection()
    local marker = config.get("context_fenced_code", true) and "```" or "`"
    if text ~= "" then editor.replace_selection(marker .. text .. marker) end
  end, function()
    return editor.get_selection() ~= ""
  end)
end

add_context_item()

contextmenu.checkbox("Formatting/Inline/Use Triple Backticks", "context_fenced_code", true,
  function(checked)
    ui.status("Context wrapping now uses " .. (checked and "triple" or "single") .. " backticks")
  end)

-- This command demonstrates that context items can also be removed and added
-- again while the plugin is running. The returned handle belongs to this
-- plugin and becomes invalid once it has been removed.
menu.add("Text/Toggle Backticks Context Item", function()
  if context_item then
    contextmenu.remove(context_item)
    context_item = nil
  else
    add_context_item()
  end
end)

-- ---- Plugins (top level of the menu) ----

menu.add("Document Statistics", function()
  local text = editor.get_text()
  local words = 0
  for _ in text:gmatch("%S+") do words = words + 1 end
  local lines = split_lines(text, editor.eol())
  local headings = 0
  for _, line in ipairs(lines) do
    if line:match("^#+%s") then headings = headings + 1 end
  end
  ui.message(string.format(
    "File: %s\nLanguage: %s\n\nCharacters: %d\nWords: %d\nLines: %d\nHeadings: %d",
    editor.file_path() or "(not saved yet)", editor.language(),
    utf8.len(text) or #text, words, #lines, headings))
end)

-- ---- added to the built-in Snippets > Markdown submenu ----

menu.add("/Snippets/Markdown/Make Heading...", function()
  local levels = { "Heading 1", "Heading 2", "Heading 3", "Heading 4", "Heading 5", "Heading 6", "Plain text" }
  local level = ui.choose(levels, "Turn the current line into:")
  if not level then return end
  local line = editor.current_line()
  local start, finish = editor.line_range(line)
  local text = editor.get_line(line):gsub("^#+%s*", "")
  if level <= 6 then text = string.rep("#", level) .. " " .. text end
  editor.replace_range(start, finish, text)
end)

-- ---- added to the built-in Snippets menu ----

local function insert_block(lines)
  editor.replace_selection(table.concat(lines, editor.eol()) .. editor.eol())
end

menu.separator("/Snippets/HTML")

menu.add("/Snippets/HTML/Collapsible Section", function()
  insert_block({ "<details>", "<summary>Summary</summary>", "", "Hidden content.", "", "</details>" })
end)

