-- Markdown structure and link commands.
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

-- ---- shared selected-line helpers ----

local function selected_line_range()
  local start, finish = editor.get_selection_range()
  local first, last = editor.line_at(start), editor.line_at(finish + 1)
  if last > first and finish + 1 == (editor.line_range(last)) then last = last - 1 end
  return first, last
end

local function replace_lines(first, last, lines)
  local start = editor.line_range(first)
  local _, finish = editor.line_range(last)
  local text = table.concat(lines, editor.eol())
  editor.replace_range(start, finish, text)
  editor.set_selection(start, start + #text - 1)
end

local function transform_selected_lines(fn)
  local first, last = selected_line_range()
  local lines = {}
  for line = first, last do lines[#lines + 1] = fn(editor.get_line(line), line, first) end
  replace_lines(first, last, lines)
end

local function markdown_document()
  return editor.language() == "markdown"
end

-- ---- structure tools ----

local function change_heading(delta)
  transform_selected_lines(function(line)
    local indent, hashes, text = line:match("^([ \t]*)(#+)[ \t]+(.*)$")
    if not hashes or #hashes > 6 then return line end
    local level = math.max(1, math.min(6, #hashes + delta))
    return indent .. string.rep("#", level) .. " " .. text
  end)
end

local function renumber_lists()
  local numbers = {}
  transform_selected_lines(function(line)
    local indent, text = line:match("^([ \t]*)%d+%.[ \t]+(.*)$")
    if not indent then numbers = {} return line end
    numbers[indent] = (numbers[indent] or 0) + 1
    return indent .. numbers[indent] .. ". " .. text
  end)
end

local function toggle_tasks()
  local first, last = selected_line_range()
  local check = false
  for line = first, last do
    if editor.get_line(line):match("^[ \t]*[-*+] %[[ ]%]") then check = true break end
  end
  transform_selected_lines(function(line)
    return (line:gsub("^([ \t]*[-*+] )%[[ xX]%]", "%1[" .. (check and "x" or " ") .. "]", 1))
  end)
end

local function strip_block_marker(line)
  local indent, body = line:match("^([ \t]*)(.*)$")
  body = body:gsub("^#+[ \t]+", "", 1)
  body = body:gsub("^>[ \t]?", "", 1)
  body = body:gsub("^[-*+][ \t]+%[[ xX]%][ \t]+", "", 1)
  body = body:gsub("^[-*+][ \t]+", "", 1)
  body = body:gsub("^%d+%.[ \t]+", "", 1)
  return indent, body
end

local function change_block_type()
  local choices = { "Paragraph", "Heading 1", "Heading 2", "Heading 3",
                    "Blockquote", "Bullet list", "Numbered list", "Task list" }
  local choice = ui.choose(choices, "Convert selected lines to:", "Markdown Block Type")
  if not choice then return end
  local number = 0
  transform_selected_lines(function(line)
    local indent, body = strip_block_marker(line)
    if choice == 1 then return indent .. body end
    if choice >= 2 and choice <= 4 then return indent .. string.rep("#", choice - 1) .. " " .. body end
    if choice == 5 then return indent .. "> " .. body end
    if choice == 6 then return indent .. "- " .. body end
    if choice == 7 then number = number + 1 return indent .. number .. ". " .. body end
    return indent .. "- [ ] " .. body
  end)
end

local function paragraph_range()
  local start, finish = editor.get_selection_range()
  if finish >= start then
    local first, last = editor.line_at(start), editor.line_at(finish + 1)
    if last > first and finish + 1 == (editor.line_range(last)) then last = last - 1 end
    return first, last
  end
  local first, last = editor.current_line(), editor.current_line()
  while first > 1 and editor.get_line(first - 1):find("%S") do first = first - 1 end
  while last < editor.line_count() and editor.get_line(last + 1):find("%S") do last = last + 1 end
  return first, last
end

local function reflow_paragraph()
  local answer = ui.prompt("Maximum line width:", tostring(config.get("reflow_width", 80)),
                           "Reflow Paragraph")
  local width = answer and tonumber(answer)
  if not width or width < 20 or width > 240 then
    if answer then ui.message("Use a width from 20 to 240.", "Reflow Paragraph", "warning") end
    return
  end
  width = math.floor(width)
  config.set("reflow_width", width)
  local first, last = paragraph_range()
  local words = {}
  for line = first, last do
    local text = editor.get_line(line)
    if text:match("^%s*[#>|]%s") or text:match("^%s*[-*+]%s") or
        text:match("^%s*%d+%.%s") or text:match("^%s*```") or text:match("^%s*~~~") then
      ui.message("Reflow handles plain paragraphs, not headings, lists, quotes, tables, or code fences.",
        "Reflow Paragraph", "warning")
      return
    end
    for word in text:gmatch("%S+") do words[#words + 1] = word end
  end
  if #words == 0 then return end
  local lines, current = {}, words[1]
  for index = 2, #words do
    if #current + 1 + #words[index] <= width then current = current .. " " .. words[index]
    else lines[#lines + 1], current = current, words[index] end
  end
  lines[#lines + 1] = current
  replace_lines(first, last, lines)
end

-- ---- link tools ----

local function heading_anchor_at_caret()
  local text = editor.get_line(editor.current_line()):match("^%s*#+%s+(.-)%s*#*%s*$")
  return text and anchor_for(plain_heading(text)) or nil
end

local function copy_heading_anchor()
  local anchor = heading_anchor_at_caret()
  if not anchor or anchor == "" then
    ui.message("Put the caret on an ATX heading first.", "Copy Heading Anchor", "warning")
    return
  end
  editor.clipboard("#" .. anchor)
  ui.status("Copied #" .. anchor)
end

local function anchor_inventory()
  local anchors, used = {}, {}
  for _, heading in ipairs(document_headings()) do
    local base = anchor_for(plain_heading(heading.text))
    local suffix = used[base] or 0
    local anchor = suffix == 0 and base or base .. "-" .. suffix
    used[base] = suffix + 1
    anchors[anchor] = true
  end
  return anchors
end

local function check_internal_links()
  local anchors, issues = anchor_inventory(), {}
  local path = editor.file_path()
  for line = 1, editor.line_count() do
    local text = editor.get_line(line)
    for anchor in text:gmatch("%b[]%(%s*#([^%)%s]+)%s*%)") do
      if not anchors[anchor] then
        local location = path and string.format("%s:%d:1", path, line) or ("Line " .. line)
        issues[#issues + 1] = location .. ": Unresolved internal anchor #" .. anchor
      end
    end
  end
  if #issues == 0 then
    problems.clear("Markdown Links")
    ui.status("Every internal Markdown link resolves.")
  else
    problems.report("Markdown Links", table.concat(issues, "\n"))
    ui.status(#issues .. " unresolved internal link" .. (#issues == 1 and "" or "s") .. ".")
  end
end

local function inline_to_references()
  local text, definitions, used_ids, next_id = editor.get_text(), {}, {}, 1
  for id in text:gmatch("^%s*%[([^%]]+)%]:") do used_ids[id:lower()] = true end
  for id in text:gmatch("[\r\n]%s*%[([^%]]+)%]:") do used_ids[id:lower()] = true end
  local converted = text:gsub("(!?)%[([^%]]-)%]%(([^%)%s]+)%)", function(image, label, url)
    while used_ids[tostring(next_id)] do next_id = next_id + 1 end
    local id = tostring(next_id)
    used_ids[id] = true
    next_id = next_id + 1
    definitions[#definitions + 1] = "[" .. id .. "]: " .. url
    return image .. "[" .. label .. "][" .. id .. "]"
  end)
  if #definitions == 0 then return ui.status("No inline Markdown links found.") end
  converted = converted:gsub("[\r\n]+$", "") .. editor.eol() .. editor.eol() ..
    table.concat(definitions, editor.eol()) .. editor.eol()
  editor.set_text(converted)
end

local function references_to_inline()
  local definitions, definition_lines, kept = {}, {}, {}
  for line = 1, editor.line_count() do
    local text = editor.get_line(line)
    local id, url = text:match("^%s*%[([^%]]+)%]:%s*(%S+)%s*$")
    if id then
      definitions[id:lower()] = url
      definition_lines[#definition_lines + 1] = { id = id:lower(), line = text }
    else
      kept[#kept + 1] = text
    end
  end
  local body = table.concat(kept, editor.eol())
  local count, used = 0, {}
  body = body:gsub("(!?)%[([^%]]-)%]%[([^%]]+)%]", function(image, label, id)
    local key = id:lower()
    local url = definitions[key]
    if not url then return image .. "[" .. label .. "][" .. id .. "]" end
    count = count + 1
    used[key] = true
    return image .. "[" .. label .. "](" .. url .. ")"
  end)
  if count == 0 then return ui.status("No resolvable reference links found.") end
  body = body:gsub("[\r\n]+$", "")
  local unused = {}
  for _, definition in ipairs(definition_lines) do
    if not used[definition.id] then unused[#unused + 1] = definition.line end
  end
  if #unused > 0 then
    body = body .. editor.eol() .. editor.eol() .. table.concat(unused, editor.eol())
  end
  editor.set_text(body .. editor.eol())
end

local function tidy_reference_definitions()
  local used, definitions, body = {}, {}, {}
  local text = editor.get_text()
  for id in text:gmatch("!?%[[^%]]-%]%[([^%]]+)%]") do used[id:lower()] = true end
  for line = 1, editor.line_count() do
    local value = editor.get_line(line)
    local id = value:match("^%s*%[([^%]]+)%]:")
    if id then
      if used[id:lower()] then definitions[#definitions + 1] = { id = id:lower(), line = value } end
    else
      body[#body + 1] = value
    end
  end
  table.sort(definitions, function(a, b) return a.id < b.id end)
  while #body > 0 and not body[#body]:find("%S") do table.remove(body) end
  local out = table.concat(body, editor.eol())
  if #definitions > 0 then
    local lines = {}
    for _, definition in ipairs(definitions) do lines[#lines + 1] = definition.line end
    out = out .. editor.eol() .. editor.eol() .. table.concat(lines, editor.eol())
  end
  editor.set_text(out .. editor.eol())
end

menu.separator("/Tools/Markdown")
menu.add("/Tools/Markdown/Structure/Promote Heading", function() change_heading(-1) end, nil, markdown_document)
menu.add("/Tools/Markdown/Structure/Demote Heading", function() change_heading(1) end, nil, markdown_document)
menu.add("/Tools/Markdown/Structure/Change Block Type...", change_block_type, nil, markdown_document)
menu.add("/Tools/Markdown/Structure/Renumber Ordered Lists", renumber_lists, nil, markdown_document)
menu.add("/Tools/Markdown/Structure/Toggle Task State", toggle_tasks, nil, markdown_document)
menu.add("/Tools/Markdown/Structure/Reflow Paragraph...", reflow_paragraph, nil, markdown_document)
menu.separator("/Tools/Markdown")
menu.add("/Tools/Markdown/Links/Copy Heading Anchor", copy_heading_anchor, nil, markdown_document)
menu.add("/Tools/Markdown/Links/Check Internal Links", check_internal_links, nil, markdown_document)
menu.add("/Tools/Markdown/Links/Convert Inline Links to References", inline_to_references, nil, markdown_document)
menu.add("/Tools/Markdown/Links/Convert References to Inline Links", references_to_inline, nil, markdown_document)
menu.add("/Tools/Markdown/Links/Sort and Remove Unused References", tidy_reference_definitions, nil, markdown_document)
