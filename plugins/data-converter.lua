-- Small, dependency-free conversions that complement the JSON and CSV
-- plugins: JSON Lines, URL query strings, and key/value records.

local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

local function replace_source(result, first, last)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function json_quote(value)
  return '"' .. value:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\b", "\\b")
    :gsub("\f", "\\f"):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    :gsub("[%z\1-\31]", function(character) return string.format("\\u%04X", character:byte()) end) .. '"'
end

local function skip_space(text, position)
  return text:find("[^ \t\r\n]", position) or (#text + 1)
end

local function scan_string(text, position)
  local start = position
  position = position + 1
  while position <= #text do
    local character = text:sub(position, position)
    if character == "\\" then
      local escaped = text:sub(position + 1, position + 1)
      if escaped == "u" then
        if not text:sub(position + 2, position + 5):match("^%x%x%x%x$") then return nil end
        position = position + 6
      elseif escaped:match('^["\\/bfnrt]$') then position = position + 2
      else return nil end
    elseif character == '"' then return position + 1, text:sub(start, position)
    elseif character:byte() < 32 then return nil
    else position = position + 1 end
  end
end

local function scan_value(text, position)
  position = skip_space(text, position)
  local start, character = position, text:sub(position, position)
  if character == '"' then return scan_string(text, position) end
  if character == "{" or character == "[" then
    local stack, quoted = { character == "{" and "}" or "]" }, false
    position = position + 1
    while position <= #text do
      local current = text:sub(position, position)
      if quoted then
        if current == "\\" then position = position + 1
        elseif current == '"' then quoted = false end
      elseif current == '"' then quoted = true
      elseif current == "{" then stack[#stack + 1] = "}"
      elseif current == "[" then stack[#stack + 1] = "]"
      elseif current == "}" or current == "]" then
        if stack[#stack] ~= current then return nil end
        stack[#stack] = nil
        if #stack == 0 then return position + 1, text:sub(start, position) end
      end
      position = position + 1
    end
    return nil
  end
  local finish = text:find("[,}%]%s]", position) or (#text + 1)
  local raw = text:sub(start, finish - 1)
  if raw == "true" or raw == "false" or raw == "null"
    or raw:match("^-?%d+$") or raw:match("^-?%d+%.%d+[eE]?[+-]?%d*$")
    or raw:match("^-?%d+[eE][+-]?%d+$") then return finish, raw end
end

local function decode_json_string(raw)
  local output, position = {}, 2
  while position < #raw do
    local character = raw:sub(position, position)
    if character ~= "\\" then output[#output + 1], position = character, position + 1
    else
      local escaped = raw:sub(position + 1, position + 1)
      local simple = { ['"']='"', ["\\"]="\\", ["/"]="/", b="\b", f="\f", n="\n", r="\r", t="\t" }
      if escaped == "u" then
        local code = tonumber(raw:sub(position + 2, position + 5), 16)
        output[#output + 1], position = utf8.char(code), position + 6
      else output[#output + 1], position = simple[escaped], position + 2 end
    end
  end
  return table.concat(output)
end

local function split_json_array(text)
  local position = skip_space(text, 1)
  if text:sub(position, position) ~= "[" then return nil, "Expected a JSON array." end
  position = skip_space(text, position + 1)
  local values = {}
  if text:sub(position, position) == "]" then
    position = skip_space(text, position + 1)
    return position > #text and values or nil, "Unexpected text after the array."
  end
  while position <= #text do
    local next_position, raw = scan_value(text, position)
    if not next_position then return nil, "Invalid JSON array value." end
    values[#values + 1], position = raw, skip_space(text, next_position)
    local separator = text:sub(position, position)
    if separator == "]" then
      position = skip_space(text, position + 1)
      if position <= #text then return nil, "Unexpected text after the array." end
      return values
    end
    if separator ~= "," then return nil, "Expected ',' or ']' in the JSON array." end
    position = skip_space(text, position + 1)
  end
  return nil, "Unterminated JSON array."
end

local function parse_flat_object(text)
  local position = skip_space(text, 1)
  if text:sub(position, position) ~= "{" then return nil, "Expected a JSON object." end
  position = skip_space(text, position + 1)
  local fields = {}
  if text:sub(position, position) == "}" then
    return skip_space(text, position + 1) > #text and fields or nil,
      "Unexpected text after the object."
  end
  while position <= #text do
    local after_key, key_raw = scan_string(text, position)
    if not after_key then return nil, "Expected a quoted object key." end
    position = skip_space(text, after_key)
    if text:sub(position, position) ~= ":" then return nil, "Expected ':' after the object key." end
    local after_value, value_raw = scan_value(text, position + 1)
    if not after_value or value_raw:match("^[%[{]") then
      return nil, "Only flat objects with scalar values are supported."
    end
    local value = value_raw:sub(1, 1) == '"' and decode_json_string(value_raw) or value_raw
    fields[#fields + 1] = { key = decode_json_string(key_raw), value = value }
    position = skip_space(text, after_value)
    local separator = text:sub(position, position)
    if separator == "}" then
      position = skip_space(text, position + 1)
      if position <= #text then return nil, "Unexpected text after the object." end
      return fields
    end
    if separator ~= "," then return nil, "Expected ',' or '}' in the object." end
    position = skip_space(text, position + 1)
  end
  return nil, "Unterminated JSON object."
end

local function url_encode(text)
  return (text:gsub("([^A-Za-z0-9%-%._~])", function(character)
    return string.format("%%%02X", character:byte())
  end))
end

local function url_decode(text)
  text = text:gsub("+", " ")
  local position = 1
  while true do
    local percent = text:find("%", position, true)
    if not percent then break end
    if not text:sub(percent + 1, percent + 2):match("^%x%x$") then return nil end
    position = percent + 3
  end
  return (text:gsub("%%(%x%x)", function(hex) return string.char(tonumber(hex, 16)) end))
end

local function fields_to_json(fields)
  local lines = {}
  for index, field in ipairs(fields) do
    lines[index] = "  " .. json_quote(field.key) .. ": " .. json_quote(field.value)
  end
  return "{" .. (#lines > 0 and editor.eol() .. table.concat(lines, "," .. editor.eol()) .. editor.eol() or "") .. "}"
end

local function convert_json_array_to_lines()
  local text, first, last = source_range()
  local values, problem = split_json_array(text)
  if not values then ui.message(problem, nil, "warning"); return end
  replace_source(table.concat(values, editor.eol()), first, last)
end

local function convert_json_lines_to_array()
  local text, first, last = source_range()
  local values = {}
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:match("%S") then
      local finish = scan_value(line, 1)
      if not finish or skip_space(line, finish) <= #line then
        ui.message("Every non-empty line must contain one JSON value.", nil, "warning"); return
      end
      values[#values + 1] = line:match("^%s*(.-)%s*$")
    end
  end
  local indent = {}
  for index, value in ipairs(values) do indent[index] = "  " .. value end
  local result = "[" .. (#indent > 0 and editor.eol() .. table.concat(indent, "," .. editor.eol()) .. editor.eol() or "") .. "]"
  replace_source(result, first, last)
end

local function query_to_json()
  local text, first, last = source_range()
  text = text:gsub("^%?", "")
  local fields = {}
  for pair in (text .. "&"):gmatch("(.-)&") do
    if pair ~= "" then
      local key, value = pair:match("^([^=]*)=(.*)$")
      if not key then key, value = pair, "" end
      key, value = url_decode(key), url_decode(value)
      if not key or not value then ui.message("The query string contains an invalid escape.", nil, "warning"); return end
      fields[#fields + 1] = { key = key, value = value }
    end
  end
  replace_source(fields_to_json(fields), first, last)
end

local function object_to_query()
  local text, first, last = source_range()
  local fields, problem = parse_flat_object(text)
  if not fields then ui.message(problem, nil, "warning"); return end
  local pairs = {}
  for index, field in ipairs(fields) do pairs[index] = url_encode(field.key) .. "=" .. url_encode(field.value) end
  replace_source(table.concat(pairs, "&"), first, last)
end

local function key_values_to_json()
  local text, first, last = source_range()
  local fields = {}
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:match("%S") then
      local key, value = line:match("^%s*([^=:]-)%s*[=:]%s*(.-)%s*$")
      if not key or key == "" then ui.message("Expected key=value or key: value on every line.", nil, "warning"); return end
      fields[#fields + 1] = { key = key, value = value }
    end
  end
  replace_source(fields_to_json(fields), first, last)
end

local function object_to_key_values()
  local text, first, last = source_range()
  local fields, problem = parse_flat_object(text)
  if not fields then ui.message(problem, nil, "warning"); return end
  local lines = {}
  for index, field in ipairs(fields) do lines[index] = field.key .. "=" .. field.value end
  replace_source(table.concat(lines, editor.eol()), first, last)
end

local ROOT = "/Tools/Data Converter"
menu.add(ROOT .. "/JSON Array to JSON Lines", convert_json_array_to_lines)
menu.add(ROOT .. "/JSON Lines to JSON Array", convert_json_lines_to_array)
menu.separator(ROOT)
menu.add(ROOT .. "/Query String to JSON", query_to_json)
menu.add(ROOT .. "/JSON to Query String", object_to_query)
menu.separator(ROOT)
menu.add(ROOT .. "/Key-Value Lines to JSON", key_values_to_json)
menu.add(ROOT .. "/JSON to Key-Value Lines", object_to_key_values)
