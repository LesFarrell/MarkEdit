-- Natural line sorting: digit runs compare by numeric magnitude, so item2
-- sorts before item10. Commands require a selection and preserve its EOL.

local function split_lines(text)
  local lines, position = {}, 1
  while true do
    local first = text:find("[\r\n]", position)
    if not first then lines[#lines + 1] = text:sub(position); return lines, false end
    lines[#lines + 1] = text:sub(position, first - 1)
    position = first + (text:sub(first, first + 1) == "\r\n" and 2 or 1)
    if position > #text then return lines, true end
  end
end

local function chunks(text)
  local result, position = {}, 1
  text = text:lower()
  while position <= #text do
    local first, last = text:find("%d+", position)
    if first ~= position then
      local finish = first and first - 1 or #text
      result[#result + 1] = { number = false, value = text:sub(position, finish) }
      position = finish + 1
    else
      local raw = text:sub(first, last)
      local significant = raw:match("^0*(.-)$")
      if significant == "" then significant = "0" end
      result[#result + 1] = { number = true, value = significant, width = #raw }
      position = last + 1
    end
  end
  return result
end

local function natural_compare(left, right)
  local a, b = left.parts, right.parts
  for index = 1, math.max(#a, #b) do
    if not a[index] then return b[index] and -1 or 0 end
    if not b[index] then return 1 end
    local x, y = a[index], b[index]
    if x.number and y.number then
      if #x.value ~= #y.value then return #x.value < #y.value and -1 or 1 end
      if x.value ~= y.value then return x.value < y.value and -1 or 1 end
      if x.width ~= y.width then return x.width < y.width and -1 or 1 end
    elseif x.number ~= y.number then
      return x.number and -1 or 1
    elseif x.value ~= y.value then
      return x.value < y.value and -1 or 1
    end
  end
  return 0
end

local function sort_lines(descending, unique)
  local text = editor.get_selection()
  if text == "" then ui.message("Select the lines to sort.", nil, "warning"); return end
  local start = editor.get_selection_range()
  local lines, trailing = split_lines(text)
  local decorated = {}
  for index, line in ipairs(lines) do
    decorated[index] = { line = line, parts = chunks(line), index = index }
  end
  table.sort(decorated, function(a, b)
    local order = natural_compare(a, b)
    if order == 0 then return a.index < b.index end
    if descending then return order > 0 end
    return order < 0
  end)
  local output, seen = {}, {}
  for _, item in ipairs(decorated) do
    if not unique or not seen[item.line] then
      output[#output + 1], seen[item.line] = item.line, true
    end
  end
  local result = table.concat(output, editor.eol())
  if trailing then result = result .. editor.eol() end
  editor.replace_selection(result)
  editor.set_selection(start, start + #result - 1)
end

menu.add("/Edit/Convert/Natural Sort Lines Ascending", function() sort_lines(false, false) end)
menu.add("/Edit/Convert/Natural Sort Lines Descending", function() sort_lines(true, false) end)
menu.add("/Edit/Convert/Natural Sort and Remove Duplicates", function() sort_lines(false, true) end)
