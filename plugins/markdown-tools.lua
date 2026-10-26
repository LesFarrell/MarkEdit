-- Markdown commands, on Snippets > Markdown:
--   Table of Contents...   a linked list of the document's headings
--
-- Uses only the plugin API documented in README.md in this folder.

-- Splits on the document's line ending.
local function split_lines(text, eol)
  local lines, start = {}, 1
  while true do
    local first, last = text:find(eol, start, true)
    if not first then break end
    lines[#lines + 1] = text:sub(start, first - 1)
    start = last + 1
  end
  lines[#lines + 1] = text:sub(start)
  return lines
end

-- ---- Table of Contents ----

-- A heading's text as it reads once rendered, near enough for a contents
-- entry: links and images become their text, and emphasis and code marks go.
local function plain_heading(text)
  return (text:gsub("!?%[(.-)%]%b()", "%1"):gsub("[*`]", ""):gsub("~~", ""))
end

-- The anchor GitHub gives a heading: lower case, punctuation other than - and
-- _ removed, spaces as -. UTF-8 characters are kept as they are, except
-- curly quotes, en/em dashes and ellipses, which are punctuation too.
local function anchor_for(text)
  return (text:lower():gsub("\226\128[\147\148\152\153\156\157\166]", ""):gsub("[^%w%s%-_\128-\255]", ""):gsub("%s", "-"))
end

-- No real Markdown heading is remotely this long. Skipping the checks below
-- for a longer line isn't just an optimisation: plain_heading()'s link
-- pattern backtracks past every "[...]" that isn't followed by "(...)", so
-- on a huge line - most often the whole document read as a single line,
-- because it has no line breaks, or because editor.eol() disagrees with
-- what the text actually uses - it can take effectively forever instead of
-- the moment a short heading line costs.
local MAX_HEADING_LINE_LENGTH = 4000

-- Every heading outside code blocks and front matter, as { level, text }.
-- Both # headings and underlined (=== and ---) headings count.
local function document_headings()
  local lines = split_lines(editor.get_text(), editor.eol())
  local headings, fence, code = {}, nil, {}
  local first = 1
  -- YAML front matter: a --- line at the very start, up to the next one.
  if lines[1] == "---" then
    for n = 2, #lines do
      if lines[n] == "---" or lines[n] == "..." then first = n + 1 break end
    end
  end
  for n = first, #lines do
    local line = lines[n]
    local marker = line:match("^ ? ? ?(```+)") or line:match("^ ? ? ?(~~~+)")
    -- Fence lines and the code between them, so none of them is underlined.
    code[n] = fence ~= nil or marker ~= nil
    if fence then
      -- A fence closes with at least as many of the same character.
      if marker and marker:sub(1, 1) == fence:sub(1, 1) and #marker >= #fence
          and not line:find("[^`~%s]") then
        fence = nil
      end
    elseif marker then
      fence = marker
    else
      local hashes, text
      if #line <= MAX_HEADING_LINE_LENGTH then
        hashes, text = line:match("^ ? ? ?(#+)%s+(.-)%s*$")
      end
      if hashes and #hashes <= 6 then
        text = text:gsub("%s+#+$", ""):gsub("^#+$", "")
        if plain_heading(text):find("%S") then
          headings[#headings + 1] = { level = #hashes, text = text }
        end
      elseif n > first and #lines[n - 1] <= MAX_HEADING_LINE_LENGTH
          and lines[n - 1]:find("%S") and not lines[n - 1]:find("^ ? ? ?#")
          and not code[n - 1] and plain_heading(lines[n - 1]):find("%S")
          and (line:find("^ ? ? ?=+%s*$") or line:find("^ ? ? ?%-+%s*$")) then
        local previous = headings[#headings]
        -- An underline can't itself be underlined: in "Title" / "===" /
        -- "---" the last line is a rule. (A # heading followed by --- is
        -- ruled out by the check above.)
        if not (previous and previous.line == n - 1) then
          headings[#headings + 1] = { level = line:find("=") and 1 or 2, text = lines[n - 1]:match("^%s*(.-)%s*$"), line = n }
        end
      end
    end
  end
  return headings
end

menu.add("/Snippets/Markdown/Table of Contents...", function()
  local depths = { "Levels 1 to 2", "Levels 1 to 3", "Levels 1 to 4", "All levels" }
  local choice = ui.choose(depths, "Include headings at:")
  if not choice then return end
  local deepest = choice == #depths and 6 or choice + 1

  local headings = document_headings()
  local shallowest = 7
  for _, heading in ipairs(headings) do
    if heading.level <= deepest then shallowest = math.min(shallowest, heading.level) end
  end
  if shallowest == 7 then
    ui.message("This document has no headings at those levels.")
    return
  end

  local entries, used = {}, {}
  for _, heading in ipairs(headings) do
    local text = plain_heading(heading.text)
    -- Every heading gets an anchor, even those left out, so repeated names
    -- are numbered -1, -2 ... the same way GitHub numbers them.
    local base = anchor_for(text)
    local anchor = base
    if used[base] then
      anchor = base .. "-" .. used[base]
      used[base] = used[base] + 1
    else
      used[base] = 1
    end
    if heading.level <= deepest then
      local indent = string.rep("  ", heading.level - shallowest)
      entries[#entries + 1] = string.format("%s- [%s](#%s)", indent, (text:gsub("([%[%]])", "\\%1")), anchor)
    end
  end
  local eol = editor.eol()
  editor.replace_selection(table.concat(entries, eol) .. eol)
end)
