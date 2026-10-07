-- MarkEdit's built-in formatting and text commands, written in Lua.
--
-- What's here (each section below is one of these):
--   Inline wrapping     Bold, Italic, Strikethrough, Inline Code, HTML tags
--   Line prefixes       Headings, Blockquote, Bullet/Numbered/Task lists
--   Snippets            Link, Image, Table, Code Block, Footnote, Alert, ...
--   JSON                Tools > JSON > Format / Minify
--   Markdown tables     Table Editor and Snippets > Markdown > Table Row/Column
--   Edit menu           Toggle Comment, Trim Trailing Whitespace, Title Case,
--                       Sort Lines, Remove Duplicate Lines
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
local function selected_lines()
  local start, finish = selection()
  local first, last = editor.line_at(start), editor.line_at(finish + 1)
  if last > first and finish + 1 == line_start(last) then last = last - 1 end
  return first, last
end

-- Replaces lines first..last (not their final line ending) with text.
local function replace_lines(first, last, text)
  local _, finish = editor.line_range(last)
  editor.set_selection(line_start(first), finish)
  editor.replace_selection(text)
end

-- ---- inline wrapping: bold, italic, HTML tags ----

-- Wraps the selection in open ... close, or unwraps it if it is already
-- wrapped in exactly that pair, leaving the inner text selected either way.
-- With no selection, inserts the pair with the caret between the two.
local function wrap_pair(open, close)
  local start, finish = selection()
  if is_empty(start, finish) then
    editor.replace_selection(open .. close)
    editor.set_caret(start + #open)
    return
  end

  local selected = editor.get_selection()
  local length = #selected
  local alreadyWrapped = length >= #open + #close
    and selected:sub(1, #open) == open
    and selected:sub(length - #close + 1) == close

  if alreadyWrapped then
    local inner = selected:sub(#open + 1, length - #close)
    editor.replace_selection(inner)
    editor.set_selection(start, start + #inner - 1)
  else
    editor.replace_selection(open .. selected .. close)
    editor.set_selection(start + #open, start + #open + length - 1)
  end
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

-- The number of spaces (at most 3) a line starts with. Markdown allows up
-- to 3 before a marker; 4 would make the line a code block.
local function leading_spaces(line)
  local count = 0
  while count < #line and count < 3 and line:byte(count + 1) == 32 do count = count + 1 end
  return count
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

-- Gives every selected line prefix (see the section comment above). With
-- numbered set, prefix is ignored and the lines are numbered 1., 2., ...
-- instead, or un-numbered when every one of them is numbered already.
local function apply_line_prefix(prefix, numbered)
  local first, last = selected_lines()
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

  replace_lines(first, last, table.concat(out, editor.eol()))
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
      lines[#lines + 1] = "    <" .. tag .. ">" .. (text or "Column " .. column) .. "</" .. tag .. ">"
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
  editor.replace_selection(open .. selected .. eol .. "```" .. eol)
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

-- ---- JSON ----
--
-- Format and Minify work in two passes: json_is_valid() checks the text
-- strictly against the JSON grammar, then json_reformat() reprints it.
-- The reprinting pass only looks at punctuation, so it relies on the check:
-- on malformed input such as [1 2] it would quietly produce a different,
-- valid value ([12]).

-- Matches any character that isn't JSON whitespace.
local NOT_SPACE = "[^ \t\r\n]"

-- Containers nested deeper than this are rejected, so a hostile document
-- can't exhaust the stack.
local JSON_MAX_DEPTH = 256

-- Whether text is exactly one JSON value, with optional whitespace around
-- it. A recursive-descent reader: each read_* function consumes one piece
-- of the grammar at `position` and says whether it was well-formed.
local function json_is_valid(text)
  local length, position = #text, 1

  local function skip_space()
    position = text:find(NOT_SPACE, position) or length + 1
  end

  -- Consumes character if it comes next.
  local function take(character)
    if text:sub(position, position) ~= character then return false end
    position = position + 1
    return true
  end

  -- Consumes a run of digits; false if there were none.
  local function read_digits()
    local after = text:find("[^0-9]", position) or length + 1
    local any = after > position
    position = after
    return any
  end

  -- "...", with \" \\ \/ \b \f \n \r \t and \uXXXX escapes, and no raw
  -- control characters.
  local function read_string()
    if not take('"') then return false end
    while position <= length do
      local byte = text:byte(position)
      position = position + 1
      if byte == 34 then return true end -- closing "
      if byte < 32 then return false end -- control character
      if byte == 92 then                 -- backslash
        if position > length then return false end
        local escape = text:sub(position, position)
        position = position + 1
        if escape == "u" then
          if not text:sub(position, position + 3):match("^%x%x%x%x$") then return false end
          position = position + 4
        elseif not escape:match('^["\\/bfnrt]$') then
          return false
        end
      end
    end
    return false -- no closing "
  end

  -- -12.5e+3: an optional minus, then 0 or digits, then optional fraction
  -- and exponent.
  local function read_number()
    take("-")
    if not take("0") and not read_digits() then return false end
    if take(".") and not read_digits() then return false end
    if take("e") or take("E") then
      if not take("+") then take("-") end
      if not read_digits() then return false end
    end
    return true
  end

  local read_value

  -- [ value, ... ] or { "key": value, ... }, opened by opener.
  local function read_container(opener, depth)
    if depth >= JSON_MAX_DEPTH then return false end
    position = position + 1
    local closer = opener == "{" and "}" or "]"
    skip_space()
    if take(closer) then return true end -- empty
    while true do
      if opener == "{" then
        skip_space()
        if not read_string() then return false end
        skip_space()
        if not take(":") then return false end
      end
      if not read_value(depth + 1) then return false end
      skip_space()
      if take(closer) then return true end
      if not take(",") then return false end
    end
  end

  read_value = function(depth)
    skip_space()
    if position > length then return false end
    local c = text:sub(position, position)
    if c == '"' then return read_string() end
    if c == "{" or c == "[" then return read_container(c, depth) end

    local literal = (c == "t" and "true") or (c == "f" and "false") or (c == "n" and "null")
    if literal then
      if text:sub(position, position + #literal - 1) ~= literal then return false end
      position = position + #literal
      return true
    end

    return read_number()
  end

  if not read_value(0) then return false end
  skip_space()
  return position == length + 1
end

-- text reprinted either "pretty" (one item per line, indented two spaces
-- per level, using eol) or minified (no whitespace at all). Returns nil when
-- text isn't valid JSON. Empty containers stay on one line: {} and [].
local function json_reformat(text, eol, pretty)
  if not json_is_valid(text) then return nil end

  local out, lastChar = {}, nil
  local function emit(piece)
    out[#out + 1] = piece
    lastChar = piece:sub(-1)
  end
  local function emit_newline(depth)
    emit(eol .. string.rep("  ", depth))
  end
  -- Whether the next non-space character from position on closes a
  -- container; also returns that character's position (nil at the end).
  local function closer_next(from)
    local nextPosition = text:find(NOT_SPACE, from)
    local c = nextPosition and text:sub(nextPosition, nextPosition)
    return c == "}" or c == "]", nextPosition
  end

  local length, i, depth = #text, 1, 0
  while i <= length do
    local c = text:sub(i, i)
    if c == " " or c == "\t" or c == "\r" or c == "\n" then
      i = i + 1 -- whitespace between tokens is dropped
    elseif c == '"' then
      -- Copy the string whole, stepping over escaped characters.
      local j = i + 1
      while j <= length and text:sub(j, j) ~= '"' do
        j = j + ((text:sub(j, j) == "\\" and j + 1 <= length) and 2 or 1)
      end
      emit(text:sub(i, j))
      i = j + 1
    elseif c == "{" or c == "[" then
      depth = depth + 1
      emit(c)
      i = i + 1
      local isEmpty, nextPosition = closer_next(i)
      if pretty and nextPosition and not isEmpty then emit_newline(depth) end
    elseif c == "}" or c == "]" then
      depth = depth - 1
      local closesEmpty = lastChar == "{" or lastChar == "["
      if pretty and lastChar and not closesEmpty then emit_newline(depth) end
      emit(c)
      i = i + 1
    elseif c == ":" then
      emit(pretty and ": " or ":")
      i = i + 1
    elseif c == "," then
      emit(",")
      if pretty then emit_newline(depth) end
      i = i + 1
    else
      -- A number or literal: copy up to the next delimiter.
      local j = text:find('[ \t\r\n,:{}%[%]"]', i) or length + 1
      emit(text:sub(i, j - 1))
      i = j
    end
  end
  return table.concat(out)
end

-- Whether line n opens or closes a fenced code block (``` or ~~~, after up
-- to three spaces).
local function line_starts_fence(n)
  local line = editor.get_line(n)
  local rest = line:sub(#line:match("^ ? ? ?") + 1)
  return rest:find("^```") ~= nil or rest:find("^~~~") ~= nil
end

-- The range between the fence lines of the fenced block holding line, or
-- nil when line isn't in one (or the block is empty).
local function fence_body(line)
  local open
  for n = line, 1, -1 do
    if line_starts_fence(n) then open = n; break end
  end
  if not open then return nil end

  local close
  for n = open + 1, editor.line_count() do
    if line_starts_fence(n) then close = n; break end
  end
  if not close or line > close or close <= open + 1 then return nil end

  local _, finish = editor.line_range(close - 1)
  return line_start(open + 1), finish
end

-- Reformats the selection, or with nothing selected the fenced code block
-- holding the caret. Text that isn't valid JSON is left untouched and
-- reported.
local function format_json(pretty)
  local start, finish = selection()
  if is_empty(start, finish) then
    start, finish = fence_body(editor.line_at(start))
    if not start then
      ui.message("Place the caret inside a fenced code block, or select the JSON to format.", "MarkEdit", "info")
      return
    end
  end

  local formatted = json_reformat(editor.get_range(start, finish), editor.eol(), pretty)
  if not formatted then
    ui.message("That text could not be read as JSON.", "MarkEdit", "warning")
    return
  end
  editor.set_selection(start, finish)
  editor.replace_selection(formatted)
  editor.set_selection(start, start + #formatted - 1)
end

-- ---- Markdown tables ----
--
-- A table is a run of lines containing "|", with a header row, a separator
-- row (| --- | :-: |) and any number of data rows. The table commands find
-- the table around the caret with find_table() and rewrite its lines.

-- Most cells a row is read as having.
local MAX_TABLE_CELLS = 128

local function trim(text)
  return text:match("^[ \t]*(.-)[ \t]*$")
end

-- The trimmed cells of a table row, at most maximum of them. Outer pipes
-- are optional, and whitespace after the closing pipe isn't an extra cell.
local function row_cells(line, maximum)
  local cursor = line:match("^[ \t]*()")
  if line:sub(cursor, cursor) == "|" then cursor = cursor + 1 end
  local limit = #line
  while limit >= cursor and line:sub(limit, limit):match("[ \t]") do limit = limit - 1 end

  local cells = {}
  while cursor <= limit and #cells < maximum do
    local bar = line:find("|", cursor, true)
    local cellEnd = (bar and bar <= limit) and bar or limit + 1
    cells[#cells + 1] = trim(line:sub(cursor, cellEnd - 1))
    if cellEnd > limit then break end
    cursor = cellEnd + 1
  end
  return cells
end

-- The 0-based column holding the byte offset bytes into a row, counting
-- cells the same way row_cells() does.
local function column_at(line, offset)
  local cursor = line:match("^[ \t]*()")
  if line:sub(cursor, cursor) == "|" then cursor = cursor + 1 end
  local column = 0
  for position = cursor, math.min(offset, #line) do
    if line:byte(position) == 124 then column = column + 1 end -- "|"
  end
  return column
end

local function has_pipe(n)
  return editor.get_line(n):find("|", 1, true) ~= nil
end

-- The first and last lines of the run of lines containing "|" around line.
local function pipe_lines_around(line)
  local first, last, count = line, line, editor.line_count()
  while first > 1 and has_pipe(first - 1) do first = first - 1 end
  while last < count and has_pipe(last + 1) do last = last + 1 end
  return first, last
end

-- The table holding the caret, as { first, last, row, column }: its first
-- and last lines, the caret's row (0 header, 1 separator, 2 and up data)
-- and the caret's 0-based column. nil when the caret isn't in a table of at
-- least a header and a separator.
local function find_table()
  local caret = editor.get_caret()
  local caretLine = editor.line_at(caret)
  local line = editor.get_line(caretLine)
  if not line:find("|", 1, true) then return nil end

  local first, last = pipe_lines_around(caretLine)
  if last - first < 1 then return nil end
  return {
    first = first,
    last = last,
    row = caretLine - first,
    column = column_at(line, caret - line_start(caretLine)),
  }
end

local function not_in_table()
  ui.message("Place the caret inside a Markdown table.", "Table", "info")
end

-- The range a rewritten table replaces, and whether the table ends the
-- document. When it does, its last line may have no line ending, and the
-- rewrite mustn't add one.
local function table_range(tbl)
  local atEnd = tbl.last >= editor.line_count()
  local finish = atEnd and editor.length() or line_start(tbl.last + 1) - 1
  return line_start(tbl.first), finish, atEnd
end

-- "| a | b |"
local function format_row(cells)
  local parts = { "|" }
  for _, cell in ipairs(cells) do parts[#parts + 1] = " " .. cell .. " |" end
  return table.concat(parts)
end

-- Rewrites every row of tbl as rewrite(cells, kind) returns it, where kind
-- is 0 for the header, 1 for the separator and 2 for a data row.
local function rewrite_table(tbl, rewrite)
  local start, finish, atEnd = table_range(tbl)
  local eol = editor.eol()
  local out = {}
  for n = tbl.first, tbl.last do
    local kind = math.min(n - tbl.first, 2)
    out[#out + 1] = format_row(rewrite(row_cells(editor.get_line(n), MAX_TABLE_CELLS), kind))
    if not (atEnd and n == tbl.last) then out[#out + 1] = eol end
  end
  editor.replace_range(start, finish, table.concat(out))
end

-- Inserts an empty data row above or below the caret's row. From the
-- header or separator, the new row always becomes the first data row.
local function insert_table_row(below)
  local tbl = find_table()
  if not tbl then return not_in_table() end

  local columns = math.max(#row_cells(editor.get_line(tbl.first), MAX_TABLE_CELLS), 1)
  local target
  if tbl.row <= 1 then
    target = tbl.first + 2
  else
    target = tbl.first + tbl.row + (below and 1 or 0)
  end

  -- Past the last line of the document there's no line to insert before,
  -- so the row is appended, after a line ending if one is needed.
  local eol = editor.eol()
  local appendAtEnd = target > editor.line_count()
  local position = appendAtEnd and editor.length() + 1 or line_start(target)
  local row = string.rep("|  ", columns) .. "|"
  if appendAtEnd then
    if needs_leading_eol(position) then row = eol .. row end
  else
    row = row .. eol
  end
  editor.insert(row, position)
  editor.set_caret(position)
end

-- Deletes the caret's data row. The header and separator can't be deleted.
local function delete_table_row()
  local tbl = find_table()
  if not tbl then return not_in_table() end
  if tbl.row <= 1 then
    ui.message("Place the caret in a data row to delete it.", "Table", "info")
    return
  end

  local line = tbl.first + tbl.row
  local start, finish
  if line < editor.line_count() then
    start, finish = line_start(line), line_start(line + 1) - 1
  else
    -- The document's last line: take the line ending before it instead,
    -- so no blank line is left behind.
    finish = editor.length()
    if line > 1 then
      local _, previousEnd = editor.line_range(line - 1)
      start = previousEnd + 1
    else
      start = line_start(line)
    end
  end
  editor.replace_range(start, finish, "")
  editor.set_caret(start)
end

-- Inserts a column left or right of the caret's, in every row.
local function insert_table_column(right)
  local tbl = find_table()
  if not tbl then return not_in_table() end

  local at = tbl.column + (right and 1 or 0)
  rewrite_table(tbl, function(cells, kind)
    local newCell = (kind == 0 and "New") or (kind == 1 and "---") or ""
    table.insert(cells, math.min(at, #cells) + 1, newCell)
    return cells
  end)
end

-- Removes the caret's column from every row. A table keeps at least one.
local function delete_table_column()
  local tbl = find_table()
  if not tbl then return not_in_table() end

  local headerCount = #row_cells(editor.get_line(tbl.first), MAX_TABLE_CELLS)
  if headerCount <= 1 then
    ui.message("A table needs at least one column.", "Table", "warning")
    return
  end
  local removeAt = math.min(tbl.column, headerCount - 1) + 1
  rewrite_table(tbl, function(cells)
    if removeAt <= #cells then table.remove(cells, removeAt) end
    return cells
  end)
end

-- Reorders the data rows by the caret's column: numerically when every
-- cell in it is a number, otherwise as text ignoring case. Rows with equal
-- keys keep their order (the sort is stable).
local function sort_table_rows(descending)
  local tbl = find_table()
  if not tbl then return not_in_table() end
  local count = tbl.last - tbl.first - 1
  if count < 2 then return end

  local rows, numeric = {}, true
  for i = 1, count do
    local line = editor.get_line(tbl.first + 1 + i)
    local key = row_cells(line, MAX_TABLE_CELLS)[tbl.column + 1] or ""
    if numeric and tonumber(key) == nil then numeric = false end
    rows[i] = { line = line, key = key, index = i }
  end
  for _, row in ipairs(rows) do
    row.key = numeric and tonumber(row.key) or row.key:lower()
  end

  table.sort(rows, function(a, b)
    if a.key ~= b.key then
      if descending then return a.key > b.key end
      return a.key < b.key
    end
    return a.index < b.index
  end)

  local start, finish, atEnd = table_range(tbl)
  local eol = editor.eol()
  local out = { editor.get_line(tbl.first), eol, editor.get_line(tbl.first + 1), eol }
  for i, row in ipairs(rows) do
    out[#out + 1] = row.line
    if not (atEnd and i == count) then out[#out + 1] = eol end
  end
  editor.replace_range(start, finish, table.concat(out))
end

-- Sets the alignment marker (":---", ":---:" or "---:") of the caret's
-- column in the separator row, leaving the other columns' as they are.
local function set_table_column_alignment(marker)
  local tbl = find_table()
  if not tbl then return not_in_table() end

  local separator = tbl.first + 1
  local cells = row_cells(editor.get_line(separator), MAX_TABLE_CELLS)
  if tbl.column >= #cells then return not_in_table() end
  cells[tbl.column + 1] = marker
  local start, finish = editor.line_range(separator)
  editor.replace_range(start, finish, format_row(cells))
end

-- Snippets > Markdown > Table Editor: asks for a size, then rebuilds the
-- table at the caret to that size (keeping its header and the cells that
-- still fit), or inserts a new table when the caret isn't in one.
local function table_editor()
  local caret = editor.get_caret()
  local caretLine = editor.line_at(caret)
  local first, last, existing = caretLine, caretLine, false
  if editor.get_line(caretLine):find("|", 1, true) then
    first, last = pipe_lines_around(caretLine)
    existing = last - first >= 1
  end

  -- The existing table's header and data cells (at most 20 columns and 50
  -- data rows, the most ask_table_size allows).
  local header, body, columns = {}, {}, 3
  if existing then
    header = row_cells(editor.get_line(first), 20)
    columns = math.max(#header, 1)
    for n = first + 2, last do
      if #body >= 50 then break end
      body[#body + 1] = row_cells(editor.get_line(n), columns)
    end
  end

  local dataRows, newColumns = ask_table_size("Table Editor", existing and #body or 3, columns)
  if not dataRows then return end

  local eol = editor.eol()
  local function header_cell(column)
    local text = header[column]
    return (text and text ~= "") and text or "Header " .. column
  end
  local function data_cell(row, column)
    return body[row] and body[row][column] or ""
  end

  local out = {}
  local function add_row(cell_for)
    local cells = {}
    for column = 1, newColumns do cells[column] = cell_for(column) end
    out[#out + 1] = format_row(cells) .. eol
  end
  add_row(header_cell)
  add_row(function() return "---" end)
  for row = 1, dataRows do
    add_row(function(column) return data_cell(row, column) end)
  end
  local text = table.concat(out)

  if existing then
    local finish = last < editor.line_count() and line_start(last + 1) - 1 or editor.length()
    editor.replace_range(line_start(first), finish, text)
  else
    if needs_leading_eol(caret) then text = eol .. text end
    editor.insert(text, caret)
  end
end

-- ---- Edit menu text commands ----

-- Edit > Convert > Title Case: capitalises the first letter of each word in
-- the selection and lowercases the rest.
--
-- Only ASCII letters count as letters, so any other byte (punctuation, a
-- digit, part of an accented letter) starts a new word. The exception is an
-- apostrophe (' or ’) straight after a letter, which keeps the word going:
-- "it's" becomes "It's", not "It'S".
local function title_case()
  local start, finish = selection()
  if is_empty(start, finish) then return end

  local selected = editor.get_selection()
  local text = selected:gsub("()(%a)(%a*)", function(position, firstLetter, rest)
    -- Up to 4 bytes before the word: room for a letter and a 3-byte ’.
    local before = selected:sub(math.max(position - 4, 1), position - 1)
    local continuesWord = before:find("%a'$") or before:find("%a\u{2019}$")
    return (continuesWord and firstLetter:lower() or firstLetter:upper()) .. rest:lower()
  end)
  editor.replace_selection(text)
  editor.set_selection(start, start + #text - 1)
end

-- Sort Lines Ascending/Descending (mode "ascending" or "descending", byte
-- order) and Remove Duplicate Lines (mode "unique", keeping the first of
-- each). Works on at least two selected lines, and rejoins them with the
-- document's line ending.
local function transform_lines(mode)
  local first, last = selected_lines()
  if last <= first then return end

  local lines = {}
  for n = first, last do lines[#lines + 1] = editor.get_line(n) end

  if mode == "unique" then
    local seen, kept = {}, {}
    for _, line in ipairs(lines) do
      if not seen[line] then
        seen[line] = true
        kept[#kept + 1] = line
      end
    end
    lines = kept
  elseif mode == "descending" then
    table.sort(lines, function(a, b) return a > b end)
  else
    table.sort(lines)
  end

  replace_lines(first, last, table.concat(lines, editor.eol()))
end

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

local html = {
  html = function() wrap_tag("html") end,
  head = function() wrap_tag("head") end,
  body = function() wrap_tag("body") end,
  paragraph = function() wrap_tag("p") end,
  bold = function() wrap_tag("b") end,
  italic = function() wrap_tag("i") end,
  underline = function() wrap_tag("u") end,
  div = function() wrap_tag("div") end,
  span = function() wrap_tag("span") end,
  line_break = function() editor.replace_selection("<br>") end,
  link = function() insert_with_url('<a href="url">', "link text", "</a>") end,
  image = function() insert_with_url('<img src="url" alt="', "alt text", '">') end,
  table = insert_html_table,
  bullet_list = function() insert_html_list(false) end,
  numbered_list = function() insert_html_list(true) end,
}

local json = {
  format = function() format_json(true) end,
  minify = function() format_json(false) end,
}

local edit = {
  title_case = title_case,
  sort_ascending = function() transform_lines("ascending") end,
  sort_descending = function() transform_lines("descending") end,
  remove_duplicate_lines = function() transform_lines("unique") end,
  trim_whitespace = trim_trailing_whitespace,
  toggle_comment = toggle_comment,
}

for group, functions in pairs({ markdown = markdown, html = html, json = json, edit = edit }) do
  for name, fn in pairs(functions) do
    commands.register(group .. "." .. name, fn)
  end
end
