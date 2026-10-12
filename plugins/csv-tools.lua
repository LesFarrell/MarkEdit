-- Tools > CSV commands for comma-, semicolon-, tab-, and whitespace-separated
-- files. All transformations share this quote-aware parser, including support
-- for escaped quotes and line endings inside quoted fields.

local ERROR_INDICATOR = 13
local ERROR_COLOUR = 0xef5350
local DELIMITERS = { comma = ",", semicolon = ";", tab = "\t", whitespace = " " }

-- Every data command uses the main selection when it contains text and falls
-- back to the whole document only when there is no selection. Replacements
-- keep the transformed range selected and leave surrounding text untouched.
local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

local function delimiter_name(delimiter)
  return delimiter == "," and "comma" or delimiter == ";" and "semicolon"
    or delimiter == "\t" and "tab" or "whitespace"
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
    elseif not quoted then
      if delimiter == " " and character:match("[ \t]") then
        count = count + 1
        repeat position = position + 1 until not line:sub(position, position):match("[ \t]")
        position = position - 1
      elseif character == delimiter then count = count + 1 end
    end
    position = position + 1
  end
  return count
end

local function detect_delimiter(text)
  local line = first_record(text)
  local best, best_count = ",", -1
  for _, delimiter in ipairs({ ",", "\t", ";", " " }) do
    local count = delimiter_count(line, delimiter)
    if count > best_count then best, best_count = delimiter, count end
  end
  return best
end

local function configured_delimiter(text)
  local mode = config.get("delimiter", "auto")
  return DELIMITERS[mode] or detect_delimiter(text)
end

local function parse(text)
  local delimiter = configured_delimiter(text)
  local whitespace = delimiter == " "
  local rows, row, field = {}, {}, {}
  local position, quoted, after_quote, field_started = 1, false, false, false
  local trailing_eol = false

  local function value() return table.concat(field) end
  local function end_field(force)
    if force or field_started or after_quote or not whitespace then row[#row + 1] = value() end
    field, field_started, after_quote = {}, false, false
  end
  local function end_row()
    end_field(not whitespace or #row > 0)
    rows[#rows + 1] = row
    row = {}
  end

  while position <= #text do
    local character = text:sub(position, position)
    if quoted then
      if character == '"' then
        if text:sub(position + 1, position + 1) == '"' then
          field[#field + 1] = '"'
          position = position + 1
        else
          quoted, after_quote = false, true
        end
      else
        field[#field + 1] = character
      end
    else
      local is_newline = character == "\r" or character == "\n"
      local is_delimiter = whitespace and character:match("[ \t]") or character == delimiter
      if after_quote and not is_newline and not is_delimiter and not character:match("[ \t]") then
        return nil, { position = position, message = "Unexpected text after a closing quote." }
      elseif not field_started and not after_quote and character == '"' then
        quoted, field_started = true, true
      elseif character == '"' then
        return nil, { position = position,
          message = "A quoted field must begin with a quote." }
      elseif is_newline then
        end_row()
        if character == "\r" and text:sub(position + 1, position + 1) == "\n" then
          position = position + 1
        end
        trailing_eol = position == #text
      elseif is_delimiter then
        if whitespace then
          if field_started or after_quote then end_field(false) end
          repeat position = position + 1 until not text:sub(position, position):match("[ \t]")
          position = position - 1
        else
          end_field(true)
        end
      elseif after_quote and character:match("[ \t]") then
        -- Padding between a quoted field and a non-whitespace delimiter.
      else
        field[#field + 1] = character
        field_started = true
      end
    end
    position = position + 1
  end
  if quoted then
    return nil, { position = math.max(1, #text), message = "Unterminated quoted field." }
  end
  if field_started or after_quote or #row > 0 or (#text > 0 and not trailing_eol) then end_row() end
  return { rows = rows, delimiter = delimiter, trailing_eol = trailing_eol }
end

local function csv_quote(value, delimiter)
  local must_quote = value:find('"', 1, true) or value:find("\r", 1, true)
    or value:find("\n", 1, true) or value:match("^%s") or value:match("%s$")
  if delimiter == " " then must_quote = must_quote or value:find("%s")
  else must_quote = must_quote or value:find(delimiter, 1, true) end
  if not must_quote then return value end
  return '"' .. value:gsub('"', '""') .. '"'
end

local function serialize(data, delimiter)
  local lines = {}
  for row_index, row in ipairs(data.rows) do
    local fields = {}
    for column, value in ipairs(row) do fields[column] = csv_quote(value, delimiter) end
    lines[row_index] = table.concat(fields, delimiter)
  end
  local result = table.concat(lines, editor.eol())
  if data.trailing_eol and #lines > 0 then result = result .. editor.eol() end
  return result
end

local function clear_error() editor.clear_indicator(ERROR_INDICATOR) end

local function show_error(text, base, problem)
  clear_error()
  local absolute = math.min(base + problem.position - 1, editor.length())
  if absolute >= 1 then editor.indicator(absolute, absolute, ERROR_INDICATOR, ERROR_COLOUR) end
  editor.set_caret(math.min(base + problem.position - 1, editor.length() + 1))
  local before = text:sub(1, problem.position - 1)
  local line = 1
  for _ in before:gmatch("\n") do line = line + 1 end
  local line_start = before:match(".*\n()") or 1
  local column_text = text:sub(line_start, problem.position - 1)
  ui.message(string.format("Delimited-text error at line %d, column %d: %s",
    line, (utf8.len(column_text) or #column_text) + 1, problem.message), nil, "error")
end

local function read_data()
  local text, first, last = source_range()
  local data, problem = parse(text)
  if not data then show_error(text, first, problem); return nil end
  clear_error()
  return data, first, last
end

local function replace_data(data, first, last, delimiter)
  local result = serialize(data, delimiter or data.delimiter)
  editor.replace_range(first, last, result)
  if result == "" then editor.set_selection(first, first - 1)
  else editor.set_selection(first, first + #result - 1) end
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

local function format_columns()
  local data, first, last = read_data()
  if not data then return end
  local columns = maximum_columns(data.rows)
  local rendered, widths = {}, {}
  for row_index, row in ipairs(data.rows) do
    rendered[row_index] = {}
    for column = 1, columns do
      local quoted = '"' .. (row[column] or ""):gsub('"', '""') .. '"'
      rendered[row_index][column] = quoted
      widths[column] = math.max(widths[column] or 0, utf8.len(quoted) or #quoted)
    end
  end
  local lines = {}
  for row_index, row in ipairs(rendered) do
    local fields = {}
    for column, value in ipairs(row) do
      local width = utf8.len(value) or #value
      fields[column] = value .. string.rep(" ", widths[column] - width)
    end
    if data.delimiter == " " then
      -- At least one separating space is needed after each padded field.
      lines[row_index] = table.concat(fields, " ")
    else
      lines[row_index] = table.concat(fields, data.delimiter)
    end
  end
  local result = table.concat(lines, editor.eol())
  if data.trailing_eol and #lines > 0 then result = result .. editor.eol() end
  editor.replace_range(first, last, result)
  if result == "" then editor.set_selection(first, first - 1)
  else editor.set_selection(first, first + #result - 1) end
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
  ui.message(string.format("Separator: %s\nRows: %d\nColumns: %d%s\nEmpty rows: %d%s",
    delimiter_name(data.delimiter), #data.rows, maximum,
    minimum ~= maximum and string.format(" (minimum %d)", minimum) or "", empty,
    inconsistent > 0 and string.format("\nRows with fewer columns: %d", inconsistent) or ""))
end

local function choose_delimiter()
  local labels = { "Auto-detect", "Comma (,)", "Semicolon (;)", "Tab", "Whitespace" }
  local values = { "auto", "comma", "semicolon", "tab", "whitespace" }
  local index = ui.choose(labels, "Separator used when reading data:")
  if index then
    config.set("delimiter", values[index])
    ui.status("CSV separator: " .. labels[index])
  end
end

local function convert(delimiter)
  local data, first, last = read_data()
  if data then replace_data(data, first, last, delimiter) end
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
  table.sort(decorated, function(a, b)
    local before
    if a.number and b.number then before = a.number < b.number
    else before = a.text < b.text end
    if (a.number and b.number and a.number == b.number) or a.text == b.text then
      return a.index < b.index
    end
    return descending and not before or before
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
menu.add("/Tools/CSV/Convert/To Space-Separated", function() convert(" ") end)
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
  for _, row in ipairs(data.rows) do table.remove(row, column) end
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

