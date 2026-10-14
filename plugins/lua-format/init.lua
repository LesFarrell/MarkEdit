-- lua-format: reindent the active Lua document, or the selected lines, from
-- Tools > Format Lua. The work is done by format.lua, which only rewrites
-- indentation and trailing whitespace. The caret keeps its line and column,
-- and formatting a selection leaves those lines selected.

local format = require("format")

local function leading_whitespace(text)
  return #(text:match("^[ \t]*") or "")
end

menu.add("/Tools/Format Lua", function()
  local text = editor.get_text()
  local start, finish = editor.get_selection_range()
  local selected = finish >= start

  local options = { indent = "  " }
  if selected then
    options.first = editor.line_at(start)
    options.last = editor.line_at(finish)
  end

  local formatted = format.format(text, options)
  if formatted == text then
    ui.status("Lua is already formatted")
    return
  end

  -- Remember where the caret sits inside its line, past any indentation.
  local caret_line = editor.current_line()
  local caret = editor.get_caret()
  local line_start = editor.line_range(caret_line)
  local column = math.max(0, caret - line_start - leading_whitespace(editor.get_line(caret_line)))

  editor.replace_range(1, editor.length(), formatted)

  if selected then
    local first = editor.line_range(options.first)
    local _, last = editor.line_range(options.last)
    editor.set_selection(first, last)
  else
    local new_start, new_finish = editor.line_range(caret_line)
    local indent = leading_whitespace(editor.get_line(caret_line))
    editor.set_caret(new_start + math.min(indent + column, new_finish - new_start))
  end
  ui.status("Formatted Lua")
end)
