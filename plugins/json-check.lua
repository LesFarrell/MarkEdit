-- JSON checking, on Tools > JSON:
--   Check JSON Syntax (Ctrl+Alt+Shift+J) checks the selection, or the whole
--                                    document when it's a JSON file, and
--                                    moves the caret to the first error
--   Copy JSON Path                   copies the path to the value at the
--                                    caret, such as $.items[2].name
--   Highlight JSON Errors as You     rechecks JSON documents after each
--   Type (on by default)             edit, and when one is opened or
--                                    switched to, without moving the caret
--   Show JSON Path in Status Bar     shows the path at the caret as it moves
--   (off by default)
--
-- The first syntax error is highlighted in red with the reason in a box
-- under its line. A key repeated in the same object is valid syntax but
-- almost always a mistake (most readers keep only the last one), so repeats
-- are highlighted in amber as warnings. Outside JSON documents, the marks
-- clear on the next edit.
--
-- The file name picks the rules:
--   .jsonl                       one JSON value per line (JSON Lines)
--   .jsonc, tsconfig*.json,      JSON with comments: // and /* */ comments
--   jsconfig*.json,              and trailing commas are allowed
--   devcontainer.json, files
--   in a .vscode folder
--   anything else                strict JSON (RFC 8259)
--
-- It reports the mistakes people actually make (trailing commas, single
-- quotes, unquoted keys, comments, missing commas, a bracket closed by the
-- wrong partner) by name rather than as a bare "unexpected character".
--
-- Uses only the plugin API documented in README.md in this folder.

-- Plugin indicator numbers run from 8 to 19; these aren't used by any other
-- bundled plugin.
local ERROR_INDICATOR = 8
local WARNING_INDICATOR = 9
local ERROR_COLOUR = 0xe5534b
local WARNING_COLOUR = 0xd29922

-- Containers nested deeper than this are reported rather than followed, as
-- with Tools > JSON > Format, so a hostile document can't exhaust the stack.
local MAX_DEPTH = 256

-- At most this many repeated keys are marked, so a generated file full of
-- them doesn't bury the editor in notes.
local MAX_WARNINGS = 100

-- Documents larger than this aren't rechecked after every edit or caret
-- move, so working in a very large file stays responsive; the commands still
-- handle them.
local LIVE_LIMIT = 1024 * 1024

-- Raised by fail() and caught by scan_json().
local JsonError = {}

local function fail(position, message)
  error(setmetatable({ position = position, message = message }, JsonError), 0)
end

-- Reads text as one JSON value with optional whitespace around it. A
-- recursive-descent reader: each read_* function consumes one piece of the
-- grammar at `position`. options.comments and options.trailing_commas relax
-- the grammar to JSON with comments. When target (a byte offset) is given,
-- the result's path is the path to the innermost value or member spanning
-- it; a caret just after a value counts as on it.
--
-- Returns a table with, relative to text:
--   error_offset, error_message   the first syntax error, if there is one
--   duplicates                    repeated keys: { offset, length, key, first }
--   path                          when target was given and text is valid:
--                                 keys (strings) and 0-based indices
local function scan_json(text, options, target)
  local result = { duplicates = {} }
  -- JSON text is Unicode, so it must be valid UTF-8.
  local valid, badByte = utf8.len(text)
  if not valid then
    result.error_offset, result.error_message = badByte, "This byte isn't valid UTF-8 text."
    return result
  end

  local length, position = #text, 1
  local path, best = {}, nil

  local function char_at(at)
    return text:sub(at, at)
  end

  -- How to name the character at `at` in a message.
  local function describe(at)
    if at > length then return "the end of the text" end
    local c = text:match(utf8.charpattern, at)
    if c == "\t" then return "a tab" end
    if c == "\r" or c == "\n" then return "a line break" end
    return "'" .. c .. "'"
  end

  -- Records the current path when the value or member that started at
  -- spanStart, and has just been read, holds the target. Inner values finish
  -- first and have longer paths, so the innermost one wins.
  local function consider(spanStart)
    if target and spanStart <= target and target <= position and (not best or #path > #best) then
      best = table.move(path, 1, #path, 1, {})
    end
  end

  local function skip_space()
    while true do
      position = text:find("[^ \t\r\n]", position) or length + 1
      if char_at(position) ~= "/" then return end
      local following = char_at(position + 1)
      if following ~= "/" and following ~= "*" then return end
      if not options.comments then
        fail(position, "Comments aren't allowed in JSON. If this is JSON with comments, name the file .jsonc.")
      end
      if following == "/" then
        position = text:find("[\r\n]", position) or length + 1
      else
        local close = text:find("*/", position + 2, true)
        if not close then fail(position, "This comment is never closed: it needs a '*/'.") end
        position = close + 2
      end
    end
  end

  local function read_digits(what)
    local after = text:find("[^0-9]", position) or length + 1
    if after == position then
      fail(position, "Expected a digit " .. what .. ", but found " .. describe(position) .. ".")
    end
    position = after
  end

  -- "...", with \" \\ \/ \b \f \n \r \t and \uXXXX escapes, and no raw
  -- control characters.
  local function read_string()
    local opening = position
    position = position + 1
    while true do
      -- Jump to the next character that needs a closer look.
      local special = text:find('["\\\0-\31]', position)
      if not special then
        fail(opening, "This string is never closed: it needs a '\"' at the end.")
      end
      position = special
      local c = char_at(position)
      if c == '"' then
        position = position + 1
        return
      elseif c == "\\" then
        local escape = char_at(position + 1)
        if escape == "u" then
          if not text:sub(position + 2, position + 5):match("^%x%x%x%x$") then
            fail(position, "A \\u escape needs exactly four hexadecimal digits.")
          end
          position = position + 6
        elseif escape ~= "" and ('"\\/bfnrt'):find(escape, 1, true) then
          position = position + 2
        else
          fail(position, "\\" .. escape .. " isn't a JSON escape. Use \\\\ for a backslash.")
        end
      elseif c == "\n" or c == "\r" then
        fail(position, "Strings can't contain a line break. Use \\n instead.")
      else
        fail(position, "Strings can't contain control characters. Use a \\u escape instead.")
      end
    end
  end

  -- -12.5e+3: an optional minus, then 0 or digits, then an optional
  -- fraction and exponent.
  local function read_number()
    if char_at(position) == "-" then position = position + 1 end
    if char_at(position) == "0" then
      position = position + 1
      if char_at(position):match("%d") then
        fail(position - 1, "Numbers can't start with a leading zero.")
      end
    else
      read_digits("in the number")
    end
    if char_at(position) == "." then
      position = position + 1
      read_digits("after the decimal point")
    end
    local e = char_at(position)
    if e == "e" or e == "E" then
      position = position + 1
      local sign = char_at(position)
      if sign == "+" or sign == "-" then position = position + 1 end
      read_digits("in the exponent")
    end
  end

  local read_value

  -- An object member's key, checked for repeats within its object.
  local function read_key(opening, seen)
    skip_space()
    local c = char_at(position)
    if c ~= '"' then
      if position > length then
        fail(opening, "This '{' is never closed.")
      elseif c == "'" then
        fail(position, "Keys must use double quotes, not single quotes.")
      elseif c:match("[%a_$]") then
        fail(position, "Keys must be strings in double quotes.")
      else
        fail(position, "Expected a key in double quotes, but found " .. describe(position) .. ".")
      end
    end
    local keyStart = position
    read_string()
    local key = text:sub(keyStart + 1, position - 2)
    local first = seen[key]
    if not first then
      seen[key] = keyStart
    elseif #result.duplicates < MAX_WARNINGS then
      result.duplicates[#result.duplicates + 1] =
        { offset = keyStart, length = position - keyStart, key = key, first = first }
    end
    return keyStart, key
  end

  -- [ value, ... ] or { "key": value, ... }, opened by opener.
  local function read_container(opener, depth)
    if depth >= MAX_DEPTH then
      fail(position, "Containers are nested more than " .. MAX_DEPTH .. " levels deep.")
    end
    local opening = position
    local closer = opener == "{" and "}" or "]"
    local wrongCloser = opener == "{" and "]" or "}"
    local seen, index = {}, 0
    position = position + 1
    skip_space()
    if char_at(position) == closer then
      position = position + 1
      return
    end
    while true do
      if opener == "{" then
        local keyStart, key = read_key(opening, seen)
        skip_space()
        if char_at(position) ~= ":" then
          fail(position, "Expected ':' after the key, but found " .. describe(position) .. ".")
        end
        position = position + 1
        path[#path + 1] = key
        read_value(depth + 1)
        consider(keyStart)
      else
        path[#path + 1] = index
        index = index + 1
        read_value(depth + 1)
      end
      path[#path] = nil

      skip_space()
      local c = char_at(position)
      if c == closer then
        position = position + 1
        return
      elseif c == "," then
        local comma = position
        position = position + 1
        skip_space()
        if char_at(position) == closer then
          if not options.trailing_commas then
            fail(comma, "Trailing commas aren't allowed in JSON.")
          end
          position = position + 1
          return
        end
      elseif position > length then
        fail(opening, "This '" .. opener .. "' is never closed.")
      elseif c == wrongCloser then
        fail(position, "This '" .. c .. "' doesn't match the '" .. opener .. "' it closes. Expected '" .. closer .. "'.")
      else
        fail(position, "Expected ',' or '" .. closer .. "', but found " .. describe(position) .. ". Is a comma missing?")
      end
    end
  end

  read_value = function(depth)
    skip_space()
    local start = position
    local c = char_at(position)
    if position > length then
      fail(position, "Expected a value, but the text ended.")
    elseif c == '"' then
      read_string()
    elseif c == "{" or c == "[" then
      read_container(c, depth)
    elseif c == "-" or c:match("%d") then
      read_number()
    elseif c:match("%a") then
      local word = text:match("^[%w_]+", position)
      if word ~= "true" and word ~= "false" and word ~= "null" then
        fail(position, "'" .. word .. "' isn't a JSON value. Strings need double quotes; the only words are true, false and null.")
      end
      position = position + #word
    elseif c == "'" then
      fail(position, "Strings must use double quotes, not single quotes.")
    elseif c == "}" or c == "]" then
      fail(position, "Expected a value before '" .. c .. "'.")
    else
      fail(position, "Expected a value, but found " .. describe(position) .. ".")
    end
    consider(start)
  end

  local ok, problem = pcall(function()
    skip_space()
    if position > length then fail(1, "There's no JSON here: the text is empty.") end
    read_value(0)
    skip_space()
    if position <= length then
      fail(position, "Unexpected " .. describe(position) .. " after the end of the JSON value.")
    end
  end)
  if ok then
    result.path = best
  else
    if getmetatable(problem) ~= JsonError then error(problem, 0) end
    result.error_offset, result.error_message = problem.position, problem.message
  end
  return result
end

-- A path as $.key, $["odd key"] and $[0] steps. Keys keep their JSON
-- escapes, so a bracketed key is itself a valid JSON string.
local function format_path(path)
  local parts = { "$" }
  for _, step in ipairs(path) do
    if type(step) == "number" then
      parts[#parts + 1] = "[" .. step .. "]"
    elseif step:match("^[%a_][%w_]*$") then
      parts[#parts + 1] = "." .. step
    else
      parts[#parts + 1] = '["' .. step .. '"]'
    end
  end
  return table.concat(parts)
end

local STRICT = {}
local WITH_COMMENTS = { comments = true, trailing_commas = true }

-- Which rules the active document follows, from its file name: "strict",
-- "comments" or "lines". nil when it isn't a JSON document at all.
local function json_mode()
  if editor.language() ~= "json" then return nil end
  local path = (editor.file_path() or ""):lower():gsub("/", "\\")
  local name = path:match("[^\\]*$")
  if name:match("%.jsonl$") then return "lines" end
  if name:match("%.jsonc$") or name:match("^tsconfig.*%.json$") or name:match("^jsconfig.*%.json$")
    or name == "devcontainer.json" or name == ".devcontainer.json"
    or path:find("\\.vscode\\", 1, true) then
    return "comments"
  end
  return "strict"
end

-- Scans the document range start..finish under mode's rules. Offsets in the
-- result are relative to start, as are target's. JSON Lines are scanned one
-- non-blank line at a time, stopping at the first line with an error.
local function scan_range(start, finish, mode, target)
  local text = editor.get_range(start, finish)
  local relative = target and target - start + 1
  if mode ~= "lines" then
    return scan_json(text, mode == "comments" and WITH_COMMENTS or STRICT, relative)
  end

  local combined = { duplicates = {} }
  local lineStart = 1
  while lineStart <= #text do
    -- Lines end with \r\n, \r or \n.
    local lineEnd = text:find("[\r\n]", lineStart) or #text + 1
    local breakWidth = text:sub(lineEnd, lineEnd + 1) == "\r\n" and 2 or 1
    local line = text:sub(lineStart, lineEnd - 1)
    if line:find("[^ \t]") then
      local shift = lineStart - 1
      local onLine = relative and relative >= lineStart and relative <= lineEnd
      local result = scan_json(line, STRICT, onLine and relative - shift or nil)
      for _, repeated in ipairs(result.duplicates) do
        if #combined.duplicates < MAX_WARNINGS then
          repeated.offset, repeated.first = repeated.offset + shift, repeated.first + shift
          combined.duplicates[#combined.duplicates + 1] = repeated
        end
      end
      if onLine then combined.path = result.path end
      if result.error_offset then
        combined.error_offset, combined.error_message = result.error_offset + shift, result.error_message
        return combined
      end
    end
    lineStart = lineEnd + breakWidth
  end
  return combined
end

-- Whether this plugin's marks are showing, so edits only clear them once.
local marked = false

local function clear_marks()
  editor.clear_indicator(ERROR_INDICATOR)
  editor.clear_indicator(WARNING_INDICATOR)
  editor.clear_annotations()
  marked = false
end

-- Marks a scan_range() result for the range starting at start, replacing
-- any earlier marks. Marks belong to a document while `marked` tracks only
-- the last one checked, so they're always cleared first. Returns the
-- error's position, line, column and message when there is one.
local function mark(result, start)
  clear_marks()
  local notes, order = {}, {}
  local function note(line, text)
    if not notes[line] then
      notes[line] = {}
      order[#order + 1] = line
    end
    table.insert(notes[line], text)
  end

  for _, repeated in ipairs(result.duplicates) do
    local position = start + repeated.offset - 1
    editor.indicator(position, position + repeated.length - 1, WARNING_INDICATOR, WARNING_COLOUR)
    local key = #repeated.key > 40 and repeated.key:sub(1, 37) .. "..." or repeated.key
    note(editor.line_at(position), string.format(
      "JSON warning: \"%s\" is repeated in this object; most readers ignore the earlier one on line %d.",
      key, editor.line_at(start + repeated.first - 1)))
  end

  local position, line, column
  if result.error_offset then
    -- An error at the very end (an unclosed string or bracket points at its
    -- opening instead) is marked on the last character there is; an empty
    -- document has none, so it gets only the note.
    local length = editor.length()
    position = math.max(math.min(start + result.error_offset - 1, length), 1)
    line = editor.line_at(position)
    local lineStart = editor.line_range(line)
    column = (utf8.len(editor.get_range(lineStart, position - 1)) or (position - lineStart)) + 1
    if length > 0 then
      local character = editor.get_range(position, math.min(position + 3, length)):match(utf8.charpattern) or " "
      editor.indicator(position, math.min(position + #character - 1, length), ERROR_INDICATOR, ERROR_COLOUR)
    end
    note(line, "JSON: " .. result.error_message)
  end

  for _, noteLine in ipairs(order) do
    editor.annotation(noteLine, table.concat(notes[noteLine], "\n"))
  end
  marked = #order > 0
  return position, line, column, result.error_message
end

local function plural(count, one, many)
  return count .. " " .. (count == 1 and one or many)
end

-- After the built-in Format JSON and Minify JSON.
menu.separator("/Tools/JSON")

menu.add("/Tools/JSON/Check JSON &Syntax", function()
  local start, finish = editor.get_selection_range()
  local mode = json_mode()
  if finish < start then
    if not mode then
      ui.message("Select the JSON to check, or open a .json file.", nil, "info")
      return
    end
    start, finish = 1, editor.length()
  end

  local result = scan_range(start, finish, mode or "strict")
  local position, line, column, message = mark(result, start)
  if position then
    editor.scroll_to(position)
    ui.status(string.format("JSON error at line %d, column %d: %s", line, column, message))
  elseif #result.duplicates > 0 then
    ui.status("JSON syntax is valid, but " .. plural(#result.duplicates, "key is", "keys are") .. " repeated.")
  else
    ui.status("JSON syntax is valid.")
  end
end, "Ctrl+Alt+Shift+J")

-- The path to the value at the caret, or nil and why there isn't one.
local function path_at_caret(mode)
  local result = scan_range(1, editor.length(), mode, editor.get_caret())
  if result.error_offset then
    return nil, "There's no JSON path while the document has a syntax error."
  end
  if not result.path then return nil, "The caret isn't on a JSON value." end
  return format_path(result.path)
end

menu.add("/Tools/JSON/Copy JSON &Path", function()
  local mode = json_mode()
  if not mode then
    ui.message("Open a .json file to copy a path from it.", nil, "info")
    return
  end
  local path, problem = path_at_caret(mode)
  if not path then
    ui.status(problem)
    return
  end
  editor.clipboard(path)
  ui.status("Copied " .. path)
end)

-- ---- as you type ----

local function live_errors_enabled()
  return config.get("highlight_as_you_type", true)
end

local function live_path_enabled()
  return config.get("path_in_status_bar", false)
end

-- Rechecks a JSON document after an edit, or when it's opened or switched
-- to, without moving the caret. In any other document, marks left by the
-- command describe text that has since changed, so they're removed.
local function live_check()
  local mode = json_mode()
  if live_errors_enabled() and mode and editor.length() > 0 and editor.length() <= LIVE_LIMIT then
    mark(scan_range(1, editor.length(), mode), 1)
  elseif marked then
    clear_marks()
  end
end

local function live_path()
  local mode = json_mode()
  if live_path_enabled() and mode and editor.length() <= LIVE_LIMIT then
    local path = path_at_caret(mode)
    if path then ui.status(path) end
  end
end

menu.separator("/Tools/JSON")

menu.checkbox("/Tools/JSON/&Highlight JSON Errors as You Type", "highlight_as_you_type", true,
  function(checked)
    if checked then live_check() elseif marked then clear_marks() end
  end)

menu.checkbox("/Tools/JSON/Show JSON Path in Status &Bar", "path_in_status_bar", false,
  function(checked)
    if checked then live_path() end
  end)

events.on("text_changed", live_check)
events.on("open", live_check)
events.on("tab_changed", live_check)
events.on("selection_changed", live_path)
