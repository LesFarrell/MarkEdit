-- Tools > JSON commands. The plugin owns strict JSON parsing, formatting,
-- JSON string tools, and JSON/YAML conversion. Checking JSON, and
-- highlighting errors as you type, is json-check.lua's job; these commands
-- only point out the error that stops them.

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

-- The document line and column (in characters) of position, so an error in
-- a selection or code block is reported where the user sees it, whatever
-- the line endings.
local function location(position)
  local line = editor.line_at(position)
  local line_start = editor.line_range(line)
  local before = editor.get_range(line_start, position - 1)
  return line, (utf8.len(before) or #before) + 1
end

-- Parsed JSON is kept small, since a large document has to fit in the
-- plugins' shared memory: a string, number or literal is its raw source
-- text (a string's begins with its quote), an array is one table
-- { "[", item, item, ... }, and an object is one table
-- { "{", key, value, key, value, ... } with each key's raw string.

local ESCAPES = {
  ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f",
  n = "\n", r = "\r", t = "\t"
}

local function is_string(node)
  return type(node) == "string" and node:sub(1, 1) == '"'
end

-- The text a JSON string literal (quotes included) stands for. raw must be
-- valid, as parse_json() and escape_string() leave it, so it is only decoded
-- where the text is needed rather than for every string in the document.
local function decode_string(raw)
  local pieces, position, finish = {}, 2, #raw - 1
  while position <= finish do
    local slash = raw:find("\\", position, true)
    if not slash or slash > finish then
      pieces[#pieces + 1] = raw:sub(position, finish)
      break
    end
    pieces[#pieces + 1] = raw:sub(position, slash - 1)
    local escape = raw:sub(slash + 1, slash + 1)
    if escape == "u" then
      local code = tonumber(raw:sub(slash + 2, slash + 5), 16)
      position = slash + 6
      if code >= 0xD800 and code <= 0xDBFF then
        local low = tonumber(raw:sub(slash + 8, slash + 11), 16)
        code = 0x10000 + (code - 0xD800) * 0x400 + low - 0xDC00
        position = slash + 12
      end
      pieces[#pieces + 1] = utf8.char(code)
    else
      pieces[#pieces + 1] = ESCAPES[escape]
      position = slash + 2
    end
  end
  return table.concat(pieces)
end

-- Collects output a piece at a time, joining every few thousand pieces, so
-- a large result is never held as one table entry per token, nor rebuilt
-- by concatenation at every nesting level.
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

-- parse_json() reports what it reads to a sink, as calls in document order:
-- value(raw) for a string, number or literal; open(kind) and
-- close(kind, count) around an "array" or "object"; item(index) before each
-- of its entries; and key(raw) before each object member's value. A sink's
-- result() is what parse_json() returns.

-- Builds the parsed value, for the commands that need all of it at once.
local function new_tree_builder()
  local stack, free_nodes, root = {}, {}, nil
  local builder = {}
  local function attach(node)
    local parent = stack[#stack]
    if parent then parent[#parent + 1] = node else root = node end
  end
  function builder.value(raw) attach(raw) end
  function builder.open(kind)
    local free_count = #free_nodes
    local node = free_count > 0 and free_nodes[free_count] or {}
    free_nodes[free_count] = nil
    node[1] = kind == "array" and "[" or "{"
    attach(node)
    stack[#stack + 1] = node
  end
  function builder.item() end
  builder.key = attach
  function builder.close() stack[#stack] = nil end
  function builder.result() return root end
  local function recycle(node)
    for index = 2, #node do
      if type(node[index]) == "table" then recycle(node[index]) end
      node[index] = nil
    end
    node[1] = nil
    free_nodes[#free_nodes + 1] = node
  end
  function builder.reset()
    if root then recycle(root) end
    root = nil
  end
  return builder
end

-- Writes JSON as it is read: indented two spaces a level with pretty set,
-- minified otherwise. Formatting needs nothing else, so it streams instead
-- of holding the whole document as a parsed value.
local function new_formatter(pretty, eol)
  local out = new_writer()
  local separator = pretty and ": " or ":"
  local depth, breaks = 0, {}
  local function line_break()
    local text = breaks[depth]
    if not text then
      text = eol .. string.rep("  ", depth)
      breaks[depth] = text
    end
    out.add(text)
  end
  local formatter = {}
  function formatter.value(raw) out.add(raw) end
  function formatter.open(kind)
    out.add(kind == "array" and "[" or "{")
    depth = depth + 1
  end
  function formatter.item(index)
    if index > 1 then out.add(",") end
    if pretty then line_break() end
  end
  function formatter.key(raw)
    out.add(raw)
    out.add(separator)
  end
  function formatter.close(kind, count)
    depth = depth - 1
    if pretty and count > 0 then line_break() end
    out.add(kind == "array" and "]" or "}")
  end
  formatter.result = out.result
  return formatter
end

local function parse_json(text, sink)
  sink = sink or new_tree_builder()
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

  local function fail(message, at)
    return nil, { position = at or position, message = message }
  end

  local function skip_space()
    local byte = text:byte(position)
    if byte ~= 32 and byte ~= 9 and byte ~= 13 and byte ~= 10 then return end
    position = text:find("[^ \t\r\n]", position) or length + 1
  end

  local function is_digit(byte)
    return byte and byte >= 48 and byte <= 57
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

  -- Checks the string at position and returns its raw text, quotes
  -- included, jumping from one quote, backslash or control character to the
  -- next rather than stepping through every character.
  local function read_string()
    local start = position
    if text:sub(position, position) ~= '"' then
      return fail("Expected a JSON string.")
    end
    position = position + 1
    while true do
      local special = text:find('["\\%z\1-\31]', position)
      if not special then return fail("Unterminated JSON string.", start) end
      position = special
      local byte = text:byte(position)
      if byte == 34 then
        position = position + 1
        return text:sub(start, position - 1)
      elseif byte < 32 then
        return fail("Unescaped control character in string.")
      end
      local slash_at = position
      position = position + 1
      local escape = text:sub(position, position)
      if escape == "" then return fail("Unterminated escape sequence.", slash_at) end
      position = position + 1
      if escape == "u" then
        local value, err = read_hex_escape()
        if not value then return nil, err end
      elseif not ESCAPES[escape] then
        -- The whole character, not just its first byte, so the message is valid UTF-8.
        return fail("Invalid escape sequence \\" .. text:match(utf8.charpattern, slash_at + 1) .. ".", slash_at)
      end
    end
  end

  local function read_number()
    local start = position
    if text:byte(position) == 45 then
      position = position + 1
      if position > length then return fail("Expected a digit after '-'.") end
    end
    local first = text:byte(position)
    if first == 48 then
      position = position + 1
      if is_digit(text:byte(position)) then
        return fail("Leading zeroes are not allowed in JSON numbers.")
      end
    elseif first and first >= 49 and first <= 57 then
      repeat position = position + 1 until not is_digit(text:byte(position))
    else
      return fail("Expected a JSON value.", start)
    end
    if text:byte(position) == 46 then
      position = position + 1
      if not is_digit(text:byte(position)) then
        return fail("Expected a digit after the decimal point.")
      end
      repeat position = position + 1 until not is_digit(text:byte(position))
    end
    local exponent = text:byte(position)
    if exponent == 101 or exponent == 69 then
      position = position + 1
      local sign = text:byte(position)
      if sign == 43 or sign == 45 then position = position + 1 end
      if not is_digit(text:byte(position)) then
        return fail("Expected a digit in the exponent.")
      end
      repeat position = position + 1 until not is_digit(text:byte(position))
    end
    return text:sub(start, position - 1)
  end

  -- Each of these returns true once it has passed what it read to the sink.
  local read_value
  local function read_array(depth)
    sink.open("array")
    position = position + 1
    skip_space()
    if text:sub(position, position) == "]" then
      position = position + 1
      sink.close("array", 0)
      return true
    end
    local count = 0
    while true do
      count = count + 1
      sink.item(count)
      local ok, err = read_value(depth + 1)
      if not ok then return nil, err end
      skip_space()
      local character = text:byte(position)
      if character == 93 then
        position = position + 1
        sink.close("array", count)
        return true
      end
      if character ~= 44 then return fail("Expected ',' or ']' after array item.") end
      position = position + 1
      skip_space()
      if text:byte(position) == 93 then
        return fail("Trailing commas are not allowed in JSON arrays.")
      end
    end
  end

  local function read_object(depth)
    sink.open("object")
    position = position + 1
    skip_space()
    if text:sub(position, position) == "}" then
      position = position + 1
      sink.close("object", 0)
      return true
    end
    local count = 0
    while true do
      local key, err = read_string()
      if not key then return nil, err end
      skip_space()
      if text:byte(position) ~= 58 then return fail("Expected ':' after object key.") end
      position = position + 1
      count = count + 1
      sink.item(count)
      sink.key(key)
      local ok
      ok, err = read_value(depth + 1)
      if not ok then return nil, err end
      skip_space()
      local character = text:byte(position)
      if character == 125 then
        position = position + 1
        sink.close("object", count)
        return true
      end
      if character ~= 44 then return fail("Expected ',' or '}' after object member.") end
      position = position + 1
      skip_space()
      if text:byte(position) == 125 then
        return fail("Trailing commas are not allowed in JSON objects.")
      end
      if text:byte(position) ~= 34 then return fail("Expected an object key string.") end
    end
  end

  read_value = function(depth)
    skip_space()
    local character = text:byte(position)
    if character == 91 or character == 123 then
      if depth >= MAX_DEPTH then return fail("JSON nesting exceeds 256 levels.") end
      if character == 91 then return read_array(depth) end
      return read_object(depth)
    end
    local raw, err
    if character == 34 then
      raw, err = read_string()
    else
      if character == 116 and text:sub(position, position + 3) == "true" then
        position, raw = position + 4, "true"
      elseif character == 102 and text:sub(position, position + 4) == "false" then
        position, raw = position + 5, "false"
      elseif character == 110 and text:sub(position, position + 3) == "null" then
        position, raw = position + 4, "null"
      end
      if not raw then raw, err = read_number() end
    end
    if not raw then return nil, err end
    sink.value(raw)
    return true
  end

  local ok, err = read_value(0)
  if not ok then return nil, err end
  skip_space()
  if position <= length then return fail("Unexpected content after the JSON value.") end
  return sink.result()
end

-- Where an object's members start in its table, in the order to write them:
-- by decoded key, keeping the document's order between equal keys.
local function sorted_members(node, sort_state)
  local order_branch = sort_state.orders
  local cacheable = true
  for index = 2, #node, 2 do
    local raw = node[index]
    local next_branch = order_branch[raw]
    if not next_branch then
      if sort_state.order_count >= 256 then cacheable = false; break end
      next_branch = {}
      order_branch[raw] = next_branch
    end
    order_branch = next_branch
  end
  local cached = cacheable and order_branch[sort_state.order_key]
  if cached then return cached end

  local order, decoded = {}, {}
  for index = 2, #node, 2 do
    order[#order + 1] = index
    local raw = node[index]
    local value = sort_state.decoded[raw]
    if value == nil then
      value = decode_string(raw)
      if sort_state.decoded_count < 1024 then
        sort_state.decoded[raw] = value
        sort_state.decoded_count = sort_state.decoded_count + 1
      end
    end
    decoded[index] = value
  end
  table.sort(order, function(a, b)
    local left, right = decoded[a], decoded[b]
    if left == right then return a < b end
    return left < right
  end)
  if cacheable then
    order_branch[sort_state.order_key] = order
    sort_state.order_count = sort_state.order_count + 1
  end
  return order
end

-- Passes a parsed value to a sink, as parse_json() would have, with each
-- object's members in key order when sort_keys is set.
local function write_tree(node, sink, sort_keys)
  if type(node) == "string" then sink.value(node); return end
  if node[1] == "[" then
    sink.open("array")
    for index = 2, #node do
      sink.item(index - 1)
      write_tree(node[index], sink, sort_keys)
    end
    sink.close("array", #node - 1)
    return
  end
  sink.open("object")
  local order = sort_keys and sorted_members(node, sort_keys)
  for member = 1, (#node - 1) // 2 do
    local at = order and order[member] or member * 2
    sink.item(member)
    sink.key(node[at])
    write_tree(node[at + 1], sink, sort_keys)
  end
  sink.close("object", (#node - 1) // 2)
end

local function render(root, pretty, eol)
  local formatter = new_formatter(pretty, eol)
  write_tree(root, formatter, false)
  return formatter.result()
end

-- Sort Object Keys as it reads: arrays pass straight to the formatter, but
-- an object is gathered whole, since its members are written in key order,
-- and written once it closes. Only the objects being read are held, so a
-- large array of records sorts one record at a time.
local function new_sorting_formatter(pretty, eol)
  local formatter = new_formatter(pretty, eol)
  local sort_state = {
    decoded = {}, decoded_count = 0,
    orders = {}, order_count = 0, order_key = {}
  }
  local builder, building, depth = new_tree_builder(), false, 0
  local sink = {}
  function sink.value(raw) (building and builder or formatter).value(raw) end
  function sink.open(kind)
    if not building and kind == "object" then
      builder.reset()
      building = true
    end
    if not building then formatter.open(kind); return end
    depth = depth + 1
    builder.open(kind)
  end
  function sink.item(index) (building and builder or formatter).item(index) end
  function sink.key(raw) builder.key(raw) end
  function sink.close(kind, count)
    if not building then formatter.close(kind, count); return end
    builder.close(kind, count)
    depth = depth - 1
    if depth == 0 then
      write_tree(builder.result(), formatter, sort_state)
      building = false
    end
  end
  sink.result = formatter.result
  return sink
end

local function clear_error()
  editor.clear_indicator(ERROR_INDICATOR)
end

-- Highlights err, a parse error in the text starting at document position
-- base, moves the caret there and says what's wrong.
local function show_error(base, err, format_name)
  clear_error()
  local absolute = base + err.position - 1
  local highlight = math.min(absolute, editor.length())
  if highlight >= 1 then
    editor.indicator(highlight, highlight, ERROR_INDICATOR, ERROR_COLOUR)
  end
  local caret = math.min(absolute, editor.length() + 1)
  editor.set_caret(caret)
  editor.scroll_to(caret)
  local line, column = location(caret)
  ui.message(string.format("Invalid %s at line %d, column %d: %s",
    format_name or "JSON", line, column, err.message), nil, "error")
end

local function transform(pretty, sort_keys)
  local text, first, last = source_range()
  local new_sink = sort_keys and new_sorting_formatter or new_formatter
  local result, err = parse_json(text, new_sink(pretty, editor.eol()))
  if not result then show_error(first, err); return end
  clear_error()
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function escape_string(text)
  local replacements = { ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b",
    ["\f"] = "\\f", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }
  return '"' .. (text:gsub('[%z\1-\31\\"]', function(character)
    return replacements[character] or string.format("\\u%04X", character:byte())
  end)) .. '"'
end

-- A sink that writes YAML block style as the JSON is read: "key: value" and
-- "- value" lines, with nested content on the lines below, indented two
-- spaces a level, and empty arrays and objects inline as [] and {}. Whether
-- a key's or dash's line ends ":" or holds [] or {} is only known once the
-- value's first entry or its end is read, so that suffix waits until then.
local function new_yaml_writer(eol)
  local out = new_writer()
  local frames, started = {}, false
  local pending -- "key" or "dash": a line written up to its value
  local function start_line(depth)
    if started then out.add(eol) end
    started = true
    out.add(string.rep("  ", depth))
  end
  -- The first entry of the innermost array or object, and its line.
  local function enter_entry()
    local frame = frames[#frames]
    if frame.pending then
      if frame.pending == "key" then out.add(":") end
      frame.pending = nil
    end
    start_line(frame.depth)
  end
  local writer = {}
  function writer.value(raw)
    if pending then out.add(pending == "key" and ": " or " ") end
    pending = nil
    out.add(raw)
  end
  function writer.open(kind)
    local parent = frames[#frames]
    frames[#frames + 1] = { kind = kind, pending = pending, depth = parent and parent.depth + 1 or 0 }
    pending = nil
  end
  function writer.item()
    if frames[#frames].kind == "array" then
      enter_entry()
      out.add("-")
      pending = "dash"
    end
  end
  function writer.key(raw)
    enter_entry()
    out.add(raw)
    pending = "key"
  end
  function writer.close(kind, count)
    local frame = table.remove(frames)
    if count == 0 then
      if frame.pending then out.add(frame.pending == "key" and ": " or " ") end
      out.add(kind == "array" and "[]" or "{}")
    end
  end
  writer.result = out.result
  return writer
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
      if not is_string(value) then
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
      return escape_string((inner:gsub("''", "'")))
    end
    if raw:sub(1, 1) == "[" or raw:sub(1, 1) == "{" then
      local value, err = parse_json(raw)
      if not value then return yaml_error(line, err.message, err.position - 1) end
      return value
    end
    local lower = raw:lower()
    if lower == "null" or raw == "~" then return "null" end
    if lower == "true" or lower == "false" then return lower end
    local number = parse_json(raw)
    if type(number) == "string" then return number end
    return escape_string(raw)
  end

  local parse_block
  local function parse_mapping(index, indent, first_content, first_line)
    local object = { "{" }
    local seen = {}
    local content, line = first_content, first_line
    while content do
      local key_text, value_text = yaml_key_value(content)
      if not key_text or key_text == "" then return yaml_error(line, "Expected a mapping key followed by ':'.") end
      local key, decoded = escape_string(key_text), key_text
      if key_text:sub(1, 1) == '"' or key_text:sub(1, 1) == "'" then
        key = scalar(key_text, line)
        if not key then return yaml_error(line, "Invalid quoted mapping key.") end
        decoded = decode_string(key)
      end
      if seen[decoded] then return yaml_error(line, "Duplicate mapping key '" .. decoded .. "'.") end
      seen[decoded] = true
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
          value = "null"
        end
      else
        value, err = scalar(value_text, line)
        if not value then return nil, err end
      end
      object[#object + 1] = key
      object[#object + 1] = value
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
      local array = { "[" }
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
            value = "null"
          end
        elseif yaml_key_value(rest) then
          -- The mapping's keys line up with its first one, after "-" and any spaces.
          value, index = parse_mapping(index, indent + #line.content:match("^%-%s*"), rest, line)
          if not value then return nil, index end
        else
          value, err = scalar(rest, line)
          if not value then return nil, err end
          if index <= #lines and lines[index].indent > indent then
            return yaml_error(lines[index], "A scalar sequence item cannot have nested content.")
          end
        end
        array[#array + 1] = value
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
  local result, err = parse_json(text, new_yaml_writer(editor.eol()))
  if not result then show_error(first, err); return end
  clear_error()
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function convert_yaml_to_json()
  local text, first, last = source_range()
  local root, err = parse_yaml(text)
  if not root then show_error(first, err, "YAML"); return end
  clear_error()
  local result = render(root, true, editor.eol())
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
    show_error(first, err)
    return
  end
  if not is_string(node) then
    ui.message("The selection must be one quoted JSON string.", nil, "warning")
    return
  end
  clear_error()
  editor.replace_selection(decode_string(node))
end

local function format_json() transform(true, false) end
local function minify_json() transform(false, false) end

-- json-check.lua's items come first; this keeps them in their own group.
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Format JSON", format_json, "Ctrl+Alt+J")
menu.add("/Tools/JSON/Minify JSON", minify_json)
menu.add("/Tools/JSON/Sort Object Keys", function() transform(true, true) end)
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Convert JSON to YAML", convert_json_to_yaml)
menu.add("/Tools/JSON/Convert YAML to JSON", convert_yaml_to_json)
menu.separator("/Tools/JSON")
menu.add("/Tools/JSON/Escape Selection as JSON String", escape_selection)
menu.add("/Tools/JSON/Unescape JSON String", unescape_selection)
