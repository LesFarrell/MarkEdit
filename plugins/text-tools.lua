-- Text commands. On Edit > Convert:
--   snake_case, camelCase, PascalCase, kebab-case, CONSTANT_CASE
-- On the Plugins menu:
--   Lines      Remove Blank Lines, Keep/Remove Lines Containing, Shuffle,
--               Sort Naturally (so "file2" comes before "file10")
--   Text        Count Occurrences
--   Insert      UUID, Lorem Ipsum
--
-- Built-in Edit menu commands already sort lines, remove duplicate lines,
-- and change to upper, lower and title case, so those aren't repeated here.
--
-- Uses only the plugin API documented in README.md in this folder.

-- The first and last lines the selection touches, or every line when
-- nothing is selected (except the empty one after a final line ending, so
-- that ending stays where it is). A selection that ends exactly at the start
-- of a line leaves that line out, as the user would expect.
local function target_lines()
  local start, finish = editor.get_selection_range()
  if finish < start then
    local last = editor.line_count()
    if last > 1 and editor.get_line(last) == "" then last = last - 1 end
    return 1, last
  end
  local first, last = editor.line_at(start), editor.line_at(finish + 1)
  if last > first and finish + 1 == (editor.line_range(last)) then last = last - 1 end
  return first, last
end

-- Replaces lines first..last with the strings in lines, rejoined with the
-- document's line ending, and selects them.
local function replace_lines(first, last, lines)
  local start = editor.line_range(first)
  local _, finish = editor.line_range(last)
  local text = table.concat(lines, editor.eol())
  editor.replace_range(start, finish, text)
  editor.set_selection(start, start + #text - 1)
end

-- Runs transform(lines) on the target lines, which returns the new list.
local function transform_lines(transform)
  local first, last = target_lines()
  local lines = {}
  for n = first, last do lines[#lines + 1] = editor.get_line(n) end
  replace_lines(first, last, transform(lines))
end

-- ---- Edit > Convert ----

-- The words in an identifier or phrase: split at spaces, punctuation, and
-- case changes, so "parseHTTPResponse", "parse_http_response" and
-- "Parse HTTP response" all give parse, HTTP/http, response. Bytes of UTF-8
-- characters count as letters, so accented words stay whole, but only ASCII
-- letters change case.
local function split_words(text)
  text = text:gsub("(%l)(%u)", "%1 %2"):gsub("(%d)(%u)", "%1 %2"):gsub("(%u)(%u%l)", "%1 %2")
  local words = {}
  for word in text:gmatch("[%w\128-\255]+") do words[#words + 1] = word end
  return words
end

local function capitalize(word)
  return word:sub(1, 1):upper() .. word:sub(2):lower()
end

-- Each case style: how to write each word (given its index), and what goes
-- between words.
local CASES = {
  snake = { join = "_", word = function(w) return w:lower() end },
  kebab = { join = "-", word = function(w) return w:lower() end },
  constant = { join = "_", word = function(w) return w:upper() end },
  pascal = { join = "", word = capitalize },
  camel = { join = "", word = function(w, i) return i == 1 and w:lower() or capitalize(w) end },
}

-- Rewrites each selected line in a case style, keeping its indentation.
local function change_case(style)
  local start, finish = editor.get_selection_range()
  if finish < start then
    ui.message("Select the text to change first.")
    return
  end
  local case = CASES[style]
  local text = editor.get_selection():gsub("([^\r\n]+)", function(line)
    local indent = line:match("^%s*")
    local words = split_words(line)
    for i, word in ipairs(words) do words[i] = case.word(word, i) end
    return indent .. table.concat(words, case.join)
  end)
  editor.replace_selection(text)
  editor.set_selection(start, start + #text - 1)
end

menu.add("/Edit/Convert/snake_case", function() change_case("snake") end)
menu.add("/Edit/Convert/camelCase", function() change_case("camel") end)
menu.add("/Edit/Convert/PascalCase", function() change_case("pascal") end)
menu.add("/Edit/Convert/kebab-case", function() change_case("kebab") end)
menu.add("/Edit/Convert/CONSTANT_CASE", function() change_case("constant") end)

-- ---- Plugins > Lines ----
--
-- These work on the selected lines, or on the whole document when nothing
-- is selected.

menu.add("Lines/Remove Blank Lines", function()
  transform_lines(function(lines)
    local kept = {}
    for _, line in ipairs(lines) do
      if line:find("%S") then kept[#kept + 1] = line end
    end
    return kept
  end)
end)

-- Keeps (keep = true) or removes the lines that contain some text, matched
-- exactly as typed, not as a Lua pattern.
local function filter_lines(keep)
  local wanted = ui.prompt(keep and "Keep only the lines containing:" or "Remove the lines containing:")
  if not wanted or wanted == "" then return end
  transform_lines(function(lines)
    local kept = {}
    for _, line in ipairs(lines) do
      if (line:find(wanted, 1, true) ~= nil) == keep then kept[#kept + 1] = line end
    end
    return kept
  end)
end

menu.add("Lines/Keep Lines Containing...", function() filter_lines(true) end)
menu.add("Lines/Remove Lines Containing...", function() filter_lines(false) end)

menu.add("Lines/Shuffle Lines", function()
  transform_lines(function(lines)
    -- Fisher-Yates.
    for i = #lines, 2, -1 do
      local j = math.random(i)
      lines[i], lines[j] = lines[j], lines[i]
    end
    return lines
  end)
end)

-- Compares runs of digits by their value, so "item 9" sorts before
-- "item 10". Letters compare ignoring case, and ties keep byte order.
local function natural_key(line)
  return (line:lower():gsub("%d+", function(digits)
    digits = digits:gsub("^0+", "")
    -- The length first, so longer numbers sort later.
    return string.format("%03d", #digits) .. digits
  end))
end

menu.add("Lines/Sort Naturally", function()
  transform_lines(function(lines)
    local keys = {}
    for _, line in ipairs(lines) do keys[line] = keys[line] or natural_key(line) end
    table.sort(lines, function(a, b)
      if keys[a] ~= keys[b] then return keys[a] < keys[b] end
      return a < b
    end)
    return lines
  end)
end)

-- ---- Plugins > Text ----

-- Word characters for Count Occurrences: ASCII letters, digits, _ and any
-- byte of a UTF-8 character.
local WORD_BYTE = "[%w_\128-\255]"

-- The word the caret is in or next to, or "".
local function word_at_caret()
  local line = editor.current_line()
  local lineStart = editor.line_range(line)
  local column = editor.get_caret() - lineStart + 1
  for wordStart, word, wordEnd in editor.get_line(line):gmatch("()(" .. WORD_BYTE .. "+)()") do
    if wordStart <= column and column <= wordEnd then return word end
  end
  return ""
end

-- Counts non-overlapping occurrences of needle in text, matched exactly as
-- typed (not as a Lua pattern). Returns the count and the number of lines
-- they're on. With wholeWord, a match next to a word character on a side
-- where needle starts or ends with one doesn't count.
local function count_occurrences(text, needle, eol, wholeWord)
  local count, lines, lastLine, line, scanned = 0, 0, 0, 1, 1
  local checkBefore = wholeWord and needle:find("^" .. WORD_BYTE)
  local checkAfter = wholeWord and needle:find(WORD_BYTE .. "$")
  local position = 1
  while true do
    local first, last = text:find(needle, position, true)
    if not first then break end
    local before, after = text:sub(first - 1, first - 1), text:sub(last + 1, last + 1)
    if not (checkBefore and before:find(WORD_BYTE)) and not (checkAfter and after:find(WORD_BYTE)) then
      count = count + 1
      -- Line endings since the last match give this match's line.
      local between = text:sub(scanned, first - 1)
      -- (No line ending has pattern magic characters, so eol is its own pattern.)
      local _, endings = between:gsub(eol, "")
      line, scanned = line + endings, first
      if line ~= lastLine then lines, lastLine = lines + 1, line end
      position = last + 1
    else
      position = first + 1
    end
  end
  return count, lines
end

local function times(n)
  return n == 1 and "once" or string.format("%d times", n)
end

-- Says how many times some text appears in the document: exactly, ignoring
-- case (ASCII letters only), and as a whole word. Starts with the selection,
-- or the word at the caret.
menu.add("Text/Count Occurrences...", function()
  local selected = editor.get_selection()
  local suggestion = (selected ~= "" and not selected:find("[\r\n]")) and selected or word_at_caret()
  local needle = ui.prompt("Count occurrences of:", suggestion)
  if not needle or needle == "" then return end

  local text, eol = editor.get_text(), editor.eol()
  local exact, lines = count_occurrences(text, needle, eol, false)
  local anyCase = count_occurrences(text:lower(), needle:lower(), eol, false)
  local wholeWord = count_occurrences(text, needle, eol, true)

  local report = { string.format("\"%s\" appears %s", needle, times(exact)) }
  if exact > 0 then report[1] = report[1] .. string.format(", on %d line%s", lines, lines == 1 and "" or "s") end
  report[1] = report[1] .. "."
  if anyCase ~= exact then report[#report + 1] = "Ignoring case: " .. times(anyCase) .. "." end
  if wholeWord ~= exact then report[#report + 1] = "As a whole word: " .. times(wholeWord) .. "." end
  ui.message(table.concat(report, "\n"))
end)

-- ---- Plugins > Insert ----

-- A random (version 4) UUID, such as 3b241101-e2bb-4255-8caf-4136c566a962.
-- math.random isn't a secure random source, so these are fine as unique
-- identifiers but not as secrets.
menu.add("Insert/UUID", function()
  -- The version digit is 4, and the variant bits (the next group's first
  -- digit) are 10 in binary.
  editor.replace_selection(string.format("%08x-%04x-%04x-%04x-%04x%08x",
    math.random(0, 0xFFFFFFFF), math.random(0, 0xFFFF),
    0x4000 | math.random(0, 0x0FFF), 0x8000 | math.random(0, 0x3FFF),
    math.random(0, 0xFFFF), math.random(0, 0xFFFFFFFF)))
end)

local LOREM = {
  "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.",
  "Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.",
  "Curabitur pretium tincidunt lacus. Nulla gravida orci a odio. Nullam varius, turpis et commodo pharetra, est eros bibendum elit, nec luctus magna felis sollicitudin mauris. Integer in mauris eu nibh euismod gravida.",
  "Praesent blandit odio eu enim. Pellentesque sed dui ut augue blandit sodales. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia curae; aliquam nibh.",
  "Mauris ac mauris sed pede pellentesque fermentum. Maecenas adipiscing ante non diam sodales hendrerit. Ut velit mauris, egestas sed, gravida nec, ornare ut, mi. Aenean ut orci vel massa suscipit pulvinar.",
}

menu.add("Insert/Lorem Ipsum...", function()
  local answer = ui.prompt("Number of paragraphs (1 to 50):", "1")
  local count = answer and math.tointeger(tonumber(answer))
  if not count then return end
  count = math.max(1, math.min(count, 50))
  local paragraphs = {}
  for i = 1, count do paragraphs[i] = LOREM[(i - 1) % #LOREM + 1] end
  local eol = editor.eol()
  editor.replace_selection(table.concat(paragraphs, eol .. eol))
end)
