-- Tinker Editor joins formatting_base.lua, formatting_tables.lua and this file
-- into one chunk before loading it, so the helpers used here are defined in
-- the earlier files. A language server checking this file on its own can't
-- see them.
---@diagnostic disable: undefined-global

-- ---- Edit menu text commands ----

-- Edit > Trim Trailing Whitespace: removes spaces and tabs from the end of
-- every line. The caret stays on the same text, or moves to the end of its
-- line if it was in the whitespace removed.
local function trim_trailing_whitespace()
  local caret = editor.get_caret()
  local trimmed = false
  -- From the last line up, so trimming a line doesn't move the ones still
  -- to be done.
  for n = editor.line_count(), 1, -1 do
    local start, finish = editor.line_range(n)
    local keptEnd = start - 1 + #editor.get_line(n):match("^(.-)[ \t]*$")
    if keptEnd ~= finish then
      editor.replace_range(keptEnd + 1, finish, "")
      if caret > finish + 1 then
        caret = caret - (finish - keptEnd) -- after this line: shift back
      elseif caret > keptEnd + 1 then
        caret = keptEnd + 1                -- in the removed whitespace
      end
      trimmed = true
    end
  end
  if trimmed then editor.set_caret(caret) end
end

-- Whether line is just delimiter, give or take spaces and tabs.
local function is_comment_delimiter(line, delimiter)
  return trim(line) == delimiter
end

-- Edit > Toggle Comment. Markdown has no line comments, so the selected
-- lines are wrapped in an HTML comment, with <!-- and --> on lines of their
-- own, or unwrapped when they are exactly that already.
local function toggle_comment()
  local first, last = selected_lines()
  local blockStart = line_start(first)
  local _, blockEnd = editor.line_range(last)

  local isCommented = last > first
    and is_comment_delimiter(editor.get_line(first), "<!--")
    and is_comment_delimiter(editor.get_line(last), "-->")

  local replacement
  if isCommented then
    replacement = ""
    if last - first >= 2 then
      local _, innerEnd = editor.line_range(last - 1)
      replacement = editor.get_range(line_start(first + 1), innerEnd)
    end
  else
    local eol = editor.eol()
    replacement = "<!--" .. eol .. editor.get_range(blockStart, blockEnd) .. eol .. "-->"
  end

  editor.set_selection(blockStart, blockEnd)
  editor.replace_selection(replacement)
  editor.set_selection(blockStart, blockStart + #replacement - 1)
end

-- ---- registration ----
--
-- Each command is registered as "<group>.<name>", e.g. "markdown.bold".
-- These are the names src/menus.lua and the formatting toolbar use, and
-- that plugins pass to commands.register to replace one. Renaming one here
-- needs the same change in src/menu_commands.c and src/menus.lua.

local markdown = {
  bold = function() wrap_pair("**", "**") end,
  italic = function() wrap_pair("*", "*") end,
  strikethrough = function() wrap_pair("~~", "~~") end,
  inline_code = function() wrap_pair("`", "`") end,

  heading1 = function() apply_line_prefix("# ") end,
  heading2 = function() apply_line_prefix("## ") end,
  heading3 = function() apply_line_prefix("### ") end,
  blockquote = function() apply_line_prefix("> ") end,
  bullet_list = function() apply_line_prefix("- ") end,
  numbered_list = function() apply_line_prefix(nil, true) end,
  task_list = function() apply_line_prefix("- [ ] ") end,

  link = function() insert_with_url("[", "link text", "](url)") end,
  image = function() insert_with_url("![", "alt text", "](url)") end,
  code_block = insert_code_block,
  footnote = insert_footnote,
  horizontal_rule = insert_horizontal_rule,
  alert = insert_alert,

  table = insert_table,
  table_editor = table_editor,
  table_row_above = function() insert_table_row(false) end,
  table_row_below = function() insert_table_row(true) end,
  table_row_delete = delete_table_row,
  table_column_left = function() insert_table_column(false) end,
  table_column_right = function() insert_table_column(true) end,
  table_column_delete = delete_table_column,
  table_sort_ascending = function() sort_table_rows(false) end,
  table_sort_descending = function() sort_table_rows(true) end,
  table_align_left = function() set_table_column_alignment(":---") end,
  table_align_center = function() set_table_column_alignment(":---:") end,
  table_align_right = function() set_table_column_alignment("---:") end,
}

local edit = {
  trim_whitespace = trim_trailing_whitespace,
  toggle_comment = toggle_comment,
}

for group, functions in pairs({ markdown = markdown, edit = edit }) do
  for name, fn in pairs(functions) do
    commands.register(group .. "." .. name, fn)
  end
end
