-- A conservative Lua source reindenter, used by the lua-format plugin.
--
-- Only the whitespace at the start of each line (indentation) and the spaces
-- and tabs at the end are rewritten. Everything else -- code, strings and
-- comments -- is copied through byte for byte, so formatting can never change
-- what the document means. Lines that belong to a multi-line long string or
-- long comment ([[ ... ]] or --[[ ... ]]) are left completely alone.
--
-- Indentation follows block structure:
--   opens   function  then  do  repeat  and a line that ends with {
--   closes  end  until  and a line that starts with }
--   dedent  end  until  else  elseif  and a line that starts with }
-- A line with a { at the end adds one level and a line with a } at the start
-- removes one, so {"Label", { ... } }, lists indent the way people write them.
-- Depth is computed from the whole document, so formatting part of a file
-- still lines up with the rest of it.

local M = {}

local DEFAULT_INDENT = "  "

-- Keyword sets that drive indentation. Braces are handled separately, from
-- the first and last code character on a line.
local OPENS = { ["function"] = true, ["then"] = true, ["do"] = true, ["repeat"] = true }
local CLOSES = { ["end"] = true, ["until"] = true, ["elseif"] = true }
-- A leading keyword here lines up with the block it closes rather than its body.
local DEDENTS = { ["end"] = true, ["until"] = true, ["else"] = true, ["elseif"] = true }

local function is_ident_start(byte)
  return (byte >= 65 and byte <= 90) or (byte >= 97 and byte <= 122) or byte == 95
end

local function is_ident_continue(byte)
  return is_ident_start(byte) or (byte >= 48 and byte <= 57)
end

-- Splits text into { text = ..., ending = ... } records, one per line. The
-- ending is "" for a final line with no line break.
local function split_lines(text)
  local lines = {}
  local from, i, n = 1, 1, #text
  while i <= n do
    local byte = text:byte(i)
    if byte == 10 then -- \n
      lines[#lines + 1] = { text = text:sub(from, i - 1), ending = "\n" }
      i = i + 1
      from = i
    elseif byte == 13 then -- \r or \r\n
      local ending = text:sub(i, i + 1) == "\r\n" and "\r\n" or "\r"
      lines[#lines + 1] = { text = text:sub(from, i - 1), ending = ending }
      i = i + #ending
      from = i
    else
      i = i + 1
    end
  end
  if from <= n or #lines == 0 then lines[#lines + 1] = { text = text:sub(from), ending = "" } end
  return lines
end

-- If s:byte(i) begins a long bracket ([ , [=[ , [==[ ... ), returns the number
-- of '=' signs and the index just past the second '['. Otherwise returns nil.
local function long_open(s, i)
  if s:byte(i) ~= 91 then return nil end
  local j = i + 1
  while s:byte(j) == 61 do j = j + 1 end
  if s:byte(j) ~= 91 then return nil end
  return j - i - 1, j + 1
end

-- Returns the index just past the closing quote of the short string that
-- starts at i (s:byte(i) is " or '), honouring backslash escapes.
local function short_string_end(s, i)
  local quote, n, j = s:byte(i), #s, i + 1
  while j <= n do
    local byte = s:byte(j)
    if byte == 92 then j = j + 2 -- backslash escape
    elseif byte == quote then return j + 1
    else j = j + 1 end
  end
  return n + 1
end

-- Scans one line of code, skipping short and long strings and comments. The
-- carried-in `level` is the long bracket level left open by the previous line,
-- or nil. Returns the keyword tokens on the line, whether the line started
-- inside a long bracket, whether trimming its end is unsafe, the level for the
-- next line, and the first and last non-whitespace code characters.
local function scan_line(s, level)
  local n = #s
  local tokens = {}
  local interior = level ~= nil
  local no_trim = interior
  local last_code = nil
  local i = 1
  while i <= n do
    local byte = s:byte(i)
    if level then
      local close = "]" .. string.rep("=", level) .. "]"
      local at = s:find(close, i, true)
      if at then i, level = at + #close, nil else break end
    elseif byte == 45 and s:byte(i + 1) == 45 then -- --
      local below, after = long_open(s, i + 2)
      if below then
        local close = "]" .. string.rep("=", below) .. "]"
        local at = s:find(close, after, true)
        if at then i = at + #close else level, no_trim, i = below, true, n + 1 end
      else
        break -- an ordinary comment runs to the end of the line
      end
    elseif byte == 91 then -- [
      local below, after = long_open(s, i)
      if below then
        local close = "]" .. string.rep("=", below) .. "]"
        local at = s:find(close, after, true)
        if at then i = at + #close else level, no_trim, i = below, true, n + 1 end
      else
        last_code, i = byte, i + 1 -- indexing, not a long bracket
      end
    elseif byte == 34 or byte == 39 then -- " or '
      i = short_string_end(s, i)
    elseif is_ident_start(byte) then
      local j = i + 1
      while j <= n and is_ident_continue(s:byte(j)) do j = j + 1 end
      local word = s:sub(i, j - 1)
      if OPENS[word] or CLOSES[word] or word == "else" then
        tokens[#tokens + 1] = { word = word, start = i, stop = j - 1 }
      end
      last_code, i = s:byte(j - 1), j
    elseif byte ~= 32 and byte ~= 9 then -- anything but space or tab
      last_code, i = byte, i + 1
    else
      i = i + 1
    end
  end
  local first = s:match("^[ \t]*(.)")
  return tokens, interior, no_trim, level, (first and first:byte()), last_code
end

-- Turns a line's tokens and its first/last code characters into its net depth
-- change and how far the line itself is pulled back because it starts with a
-- closing keyword or brace. Only contents that run from the start of the line
-- count, so the "end" in "else x() end" is not treated as a leading close.
-- "else" closes one branch and opens the next, so it adds nothing.
local function analyze(s, tokens, closes_brace, opens_brace)
  local delta, dedent, leading, boundary = 0, 0, true, 0
  if closes_brace then delta, dedent = delta - 1, dedent + 1 end
  if opens_brace then delta = delta + 1 end
  for _, token in ipairs(tokens) do
    if leading and DEDENTS[token.word] and s:sub(boundary + 1, token.start - 1):match("^[ \t]*$") then
      dedent = dedent + 1
    else
      leading = false
    end
    boundary = token.stop
    if OPENS[token.word] then delta = delta + 1
    elseif CLOSES[token.word] then delta = delta - 1 end
  end
  return delta, dedent
end

-- Formats text. options.first and options.last are the 1-based lines to change
-- (default: all of them); options.indent is the indentation unit. Lines outside
-- the range, and lines inside a long string or comment, are returned unchanged.
function M.format(text, options)
  options = options or {}
  local unit = options.indent or DEFAULT_INDENT
  local lines = split_lines(text)
  local first = math.max(1, options.first or 1)
  local last = math.min(#lines, options.last or #lines)

  local depth, level = 0, nil
  local out = {}
  for index, line in ipairs(lines) do
    local s = line.text
    local tokens, interior, no_trim, next_level, first_code, last_code = scan_line(s, level)
    level = next_level
    local delta, dedent = analyze(s, tokens, first_code == 125, last_code == 123)

    if interior or index < first or index > last then
      out[index] = s
    else
      local body = s:gsub("^[ \t]*", "")
      if not no_trim then body = body:gsub("[ \t]+$", "") end
      local indent = math.max(0, depth - dedent)
      out[index] = (body ~= "" and string.rep(unit, indent) or "") .. body
    end

    depth = math.max(0, depth + delta)
  end

  local parts = {}
  for index, line in ipairs(lines) do
    parts[#parts + 1] = out[index]
    parts[#parts + 1] = line.ending
  end
  return table.concat(parts)
end

return M
