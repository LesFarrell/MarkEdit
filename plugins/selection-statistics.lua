-- Document and selection statistics, with an optional live status summary.

local function selected_or_document()
  local selected = editor.get_selection()
  return selected ~= "" and selected or editor.get_text(), selected ~= ""
end

local function statistics(text)
  local characters = utf8.len(text)
  local words, in_word, sentences = 0, false, 0
  if characters then
    for _, code in utf8.codes(text) do
      local ascii_word = code < 128 and
        ((code >= 48 and code <= 57) or (code >= 65 and code <= 90) or
         (code >= 97 and code <= 122) or code == 95)
      local word = ascii_word or code >= 0xC0
      if word and not in_word then words = words + 1 end
      in_word = word
      if code == 46 or code == 33 or code == 63 then sentences = sentences + 1 end
    end
  else
    characters = #text
    for index = 1, #text do
      local byte = text:byte(index)
      local word = (byte >= 48 and byte <= 57) or (byte >= 65 and byte <= 90) or
        (byte >= 97 and byte <= 122) or byte == 95 or byte >= 0xC0
      if word and not in_word then words = words + 1 end
      in_word = word
      if byte == 46 or byte == 33 or byte == 63 then sentences = sentences + 1 end
    end
  end
  local lines = 1
  local line_text = text:gsub("\r\n", "\n")
  for _ in line_text:gmatch("[\r\n]") do lines = lines + 1 end
  if text == "" then lines = 0 end
  local bytes = #text
  local minutes = words == 0 and 0 or math.max(1, math.ceil(words / 200))
  return { words = words, characters = characters, bytes = bytes, lines = lines,
           sentences = sentences, minutes = minutes }
end

local function summary(stats)
  return string.format("%d words, %d characters, %d lines, ~%d min read",
    stats.words, stats.characters, stats.lines, stats.minutes)
end

local function show_report()
  local text, selected = selected_or_document()
  local stats = statistics(text)
  output.report(selected and "Selection Statistics" or "Document Statistics", string.format(
    "Words: %d\nCharacters: %d\nUTF-8 bytes: %d\nLines: %d\nSentences: %d\nEstimated reading time: %d minute%s",
    stats.words, stats.characters, stats.bytes, stats.lines, stats.sentences,
    stats.minutes, stats.minutes == 1 and "" or "s"))
end

local function live_summary()
  if not config.get("live", false) then return end
  local text, selected = selected_or_document()
  ui.status((selected and "Selection: " or "Document: ") .. summary(statistics(text)))
end

menu.add("/Tools/Statistics/Document or Selection Report", show_report)
menu.checkbox("/Tools/Statistics/Show Selection Statistics in Status Bar", "live", false,
  function(checked)
    if checked then live_summary() else ui.status("Live selection statistics disabled.") end
  end)

events.on("selection_changed", live_summary)
