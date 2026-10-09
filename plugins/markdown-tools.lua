-- Markdown commands, on Snippets > Markdown:
--   Table of Contents...   a linked list of the document's headings
--   CSV to Table           comma-, semicolon- or tab-separated lines as a
--                          Markdown table
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
-- _ removed, spaces as -. UTF-8 characters are kept as they are.
local function anchor_for(text)
  return (text:lower():gsub("[^%w%s%-_\128-\255]", ""):gsub("%s", "-"))
end

-- Every heading outside code blocks and front matter, as { level, text }.
-- Both # headings and underlined (=== and ---) headings count.
local function document_headings()
  local lines = split_lines(editor.get_text(), editor.eol())
  local headings, fence = {}, nil
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
    if fence then
      -- A fence closes with at least as many of the same character.
      if marker and marker:sub(1, 1) == fence:sub(1, 1) and #marker >= #fence
          and not line:find("[^`~%s]") then
        fence = nil
      end
    elseif marker then
      fence = marker
    else
      local hashes, text = line:match("^ ? ? ?(#+)%s+(.-)%s*$")
      if hashes and #hashes <= 6 then
        text = text:gsub("%s+#+$", ""):gsub("^#+$", "")
        headings[#headings + 1] = { level = #hashes, text = text }
      elseif n > first and lines[n - 1]:find("%S") and not lines[n - 1]:find("^ ? ? ?#")
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

-- ---- CSV to Table ----

-- Splits one line of delimited text into fields. A field in double quotes
-- can contain the delimiter, and "" inside one is a literal quote. Quoted
-- fields can't span lines.
local function parse_delimited(line, delimiter)
  local fields, i = {}, 1
  while true do
    local field
    if line:sub(i, i) == '"' then
      local parts, j = {}, i + 1
      while true do
        local quote = line:find('"', j, true)
        if not quote then
          parts[#parts + 1] = line:sub(j)
          j = #line + 1
          break
        end
        parts[#parts + 1] = line:sub(j, quote - 1)
        if line:sub(quote + 1, quote + 1) == '"' then
          parts[#parts + 1] = '"'
          j = quote + 2
        else
          j = quote + 1
          break
        end
      end
      field = table.concat(parts)
      -- Anything between the closing quote and the delimiter is kept.
      local stop = line:find(delimiter, j, true) or #line + 1
      field = field .. line:sub(j, stop - 1)
      i = stop
    else
      local stop = line:find(delimiter, i, true) or #line + 1
      field = line:sub(i, stop - 1)
      i = stop
    end
    fields[#fields + 1] = field:match("^%s*(.-)%s*$")
    if i > #line then return fields end
    i = i + #delimiter
  end
end

-- Tabs if the first line has any, otherwise whichever of , and ; it has
-- more of.
local function guess_delimiter(line)
  if line:find("\t", 1, true) then return "\t" end
  local _, commas = line:gsub(",", "")
  local _, semicolons = line:gsub(";", "")
  return semicolons > commas and ";" or ","
end

local function display_width(text)
  return utf8.len(text) or #text
end

menu.add("/Snippets/Markdown/CSV to Table", function()
  local start, finish = editor.get_selection_range()
  if finish < start then
    ui.message("Select the comma-, semicolon- or tab-separated lines to convert, starting with the header row.")
    return
  end
  local lines = {}
  for _, line in ipairs(split_lines(editor.get_selection(), editor.eol())) do
    if line:find("%S") then lines[#lines + 1] = line end
  end
  if #lines == 0 then return end

  local delimiter = guess_delimiter(lines[1])
  local rows, widths = {}, {}
  for r, line in ipairs(lines) do
    rows[r] = parse_delimited(line, delimiter)
    for c, cell in ipairs(rows[r]) do
      cell = cell:gsub("|", "\\|")
      rows[r][c] = cell
      widths[c] = math.max(widths[c] or 3, display_width(cell))
    end
  end

  local function format_row(cells)
    local padded = {}
    for c, width in ipairs(widths) do
      local cell = cells[c] or ""
      padded[c] = cell .. string.rep(" ", width - display_width(cell))
    end
    return "| " .. table.concat(padded, " | ") .. " |"
  end

  local separator = {}
  for c, width in ipairs(widths) do separator[c] = string.rep("-", width) end
  local out = { format_row(rows[1]), format_row(separator) }
  for r = 2, #rows do out[#out + 1] = format_row(rows[r]) end

  local text = table.concat(out, editor.eol())
  -- Keep a line ending the selection had at its end.
  if editor.get_selection():sub(-#editor.eol()) == editor.eol() then text = text .. editor.eol() end
  editor.replace_selection(text)
end)
