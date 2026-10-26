-- MarkEdit's built-in Markdown and HTML formatting commands, written in Lua.
--
-- MarkEdit joins this file, formatting_tables.lua and formatting_text.lua
-- into one chunk before loading it, so many locals here are only used by the
-- later files. A language server checking this file on its own reports them
-- as unused.
---@diagnostic disable: unused-local, unused-function
--
-- What's here (each section below is one of these):
--   Inline wrapping     Bold, Italic, Strikethrough, Inline Code, HTML tags
--   Line prefixes       Headings, Blockquote, Bullet/Numbered/Task lists
--   Snippets            Link, Image, Table, Code Block, Footnote, Alert, ...
--   Markdown tables     Table Editor and Snippets > Markdown > Table Row/Column
--   Edit menu           Toggle Comment, Trim Trailing Whitespace
--   Registration        the command names, at the bottom of the file
--
-- How it's loaded: markedit.rc builds this file into markedit.exe, and
-- MarkEdit runs it after the menu definition and before any plugin, so a
-- plugin can replace any of these commands by registering the same name:
--   commands.register("markdown.bold", function() ... end)
-- It uses only the plugin API (editor.*, ui.*, commands.*), documented in
-- plugins/README.md. docs/lua.md explains how all of MarkEdit's Lua fits
-- together. Changes here need a rebuild; tests/test_formatting.c tests it.
--
-- Conventions:
--   * Positions are 1-based byte offsets, and a range start, finish includes
--     both ends, as with string.sub. An empty range (just a caret) has
--     finish == start - 1.
--   * Line numbers are 1-based. A line's text never includes its line
--     ending; new lines are joined with editor.eol(), the document's own.
--   * Every command runs as one undo step, so a command may change the
--     document in several steps without the user seeing the pieces.

-- ---- shared helpers ----

-- The selection as start, finish (an empty range at the caret when nothing
-- is selected).
local function selection()
  return editor.get_selection_range()
end

local function is_empty(start, finish)
  return finish < start
end

-- The position line n starts at.
local function line_start(n)
  return (editor.line_range(n))
end

-- Whether text inserted at position would land partway through a line, so
-- a block inserted there needs a line ending first.
local function needs_leading_eol(position)
  if position <= 1 then return false end
  local previous = editor.get_range(position - 1, position - 1)
  return previous ~= "\r" and previous ~= "\n"
end

-- The first and last lines the selection touches. A selection that ends
-- exactly at the start of a line (Home, then Shift+Down) leaves that line
-- out, as the user would expect.
local function lines_of(start, finish)
  local first, last = editor.line_at(start), editor.line_at(finish + 1)
  if last > first and finish + 1 == line_start(last) then last = last - 1 end
  return first, last
end

local function selected_lines()
  return lines_of(selection())
end

-- Replaces lines first..last (not their final line ending) with text.
local function replace_lines(first, last, text)
  local _, finish = editor.line_range(last)
  editor.set_selection(line_start(first), finish)
  editor.replace_selection(text)
end

-- ---- inline wrapping: bold, italic, HTML tags ----

-- Whether selected is one open ... close pair around its inner text. For a
-- Markdown marker made of one repeated character, "*" mustn't be half of
-- "**" ("***" is both, so either unwraps it), and a marker inside the inner
-- text ("**a** and **b**") shows the ends belong to different pairs.
local function is_wrapped(selected, open, close)
  local length = #selected
  if length < #open + #close or selected:sub(1, #open) ~= open
    or selected:sub(length - #close + 1) ~= close then
    return false
  end
  local inner = selected:sub(#open + 1, length - #close)
  local c = open:sub(1, 1)
  if open ~= close or open ~= c:rep(#open) then
    return not inner:find(open, 1, true) and not inner:find(close, 1, true)
  end

  local emphasis = c == "*" or c == "_"
  local run = "%" .. c .. "+"
  local leading, trailing = #selected:match("^" .. run), #selected:match(run .. "$")
  if leading ~= trailing or (leading ~= #open and not (emphasis and leading == 3)) then
    return false
  end
  for marker in inner:gmatch(run) do
    if #marker == #open or (emphasis and #marker == 3) then return false end
  end
  return true
end

-- The text that replaces selected when wrapping or unwrapping it, and the
-- first and last positions of the inner text within that replacement.
local function wrapped_text(selected, open, close)
  if is_wrapped(selected, open, close) then
    local inner = selected:sub(#open + 1, #selected - #close)
    return inner, 1, #inner
  end
  return open .. selected .. close, #open + 1, #open + #selected
end

-- Wraps each selection in open ... close, or unwraps it if it is already
-- wrapped in exactly that pair, leaving the inner text selected either way.
-- With no selection, inserts the pair with the caret between the two.
local function wrap_pair(open, close)
  if editor.selection_count() > 1 then
    local texts, order = {}, {}
    for i, range in ipairs(editor.get_selections()) do
      local text, innerStart, innerFinish =
        wrapped_text(editor.get_range(range.start, range.finish), open, close)
      texts[i] = text
      order[i] = { start = range.start, length = range.finish - range.start + 1,
                   text = text, innerStart = innerStart, innerFinish = innerFinish }
    end
    editor.replace_selections(texts)

    -- replace_selections leaves one caret per selection, in document order;
    -- select each inner text, allowing for the edits before it.
    table.sort(order, function(a, b) return a.start < b.start end)
    if editor.selection_count() ~= #order then return end
    local shift = 0
    for n, item in ipairs(order) do
      local base = item.start + shift - 1
      editor.set_selection(base + item.innerStart, base + item.innerFinish, n)
      shift = shift + #item.text - item.length
    end
    return
  end

  local start, finish = selection()
  if is_empty(start, finish) then
    editor.replace_selection(open .. close)
    editor.set_caret(start + #open)
    return
  end

  local text, innerStart, innerFinish = wrapped_text(editor.get_selection(), open, close)
  editor.replace_selection(text)
  editor.set_selection(start + innerStart - 1, start + innerFinish - 1)
end

local function wrap_tag(tag)
  wrap_pair("<" .. tag .. ">", "</" .. tag .. ">")
end

-- ---- line prefixes: headings, quotes, lists ----
--
-- These commands give each selected line a prefix such as "# " or "- ".
-- A line that already has a different prefix has it replaced rather than
-- stacked ("# " on "- item" gives "# item"), and a line that already has
-- exactly the prefix has it removed, so each command toggles.

-- The number of spaces and tabs a line starts with. Indentation is kept as
-- it is, so a nested list item ("    - item") stays nested.
local function leading_spaces(line)
  return #line:match("^[ \t]*")
end

-- A marker ending at byte `last` of text counts only when it is followed by
-- the end of the line, or by a space or tab (which then belongs to the
-- marker). Returns the marker's length, or nil.
local function marker_then_blank(text, last)
  local following = text:sub(last + 1, last + 1)
  if following == "" then return last end
  if following == " " or following == "\t" then return last + 1 end
  return nil
end

-- The length of the line-prefix marker text starts with, or nil:
--   >                a quote, with an optional space or tab after it
--   # to ######      a heading
--   - [ ]  - [x]     a task (also with *)
--   -  *             a bullet
--   1.  12.          a numbered item
local function marker_length(text)
  if text:sub(1, 1) == ">" then
    return text:match("^>[ \t]?()") - 1
  end

  local hashes = text:match("^#+")
  if hashes then
    return marker_then_blank(text, math.min(#hashes, 6))
  end

  if text:match("^[-*]") then
    local afterTask = text:match("^[-*] %[[ xX]%]()")
    return (afterTask and marker_then_blank(text, afterTask - 1)) or marker_then_blank(text, 1)
  end

  local afterNumber = text:match("^%d+%.()")
  if afterNumber then
    return marker_then_blank(text, afterNumber - 1)
  end

  return nil
end

-- How many bytes at the start of line are a prefix these commands manage:
-- the leading spaces plus the marker. 0 when the line has no marker.
local function prefix_length(line)
  local indent = leading_spaces(line)
  local marker = marker_length(line:sub(indent + 1))
  return marker and indent + marker or 0
end

-- Whether line's prefix is a numbered-list marker.
local function is_numbered(line)
  local indent = leading_spaces(line)
  return prefix_length(line) > indent and line:sub(indent + 1, indent + 1):match("%d") ~= nil
end

-- Lines first..last given prefix (see the section comment above), joined
-- with the document's line ending. With numbered set, prefix is ignored and
-- the lines are numbered 1., 2., ... instead, or un-numbered when every one
-- of them is numbered already.
local function prefixed_lines(first, last, prefix, numbered)
  local lines = {}
  for n = first, last do lines[#lines + 1] = editor.get_line(n) end

  local removeNumbers = false
  if numbered then
    removeNumbers = true
    for _, line in ipairs(lines) do
      if not is_numbered(line) then
        removeNumbers = false
        break
      end
    end
  end

  local out, number = {}, 1
  for i, line in ipairs(lines) do
    local indent = leading_spaces(line)
    local existing = prefix_length(line)
    local markerLength = existing > 0 and existing - indent or 0
    local hasExactPrefix = not numbered and markerLength == #prefix
      and line:sub(indent + 1, indent + markerLength) == prefix

    local newPrefix
    if hasExactPrefix or removeNumbers then
      newPrefix = ""
    elseif numbered then
      newPrefix = number .. ". "
      number = number + 1
    else
      newPrefix = prefix
    end

    local contentStart = existing > 0 and existing or indent
    out[i] = line:sub(1, indent) .. newPrefix .. line:sub(contentStart + 1)
  end
  return table.concat(out, editor.eol())
end

-- Gives every selected line prefix, as prefixed_lines() does. With several
-- selections, the lines of each are done, touching or overlapping blocks
-- of lines as one, from the last block up so earlier positions stay valid.
local function apply_line_prefix(prefix, numbered)
  if editor.selection_count() == 1 then
    local first, last = selected_lines()
    replace_lines(first, last, prefixed_lines(first, last, prefix, numbered))
    return
  end

  local blocks = {}
  for _, range in ipairs(editor.get_selections()) do
    local first, last = lines_of(range.start, range.finish)
    blocks[#blocks + 1] = { first = first, last = last }
  end
  table.sort(blocks, function(a, b) return a.first < b.first end)
  local merged = {}
  for _, block in ipairs(blocks) do
    local previous = merged[#merged]
    if previous and block.first <= previous.last + 1 then
      previous.last = math.max(previous.last, block.last)
    else
      merged[#merged + 1] = block
    end
  end
  for i = #merged, 1, -1 do
    local block = merged[i]
    local _, finish = editor.line_range(block.last)
    editor.replace_range(line_start(block.first), finish,
      prefixed_lines(block.first, block.last, prefix, numbered))
  end
end

-- ---- snippets: links, images, tables, blocks ----

-- Inserts before .. middle .. after, where middle is the selection (or
-- placeholder when nothing is selected), then selects the word "url" in
-- before or after so the user can type over it. The "url" is never looked
-- for in the selected text.
local function insert_with_url(before, placeholder, after)
  local start, finish = selection()
  local middle = is_empty(start, finish) and placeholder or editor.get_selection()
  editor.replace_selection(before .. middle .. after)

  local urlStart
  local inBefore = before:find("url", 1, true)
  if inBefore then
    urlStart = start + inBefore - 1
  else
    urlStart = start + #before + #middle + after:find("url", 1, true) - 1
  end
  editor.set_selection(urlStart, urlStart + #"url" - 1)
end

-- Inserts text in place of the selection, starting on a line of its own.
local function insert_block(text)
  local start = selection()
  local leading = needs_leading_eol(start) and editor.eol() or ""
  editor.replace_selection(leading .. text)
end

-- The number at the start of text (after any spaces), or 0.
local function leading_integer(text)
  return tonumber(text:match("^%s*([+-]?%d+)")) or 0
end

-- Asks for a table's number of data rows and columns, offering the given
-- defaults. Returns rows, columns, or nil when the user cancels or gives a
-- size out of range (1-50 rows, 1-20 columns), which is reported.
local function ask_table_size(title, rows, columns)
  local rowsText = ui.prompt("Data rows (excluding the header):", tostring(rows), title)
  if not rowsText then return nil end
  local columnsText = ui.prompt("Columns:", tostring(columns), title)
  if not columnsText then return nil end

  rows, columns = leading_integer(rowsText), leading_integer(columnsText)
  if rows < 1 or rows > 50 or columns < 1 or columns > 20 then
    ui.message("Use 1–50 data rows and 1–20 columns.", title, "warning")
    return nil
  end
  return rows, columns
end

-- Snippets > Markdown > Table: a new table of the size the user asks for.
local function insert_table()
  local rows, columns = ask_table_size("Insert Table", 1, 3)
  if not rows then return end

  local header, separator, cells = {}, {}, {}
  for column = 1, columns do
    header[column] = "Column " .. column
    separator[column] = "---"
    cells[column] = "Cell"
  end
  local function row(values) return "| " .. table.concat(values, " | ") .. " |" end

  local lines = { row(header), row(separator) }
  for _ = 1, rows do lines[#lines + 1] = row(cells) end
  local eol = editor.eol()
  insert_block(table.concat(lines, eol) .. eol)
end

-- Snippets > HTML > Table.
local function insert_html_table()
  local rows, columns = ask_table_size("Insert HTML Table", 1, 3)
  if not rows then return end

  local lines = { "<table>" }
  -- One <tr> of tag cells, holding text, or "Column n" when text is nil.
  local function add_row(tag, text)
    lines[#lines + 1] = "  <tr>"
    for column = 1, columns do
      lines[#lines + 1] = "    <" .. tag .. ">" .. (text or ("Column " .. column)) .. "</" .. tag .. ">"
    end
    lines[#lines + 1] = "  </tr>"
  end
  add_row("th")
  for _ = 1, rows do add_row("td", "Cell") end
  lines[#lines + 1] = "</table>"
  lines[#lines + 1] = "" -- so the block ends with a line ending

  insert_block(table.concat(lines, editor.eol()))
end

-- Snippets > HTML > Bullet/Numbered List: wraps each selected line in <li>
-- and the lines in <ul> or <ol>. With no selection, inserts a list with one
-- placeholder item, selected.
local function insert_html_list(ordered)
  local eol = editor.eol()
  local tag = ordered and "ol" or "ul"
  local start, finish = selection()

  if is_empty(start, finish) then
    local placeholder = "List item"
    local before = "<" .. tag .. ">" .. eol .. "  <li>"
    editor.replace_selection(before .. placeholder .. "</li>" .. eol .. "</" .. tag .. ">")
    editor.set_selection(start + #before, start + #before + #placeholder - 1)
    return
  end

  local first, last = selected_lines()
  local out = { "<" .. tag .. ">" .. eol }
  for n = first, last do out[#out + 1] = "  <li>" .. editor.get_line(n) .. "</li>" .. eol end
  out[#out + 1] = "</" .. tag .. ">"
  replace_lines(first, last, table.concat(out))
end

-- Fences the selection in ``` lines, leaving it selected. With nothing
-- selected, inserts an empty fence with the caret just after the opening
-- ```, ready for a language name.
local function insert_code_block()
  local eol = editor.eol()
  local start, finish = selection()
  local leading = needs_leading_eol(start) and eol or ""

  if is_empty(start, finish) then
    editor.replace_selection(leading .. "```" .. eol .. eol .. "```" .. eol)
    editor.set_caret(start + #leading + #"```")
    return
  end

  local selected = editor.get_selection()
  local open = leading .. "```" .. eol
  -- A selection of whole lines already ends with a line ending.
  local lastChar = selected:sub(-1)
  local bodyEnd = (lastChar == "\r" or lastChar == "\n") and "" or eol
  editor.replace_selection(open .. selected .. bodyEnd .. "```" .. eol)
  editor.set_selection(start + #open, start + #open + #selected - 1)
end

local function insert_horizontal_rule()
  insert_block("---" .. editor.eol())
end

-- Puts "> " at the start of each line of text (split at "\n"). A final
-- line ending doesn't start another line.
local function quote_lines(text)
  local quoted = "> " .. text:gsub("\n", "\n> ")
  if text:sub(-1) == "\n" then quoted = quoted:sub(1, -3) end
  return quoted
end

-- A GitHub "> [!NOTE]" alert holding the selection (or placeholder text),
-- every line of it quoted.
local function insert_alert()
  local eol = editor.eol()
  local start, finish = selection()
  local body = is_empty(start, finish) and "Alert text" or editor.get_selection()

  local leading = needs_leading_eol(start) and eol or ""
  local text = leading .. "> [!NOTE]" .. eol .. quote_lines(body)
  local lastChar = text:sub(-1)
  if lastChar ~= "\r" and lastChar ~= "\n" then text = text .. eol end
  editor.replace_selection(text)
end

-- Inserts a footnote reference numbered one past the highest [^n] in the
-- document, and appends its note (the selection, or placeholder text) at
-- the end of the document, selected for editing.
local function insert_footnote()
  local nextNumber = 1
  for digits in editor.get_text():gmatch("%[%^(%d+)%]") do
    nextNumber = math.max(nextNumber, tonumber(digits) + 1)
  end

  local start, finish = selection()
  local note = is_empty(start, finish) and "Footnote text" or editor.get_selection()
  local eol = editor.eol()
  local reference = "[^" .. nextNumber .. "]"

  editor.replace_selection(reference)
  editor.set_caret(editor.length() + 1)
  editor.replace_selection(eol .. eol .. reference .. ": ")
  local noteStart = editor.get_caret()
  editor.replace_selection(note)
  editor.replace_selection(eol)
  editor.set_selection(noteStart, noteStart + #note - 1)
end
