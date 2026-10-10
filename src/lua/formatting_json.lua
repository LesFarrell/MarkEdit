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
  -- Lua strings can contain arbitrary bytes, but JSON text is Unicode and
  -- therefore must be valid UTF-8. Without this check malformed bytes inside
  -- a quoted string passed the byte-oriented grammar below unchanged.
  if utf8.len(text) == nil then return false end
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

-- A fence line's marker, length and remaining text, or nil. Opening fences
-- may have info text; closing fences must have only whitespace after a marker
-- of the same character and at least the opening marker's length.
local function fence_marker(n)
  local line = editor.get_line(n)
  local rest = line:sub(#line:match("^ ? ? ?") + 1)
  local marker = rest:match("^(```+)") or rest:match("^(~~~+)")
  if not marker then return nil end
  return marker:sub(1, 1), #marker, rest:sub(#marker + 1)
end

-- The range between the fence lines of the fenced block holding line, or
-- nil when line isn't in one (or the block is empty).
local function fence_body(line)
  local open, marker, markerLength
  -- Scan forward, so a closing fence from an earlier block can never be
  -- mistaken for the opening fence of a later block.
  for n = 1, line do
    local char, length, tail = fence_marker(n)
    if not open then
      -- CommonMark does not allow a backtick in the info string of a
      -- backtick fence; treating such a line as an opener mis-pairs blocks.
      if char and not (char == "`" and tail:find("`", 1, true)) then
        open, marker, markerLength = n, char, length
      end
    elseif char == marker and length >= markerLength and tail:match("^[ \t]*$") then
      open, marker, markerLength = nil, nil, nil
    end
  end
  if not open or line == open then return nil end

  local close
  for n = line + 1, editor.line_count() do
    local char, length, tail = fence_marker(n)
    if char == marker and length >= markerLength and tail:match("^[ \t]*$") then
      close = n
      break
    end
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
