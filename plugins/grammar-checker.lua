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

local function add_issue(issues, first, last, message)
  issues[#issues + 1] = { first = first, last = math.max(first, last), message = message }
end

local function inspect(text, offset)
  local issues = {}
  offset = offset or 0

  for first, left, _, right, after in text:gmatch("()([A-Za-z]+)(%s+)([A-Za-z]+)()") do
    if left:lower() == right:lower() then
      add_issue(issues, offset + first, offset + after - 1, "Repeated word: " .. right)
    end
  end
  for first, punctuation in text:gmatch("()%s+([,.;:!?])") do
    local spaces = text:match("^%s+", first)
    add_issue(issues, offset + first, offset + first + #spaces - 1,
      "Unexpected space before '" .. punctuation .. "'.")
  end
  for position, punctuation in text:gmatch("()([,;:!?])%a") do
    add_issue(issues, offset + position, offset + position,
      "Add a space after '" .. punctuation .. "'.")
  end

  local first_letter = text:find("%a")
  if first_letter and text:sub(first_letter, first_letter):match("%l") then
    add_issue(issues, offset + first_letter, offset + first_letter, "Sentence starts with a lowercase letter.")
  end
  for _, _, position in text:gmatch("()([.!?]%s+())%l") do
    add_issue(issues, offset + position, offset + position, "Sentence starts with a lowercase letter.")
  end

  for position, article, word in text:gmatch("()(%f[%a][Aa]n?%f[^%a])%s+([A-Za-z]+)") do
    local vowel = word:lower():match("^[aeiou]") ~= nil
    local uses_an = article:lower() == "an"
    if vowel ~= uses_an then
      add_issue(issues, offset + position, offset + position + #article - 1,
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
        add_issue(issues, offset + first, offset + last, suggestion)
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
  local result = text
  result = result:gsub("(%f[%a])([A-Za-z]+)(%s+)%2(%f[^%a])", "%1%2%4")
  result = result:gsub("%s+([,.;:!?])", "%1")
  result = result:gsub("([,;:!?])([A-Za-z])", "%1 %2")
  result = result:gsub("%f[%a][Aa]lot%f[^%a]", function(value)
    return value:sub(1, 1) == "A" and "A lot" or "a lot"
  end)
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
