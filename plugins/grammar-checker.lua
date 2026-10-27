-- Offline grammar and punctuation checks, on Tools > Grammar Checker. These
-- deliberately use small, explainable rules rather than pretending to be a
-- full language model, and each can be switched off on the Rules submenu.
--   Check              lists the issues in the output panel; a saved
--                      document's are path:line:column, to double-click
--   Highlight Issues   marks them in the editor
--   Fix Safe Issues    applies the fixes that can't change the meaning
--   Live Highlighting  marks them as you type (off by default)
-- Checks the selection, or the whole document when nothing is selected.

local INDICATOR = 12
local COLOUR = 0xffa726

-- Live Highlighting skips documents larger than this (about 40,000 words of
-- prose), so typing in a very large file stays responsive; the commands
-- still handle them.
local LIVE_LIMIT = 256 * 1024

-- Check lists at most this many issues; Highlight Issues marks them all.
local REPORT_LIMIT = 500

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
        local close
        local close_last = open_last
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

-- ---- rules ----
-- Each rule has a checkbox on Tools > Grammar Checker > Rules. Rules marked
-- english are skipped when the Windows spelling dictionary (View > Spell
-- Check Language) is for another language. Style rules are matters of taste,
-- so they start switched off.

local RULES = {
  { key = "repeated", label = "Repeated Words", default = true },
  { key = "spacing", label = "Spacing Around Punctuation", default = true },
  { key = "doubled", label = "Doubled Punctuation", default = true },
  { key = "brackets", label = "Spaces Inside Parentheses", default = true },
  { key = "capitals", label = "Sentence Capitals", default = true },
  { key = "articles", label = "A and An", default = true, english = true },
  { key = "contractions", label = "Missing Apostrophes", default = true, english = true },
  { key = "pronoun", label = "Lowercase I", default = true, english = true },
  { key = "days", label = "Days of the Week", default = true, english = true },
  { key = "agreement", label = "Subject and Verb Agreement", default = true, english = true },
  { key = "confusions", label = "Commonly Confused Words", default = true, english = true },
  { key = "malapropisms", label = "Misheard Phrases", default = true, english = true },
  { key = "redundancy", label = "Redundant Phrases", default = false, english = true, style = true },
  { key = "double_spaces", label = "Double Spaces", default = false, style = true },
  { key = "plurals", label = "Apostrophes in Plurals", default = false, english = true, style = true },
}

-- replacement with the capitals of original: all capitals, or a capital
-- first letter.
local function like(replacement, original)
  if #original > 1 and original == original:upper() and original:find("%u") then
    return replacement:upper()
  end
  if original:match("^%u") then return replacement:sub(1, 1):upper() .. replacement:sub(2) end
  return replacement
end

-- Words and phrases, by rule. A string is the message; a table gives the
-- message and a replacement, which Fix Safe Issues applies when the rule is
-- fixable. exact entries match only lower-case text, so "I" and "Monday"
-- aren't flagged; skip names a spelling that is left alone.
local PHRASES = {
  contractions = {},
  pronoun = { ["i"] = { "Use 'I' for the pronoun.", "I", exact = true } },
  days = {},
  agreement = {
    ["he don't"] = "Use 'doesn't' after he.", ["she don't"] = "Use 'doesn't' after she.",
    ["it don't"] = "Use 'doesn't' after it.",
    ["they was"] = "Use 'were' after they.", ["we was"] = "Use 'were' after we.",
    ["you was"] = "Use 'were' after you.", ["they is"] = "Use 'are' after they.",
    ["we is"] = "Use 'are' after we.", ["i is"] = "Use 'am' after I.", ["i are"] = "Use 'am' after I.",
  },
  confusions = {
    ["its a"] = "If you mean 'it is', use \"it's\".",
    ["your welcome"] = "Consider \"you're welcome\".",
    ["your going"] = "Consider \"you're going\".",
    ["their is"] = "Consider 'there is'.", ["their are"] = "Consider 'there are'.",
    ["their was"] = "Consider 'there was'.", ["their were"] = "Consider 'there were'.",
    ["it's own"] = "Use 'its own'.", ["whose going"] = "Consider \"who's going\".",
    ["could of"] = "Consider 'could have'.", ["should of"] = "Consider 'should have'.",
    ["would of"] = "Consider 'would have'.", ["loose weight"] = "Consider 'lose weight'.",
  },
  malapropisms = {
    ["alot"] = { "Use 'a lot'.", "a lot" },
    ["irregardless"] = "Use 'regardless'.",
    ["for all intensive purposes"] = "The phrase is 'for all intents and purposes'.",
    ["per say"] = "The phrase is 'per se'.", ["case and point"] = "The phrase is 'case in point'.",
    ["nip it in the butt"] = "The phrase is 'nip it in the bud'.",
    ["could care less"] = "The phrase is \"couldn't care less\".",
    ["one in the same"] = "The phrase is 'one and the same'.",
    ["supposably"] = "Consider 'supposedly'.", ["escape goat"] = "Use 'scapegoat'.",
    ["peaked my interest"] = "The phrase is 'piqued my interest'.",
    ["deep seeded"] = "The phrase is 'deep-seated'.",
    ["tongue and cheek"] = "The phrase is 'tongue-in-cheek'.",
    ["baited breath"] = "The phrase is 'bated breath'.", ["mute point"] = "The phrase is 'moot point'.",
  },
  redundancy = {},
}

-- Than, not then, after a comparison.
for _, word in ipairs({ "more", "less", "better", "worse", "rather", "other", "greater",
    "larger", "smaller", "fewer", "higher", "lower", "faster", "slower", "longer", "shorter",
    "bigger", "older", "younger" }) do
  PHRASES.confusions[word .. " then"] = "After a comparison, use 'than'."
end

-- Words that are only ever a contraction missing its apostrophe; "cant",
-- "wont", "well", "shell" and "were" are words in their own right.
for word, fixed in pairs({
  dont = "don't", doesnt = "doesn't", didnt = "didn't", isnt = "isn't", wasnt = "wasn't",
  arent = "aren't", werent = "weren't", hasnt = "hasn't", havent = "haven't", hadnt = "hadn't",
  couldnt = "couldn't", wouldnt = "wouldn't", shouldnt = "shouldn't", mustnt = "mustn't",
  neednt = "needn't", youre = "you're", theyre = "they're", youve = "you've", theyve = "they've",
  youll = "you'll", theyll = "they'll", ive = "I've",
}) do
  PHRASES.contractions[word] = { "Add the apostrophe: '" .. fixed .. "'.", fixed }
end
-- "IM" is an abbreviation; "im" and "Im" are a missing "I'm".
PHRASES.contractions.im = { "Add the apostrophe: 'I'm'.", "I'm", skip = "IM" }

for _, day in ipairs({ "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday" }) do
  local name = day:sub(1, 1):upper() .. day:sub(2)
  PHRASES.days[day] = { "Days of the week take a capital: '" .. name .. "'.", name, exact = true }
end

for phrase, better in pairs({
  ["very unique"] = "unique", ["past history"] = "history", ["free gift"] = "gift",
  ["pin number"] = "PIN", ["atm machine"] = "ATM", ["each and every"] = "each",
  ["end result"] = "result", ["added bonus"] = "bonus", ["close proximity"] = "proximity",
  ["advance planning"] = "planning", ["future plans"] = "plans", ["revert back"] = "revert",
  ["unexpected surprise"] = "surprise", ["basic fundamentals"] = "fundamentals",
  ["final outcome"] = "outcome", ["true fact"] = "fact", ["repeat again"] = "repeat",
}) do
  PHRASES.redundancy[phrase] = "Redundant: consider '" .. better .. "'."
end

-- Rules whose replacements Fix Safe Issues applies. The others only make
-- suggestions, since the right wording depends on the sentence.
local FIXABLE = { contractions = true, pronoun = true, days = true, malapropisms = true }

-- Every phrase, by its first word, as { phrase, rule, message, replacement, exact, skip }.
local BY_FIRST_WORD = {}
for rule, entries in pairs(PHRASES) do
  for phrase, entry in pairs(entries) do
    local message, replacement = entry, nil
    if type(entry) == "table" then message, replacement = entry[1], entry[2] end
    local first_word = phrase:match("^[^ ]+")
    BY_FIRST_WORD[first_word] = BY_FIRST_WORD[first_word] or {}
    table.insert(BY_FIRST_WORD[first_word], { phrase = phrase, rule = rule, message = message,
      replacement = FIXABLE[rule] and replacement or nil,
      exact = type(entry) == "table" and entry.exact, skip = type(entry) == "table" and entry.skip })
  end
end

-- The rules switched on, as a set of keys.
local function enabled_rules()
  local language = spell.language()
  local english = not language or language:lower():match("^en") ~= nil
  local enabled = {}
  for _, rule in ipairs(RULES) do
    if config.get("rule." .. rule.key, rule.default) and (english or not rule.english) then
      enabled[rule.key] = true
    end
  end
  return enabled
end

-- ---- a and an ----

-- Words spelled with a vowel but said with a consonant ("a university",
-- "a one-off"), by prefix, apart from un- words such as "uninteresting".
local CONSONANT_SOUNDS = { "eu", "ewe", "uni", "use", "usu", "usa", "uti", "ubiq" }
local VOWEL_SOUNDS = { "hour", "honest", "honor", "honour", "heir" }
-- Words where either article is accepted.
local EITHER = { historic = true, historical = true, herb = true, herbs = true, herbal = true }
-- Letters whose names start with a vowel sound: "an FBI agent", "an MRI".
local VOWEL_LETTERS = "AEFHILMNORSX"

local function starts_with_any(word, prefixes)
  for _, prefix in ipairs(prefixes) do
    if word:sub(1, #prefix) == prefix then return true end
  end
  return false
end

-- Whether word is said starting with a vowel sound, or nil when either
-- article would do.
local function vowel_sound(word)
  local lower = word:lower()
  if EITHER[lower] then return nil end
  -- Single, short or vowel-less capitals are said letter by letter: "an
  -- X-ray", "a U-turn".
  if word == word:upper() and (#word <= 3 or not word:find("[AEIOU]")) then
    return VOWEL_LETTERS:find(word:sub(1, 1), 1, true) ~= nil
  end
  if lower == "one" or lower == "once" or lower:sub(1, 4) == "one-" then return false end
  if starts_with_any(lower, VOWEL_SOUNDS) then return true end
  if starts_with_any(lower, CONSONANT_SOUNDS) and not lower:match("^uni[nmd]") then return false end
  return lower:match("^[aeiou]") ~= nil
end

-- ---- sentences ----

-- Abbreviations whose full stop doesn't end a sentence, without their dots.
local ABBREVIATIONS = {}
for word in ("eg ie etc vs cf approx dr mr mrs ms st fig eq ca al incl esp resp viz"):gmatch("%a+") do
  ABBREVIATIONS[word] = true
end

-- Whether the start of text's last line looks like a list marker: "- ",
-- "* ", "1. " or "- [ ] ".
local function after_list_marker(before)
  local line = before:match("[^\n]*$")
  return line:match("^%s*[-*+]%s+$") ~= nil or line:match("^%s*%d+[.)]%s+$") ~= nil
    or line:match("^%s*[-*+]%s+%[[ xX]%]%s+$") ~= nil
end

-- Issues in text, each with first/last shifted by offset. Fixable issues carry
-- fix = { first, last, replacement } relative to text.
local function inspect(text, offset)
  local issues = {}
  local code = code_ranges(text)
  local enabled = enabled_rules()
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
  if enabled.repeated then
    local previous, previous_first, previous_last
    for first, word, after in words:gmatch("()([%w_'\128-\255]+)()") do
      local lower = word:lower()
      if lower == previous and not lower:find("[%d_]")
          and text:sub(previous_last + 1, first - 1):match("^%s+$") then
        add_issue(previous_first, after - 1, "Repeated word: " .. word, { previous_last + 1, after - 1, "" })
      end
      previous, previous_first, previous_last = lower, first, after - 1
    end
  end

  if enabled.spacing then
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
  end

  if enabled.doubled then
    for first, run in text:gmatch("()([,;!?][,;!?]+)") do
      local punctuation = run:sub(1, 1)
      if run == punctuation:rep(#run) then
        add_issue(first, first + #run - 1, "Repeated '" .. punctuation .. "'.",
          { first, first + #run - 1, punctuation })
      end
    end
    -- Exactly two full stops: three are an ellipsis, and "../" is a path.
    for first, after in text:gmatch("()%.%.+()") do
      if after - first == 2 and not is_token(text, first) then
        add_issue(first, after - 1, "Repeated '.'.", { first, after - 1, "." })
      end
    end
  end

  if enabled.brackets then
    for first, spaces in text:gmatch("%(()([ \t]+)%S") do
      add_issue(first, first + #spaces - 1, "Unexpected space after '('.",
        { first, first + #spaces - 1, "" })
    end
    for first, spaces in text:gmatch("%S()([ \t]+)%)") do
      add_issue(first, first + #spaces - 1, "Unexpected space before ')'.",
        { first, first + #spaces - 1, "" })
    end
  end

  if enabled.double_spaces then
    for first, spaces in text:gmatch("%S()(  +)%S") do
      add_issue(first, first + #spaces - 1, "More than one space between words.",
        { first, first + #spaces - 1, " " })
    end
  end

  if enabled.capitals then
    local first_letter = text:find("%a")
    if first_letter and text:sub(first_letter, first_letter):match("%l")
        and not after_list_marker(text:sub(1, first_letter - 1)) then
      add_issue(first_letter, first_letter, "Sentence starts with a lowercase letter.")
    end
    for mark, position in text:gmatch("()[.!?]%s+()%l") do
      -- "e.g. this", "Dr. who" and the "2. " of a numbered list don't end a
      -- sentence.
      local window = text:sub(math.max(1, mark - 12), mark - 1)
      local before = window:match("[^\n]*$")
      local numbered = before:match("^%s*%d+$") and (#before < #window or mark <= 13)
      local abbreviation = text:sub(mark, mark) == "."
        and ABBREVIATIONS[(window:match("([%a.]+)$") or ""):gsub("%.", ""):lower()]
      if not numbered and not abbreviation then
        add_issue(position, position, "Sentence starts with a lowercase letter.")
      end
    end
  end

  if enabled.articles then
    for position, article, word in text:gmatch("()(%f[%a][Aa]n?%f[^%a])%s+([A-Za-z]+)") do
      -- A capital A after a word is usually a label, as in "plan A is".
      local window = text:sub(math.max(1, position - 3), position - 1)
      local label = article == "A" and window:match("[%w,]%s*$")
      local vowel = vowel_sound(word)
      local uses_an = article:lower() == "an"
      if vowel ~= nil and vowel ~= uses_an and not label then
        add_issue(position, position + #article - 1,
          string.format("Use '%s' before '%s'.", vowel and "an" or "a", word))
      end
    end
  end

  -- Words and phrases, found from their first word in one pass. A missing
  -- apostrophe is only reported if the Windows dictionary, when there is
  -- one, rejects the word too.
  local lower = text:lower()
  local unconfirmed = {}
  for first, word in lower:gmatch("()([%a']+)") do
    for _, entry in ipairs(BY_FIRST_WORD[word] or {}) do
      local last = first + #entry.phrase - 1
      local original = text:sub(first, last)
      local after = lower:sub(last + 1, last + 1)
      if enabled[entry.rule] and lower:sub(first, last) == entry.phrase
          and not after:match("[%w_']") and not lower:sub(first - 1, first - 1):match("[%w_']")
          and (not entry.exact or original == entry.phrase) and original ~= entry.skip
          and not is_token(text, first)
          -- "i" in "(i)", "i.e.", "i)" and "i++" is a numeral, abbreviation or code.
          and not (entry.rule == "pronoun" and (after:match("[.)%-+=]")
            or text:sub(first - 1, first - 1):match("[(.]"))) then
        local fix = entry.replacement and { first, last, like(entry.replacement, original) } or nil
        local issue = { first, last, entry.message, fix }
        if entry.rule == "contractions" and spell.language() then
          unconfirmed[#unconfirmed + 1] = { issue = issue, word = original }
        else
          add_issue(table.unpack(issue))
        end
      end
    end
  end
  if #unconfirmed > 0 then
    local candidates = {}
    for index, item in ipairs(unconfirmed) do candidates[index] = item.word end
    for index, correct in ipairs(spell.check(candidates)) do
      if not correct then add_issue(table.unpack(unconfirmed[index].issue)) end
    end
  end

  if enabled.plurals then
    for first, plural in text:gmatch("()(%d%d+)'s%f[^%w]") do
      add_issue(first, first + #plural + 1, "If this is a plural, write '" .. plural .. "s'.")
    end
    for first, plural in text:gmatch("()(%u%u+)'s%f[^%w]") do
      add_issue(first, first + #plural + 1, "If this is a plural, write '" .. plural .. "s'.")
    end
  end

  table.sort(issues, function(a, b) return a.first < b.first end)
  return issues
end

local function clear()
  editor.clear_indicator(INDICATOR)
end

-- Replaces the highlights with the issues in text, which starts at document
-- position first. Returns the number of issues.
local function mark(text, first)
  local issues = inspect(text, first - 1)
  clear()
  for _, issue in ipairs(issues) do
    editor.indicator(issue.first, issue.last, INDICATOR, COLOUR)
  end
  return #issues
end

local function highlight()
  local count = mark(source_range())
  ui.status(string.format("Grammar checker highlighted %d issue%s.", count, count == 1 and "" or "s"))
end

-- Live Highlighting always checks the whole document, as clear() removes
-- highlights everywhere; the selection is just where the user is typing.
local function live_highlight()
  if editor.length() <= LIVE_LIMIT then mark(editor.get_text(), 1) else clear() end
end

local function refresh_if_live()
  if config.get("live", false) then live_highlight() end
end

local function check()
  local text, first = source_range()
  local issues = inspect(text, first - 1)
  if #issues == 0 then ui.message("No heuristic grammar issues found.", "Grammar Checker"); return end
  -- In the output panel, where a saved document's "path:line:column" is
  -- double-clicked to go there.
  local path = editor.file_path()
  local lines = { string.format("Found %d possible issue%s:", #issues, #issues == 1 and "" or "s") }
  for index = 1, math.min(#issues, REPORT_LIMIT) do
    local issue = issues[index]
    local line_number = editor.line_at(issue.first)
    local line_start = editor.line_range(line_number)
    local before = editor.get_range(line_start, issue.first - 1)
    local column = (utf8.len(before) or #before) + 1
    local preview = editor.get_line(line_number):match("^%s*(.-)%s*$")
    if (utf8.len(preview) or 0) > 80 then preview = preview:sub(1, utf8.offset(preview, 78) - 1) .. "..." end
    local location = path and string.format("%s:%d:%d", path, line_number, column)
      or string.format("Line %d, column %d", line_number, column)
    lines[#lines + 1] = string.format("%s: %s\n    %s", location, issue.message, preview)
  end
  if #issues > REPORT_LIMIT then
    lines[#lines + 1] = string.format("...and %d more.", #issues - REPORT_LIMIT)
  end
  output.report("Grammar Checker", table.concat(lines, "\n"))
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
menu.add("/Tools/Grammar Checker/Highlight Issues", highlight)
menu.add("/Tools/Grammar Checker/Fix Safe Issues", fix_safe)
menu.add("/Tools/Grammar Checker/Clear Highlights", clear)
menu.separator("/Tools/Grammar Checker")
menu.checkbox("/Tools/Grammar Checker/Live Highlighting", "live", false, function(checked)
  if checked then live_highlight() else clear() end
end)
menu.separator("/Tools/Grammar Checker")
local style_separated = false
for _, rule in ipairs(RULES) do
  if rule.style and not style_separated then
    menu.separator("/Tools/Grammar Checker/Rules")
    style_separated = true
  end
  menu.checkbox("/Tools/Grammar Checker/Rules/" .. rule.label .. (rule.style and " (Style)" or ""),
    "rule." .. rule.key, rule.default, refresh_if_live)
end

events.on("text_changed", refresh_if_live)
events.on("open", refresh_if_live)
events.on("tab_changed", refresh_if_live)
