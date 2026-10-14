-- Tools > JSON commands. The plugin owns strict JSON parsing, formatting,
-- validation/error highlighting, JSON string tools, and JSON/YAML conversion.

local ERROR_INDICATOR = 17
local ERROR_COLOUR = 0xef5350
local MAX_DEPTH = 256

local function fence_marker(line_number)
  local line = editor.get_line(line_number)
  local rest = line:sub(#line:match("^ ? ? ?") + 1)
  local marker = rest:match("^(```+)") or rest:match("^(~~~+)")
  if not marker then return nil end
  return marker:sub(1, 1), #marker, rest:sub(#marker + 1)
end

local function fenced_body(line_number)
  local opening, marker, marker_length
  for line = 1, line_number do
    local character, length, tail = fence_marker(line)
    if not opening then
      if character and not (character == "`" and tail:find("`", 1, true)) then
        opening, marker, marker_length = line, character, length
      end
    elseif character == marker and length >= marker_length and tail:match("^[ \t]*$") then
      opening, marker, marker_length = nil, nil, nil
    end
  end
  if not opening or line_number == opening then return nil end
  for line = line_number + 1, editor.line_count() do
    local character, length, tail = fence_marker(line)
    if character == marker and length >= marker_length and tail:match("^[ \t]*$") then
      if line <= opening + 1 then return nil end
      local first = editor.line_range(opening + 1)
      local _, last = editor.line_range(line - 1)
      return first, last
    end
  end
end

local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then
    local fence_first, fence_last = fenced_body(editor.line_at(first))
    if fence_first then
      return editor.get_range(fence_first, fence_last), fence_first, fence_last
    end
    return editor.get_text(), 1, editor.length()
  end
  return editor.get_range(first, last), first, last
end

local function location(text, position)
  local before = text:sub(1, math.max(0, position - 1))
  local line = 1
  for _ in before:gmatch("\n") do line = line + 1 end
  local line_start = before:match(".*\n()") or 1
  local column_text = text:sub(line_start, math.max(line_start - 1, position - 1))
  local column = utf8.len(column_text) or #column_text
  return line, column + 1
end

local function parse_json(text)
  if utf8.len(text) == nil then
    local position = 1
    local function continuation(at)
      local byte = text:byte(at)
      return byte and byte >= 0x80 and byte <= 0xBF
    end
    while position <= #text do
      local byte = text:byte(position)
      local width = byte < 0x80 and 1
        or (byte >= 0xC2 and byte <= 0xDF and 2)
        or (byte >= 0xE0 and byte <= 0xEF and 3)
        or (byte >= 0xF0 and byte <= 0xF4 and 4)
      local valid = width and true or false
      for offset = 1, (width or 1) - 1 do valid = valid and continuation(position + offset) end
      local second = text:byte(position + 1)
      if byte == 0xE0 then valid = valid and second >= 0xA0 end
      if byte == 0xED then valid = valid and second <= 0x9F end
      if byte == 0xF0 then valid = valid and second >= 0x90 end
      if byte == 0xF4 then valid = valid and second <= 0x8F end
      if not valid then break end
      position = position + width
    end
    return nil, { position = position, message = "Invalid UTF-8 byte." }
  end

  local position, length = 1, #text
  local duplicate_keys = {}

  local function fail(message, at)
    return nil, { position = at or position, message = message }
  end

  local function skip_space()
    position = text:find("[^ \t\r\n]", position) or length + 1
  end

  local function read_hex_escape()
    local escape_at = position - 2
    local hex = text:sub(position, position + 3)
    if not hex:match("^%x%x%x%x$") then
      return fail("Expected four hexadecimal digits after \\u.", escape_at)
    end
    position = position + 4
    local code = tonumber(hex, 16)
    if code >= 0xD800 and code <= 0xDBFF then
      if text:sub(position, position + 1) ~= "\\u" then
        return fail("A high surrogate must be followed by a low surrogate.", escape_at)
      end
      local low_hex = text:sub(position + 2, position + 5)
      if not low_hex:match("^%x%x%x%x$") then
        return fail("Expected four hexadecimal digits in the low surrogate.", position)
      end
      local low = tonumber(low_hex, 16)
      if low < 0xDC00 or low > 0xDFFF then
        return fail("Expected a low surrogate after the high surrogate.", position)
      end
      position = position + 6
      code = 0x10000 + (code - 0xD800) * 0x400 + low - 0xDC00
    elseif code >= 0xDC00 and code <= 0xDFFF then
      return fail("A low surrogate cannot appear without a high surrogate.", escape_at)
    end
    return utf8.char(code)
  end

  local escapes = {
    ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f",
    n = "\n", r = "\r", t = "\t"
  }

  local function read_string()
    local start = position
    if text:sub(position, position) ~= '"' then
      return fail("Expected a JSON string.")
    end
    position = position + 1
    local decoded = {}
    while position <= length do
      local byte = text:byte(position)
      if byte == 34 then
        position = position + 1
        return { kind = "string", raw = text:sub(start, position - 1), decoded = table.concat(decoded) }
      elseif byte < 32 then
        return fail("Unescaped control character in string.")
      elseif byte == 92 then
        local slash_at = position
        position = position + 1
        local escape = text:sub(position, position)
        if escape == "" then return fail("Unterminated escape sequence.", slash_at) end
        position = position + 1
        if escape == "u" then
          local value, err = read_hex_escape()
          if not value then return nil, err end
          decoded[#decoded + 1] = value
        elseif escapes[escape] then
          decoded[#decoded + 1] = escapes[escape]
        else
          return fail("Invalid escape sequence \\" .. escape .. ".", slash_at)
        end
      else
        local next_position = utf8.offset(text, 2, position) or length + 1
        decoded[#decoded + 1] = text:sub(position, next_position - 1)
        position = next_position
      end
    end
    return fail("Unterminated JSON string.", start)
  end

  local function read_number()
    local start = position
    if text:sub(position, position) == "-" then
      position = position + 1
      if position > length then return fail("Expected a digit after '-'.") end
    end
    local first = text:sub(position, position)
    if first == "0" then
      position = position + 1
      if text:sub(position, position):match("%d") then
        return fail("Leading zeroes are not allowed in JSON numbers.")
      end
    elseif first:match("[1-9]") then
      repeat position = position + 1 until not text:sub(position, position):match("%d")
    else
      return fail("Expected a JSON value.", start)
    end
    if text:sub(position, position) == "." then
      position = position + 1
      if not text:sub(position, position):match("%d") then
        return fail("Expected a digit after the decimal point.")
      end
      repeat position = position + 1 until not text:sub(position, position):match("%d")
    end
    local exponent = text:sub(position, position)
    if exponent == "e" or exponent == "E" then
      position = position + 1
      local sign = text:sub(position, position)
      if sign == "+" or sign == "-" then position = position + 1 end
      if not text:sub(position, position):match("%d") then
        return fail("Expected a digit in the exponent.")
      end
      repeat position = position + 1 until not text:sub(position, position):match("%d")
    end
    return { kind = "scalar", raw = text:sub(start, position - 1) }
  end

  local read_value
  local function read_array(depth)
    local node = { kind = "array", items = {} }
    position = position + 1
    skip_space()
    if text:sub(position, position) == "]" then position = position + 1; return node end
    while true do
      local value, err = read_value(depth + 1)
      if not value then return nil, err end
      node.items[#node.items + 1] = value
      skip_space()
      local character = text:sub(position, position)
      if character == "]" then position = position + 1; return node end
      if character ~= "," then return fail("Expected ',' or ']' after array item.") end
      position = position + 1
      skip_space()
      if text:sub(position, position) == "]" then
        return fail("Trailing commas are not allowed in JSON arrays.")
      end
    end
  end

  local function read_object(depth)
    local node = { kind = "object", members = {} }
    local seen = {}
    position = position + 1
    skip_space()
    if text:sub(position, position) == "}" then position = position + 1; return node end
    while true do
      local key_position = position
      local key, err = read_string()
      if not key then return nil, err end
      if seen[key.decoded] then
        duplicate_keys[#duplicate_keys + 1] = { position = key_position, key = key.decoded }
      end
      seen[key.decoded] = true
      skip_space()
      if text:sub(position, position) ~= ":" then return fail("Expected ':' after object key.") end
      position = position + 1
      local value
      value, err = read_value(depth + 1)
      if not value then return nil, err end
      node.members[#node.members + 1] = { key = key, value = value, order = #node.members + 1 }
      skip_space()
      local character = text:sub(position, position)
      if character == "}" then position = position + 1; return node end
      if character ~= "," then return fail("Expected ',' or '}' after object member.") end
      position = position + 1
      skip_space()
      if text:sub(position, position) == "}" then
        return fail("Trailing commas are not allowed in JSON objects.")
      end
      if text:sub(position, position) ~= '"' then return fail("Expected an object key string.") end
    end
  end

  read_value = function(depth)
    skip_space()
    local character = text:sub(position, position)
    if character == '"' then return read_string() end
    if character == "[" or character == "{" then
      if depth >= MAX_DEPTH then return fail("JSON nesting exceeds 256 levels.") end
      if character == "[" then return read_array(depth) end
      return read_object(depth)
    end
    for _, literal in ipairs({ "true", "false", "null" }) do
      if text:sub(position, position + #literal - 1) == literal then
        position = position + #literal
        return { kind = "scalar", raw = literal }
      end
    end
    return read_number()
  end

  local root, err = read_value(0)
  if not root then return nil, err end
  skip_space()
  if position <= length then return fail("Unexpected content after the JSON value.") end
  return root, nil, duplicate_keys
end

local function render(node, pretty, eol, depth, sort_keys)
  if node.kind == "string" or node.kind == "scalar" then return node.raw end
  local open, close, entries = "[", "]", node.items
  if node.kind == "object" then
    open, close, entries = "{", "}", node.members
    if sort_keys then
      table.sort(entries, function(a, b)
        if a.key.decoded == b.key.decoded then return a.order < b.order end
        return a.key.decoded < b.key.decoded
      end)
    end
  end
  if #entries == 0 then return open .. close end
  local pieces = {}
  for _, entry in ipairs(entries) do
    local value = entry
    local prefix = ""
    if node.kind == "object" then
      value = entry.value
      prefix = entry.key.raw .. (pretty and ": " or ":")
    end
    pieces[#pieces + 1] = prefix .. render(value, pretty, eol, depth + 1, sort_keys)
  end
  if not pretty then return open .. table.concat(pieces, ",") .. close end
  local inner_indent = string.rep("  ", depth + 1)
  local outer_indent = string.rep("  ", depth)
  return open .. eol .. inner_indent .. table.concat(pieces, "," .. eol .. inner_indent)
    .. eol .. outer_indent .. close
end

local function clear_error()
  editor.clear_indicator(ERROR_INDICATOR)
end

local function show_error(text, base, err, quiet, format_name)
  clear_error()
  local absolute = base + err.position - 1
  local highlight = math.min(absolute, editor.length())
  if highlight >= 1 then
    editor.indicator(highlight, highlight, ERROR_INDICATOR, ERROR_COLOUR)
  end
  if not quiet then
    editor.set_caret(math.min(absolute, editor.length() + 1))
    editor.scroll_to(math.min(absolute, editor.length() + 1))
  end
  local line, column = location(text, err.position)
  local message = string.format("Invalid %s at line %d, column %d: %s",
    format_name or "JSON", line, column, err.message)
  if quiet then ui.status(message) else ui.message(message, nil, "error") end
end

local function transform(pretty, sort_keys)
  local text, first, last = source_range()
  local root, err = parse_json(text)
  if not root then show_error(text, first, err, false); return end
  clear_error()
  local result = render(root, pretty, editor.eol(), 0, sort_keys)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function validate()
  local text, first = source_range()
  local root, err, duplicates = parse_json(text)
  if not root then show_error(text, first, err, false); return end
  clear_error()
  local suffix = #duplicates == 0 and "" or string.format(" Warning: %d duplicate object key%s found.",
    #duplicates, #duplicates == 1 and "" or "s")
  ui.message("Valid JSON." .. suffix, nil, #duplicates == 0 and "info" or "warning")
end

local function escape_string(text)
  local replacements = { ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b",
    ["\f"] = "\\f", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }
  return '"' .. (text:gsub('[%z\1-\31\\"]', function(character)
    return replacements[character] or string.format("\\u%04X", character:byte())
  end)) .. '"'
end

local function yaml_inline(node)
  return node.kind == "string" or node.kind == "scalar"
    or (node.kind == "array" and #node.items == 0)
    or (node.kind == "object" and #node.members == 0)
end

local function render_yaml(node, eol, depth)
  if node.kind == "string" or node.kind == "scalar" then return node.raw end
  if node.kind == "array" and #node.items == 0 then return "[]" end
  if node.kind == "object" and #node.members == 0 then return "{}" end
  local indent = string.rep("  ", depth)
  local lines = {}
  if node.kind == "array" then
    for _, value in ipairs(node.items) do
      if yaml_inline(value) then
        lines[#lines + 1] = indent .. "- " .. render_yaml(value, eol, depth + 1)
      else
        lines[#lines + 1] = indent .. "-" .. eol .. render_yaml(value, eol, depth + 1)
      end
    end
  else
    for _, member in ipairs(node.members) do
      if yaml_inline(member.value) then
        lines[#lines + 1] = indent .. member.key.raw .. ": "
          .. render_yaml(member.value, eol, depth + 1)
      else
        lines[#lines + 1] = indent .. member.key.raw .. ":" .. eol
          .. render_yaml(member.value, eol, depth + 1)
      end
    end
  end
  return table.concat(lines, eol)
end

local function strip_yaml_comment(text)
  local quote, escaped, position = nil, false, 1
  while position <= #text do
    local character = text:sub(position, position)
    if quote == '"' then
      if escaped then escaped = false
      elseif character == "\\" then escaped = true
      elseif character == quote then quote = nil end
    elseif quote == "'" then
      if character == "'" and text:sub(position + 1, position + 1) == "'" then
        position = position + 1
      elseif character == "'" then quote = nil end
    elseif character == '"' or character == "'" then
      quote = character
    elseif character == "#" and (position == 1 or text:sub(position - 1, position - 1):match("%s")) then
      return text:sub(1, position - 1)
    end
    position = position + 1
  end
  return text
end

local function yaml_key_value(content)
  local quote, escaped, position = nil, false, 1
  while position <= #content do
    local character = content:sub(position, position)
    if quote == '"' then
      if escaped then escaped = false
      elseif character == "\\" then escaped = true
      elseif character == quote then quote = nil end
    elseif quote == "'" then
      if character == "'" and content:sub(position + 1, position + 1) == "'" then
        position = position + 1
      elseif character == "'" then quote = nil end
    elseif character == '"' or character == "'" then
      quote = character
    elseif character == ":" then
      local next_character = content:sub(position + 1, position + 1)
      if next_character == "" or next_character:match("%s") then
        return content:sub(1, position - 1):match("^%s*(.-)%s*$"),
          content:sub(position + 1):match("^%s*(.-)%s*$")
      end
    end
    position = position + 1
  end
end

local function parse_yaml(text)
  if utf8.len(text) == nil then return nil, { position = 1, message = "Invalid UTF-8 byte." } end
  local lines, position, line_number = {}, 1, 1
  while position <= #text + 1 do
    local newline = text:find("\n", position, true)
    local finish = newline and newline - 1 or #text
    local raw = text:sub(position, finish)
    if raw:sub(-1) == "\r" then raw = raw:sub(1, -2) end
    if raw:match("^ *\t") then
      return nil, { position = position + #(raw:match("^ *") or ""),
        message = "Tabs cannot be used for YAML indentation." }
    end
    local without_comment = strip_yaml_comment(raw):gsub("%s+$", "")
    if not without_comment:match("^%s*$") then
      local spaces = #(without_comment:match("^ *") or "")
      lines[#lines + 1] = { indent = spaces, content = without_comment:sub(spaces + 1),
        position = position + spaces, line = line_number }
    end
    if not newline then break end
    position, line_number = newline + 1, line_number + 1
  end

  if #lines > 0 and lines[1].content == "---" then table.remove(lines, 1) end
  if #lines > 0 and lines[#lines].content == "..." then table.remove(lines) end
  if #lines == 0 then return nil, { position = 1, message = "Expected a YAML value." } end
  for _, line in ipairs(lines) do
    if line.content == "---" or line.content == "..." then
      return nil, { position = line.position, message = "Multiple YAML documents are not supported." }
    end
  end

  local function yaml_error(line, message, offset)
    return nil, { position = line.position + (offset or 0), message = message }
  end

  local function scalar(raw, line)
    if raw == "|" or raw == ">" or raw:match("^[|>][%+%-]$") then
      return yaml_error(line, "Block scalars are not supported; use a quoted string.")
    end
    if raw:match("^[&*!]") or raw:find("<<:", 1, true) then
      return yaml_error(line, "YAML tags, anchors, aliases, and merge keys are not supported.")
    end
    if raw:sub(1, 1) == '"' then
      local value, err = parse_json(raw)
      if not value or value.kind ~= "string" then
        return yaml_error(line, err and err.message or "Expected one double-quoted string.",
          err and err.position - 1 or 0)
      end
      return value
    end
    if raw:sub(1, 1) == "'" then
      if raw:sub(-1) ~= "'" or #raw < 2 then return yaml_error(line, "Unterminated quoted string.") end
      local inner = raw:sub(2, -2)
      local probe = inner:gsub("''", "")
      if probe:find("'", 1, true) then
        return yaml_error(line, "A single quote inside a YAML string must be doubled.")
      end
      local decoded = inner:gsub("''", "'")
      return { kind = "string", raw = escape_string(decoded), decoded = decoded }
    end
    if raw:sub(1, 1) == "[" or raw:sub(1, 1) == "{" then
      local value, err = parse_json(raw)
      if not value then return yaml_error(line, err.message, err.position - 1) end
      return value
    end
    local lower = raw:lower()
    if lower == "null" or raw == "~" then return { kind = "scalar", raw = "null" } end
    if lower == "true" or lower == "false" then return { kind = "scalar", raw = lower } end
    local number = parse_json(raw)
    if number and number.kind == "scalar" then return number end
    return { kind = "string", raw = escape_string(raw), decoded = raw }
  end

  local parse_block
  local function parse_mapping(index, indent, first_content, first_line)
    local object = { kind = "object", members = {} }
    local seen = {}
    local content, line = first_content, first_line
    while content do
      local key_text, value_text = yaml_key_value(content)
      if not key_text or key_text == "" then return yaml_error(line, "Expected a mapping key followed by ':'.") end
      local key
      if key_text:sub(1, 1) == '"' or key_text:sub(1, 1) == "'" then
        key = scalar(key_text, line)
        if not key then return yaml_error(line, "Invalid quoted mapping key.") end
      else
        key = { kind = "string", raw = escape_string(key_text), decoded = key_text }
      end
      if seen[key.decoded] then return yaml_error(line, "Duplicate mapping key '" .. key.decoded .. "'.") end
      seen[key.decoded] = true
      local value, err
      if value_text == "" then
        if index <= #lines and lines[index].indent > indent then
          value, index, err = parse_block(index, lines[index].indent)
          if not value then return nil, index end
        elseif index <= #lines and lines[index].indent == indent
            and (lines[index].content == "-" or lines[index].content:match("^%-%s")) then
          -- A sequence may sit at its key's indent: "key:\n- a\n- b".
          value, index = parse_block(index, indent)
          if not value then return nil, index end
        else
          value = { kind = "scalar", raw = "null" }
        end
      else
        value, err = scalar(value_text, line)
        if not value then return nil, err end
      end
      object.members[#object.members + 1] = { key = key, value = value,
        order = #object.members + 1 }
      if index > #lines or lines[index].indent ~= indent then break end
      content, line = lines[index].content, lines[index]
      if content:match("^%-%s") or content == "-" then break end
      index = index + 1
    end
    return object, index
  end

  parse_block = function(index, indent)
    local first = lines[index]
    if not first or first.indent ~= indent then
      return yaml_error(first or lines[#lines], "Inconsistent indentation.")
    end
    if first.content == "-" or first.content:match("^%-%s") then
      local array = { kind = "array", items = {} }
      while index <= #lines and lines[index].indent == indent do
        local line = lines[index]
        if line.content ~= "-" and not line.content:match("^%-%s") then break end
        local rest = line.content == "-" and "" or line.content:sub(2):match("^%s+(.-)%s*$")
        index = index + 1
        local value, err
        if rest == "" then
          if index <= #lines and lines[index].indent > indent then
            value, index = parse_block(index, lines[index].indent)
            if not value then return nil, index end
          else
            value = { kind = "scalar", raw = "null" }
          end
        elseif yaml_key_value(rest) then
          value, index = parse_mapping(index, indent + 2, rest, line)
          if not value then return nil, index end
        else
          value, err = scalar(rest, line)
          if not value then return nil, err end
          if index <= #lines and lines[index].indent > indent then
            return yaml_error(lines[index], "A scalar sequence item cannot have nested content.")
          end
        end
        array.items[#array.items + 1] = value
      end
      return array, index
    end
    local key = yaml_key_value(first.content)
    if key then return parse_mapping(index + 1, indent, first.content, first) end
    local value, err = scalar(first.content, first)
    if not value then return nil, err end
    return value, index + 1
  end

  local root, next_index = parse_block(1, lines[1].indent)
  if not root then return nil, next_index end
  if next_index <= #lines then return yaml_error(lines[next_index], "Unexpected indentation or content.") end
  return root
end

local function convert_json_to_yaml()
  local text, first, last = source_range()
  local root, err = parse_json(text)
  if not root then show_error(text, first, err, false); return end
  clear_error()
  local result = render_yaml(root, editor.eol(), 0)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function convert_yaml_to_json()
  local text, first, last = source_range()
  local root, err = parse_yaml(text)
  if not root then show_error(text, first, err, false, "YAML"); return end
  clear_error()
  local result = render(root, true, editor.eol(), 0, false)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function escape_selection()
  local text = editor.get_selection()
  if text == "" then ui.message("Select text to escape as a JSON string.", nil, "warning"); return end
  editor.replace_selection(escape_string(text))
end

local function unescape_selection()
  local text = editor.get_selection()
  if text == "" then ui.message("Select a quoted JSON string to unescape.", nil, "warning"); return end
  local node, err = parse_json(text)
  if not node then
    local first = editor.get_selection_range()
    show_error(text, first, err, false)
    return
  end
  if node.kind ~= "string" then
    ui.message("The selection must be one quoted JSON string.", nil, "warning")
    return
  end
  clear_error()
  editor.replace_selection(node.decoded)
end

local function live_validate()
  if not config.get("validate_on_change", false) or editor.language() ~= "json" then return end
  local text = editor.get_text()
  if text:match("^%s*$") then clear_error(); return end
  local root, err = parse_json(text)
  if root then clear_error() else show_error(text, 1, err, true) end
end

local function format_json() transform(true, false) end
local function minify_json() transform(false, false) end

menu.add("/Tools/JSON/Format JSON", format_json, "Ctrl+Alt+J")
menu.add("/Tools/JSON/Minify JSON", minify_json)
menu.add("/Tools/JSON/Validate", validate, "Ctrl+Alt+V")
menu.add("/Tools/JSON/Sort Object Keys", function() transform(true, true) end)
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Convert JSON to YAML", convert_json_to_yaml)
menu.add("/Tools/JSON/Convert YAML to JSON", convert_yaml_to_json)
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Escape Selection as JSON String", escape_selection)
menu.add("/Tools/JSON/Unescape JSON String", unescape_selection)
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Clear Error Highlight", clear_error)
menu.checkbox("/Tools/JSON/Validate on Change", "validate_on_change", false, function(checked)
  if checked then live_validate() else clear_error() end
end)

events.on("text_changed", live_validate)
events.on("open", live_validate)
events.on("tab_changed", live_validate)
