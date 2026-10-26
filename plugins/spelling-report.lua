-- Tools > Spelling Report: lists every word the Windows spelling dictionary
-- (View > Spell Check Language) rejects in the selection, or the whole
-- document, with how often each appears, most frequent first. Choosing one
-- selects all its occurrences, so typing the correction fixes them together.
--
-- Like spell checking in the editor, it skips Markdown code (fenced blocks
-- and inline spans), link and image targets, autolinks and HTML tags, and
-- words inside URLs, paths, email addresses and file names. It reads the
-- text rather than the editor's styling, which only covers what has been
-- shown, so a long document is checked the same way throughout.
--
-- Uses only the plugin API documented in README.md in this folder.

local BATCH = 5000

-- Ranges of text that isn't prose, as { first, last } pairs: fenced code
-- blocks, inline code spans, ](link targets), <autolinks> and <html tags>.
local function skipped_ranges(text)
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
      for first, last in line:gmatch("%]%(()[^)]*()%)") do
        ranges[#ranges + 1] = { position + first - 1, position + last - 2 }
      end
      for first, last in line:gmatch("()<[/!%a][^<>]*>()") do
        ranges[#ranges + 1] = { position + first - 1, position + last - 2 }
      end
    end
    position = line_end + 1
  end
  if fence then ranges[#ranges + 1] = { fence_start, #text } end
  table.sort(ranges, function(a, b) return a[1] < b[1] end)
  return ranges
end

-- Whether the whitespace-separated token around position is a URL, path,
-- email address, file name or :emoji: shortcode rather than prose.
local function in_token(text, position)
  local first = position
  while first > 1 and not text:sub(first - 1, first - 1):match("%s") do first = first - 1 end
  local token = text:match("^%S+", first)
  return token:find("[/\\=&@]") ~= nil or token:find("www.", 1, true) ~= nil
    or token:find("%a%.%a") ~= nil or token:find(":[%w_+%-]+:") ~= nil
end

-- Whether a character beyond ASCII separates words: Latin-1 punctuation,
-- × and ÷, general punctuation (except ’, which is also the apostrophe in
-- "don’t"), arrows and other symbols, CJK punctuation and emoji.
local function is_separator(code)
  return code <= 0xbf or code == 0xd7 or code == 0xf7
    or (code >= 0x2000 and code <= 0x206f and code ~= 0x2019)
    or (code >= 0x2190 and code <= 0x2bff) or (code >= 0x2e00 and code <= 0x2e7f)
    or (code >= 0x3000 and code <= 0x303f) or code >= 0x1f000
end

-- text with separators beyond ASCII blanked, keeping their byte length so
-- positions still match.
local function mask_separators(text)
  return (text:gsub("[\192-\255][\128-\191]*", function(character)
    local ok, code = pcall(utf8.codepoint, character)
    if not ok or is_separator(code) then return (" "):rep(#character) end
  end))
end

local APOSTROPHES = { "'", "\226\128\153" }

-- The words to check, each { text, first, last }. Bytes 128-255 count as
-- letters once separators are blanked, so accented words stay whole.
local function words_in(text, offset)
  local masked = mask_separators(text)
  local skipped, next_skip = skipped_ranges(text), 1
  local words = {}
  for first, word in masked:gmatch("()([%a\128-\255][%a\128-\255']*)") do
    local after = first + #word
    -- Apostrophes around a word ('tis, dogs') aren't part of it.
    for _, mark in ipairs(APOSTROPHES) do
      while word:sub(1, #mark) == mark do word, first = word:sub(#mark + 1), first + #mark end
      while #word > 0 and word:sub(-#mark) == mark do word = word:sub(1, -#mark - 1) end
    end
    local last = first + #word - 1
    while skipped[next_skip] and skipped[next_skip][2] < first do next_skip = next_skip + 1 end
    local range = skipped[next_skip]
    local before, following = masked:sub(first - 1, first - 1), masked:sub(after, after)
    if (utf8.len(word) or 0) > 1 and not (range and range[1] <= last)
        and not before:match("[%d_]") and not following:match("[%d_]")
        and not in_token(masked, first) then
      words[#words + 1] = { text = word, first = offset + first, last = offset + last }
    end
  end
  return words
end

-- The rejected words as { text, count, places }, most frequent first.
local function misspellings(words)
  local by_text, distinct = {}, {}
  for _, word in ipairs(words) do
    local entry = by_text[word.text]
    if not entry then
      entry = { text = word.text, places = {} }
      by_text[word.text] = entry
      distinct[#distinct + 1] = word.text
    end
    entry.places[#entry.places + 1] = word
  end
  local rejected = {}
  for first = 1, #distinct, BATCH do
    local batch = table.move(distinct, first, math.min(first + BATCH - 1, #distinct), 1, {})
    for index, correct in ipairs(spell.check(batch)) do
      if not correct then rejected[#rejected + 1] = by_text[batch[index]] end
    end
  end
  table.sort(rejected, function(a, b)
    if #a.places ~= #b.places then return #a.places > #b.places end
    return a.text:lower() < b.text:lower()
  end)
  return rejected, #distinct
end

-- Scrolls first: editor.scroll_to moves the caret, which would drop the
-- other selections.
local function select_all(places)
  editor.scroll_to(places[1].first)
  editor.set_selection(places[1].first, places[1].last)
  for index = 2, #places do editor.add_selection(places[index].first, places[index].last) end
  editor.set_main_selection(1)
end

local function spelling_report()
  if not spell.language() then
    ui.message("Windows has no spelling dictionary to check words against. " ..
      "Add a language with spelling support in Windows Settings > Time & language.", nil, "warning")
    return
  end
  local selected = editor.get_selection()
  local offset = selected ~= "" and editor.get_selection_range() - 1 or 0
  local words = words_in(selected ~= "" and selected or editor.get_text(), offset)
  local scope = selected ~= "" and "selection" or "document"
  local rejected, distinct = misspellings(words)
  if #rejected == 0 then
    ui.message(string.format("No spelling mistakes in the %s's %d words (%s).",
      scope, #words, spell.language()), "Spelling Report")
    return
  end
  local items, occurrences = {}, 0
  for index, entry in ipairs(rejected) do
    -- ui.choose shows at most 10,000; the rest are the least frequent.
    if index <= 10000 then items[index] = string.format("%s  (%d)", entry.text, #entry.places) end
    occurrences = occurrences + #entry.places
  end
  local index = ui.choose(items, string.format(
    "%d misspelled of %d different words in the %s, %d times in all (%s). " ..
    "Choose one to select every occurrence:",
    #rejected, distinct, scope, occurrences, spell.language()), "Spelling Report")
  if not index then return end
  select_all(rejected[index].places)
  ui.status(string.format("Selected %d occurrence%s of \"%s\". Type to replace them all.",
    #rejected[index].places, #rejected[index].places == 1 and "" or "s", rejected[index].text))
end

menu.add("/Tools/Spelling Report...", spelling_report)
