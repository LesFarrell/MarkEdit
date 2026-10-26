-- Tools > CSV commands for comma-, semicolon-, and tab-separated files. All transformations share this quote-aware parser, including support
-- for escaped quotes and line endings inside quoted fields. Inspect and Validate
-- reports in the output panel.

local ERROR_INDICATOR = 13
local ERROR_COLOUR = 0xef5350
local DELIMITERS = { comma = ",", semicolon = ";", tab = "\t" }

-- Every data command uses the main selection when it contains text and falls
-- back to the whole document only when there is no selection. Replacements
-- keep the transformed range selected and leave surrounding text untouched.
local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

local function delimiter_name(delimiter)
  return delimiter == ";" and "semicolon" or delimiter == "\t" and "tab" or "comma"
end

local function first_record(text)
  local quoted, position = false, 1
  while position <= #text do
    local character = text:sub(position, position)
    if character == '"' then
      if quoted and text:sub(position + 1, position + 1) == '"' then position = position + 1
      else quoted = not quoted end
    elseif not quoted and (character == "\r" or character == "\n") then
      return text:sub(1, position - 1)
    end
    position = position + 1
  end
  return text
end

local function delimiter_count(line, delimiter)
  local quoted, count, position = false, 0, 1
  while position <= #line do
    local character = line:sub(position, position)
    if character == '"' then
      if quoted and line:sub(position + 1, position + 1) == '"' then position = position + 1
      else quoted = not quoted end
    elseif not quoted and character == delimiter then
      count = count + 1
    end
    position = position + 1
  end
  return count
end

-- The separator the first record uses most, comma when it has none.
local function detect_delimiter(text)
  local line = first_record(text)
  local best, best_count = ",", 0
  for _, delimiter in ipairs({ ",", "\t", ";" }) do
    local count = delimiter_count(line, delimiter)
    if count > best_count then best, best_count = delimiter, count end
  end
  return best
end

local function configured_delimiter(text)
  local mode = config.get("delimiter", "auto")
  return DELIMITERS[mode] or detect_delimiter(text)
end

-- text:sub(first, last) without the blanks around it. Stepping over them
-- keeps this linear, where a "^%s*(.-)%s*$" match is quadratic on long runs.
local function trimmed(text, first, last)
  while first <= last and text:find("^%s", first) do first = first + 1 end
  while last >= first and text:find("^%s", last) do last = last - 1 end
  return text:sub(first, last)
end

-- Splits text into rows of field values. With raw set, each field is its
-- source text as written instead (quotes included, surrounding blanks
-- trimmed), for Format Columns to realign without re-quoting. Fields are
-- scanned a run at a time rather than a character at a time. With on_row
-- given, each row is passed to it as soon as it is read instead of being
-- kept in rows, so a command that handles a row at a time streams.
local function parse(text, raw, on_row)
  local delimiter = configured_delimiter(text)
  local stops = "[" .. delimiter:gsub("%p", "%%%0") .. '\r\n"]'
  local rows, row = {}, {}
  local position, length = 1, #text
  local trailing_eol = false
  if length == 0 then return { rows = rows, delimiter = delimiter, trailing_eol = false } end

  while true do
    local field_begin = position
    local stop = text:find(stops, position) or length + 1
    local value
    if text:sub(stop, stop) == '"' then
      -- Blanks before the opening quote, as in a, "b", aren't part of it.
      if not text:sub(position, stop - 1):match("^[ \t]*$") then
        return nil, { position = stop, message = "A quoted field must begin with a quote." }
      end
      local pieces, from = {}, stop + 1
      while true do
        local quote = text:find('"', from, true)
        if not quote then
          return nil, { position = math.max(1, length), message = "Unterminated quoted field." }
        end
        if text:sub(quote + 1, quote + 1) == '"' then
          pieces[#pieces + 1] = text:sub(from, quote)
          from = quote + 2
        else
          pieces[#pieces + 1] = text:sub(from, quote - 1)
          stop = quote + 1
          break
        end
      end
      value = table.concat(pieces)
      -- Only padding may sit between the closing quote and the separator,
      -- which a tab separator can't be.
      stop = text:find(delimiter == "\t" and "[^ ]" or "[^ \t]", stop) or length + 1
      local after = text:sub(stop, stop)
      if after ~= "" and after ~= delimiter and after ~= "\r" and after ~= "\n" then
        return nil, { position = stop, message = "Unexpected text after a closing quote." }
      end
    else
      value = text:sub(position, stop - 1)
    end
    row[#row + 1] = raw and trimmed(text, field_begin, stop - 1) or value

    local ending = text:sub(stop, stop)
    if ending == delimiter then
      position = stop + 1
    else
      if on_row then on_row(row) else rows[#rows + 1] = row end
      row = {}
      if ending == "" then break end
      if ending == "\r" and text:sub(stop + 1, stop + 1) == "\n" then stop = stop + 1 end
      position = stop + 1
      -- A line ending can only be trailing when it's the last character;
      -- anywhere else, a record follows it.
      if position > length then trailing_eol = true; break end
    end
  end
  return { rows = rows, delimiter = delimiter, trailing_eol = trailing_eol }
end

local function csv_quote(value, delimiter)
  local must_quote = value:find('"', 1, true) or value:find("\r", 1, true)
    or value:find("\n", 1, true) or value:match("^%s") or value:match("%s$")
    or value:find(delimiter, 1, true)
  if not must_quote then return value end
  return '"' .. value:gsub('"', '""') .. '"'
end

-- Collects output a piece at a time, joining every few thousand pieces, so
-- a large result is never held as one table entry per line.
local function new_writer()
  local chunks, pieces = {}, {}
  local writer = {}
  function writer.add(piece)
    pieces[#pieces + 1] = piece
    if #pieces >= 4096 then
      chunks[#chunks + 1] = table.concat(pieces)
      pieces = {}
    end
  end
  function writer.result()
    chunks[#chunks + 1] = table.concat(pieces)
    return table.concat(chunks)
  end
  return writer
end

-- Writes rows as delimited lines: add(row) for each, in order, then
-- result(trailing_eol) for the text.
local function new_row_writer(delimiter)
  local out, fields, eol, count = new_writer(), {}, editor.eol(), 0
  local rows = {}
  function rows.add(row)
    if count > 0 then out.add(eol) end
    count = count + 1
    for column, value in ipairs(row) do fields[column] = csv_quote(value, delimiter) end
    out.add(table.concat(fields, delimiter, 1, #row))
  end
  function rows.result(trailing_eol)
    if trailing_eol and count > 0 then out.add(eol) end
    return out.result()
  end
  return rows
end

local function serialize(data, delimiter)
  local writer = new_row_writer(delimiter)
  for _, row in ipairs(data.rows) do writer.add(row) end
  return writer.result(data.trailing_eol)
end

-- Replaces first..last with result and leaves it selected.
local function replace_text(result, first, last)
  editor.replace_range(first, last, result)
  if result == "" then editor.set_selection(first, first - 1)
  else editor.set_selection(first, first + #result - 1) end
end

local function clear_error() editor.clear_indicator(ERROR_INDICATOR) end

-- Highlights problem, found in the text starting at document position base,
-- and reports it at its document line and column, so an error in a
-- selection is described where the user sees it, whatever the line endings.
local function show_error(base, problem)
  clear_error()
  local absolute = math.min(base + problem.position - 1, editor.length())
  if absolute >= 1 then editor.indicator(absolute, absolute, ERROR_INDICATOR, ERROR_COLOUR) end
  local caret = math.min(base + problem.position - 1, editor.length() + 1)
  editor.set_caret(caret)
  local line = editor.line_at(caret)
  local line_start = editor.line_range(line)
  local before = editor.get_range(line_start, caret - 1)
  ui.message(string.format("Delimited-text error at line %d, column %d: %s",
    line, (utf8.len(before) or #before) + 1, problem.message), nil, "error")
end

local function read_data(raw, on_row)
  local text, first, last = source_range()
  local data, problem = parse(text, raw, on_row)
  if not data then show_error(first, problem); return nil end
  clear_error()
  return data, first, last
end

local function replace_data(data, first, last)
  replace_text(serialize(data, data.delimiter), first, last)
end

local function maximum_columns(rows)
  local maximum = 0
  for _, row in ipairs(rows) do maximum = math.max(maximum, #row) end
  return maximum
end

local function transform(fn)
  local data, first, last = read_data()
  if not data then return end
  fn(data)
  replace_data(data, first, last)
end

-- Each field keeps the text it was written with - quoted or not - so only
-- the separators move. The blanks around it were trimmed by the parser,
-- which also drops the padding of an earlier Format Columns. The text is
-- read twice, for the column widths and then to write it, rather than kept.
local function format_columns()
  local text, first, last = source_range()
  local columns, widths = 0, {}
  local data, problem = parse(text, true, function(row)
    columns = math.max(columns, #row)
    for column, value in ipairs(row) do
      widths[column] = math.max(widths[column] or 0, utf8.len(value) or #value)
    end
  end)
  if not data then show_error(first, problem); return end
  clear_error()
  local out, fields, eol, count = new_writer(), {}, editor.eol(), 0
  parse(text, true, function(row)
    if count > 0 then out.add(eol) end
    count = count + 1
    for column = 1, columns do
      -- The last column has no separator after it to line up, so it isn't
      -- padded: lines don't end in trailing blanks.
      local value = row[column] or ""
      local width = utf8.len(value) or #value
      local padding = column < columns and widths[column] - width or 0
      fields[column] = value .. string.rep(" ", padding)
    end
    out.add(table.concat(fields, data.delimiter, 1, columns))
  end)
  if data.trailing_eol and count > 0 then out.add(eol) end
  replace_text(out.result(), first, last)
end

local function ask_column(label, maximum)
  local answer = ui.prompt(label .. string.format(" (1-%d):", maximum), "1")
  local column = answer and tonumber(answer)
  if not column or column % 1 ~= 0 or column < 1 or column > maximum then
    if answer then ui.message("Enter a column number from 1 to " .. maximum .. ".", nil, "warning") end
    return nil
  end
  return column
end

local function inspect_data()
  local data = read_data()
  if not data then return end
  local maximum, minimum = maximum_columns(data.rows), math.huge
  local empty, inconsistent = 0, 0
  for _, row in ipairs(data.rows) do
    minimum = math.min(minimum, #row)
    if #row ~= maximum then inconsistent = inconsistent + 1 end
    local blank = true
    for _, value in ipairs(row) do if value ~= "" then blank = false; break end end
    if blank then empty = empty + 1 end
  end
  if minimum == math.huge then minimum = 0 end
  output.report("CSV Inspection", string.format("Separator: %s\nRows: %d\nColumns: %d%s\nEmpty rows: %d%s",
    delimiter_name(data.delimiter), #data.rows, maximum,
    minimum ~= maximum and string.format(" (minimum %d)", minimum) or "", empty,
    inconsistent > 0 and string.format("\nRows with fewer columns: %d", inconsistent) or ""))
end

local function choose_delimiter()
  local labels = { "Auto-detect", "Comma (,)", "Semicolon (;)", "Tab" }
  local values = { "auto", "comma", "semicolon", "tab" }
  local index = ui.choose(labels, "Separator used when reading data:")
  if index then
    config.set("delimiter", values[index])
    ui.status("CSV separator: " .. labels[index])
  end
end

-- Writes each row as it is read, so converting doesn't keep the table.
local function convert(delimiter)
  local writer = new_row_writer(delimiter)
  local data, first, last = read_data(false, writer.add)
  if data then replace_text(writer.result(data.trailing_eol), first, last) end
end

local function sort_rows(descending)
  local data, first, last = read_data()
  if not data or #data.rows < 2 then return end
  local column = ask_column("Sort column", maximum_columns(data.rows))
  if not column then return end
  local header = table.remove(data.rows, 1)
  local decorated = {}
  for index, row in ipairs(data.rows) do
    local value = row[column] or ""
    decorated[index] = { row = row, text = value:lower(), number = tonumber(value), index = index }
  end
  -- Numbers sort before text, so a column mixing both still has one
  -- consistent order. Returns nil for equal keys.
  local function precedes(a, b)
    if a.number and b.number then
      if a.number ~= b.number then return a.number < b.number end
    elseif a.number or b.number then
      return a.number ~= nil
    elseif a.text ~= b.text then
      return a.text < b.text
    end
    return nil
  end
  table.sort(decorated, function(a, b)
    local before = precedes(a, b)
    -- Equal keys keep their original order in both directions.
    if before == nil then return a.index < b.index end
    if descending then return not before end
    return before
  end)
  data.rows = { header }
  for _, item in ipairs(decorated) do data.rows[#data.rows + 1] = item.row end
  replace_data(data, first, last)
end

local function json_string(value)
  return '"' .. value:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\b", "\\b")
    :gsub("\f", "\\f"):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    :gsub("[%z\1-\31]", function(character) return string.format("\\u%04X", character:byte()) end) .. '"'
end

local function to_json()
  local data, first, last = read_data()
  if not data or #data.rows == 0 then return end
  local headers, seen = {}, {}
  for column = 1, maximum_columns(data.rows) do
    local base = data.rows[1][column] or ""
    if base == "" then base = "Column " .. column end
    local name, suffix = base, 2
    while seen[name] do name, suffix = base .. " " .. suffix, suffix + 1 end
    seen[name], headers[column] = true, name
  end
  local eol, objects = editor.eol(), {}
  for row_index = 2, #data.rows do
    local fields = {}
    for column, header in ipairs(headers) do
      fields[column] = "    " .. json_string(header) .. ": " .. json_string(data.rows[row_index][column] or "")
    end
    objects[#objects + 1] = "  {" .. eol .. table.concat(fields, "," .. eol) .. eol .. "  }"
  end
  local result = "[" .. (#objects > 0 and eol .. table.concat(objects, "," .. eol) .. eol or "") .. "]"
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function markdown_cell(value)
  return value:gsub("\\", "\\\\"):gsub("|", "\\|"):gsub("[\r\n]+", " ")
end

local function to_markdown()
  local data, first, last = read_data()
  if not data or #data.rows == 0 then return end
  local columns, lines = maximum_columns(data.rows), {}
  local function row(values)
    local cells = {}
    for column = 1, columns do cells[column] = " " .. markdown_cell(values[column] or "") .. " " end
    return "|" .. table.concat(cells, "|") .. "|"
  end
  lines[1] = row(data.rows[1])
  local separators = {}
  for column = 1, columns do separators[column] = "---" end
  lines[2] = row(separators)
  for index = 2, #data.rows do lines[#lines + 1] = row(data.rows[index]) end
  local result = table.concat(lines, editor.eol())
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

menu.add("/Tools/CSV/Inspect and Validate", inspect_data)
menu.add("/Tools/CSV/Choose Input Separator...", choose_delimiter)
menu.add("/Tools/CSV/Normalize Quoting", function() transform(function() end) end)
menu.add("/Tools/CSV/Format Columns", format_columns)
menu.separator("/Tools/CSV")
menu.add("/Tools/CSV/Convert/To Comma-Separated", function() convert(",") end)
menu.add("/Tools/CSV/Convert/To Semicolon-Separated", function() convert(";") end)
menu.add("/Tools/CSV/Convert/To Tab-Separated", function() convert("\t") end)
menu.add("/Tools/CSV/Convert/To JSON Objects", to_json)
menu.add("/Tools/CSV/Convert/To Markdown Table", to_markdown)
menu.separator("/Tools/CSV")
menu.add("/Tools/CSV/Rows/Sort by Column Ascending...", function() sort_rows(false) end)
menu.add("/Tools/CSV/Rows/Sort by Column Descending...", function() sort_rows(true) end)
menu.add("/Tools/CSV/Rows/Reverse (Keep Header)", function()
  transform(function(data)
    local reversed = { data.rows[1] }
    for index = #data.rows, 2, -1 do reversed[#reversed + 1] = data.rows[index] end
    data.rows = reversed
  end)
end)
menu.add("/Tools/CSV/Rows/Remove Duplicate Rows", function()
  transform(function(data)
    local kept, seen = {}, {}
    for index, row in ipairs(data.rows) do
      -- Length prefixes avoid collisions when field text contains the
      -- separator byte (for example {"a\0b", "c"} versus {"a", "b\0c"}).
      local parts = {}
      for column, value in ipairs(row) do
        parts[column] = tostring(#value) .. ":" .. value
      end
      local key = table.concat(parts)
      if index == 1 or not seen[key] then kept[#kept + 1], seen[key] = row, true end
    end
    data.rows = kept
  end)
end)
menu.add("/Tools/CSV/Rows/Remove Empty Rows", function()
  transform(function(data)
    local kept = {}
    for _, row in ipairs(data.rows) do
      local nonempty = false
      for _, value in ipairs(row) do if value ~= "" then nonempty = true; break end end
      if nonempty then kept[#kept + 1] = row end
    end
    data.rows = kept
  end)
end)
menu.add("/Tools/CSV/Rows/Filter Rows Containing...", function()
  local data, first, last = read_data()
  if not data then return end
  local query = ui.prompt("Keep rows containing:")
  if not query then return end
  query = query:lower()
  local kept = { data.rows[1] }
  for row_index = 2, #data.rows do
    for _, value in ipairs(data.rows[row_index]) do
      if value:lower():find(query, 1, true) then kept[#kept + 1] = data.rows[row_index]; break end
    end
  end
  data.rows = kept
  replace_data(data, first, last)
end)
menu.separator("/Tools/CSV/Rows")
menu.add("/Tools/CSV/Columns/Trim Cell Whitespace", function()
  transform(function(data)
    for _, row in ipairs(data.rows) do
      for column, value in ipairs(row) do row[column] = value:match("^%s*(.-)%s*$") end
    end
  end)
end)
menu.add("/Tools/CSV/Columns/Transpose Rows and Columns", function()
  transform(function(data)
    local transposed, columns = {}, maximum_columns(data.rows)
    for column = 1, columns do
      transposed[column] = {}
      for row = 1, #data.rows do transposed[column][row] = data.rows[row][column] or "" end
    end
    data.rows = transposed
  end)
end)
menu.add("/Tools/CSV/Columns/Add Row Number Column", function()
  transform(function(data)
    for index, row in ipairs(data.rows) do table.insert(row, 1, index == 1 and "Row" or tostring(index - 1)) end
  end)
end)
menu.add("/Tools/CSV/Columns/Remove Column...", function()
  local data, first, last = read_data()
  if not data then return end
  local column = ask_column("Remove column", maximum_columns(data.rows))
  if not column then return end
  for _, row in ipairs(data.rows) do
    -- Short (ragged) rows may not have this column at all.
    if column <= #row then table.remove(row, column) end
  end
  replace_data(data, first, last)
end)
menu.add("/Tools/CSV/Columns/Keep Columns...", function()
  local data, first, last = read_data()
  if not data then return end
  local answer = ui.prompt("Column numbers to keep (for example 1,3,5):")
  if not answer then return end
  local wanted = {}
  for number in answer:gmatch("%d+") do wanted[#wanted + 1] = tonumber(number) end
  if #wanted == 0 then ui.message("Enter at least one column number.", nil, "warning"); return end
  for row_index, row in ipairs(data.rows) do
    local replacement = {}
    for _, column in ipairs(wanted) do replacement[#replacement + 1] = row[column] or "" end
    data.rows[row_index] = replacement
  end
  replace_data(data, first, last)
end)

