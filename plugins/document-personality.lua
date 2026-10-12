-- A playful, transparent description of a document's writing style.
-- The results are simple heuristics, not a linguistic or psychological test.

local LONG_SENTENCE_INDICATOR = 18
local REPEATED_WORD_INDICATOR = 19

local stop_words = {
  a = true, an = true, ["and"] = true, are = true, as = true, at = true, be = true,
  ["but"] = true, by = true, ["for"] = true, from = true, had = true, has = true,
  have = true, he = true, her = true, his = true, i = true, ["in"] = true,
  is = true, it = true, its = true, me = true, my = true, ["not"] = true,
  of = true, on = true, ["or"] = true, our = true, she = true, so = true,
  that = true, the = true, their = true, them = true, they = true,
  this = true, to = true, was = true, we = true, were = true, with = true,
  you = true, your = true
}

local function words_in(text, offset)
  local words = {}
  offset = offset or 0
  for first, word, after in text:gmatch("()([A-Za-z0-9_\128-\255]+)()") do
    words[#words + 1] = {
      text = word,
      key = word:lower(),
      first = offset + first,
      last = offset + after - 1
    }
  end
  return words
end

local function sentence_ranges(text, offset)
  local ranges, start, position = {}, 1, 1
  offset = offset or 0
  while position <= #text do
    local punctuation = text:find("[.!?]", position)
    if not punctuation then break end
    local finish = punctuation
    while finish < #text and text:sub(finish + 1, finish + 1):match("[.!?]") do
      finish = finish + 1
    end
    local sentence = text:sub(start, finish)
    local count = #words_in(sentence)
    if count > 0 then
      local leading = sentence:match("^%s*") or ""
      ranges[#ranges + 1] = {
        first = offset + start + #leading,
        last = offset + finish,
        text = sentence:sub(#leading + 1),
        words = count
      }
    end
    start, position = finish + 1, finish + 1
  end
  local tail = text:sub(start)
  local count = #words_in(tail)
  if count > 0 then
    local leading = tail:match("^%s*") or ""
    ranges[#ranges + 1] = {
      first = offset + start + #leading,
      last = offset + #text,
      text = tail:sub(#leading + 1),
      words = count
    }
  end
  return ranges
end

local function count_plain(text, needle)
  local count, position = 0, 1
  while true do
    local found = text:find(needle, position, true)
    if not found then return count end
    count, position = count + 1, found + #needle
  end
end

local function paragraph_count(text)
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  local count, in_paragraph = 0, false
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:match("%S") then
      if not in_paragraph then count = count + 1 end
      in_paragraph = true
    else
      in_paragraph = false
    end
  end
  return count
end

local function line_count(text)
  if text == "" then return 0 end
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  local _, breaks = text:gsub("\n", "")
  return breaks + 1
end

local function classify_mood(stats)
  if stats.letters >= 10 and stats.uppercase / stats.letters >= 0.65 then
    return "LOUD", "Most letters are capitalised; this document has brought a megaphone."
  end
  if stats.exclamations >= math.max(2, stats.sentences / 3) then
    return "Enthusiastic", "Exclamation marks give it plenty of forward energy."
  end
  if stats.questions >= math.max(2, stats.sentences / 3) then
    return "Curious", "It asks more questions than most documents dare to."
  end
  if stats.ellipses >= 2 then
    return "Suspenseful", "Those ellipses keep leaving thoughts hanging..."
  end
  if stats.average_sentence >= 20 then
    return "Contemplative", "Long sentences let ideas wander before coming home."
  end
  if stats.average_sentence <= 8 then
    return "Punchy", "Short sentences keep the rhythm brisk."
  end
  if stats.words >= 20 and stats.unique / stats.words >= 0.65 then
    return "Eclectic", "Its vocabulary likes taking the scenic route."
  end
  return "Measured", "Its rhythm and punctuation are pleasantly balanced."
end

local function analyse(text)
  local words, sentences = words_in(text), sentence_ranges(text)
  if #words == 0 then return nil end

  local frequencies, unique, favourite, favourite_count = {}, 0, nil, 0
  local letters, uppercase = 0, 0
  for _, entry in ipairs(words) do
    if not frequencies[entry.key] then unique = unique + 1 end
    frequencies[entry.key] = (frequencies[entry.key] or 0) + 1
    if not stop_words[entry.key] and #entry.key >= 3 then
      local count = frequencies[entry.key]
      if count > favourite_count or
        (count == favourite_count and (not favourite or entry.key < favourite)) then
        favourite, favourite_count = entry.key, count
      end
    end
    for letter in entry.text:gmatch("[A-Za-z]") do
      letters = letters + 1
      if letter:match("%u") then uppercase = uppercase + 1 end
    end
  end

  local longest, total_sentence_words = nil, 0
  for _, sentence in ipairs(sentences) do
    total_sentence_words = total_sentence_words + sentence.words
    if not longest or sentence.words > longest.words then longest = sentence end
  end

  local stats = {
    words = #words,
    unique = unique,
    sentences = math.max(#sentences, 1),
    average_sentence = total_sentence_words / math.max(#sentences, 1),
    exclamations = count_plain(text, "!"),
    questions = count_plain(text, "?"),
    ellipses = count_plain(text, "..."),
    letters = letters,
    uppercase = uppercase
  }
  stats.mood, stats.reason = classify_mood(stats)
  stats.favourite, stats.favourite_count = favourite, favourite_count
  stats.longest = longest
  stats.lines = line_count(text)
  stats.paragraphs = paragraph_count(text)
  return stats
end

local function preview(text)
  text = text:gsub("%s+", " "):match("^%s*(.-)%s*$")
  if #text > 64 then return text:sub(1, 61) .. "..." end
  return text
end

local function show_report()
  local selected = editor.get_selection()
  local text = selected ~= "" and selected or editor.get_text()
  local stats = analyse(text)
  if not stats then
    ui.message("There are no words to analyse.", nil, "warning")
    return
  end

  local reading_seconds = math.max(1, math.floor(stats.words / 200 * 60 + 0.5))
  local reading_time = reading_seconds < 60 and reading_seconds .. " seconds"
    or math.ceil(reading_seconds / 60) .. " minutes"
  local favourite = stats.favourite and
    string.format("%s (%d times)", stats.favourite, stats.favourite_count) or "No clear favourite"
  local longest = stats.longest and
    string.format("%d words — %s", stats.longest.words, preview(stats.longest.text)) or "None"
  local scope = selected ~= "" and "Selection" or "Document"

  ui.message(table.concat({
    scope .. " mood: " .. stats.mood,
    stats.reason,
    "",
    string.format("Words: %d  |  Unique: %d  |  Sentences: %d", stats.words, stats.unique,
      stats.sentences),
    string.format("Paragraphs: %d  |  Lines: %d  |  Reading time: %s", stats.paragraphs,
      stats.lines, reading_time),
    string.format("Average sentence: %.1f words", stats.average_sentence),
    "Favourite word: " .. favourite,
    "Longest sentence: " .. longest
  }, "\n"), "Document Personality")
end

local function positive_integer(label, default, maximum)
  local answer = ui.prompt(label, tostring(default))
  if answer == nil then return nil end
  local value = tonumber(answer)
  if not value or value % 1 ~= 0 or value < 2 or value > maximum then
    ui.message("Enter a whole number from 2 to " .. maximum .. ".", nil, "warning")
    return nil
  end
  return value
end

local function highlight_long_sentences()
  local minimum = positive_integer("Highlight sentences with at least this many words:", 25, 500)
  if not minimum then return end
  editor.clear_indicator(LONG_SENTENCE_INDICATOR)
  local count = 0
  for _, sentence in ipairs(sentence_ranges(editor.get_text())) do
    if sentence.words >= minimum then
      editor.indicator(sentence.first, sentence.last, LONG_SENTENCE_INDICATOR, 0xffa726)
      count = count + 1
    end
  end
  ui.status(string.format("Highlighted %d long sentence%s.", count, count == 1 and "" or "s"))
end

local function highlight_repeated_words()
  local minimum = positive_integer("Highlight words used at least this many times:", 3, 100)
  if not minimum then return end
  local words, frequencies = words_in(editor.get_text()), {}
  for _, entry in ipairs(words) do
    if not stop_words[entry.key] and #entry.key >= 4 then
      frequencies[entry.key] = (frequencies[entry.key] or 0) + 1
    end
  end
  editor.clear_indicator(REPEATED_WORD_INDICATOR)
  local count, kinds = 0, {}
  for _, entry in ipairs(words) do
    if (frequencies[entry.key] or 0) >= minimum then
      editor.indicator(entry.first, entry.last, REPEATED_WORD_INDICATOR, 0xab47bc)
      count = count + 1
      kinds[entry.key] = true
    end
  end
  local kind_count = 0
  for _ in pairs(kinds) do kind_count = kind_count + 1 end
  ui.status(string.format("Highlighted %d occurrence%s of %d repeated word%s.", count,
    count == 1 and "" or "s", kind_count, kind_count == 1 and "" or "s"))
end

menu.add("Document Personality/Analyse", show_report)
menu.separator("Document Personality")
menu.add("Document Personality/Highlight Long Sentences...", highlight_long_sentences)
menu.add("Document Personality/Highlight Repeated Words...", highlight_repeated_words)
menu.add("Document Personality/Clear Highlights", function()
  editor.clear_indicator(LONG_SENTENCE_INDICATOR)
  editor.clear_indicator(REPEATED_WORD_INDICATOR)
end)
