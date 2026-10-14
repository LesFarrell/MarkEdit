-- Offline grammar and punctuation checks. These deliberately use small,
-- explainable rules rather than pretending to be a full language model.

local INDICATOR = 12
local COLOUR = 0xffa726

local function source_range()
  local text = editor.get_selection()
  if text ~= "" then
    local first, last = editor.get_selection_range()
    return text, first, last
  end
  return editor.get_text(), 1, editor.length()
end

-- Ranges of fenced code blocks and inline code spans, which aren't prose.
local function code_ranges(text)
  local ranges = {}
  local fence, fence_start
  local position = 1
  while position <= #text do
    local line_end = text:find("\n", position, true) or #text
    local line = text:sub(position, line_end)
    local marker = line:match("^ ? ? ?(```+)") or line:match("^ ? ? ?(~~~+)")
    if fence then
      if marker and marker:sub(1, 1) == fence:sub(1, 1) and #marker >= #fence then
        ranges[#ranges + 1] = { fence_start, line_end }
        fence = nil
      end
    elseif marker then
      fence, fence_start = marker, position
    else
      -- An inline span closes at the next run of the same number of backticks.
      local at = 1
      while true do
        local open, open_last = line:find("`+", at)
        if not open then break end
        local close, close_last = open_last, open_last
        repeat
          close, close_last = line:find("`+", close_last + 1)
        until not close or close_last - close == open_last - open
        if close then
          ranges[#ranges + 1] = { position + open - 1, position + close_last - 1 }
          at = close_last + 1
        else
          at = open_last + 1
        end
      end
    end
    position = line_end + 1
  end
  if fence then ranges[#ranges + 1] = { fence_start, #text } end
  return ranges
end

local function in_ranges(ranges, first, last)
  for _, range in ipairs(ranges) do
    if first <= range[2] and last >= range[1] then return true end
  end
  return false
end

-- Whether the whitespace-separated word around position looks like a URL,
-- path, query string, address or :emoji: shortcode rather than prose.
local function is_token(text, position)
  local first = position
  while first > 1 and not text:sub(first - 1, first - 1):match("%s") do first = first - 1 end
  local token = text:match("^%S+", first)
  return token:find("[/\\=&@]") ~= nil or token:find("www.", 1, true) ~= nil
    or token:find(":[%w_+%-]+:") ~= nil
end

-- Issues in text, each with first/last shifted by offset. Fixable issues carry
-- fix = { first, last, replacement } relative to text.
local function inspect(text, offset)
  local issues = {}
  local code = code_ranges(text)
  offset = offset or 0

  local function add_issue(first, last, message, fix)
    if in_ranges(code, first, math.max(first, last)) then return end
    issues[#issues + 1] = { first = offset + first, last = offset + math.max(first, last),
      message = message, fix = fix }
  end

  -- Each word is compared with the one before, so every repeat in a run like
  -- "is is is" is found, and fixing them all leaves a single word.
  -- Curly double quotes, opening single quote, en/em dash, ellipsis and NBSP
  -- are blanked with the same number of bytes so positions don't move; the
  -- closing quote ’ stays, since it is also the apostrophe in "don’t".
  local words = text:gsub("\226\128[\147\148\152\156\157\166]", "   "):gsub("\194\160", "  ")
  local previous, previous_first, previous_last
  for first, word, after in words:gmatch("()([%w_'\128-\255]+)()") do
    local lower = word:lower()
    if lower == previous and not lower:find("[%d_]")
        and text:sub(previous_last + 1, first - 1):match("^%s+$") then
      add_issue(previous_first, after - 1, "Repeated word: " .. word, { previous_last + 1, after - 1, "" })
    end
    previous, previous_first, previous_last = lower, first, after - 1
  end
  -- Only spaces on the same line, and only before punctuation that ends a
  -- clause, so ".NET", " :smile:" and "x ? y : z" are left alone.
  for first, spaces, punctuation, after in text:gmatch("()([ \t]+)([,.;:!?])()") do
    local before, following = text:sub(first - 1, first - 1), text:sub(after, after)
    local ends_line = following == "" or following:match("[\r\n]")
    if before ~= "" and not before:match("[\r\n]")
        and (ends_line or (following:match("[ \t]") and not punctuation:match("[?:]"))) then
      add_issue(first, first + #spaces - 1, "Unexpected space before '" .. punctuation .. "'.",
        { first, first + #spaces - 1, "" })
    elseif before:match("%a") and following:match("%a") and punctuation:match("[,;]") then
      -- "word ,word": the space belongs after the comma, not before it.
      add_issue(first, after - 1, "Unexpected space before '" .. punctuation .. "'.",
        { first, after - 1, punctuation .. " " })
    end
  end
  for position, punctuation in text:gmatch("()([,;:!?])%f[%a]") do
    if text:sub(position - 1, position - 1):match("%a") and not is_token(text, position) then
      add_issue(position, position, "Add a space after '" .. punctuation .. "'.",
        { position + 1, position, " " })
    end
  end

  local first_letter = text:find("%a")
  if first_letter and text:sub(first_letter, first_letter):match("%l") then
    add_issue(first_letter, first_letter, "Sentence starts with a lowercase letter.")
  end
  for _, _, position in text:gmatch("()([.!?]%s+())%l") do
    add_issue(position, position, "Sentence starts with a lowercase letter.")
  end

  for position, article, word in text:gmatch("()(%f[%a][Aa]n?%f[^%a])%s+([A-Za-z]+)") do
    local vowel = word:lower():match("^[aeiou]") ~= nil
    local uses_an = article:lower() == "an"
    if vowel ~= uses_an then
      add_issue(position, position + #article - 1,
        string.format("Use '%s' before '%s'.", vowel and "an" or "a", word))
    end
  end

  local lower = text:lower()
  for phrase, suggestion in pairs({
    ["alot"] = "Use 'a lot'.", ["could of"] = "Consider 'could have'.",
    ["should of"] = "Consider 'should have'.", ["would of"] = "Consider 'would have'.",
    ["your welcome"] = "Consider \"you're welcome\".",
    ["its a"] = "If you mean 'it is', use \"it's\"."
  }) do
    local position = 1
    while true do
      local first, last = lower:find(phrase, position, true)
      if not first then break end
      local before, after = lower:sub(first - 1, first - 1), lower:sub(last + 1, last + 1)
      if not before:match("[%w_]") and not after:match("[%w_]") then
        local fix = phrase == "alot" and { first, last, text:sub(first, first) .. " lot" } or nil
        add_issue(first, last, suggestion, fix)
      end
      position = last + 1
    end
  end
  table.sort(issues, function(a, b) return a.first < b.first end)
  return issues
end

local function clear()
  editor.clear_indicator(INDICATOR)
end

local function highlight(quiet)
  local text, first = source_range()
  local issues = inspect(text, first - 1)
  clear()
  for _, issue in ipairs(issues) do
    editor.indicator(issue.first, issue.last, INDICATOR, COLOUR)
  end
  if not quiet then ui.status(string.format("Grammar checker highlighted %d issue%s.",
    #issues, #issues == 1 and "" or "s")) end
  return issues
end

local function check()
  local text, first = source_range()
  local issues = inspect(text, first - 1)
  if #issues == 0 then ui.message("No heuristic grammar issues found.", "Grammar Checker"); return end
  local lines = { string.format("Found %d possible issue%s:", #issues, #issues == 1 and "" or "s"), "" }
  for index = 1, math.min(#issues, 20) do
    local issue = issues[index]
    local line_number = editor.line_at(issue.first)
    local line_start = editor.line_range(line_number)
    local before = editor.get_range(line_start, issue.first - 1)
    local column = (utf8.len(before) or #before) + 1
    local preview = editor.get_line(line_number):match("^%s*(.-)%s*$")
    if #preview > 80 then preview = preview:sub(1, 77) .. "..." end
    lines[#lines + 1] = string.format("%d. Line %d, column %d: %s\n   %s",
      index, line_number, column, issue.message, preview)
  end
  if #issues > 20 then lines[#lines + 1] = string.format("...and %d more.", #issues - 20) end
  ui.message(table.concat(lines, "\n"), "Grammar Checker", "warning")
end

local function fix_safe()
  local text, first, last = source_range()
  -- Applies the same fixes Check reports, in order, skipping any that overlap.
  local fixes = {}
  for _, issue in ipairs(inspect(text)) do fixes[#fixes + 1] = issue.fix end
  table.sort(fixes, function(a, b) return a[1] < b[1] or (a[1] == b[1] and a[2] < b[2]) end)
  local parts, cursor = {}, 1
  for _, fix in ipairs(fixes) do
    if fix[1] >= cursor then
      parts[#parts + 1] = text:sub(cursor, fix[1] - 1)
      parts[#parts + 1] = fix[3]
      cursor = fix[2] + 1
    end
  end
  parts[#parts + 1] = text:sub(cursor)
  local result = table.concat(parts)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
  clear()
end

menu.add("/Tools/Grammar Checker/Check", check)
menu.add("/Tools/Grammar Checker/Highlight Issues", function() highlight(false) end)
menu.add("/Tools/Grammar Checker/Fix Safe Issues", fix_safe)
menu.add("/Tools/Grammar Checker/Clear Highlights", clear)
menu.separator("/Tools/Grammar Checker")
menu.checkbox("/Tools/Grammar Checker/Live Highlighting", "live", false, function(checked)
  if checked then highlight(true) else clear() end
end)

events.on("text_changed", function()
  if config.get("live", false) then highlight(true) end
end)
