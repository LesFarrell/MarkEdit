-- Tinker Editor joins formatting_base.lua, this file and formatting_text.lua into
-- one chunk before loading it, so the helpers used here are formatting_base's
-- locals, and the table commands here are only used by formatting_text.lua.
-- A language server checking this file on its own can't see either.
---@diagnostic disable: undefined-global, unused-local, unused-function

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

local BACKSLASH, PIPE = ("\\"):byte(), ("|"):byte()

-- Finds the next table delimiter. A pipe preceded by an odd run of
-- backslashes is literal Markdown cell text ("a \| b"), not a boundary.
local function next_table_pipe(line, from, limit)
  local escaped = false
  for position = from, limit do
    local byte = line:byte(position)
    if byte == BACKSLASH then
      escaped = not escaped
    else
      if byte == PIPE and not escaped then return position end
      escaped = false
    end
  end
  return nil
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
    local bar = next_table_pipe(line, cursor, limit)
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
  local limit = math.min(offset, #line)
  while cursor <= limit do
    local bar = next_table_pipe(line, cursor, limit)
    if not bar then break end
    column = column + 1
    cursor = bar + 1
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

-- A Markdown table is distinguished from ordinary prose containing pipes by
-- its delimiter row. Require at least three hyphens in every cell, with only
-- the optional leading/trailing colons Markdown uses for alignment.
local function is_table_separator(line)
  local cells = row_cells(line, MAX_TABLE_CELLS)
  if #cells == 0 then return nil end
  for _, cell in ipairs(cells) do
    local hyphens = cell:match("^:?(%-+):?$")
    if not hyphens or #hyphens < 3 then return nil end
  end
  return #cells
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

  local runFirst, last = pipe_lines_around(caretLine)
  local separator
  -- When the caret is in the header, the delimiter is the following line;
  -- otherwise it must already have occurred. A preceding pipe-containing
  -- prose line is therefore not accidentally pulled into the table.
  for n = runFirst + 1, math.min(caretLine + 1, last) do
    local separatorColumns = is_table_separator(editor.get_line(n))
    if separatorColumns and
       separatorColumns == #row_cells(editor.get_line(n - 1), MAX_TABLE_CELLS) then
      separator = n
      break
    end
  end
  if not separator then return nil end
  local first = separator - 1
  if caretLine < first then return nil end
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
  local caret = position
  local row = string.rep("|  ", columns) .. "|"
  if appendAtEnd then
    if needs_leading_eol(position) then
      row = eol .. row
      caret = caret + #eol
    end
  else
    row = row .. eol
  end
  editor.insert(row, position)
  editor.set_caret(caret)
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
    rows[i] = { line = line, cell = key, index = i }
  end
  -- Sort by number when every key is one, otherwise by text ignoring case.
  for _, row in ipairs(rows) do
    if numeric then row.key = tonumber(row.cell) else row.key = row.cell:lower() end
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
  local tbl = find_table()
  local existing = tbl ~= nil

  -- The existing table's header and data cells. A table bigger than
  -- ask_table_size allows (50 data rows, 20 columns) is refused, since
  -- rebuilding it would drop the rows or columns past those limits.
  local header, body, columns = {}, {}, 3
  if tbl then
    header = row_cells(editor.get_line(tbl.first), MAX_TABLE_CELLS)
    columns = math.max(#header, 1)
    if columns > 20 or tbl.last - tbl.first - 1 > 50 then
      ui.message("The Table Editor handles tables of up to 50 data rows and 20 columns.",
        "Table Editor", "warning")
      return
    end
    for n = tbl.first + 2, tbl.last do
      body[#body + 1] = row_cells(editor.get_line(n), columns)
    end
  end

  -- A table with no data rows yet is offered one, the fewest allowed.
  local dataRows, newColumns = ask_table_size("Table Editor",
    existing and math.max(#body, 1) or 3, columns)
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
    local start, finish, atEnd = table_range(tbl)
    if atEnd then text = text:sub(1, -#eol - 1) end
    editor.replace_range(start, finish, text)
  else
    if needs_leading_eol(caret) then text = eol .. text end
    editor.insert(text, caret)
  end
end
