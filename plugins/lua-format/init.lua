-- lua-format: reindent Lua from Tools > Interpreters > Lua > Format Document
-- (the whole file) or Format Selection (only the selected lines), and check
-- its syntax, or the selection's, with Check Syntax or Check Selection Syntax. Formatting is done by format.lua, which only
-- rewrites indentation and trailing whitespace. The caret keeps its line and
-- column, and formatting a selection leaves those lines selected.

local format = require("format")

local function leading_whitespace(text)
  return #(text:match("^[ \t]*") or "")
end

local function format_lua(selection_only)
  local text = editor.get_text()
  local start, finish = editor.get_selection_range()
  local selected = finish >= start
  if selection_only and not selected then
    ui.message("Select the lines to format.", nil, "warning")
    return
  end

  local options = { indent = "  " }
  if selection_only then
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

  if selection_only then
    local first = editor.line_range(options.first)
    local _, last = editor.line_range(options.last)
    editor.set_selection(first, last)
  else
    local new_start, new_finish = editor.line_range(caret_line)
    local indent = leading_whitespace(editor.get_line(caret_line))
    editor.set_caret(new_start + math.min(indent + column, new_finish - new_start))
  end
  ui.status("Formatted Lua")
end

-- Compiles the document, or the selected text, without running it and
-- reports the first syntax error in the output panel, as "path:line:
-- message" so it can be double-clicked when the document is saved. A
-- selection is preceded by one line break per line above it, so its errors
-- carry document line numbers. A "#!" first line is skipped, as the
-- standalone lua interpreter does, keeping the line numbers.
local function check_syntax(selection_only)
  local code, scope = editor.get_text(), ""
  if selection_only then
    local start, finish = editor.get_selection_range()
    if finish < start then
      ui.message("Select the Lua code to check.", nil, "warning")
      return
    end
    local first, last = editor.line_at(start), editor.line_at(finish)
    code = string.rep("\n", first - 1) .. editor.get_selection()
    scope = first == last and string.format("Line %d: ", first) or string.format("Lines %d-%d: ", first, last)
  end
  code = code:gsub("^#[^\r\n]*", "", 1)
  local _, problem = load(code, "=" .. (editor.file_path() or "untitled"), "t", {})
  output.report("Lua Syntax Check", problem or scope .. "No syntax errors.")
  ui.status(problem and "Lua syntax error" or "No Lua syntax errors")
end

menu.add("/Tools/Interpreters/Lua/Format Document", function() format_lua(false) end)
menu.add("/Tools/Interpreters/Lua/Format Selection", function() format_lua(true) end)
menu.add("/Tools/Interpreters/Lua/Check Syntax", function() check_syntax(false) end)
menu.add("/Tools/Interpreters/Lua/Check Selection Syntax", function() check_syntax(true) end)
menu.separator("/Tools/Interpreters/Lua")