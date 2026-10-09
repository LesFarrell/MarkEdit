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

-- A small private pseudo-random generator avoids changing Lua's shared
-- math.random state, which could surprise another plugin.
local random_state = 1
local seed_text = app.date_time()
for index = 1, #seed_text do
  random_state = (random_state * 131 + seed_text:byte(index)) % 2147483647
end

local function random_index(limit)
  random_state = (random_state * 48271) % 2147483647
  return random_state % limit + 1
end

menu.add("Line Tools/Order/Shuffle", function()
  transform(function(lines)
    local original = table.concat(lines, "\n")
    for index = #lines, 2, -1 do
      local other = random_index(index)
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
