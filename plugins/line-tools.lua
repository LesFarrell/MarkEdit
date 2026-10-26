-- Selection-only helpers for cleaning up, sorting, and rearranging lines.
-- Commands keep the transformed text selected, so each action is easy to
-- inspect, repeat, or undo.

local function selected_text()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select one or more lines first.", nil, "warning")
    return nil
  end
  return text
end

-- Accept LF, CRLF, or CR in pasted text. The document's preferred line ending
-- is used when the transformed selection is put back.
local function split_lines(text)
  local lines, position = {}, 1
  while true do
    local first = text:find("[\r\n]", position)
    if not first then
      lines[#lines + 1] = text:sub(position)
      return lines, false
    end

    lines[#lines + 1] = text:sub(position, first - 1)
    if text:sub(first, first + 1) == "\r\n" then
      position = first + 2
    else
      position = first + 1
    end

    if position > #text then return lines, true end
  end
end

local function replace_selected_lines(lines, trailing_eol)
  local start = editor.get_selection_range()
  local text = table.concat(lines, editor.eol())
  if trailing_eol and #lines > 0 then text = text .. editor.eol() end

  editor.replace_selection(text)
  if text == "" then
    editor.set_selection(start, start - 1)
  else
    editor.set_selection(start, start + #text - 1)
  end
end

local function transform(fn)
  local text = selected_text()
  if not text then return end
  local lines, trailing_eol = split_lines(text)
  lines = fn(lines)
  replace_selected_lines(lines, trailing_eol)
end

local function decorated_sort(lines, compare)
  local decorated = {}
  for index, line in ipairs(lines) do
    decorated[index] = { line = line, key = line:lower(), index = index }
  end
  table.sort(decorated, function(a, b)
    if a.key == b.key then return a.index < b.index end
    return compare(a.key, b.key)
  end)
  for index, item in ipairs(decorated) do lines[index] = item.line end
  return lines
end

-- Delimiters that end what comes before them, as in "key: value" and
-- "a, b", keep the padding after them; others, such as "=" and "|", line up
-- in a column with a space on each side.
local TRAILING_DELIMITERS = { [","] = true, [":"] = true, [";"] = true }

local function width(text)
  return utf8.len(text) or #text
end

-- Lines up every occurrence of delimiter, column by column. Lines without
-- it are left alone, and a line's indentation is kept.
local function align_on(lines, delimiter)
  local rows, widths = {}, {}
  for index, line in ipairs(lines) do
    local cells, position = {}, 1
    while true do
      local found = line:find(delimiter, position, true)
      if not found then break end
      cells[#cells + 1] = line:sub(position, found - 1)
      position = found + #delimiter
    end
    if #cells > 0 then
      cells[#cells + 1] = line:sub(position)
      for column, cell in ipairs(cells) do
        cells[column] = column == 1 and cell:match("^(.-)%s*$") or cell:match("^%s*(.-)%s*$")
        if column < #cells then
          widths[column] = math.max(widths[column] or 0, width(cells[column]))
        end
      end
      rows[index] = cells
    end
  end
  for index, cells in pairs(rows) do
    local parts = {}
    for column, cell in ipairs(cells) do
      if column == #cells then
        parts[#parts + 1] = cell
      else
        local padding = (" "):rep(widths[column] - width(cell))
        if TRAILING_DELIMITERS[delimiter] then
          parts[#parts + 1] = cell .. delimiter .. padding .. " "
        else
          -- A row of "| a | b |" starts with an empty cell: no space before it.
          local space = (column == 1 and widths[1] == 0) and "" or " "
          parts[#parts + 1] = cell .. padding .. space .. delimiter .. " "
        end
      end
    end
    lines[index] = table.concat(parts):match("^(.-)%s*$")
  end
  return lines
end

menu.add("Line Tools/Align on Delimiter...", function()
  if not selected_text() then return end
  local delimiter = ui.prompt("Align the selected lines on:", config.get("align_delimiter", "="))
  if not delimiter then return end
  if delimiter == "" then
    ui.message("Enter the text to align on, such as = or |.", nil, "warning")
    return
  end
  config.set("align_delimiter", delimiter)
  transform(function(lines) return align_on(lines, delimiter) end)
end)
menu.separator("Line Tools")

menu.add("Line Tools/Clean/Remove Blank Lines", function()
  transform(function(lines)
    local kept = {}
    for _, line in ipairs(lines) do
      if not line:match("^%s*$") then kept[#kept + 1] = line end
    end
    return kept
  end)
end)

menu.add("Line Tools/Clean/Collapse Blank Lines", function()
  transform(function(lines)
    local kept, previous_blank = {}, false
    for _, line in ipairs(lines) do
      local blank = line:match("^%s*$") ~= nil
      if not blank or not previous_blank then
        kept[#kept + 1] = blank and "" or line
      end
      previous_blank = blank
    end
    return kept
  end)
end)

menu.add("Line Tools/Clean/Remove Duplicate Lines", function()
  transform(function(lines)
    local kept, seen = {}, {}
    for _, line in ipairs(lines) do
      if not seen[line] then
        seen[line] = true
        kept[#kept + 1] = line
      end
    end
    return kept
  end)
end)

menu.add("Line Tools/Order/Sort A to Z", function()
  transform(function(lines)
    return decorated_sort(lines, function(a, b) return a < b end)
  end)
end)

menu.add("Line Tools/Order/Sort Z to A", function()
  transform(function(lines)
    return decorated_sort(lines, function(a, b) return a > b end)
  end)
end)

menu.add("Line Tools/Order/Sort by Length", function()
  transform(function(lines)
    local decorated = {}
    for index, line in ipairs(lines) do
      decorated[index] = { line = line, length = utf8.len(line) or #line, index = index }
    end
    table.sort(decorated, function(a, b)
      if a.length == b.length then return a.index < b.index end
      return a.length < b.length
    end)
    for index, item in ipairs(decorated) do lines[index] = item.line end
    return lines
  end)
end)

menu.add("Line Tools/Order/Reverse", function()
  transform(function(lines)
    local reversed = {}
    for index = #lines, 1, -1 do reversed[#reversed + 1] = lines[index] end
    return reversed
  end)
end)

menu.add("Line Tools/Order/Shuffle", function()
  transform(function(lines)
    local original = table.concat(lines, "\n")
    -- Lua 5.4 seeds math.random randomly when it starts. Its generator is
    -- shared by every plugin (only the tables are per plugin), but drawing
    -- from it without calling math.randomseed changes nothing another
    -- plugin could rely on, so no private generator is needed.
    for index = #lines, 2, -1 do
      local other = math.random(index)
      lines[index], lines[other] = lines[other], lines[index]
    end

    -- A shuffle that visibly does nothing feels broken. If possible, rotate
    -- once when Fisher-Yates happens to produce the original order.
    if #lines > 1 and table.concat(lines, "\n") == original then
      local first = table.remove(lines, 1)
      lines[#lines + 1] = first
    end
    return lines
  end)
end)
