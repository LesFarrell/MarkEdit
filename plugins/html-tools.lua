-- HTML snippets under Snippets > HTML. These are ordinary bundled-plugin
-- commands: they need only the public editor and ui APIs, and can be disabled
-- independently from Tinker Editor's built-in Markdown formatting.

local ROOT = "/Snippets/HTML"

local function selection()
  return editor.get_selection_range()
end

local function is_empty(start, finish)
  return finish < start
end

local function line_start(line)
  return (editor.line_range(line))
end

local function selected_lines()
  local start, finish = selection()
  local first, last = editor.line_at(start), editor.line_at(finish + 1)
  if last > first and finish + 1 == line_start(last) then last = last - 1 end
  return first, last
end

local function replace_lines(first, last, text)
  local _, finish = editor.line_range(last)
  editor.set_selection(line_start(first), finish)
  editor.replace_selection(text)
end

local function needs_leading_eol(position)
  if position <= 1 then return false end
  local previous = editor.get_range(position - 1, position - 1)
  return previous ~= "\r" and previous ~= "\n"
end

local function is_wrapped(selected, open, close)
  local length = #selected
  if length < #open + #close or selected:sub(1, #open) ~= open
    or selected:sub(length - #close + 1) ~= close then
    return false
  end
  local inner = selected:sub(#open + 1, length - #close)
  return not inner:find(open, 1, true) and not inner:find(close, 1, true)
end

local function wrapped_text(selected, open, close)
  if is_wrapped(selected, open, close) then
    local inner = selected:sub(#open + 1, #selected - #close)
    return inner, 1, #inner
  end
  return open .. selected .. close, #open + 1, #open + #selected
end

local function wrap_pair(open, close)
  if editor.selection_count() > 1 then
    local texts, order = {}, {}
    for index, range in ipairs(editor.get_selections()) do
      local text, innerStart, innerFinish =
        wrapped_text(editor.get_range(range.start, range.finish), open, close)
      texts[index] = text
      order[index] = { start = range.start, length = range.finish - range.start + 1,
                       text = text, innerStart = innerStart, innerFinish = innerFinish }
    end
    editor.replace_selections(texts)
    table.sort(order, function(a, b) return a.start < b.start end)
    if editor.selection_count() ~= #order then return end
    local shift = 0
    for index, item in ipairs(order) do
      local base = item.start + shift - 1
      editor.set_selection(base + item.innerStart, base + item.innerFinish, index)
      shift = shift + #item.text - item.length
    end
    return
  end

  local start, finish = selection()
  if is_empty(start, finish) then
    editor.replace_selection(open .. close)
    editor.set_caret(start + #open)
    return
  end
  local text, innerStart, innerFinish = wrapped_text(editor.get_selection(), open, close)
  editor.replace_selection(text)
  editor.set_selection(start + innerStart - 1, start + innerFinish - 1)
end

local function wrap_tag(tag)
  wrap_pair("<" .. tag .. ">", "</" .. tag .. ">")
end

local function insert_with_url(before, placeholder, after)
  local start, finish = selection()
  local middle = is_empty(start, finish) and placeholder or editor.get_selection()
  editor.replace_selection(before .. middle .. after)
  local inBefore = before:find("url", 1, true)
  local urlStart = inBefore and (start + inBefore - 1)
    or (start + #before + #middle + after:find("url", 1, true) - 1)
  editor.set_selection(urlStart, urlStart + 2)
end

local function insert_block(text)
  local start = selection()
  local leading = needs_leading_eol(start) and editor.eol() or ""
  editor.replace_selection(leading .. text)
end

local function ask_table_size()
  local rowsText = ui.prompt("Data rows (excluding the header):", "1", "Insert HTML Table")
  if not rowsText then return nil end
  local columnsText = ui.prompt("Columns:", "3", "Insert HTML Table")
  if not columnsText then return nil end
  local rows = tonumber(rowsText:match("^%s*([+-]?%d+)")) or 0
  local columns = tonumber(columnsText:match("^%s*([+-]?%d+)")) or 0
  if rows < 1 or rows > 50 or columns < 1 or columns > 20 then
    ui.message("Use 1–50 data rows and 1–20 columns.", "Insert HTML Table", "warning")
    return nil
  end
  return rows, columns
end

local function insert_table()
  local rows, columns = ask_table_size()
  if not rows then return end
  local lines = { "<table>" }
  local function add_row(tag, text)
    lines[#lines + 1] = "  <tr>"
    for column = 1, columns do
      lines[#lines + 1] = "    <" .. tag .. ">" .. (text or ("Column " .. column)) .. "</" .. tag .. ">"
    end
    lines[#lines + 1] = "  </tr>"
  end
  add_row("th")
  for _ = 1, rows do add_row("td", "Cell") end
  lines[#lines + 1] = "</table>"
  lines[#lines + 1] = ""
  insert_block(table.concat(lines, editor.eol()))
end

local function insert_list(ordered)
  local eol = editor.eol()
  local tag = ordered and "ol" or "ul"
  local start, finish = selection()
  if is_empty(start, finish) then
    local placeholder = "List item"
    local before = "<" .. tag .. ">" .. eol .. "  <li>"
    editor.replace_selection(before .. placeholder .. "</li>" .. eol .. "</" .. tag .. ">")
    editor.set_selection(start + #before, start + #before + #placeholder - 1)
    return
  end
  local first, last = selected_lines()
  local out = { "<" .. tag .. ">" .. eol }
  for line = first, last do out[#out + 1] = "  <li>" .. editor.get_line(line) .. "</li>" .. eol end
  out[#out + 1] = "</" .. tag .. ">"
  replace_lines(first, last, table.concat(out))
end

local function insert_document()
  local eol = editor.eol()
  insert_block(table.concat({
    "<!doctype html>", '<html lang="en">', "<head>", '  <meta charset="utf-8">',
    '  <meta name="viewport" content="width=device-width, initial-scale=1">',
    "  <title>Document</title>", "</head>", "<body>", "  ", "</body>", "</html>", ""
  }, eol))
end

local function insert_details()
  local start, finish = selection()
  local body = is_empty(start, finish) and "Details" or editor.get_selection()
  local eol = editor.eol()
  local before = "<details>" .. eol .. "  <summary>Summary</summary>" .. eol .. "  "
  editor.replace_selection(before .. body .. eol .. "</details>")
  if is_empty(start, finish) then editor.set_selection(start + #before, start + #before + #body - 1) end
end

local function insert_figure()
  local source = ui.prompt("Image URL:", "image.png", "Insert Figure")
  if not source then return end
  local caption = ui.prompt("Caption:", "Caption", "Insert Figure")
  if not caption then return end
  local alt = editor.get_selection()
  if alt == "" then alt = caption end
  local eol = editor.eol()
  insert_block('<figure>' .. eol .. '  <img src="' .. source .. '" alt="' .. alt .. '">' ..
    eol .. "  <figcaption>" .. caption .. "</figcaption>" .. eol .. "</figure>" .. eol)
end

local function wrap_chosen_tag()
  local tag = ui.prompt("HTML tag:", "section", "Wrap with HTML Tag")
  if not tag then return end
  tag = tag:match("^%s*(.-)%s*$")
  if not tag:match("^[A-Za-z][A-Za-z0-9:_-]*$") then
    ui.message("Use a plain HTML tag name without brackets or attributes.",
      "Wrap with HTML Tag", "warning")
    return
  end
  wrap_tag(tag:lower())
end

local function insert_description_list()
  local start, finish = selection()
  local lines = {}
  if is_empty(start, finish) then
    lines = { "Term", "Description" }
  else
    local first, last = selected_lines()
    for line = first, last do lines[#lines + 1] = editor.get_line(line) end
  end
  local eol, out = editor.eol(), { "<dl>" }
  for index = 1, #lines, 2 do
    out[#out + 1] = "  <dt>" .. lines[index] .. "</dt>"
    out[#out + 1] = "  <dd>" .. (lines[index + 1] or "Description") .. "</dd>"
  end
  out[#out + 1] = "</dl>"
  local text = table.concat(out, eol)
  if is_empty(start, finish) then
    editor.replace_selection(text)
    local term = text:find("Term", 1, true)
    editor.set_selection(start + term - 1, start + term + 2)
  else
    local first, last = selected_lines()
    replace_lines(first, last, text)
  end
end

local function markdown_links_to_html()
  local selected = editor.get_selection()
  local converted, count = selected:gsub("(!?)%[([^%]]-)%]%(([^%)%s]+)%)",
    function(image, label, url)
      if image == "!" then return '<img src="' .. url .. '" alt="' .. label .. '">' end
      return '<a href="' .. url .. '">' .. label .. '</a>'
    end)
  if count == 0 then return ui.status("The selection has no inline Markdown links.") end
  editor.replace_selection(converted)
end

local function has_selection()
  return editor.get_selection() ~= ""
end

menu.add(ROOT .. "/&HTML", function() wrap_tag("html") end)
menu.add(ROOT .. "/H&ead", function() wrap_tag("head") end)
menu.add(ROOT .. "/&Body", function() wrap_tag("body") end)
menu.separator(ROOT)
menu.add(ROOT .. "/&Paragraph", function() wrap_tag("p") end)
menu.add(ROOT .. "/B&old", function() wrap_tag("b") end)
menu.add(ROOT .. "/&Italic", function() wrap_tag("i") end)
menu.add(ROOT .. "/&Underline", function() wrap_tag("u") end)
menu.add(ROOT .. "/&Div", function() wrap_tag("div") end)
menu.add(ROOT .. "/&Span", function() wrap_tag("span") end)
menu.separator(ROOT)
menu.add(ROOT .. "/&Link", function() insert_with_url('<a href="url">', "link text", "</a>") end)
menu.add(ROOT .. "/I&mage", function() insert_with_url('<img src="url" alt="', "alt text", '">') end)
menu.add(ROOT .. "/&Table", insert_table)
menu.add(ROOT .. "/B&ullet List", function() insert_list(false) end)
menu.add(ROOT .. "/&Numbered List", function() insert_list(true) end)
menu.separator(ROOT)
menu.add(ROOT .. "/Line &Break", function() editor.replace_selection("<br>") end)
menu.separator(ROOT)
menu.add(ROOT .. "/HTML5 Document Boilerplate", insert_document)
menu.add(ROOT .. "/Details and Summary", insert_details)
menu.add(ROOT .. "/Figure...", insert_figure)
menu.add(ROOT .. "/Description List", insert_description_list)
menu.add(ROOT .. "/Wrap with Tag...", wrap_chosen_tag)
menu.add(ROOT .. "/Convert Selected Markdown Links", markdown_links_to_html, nil, has_selection)
for _, item in ipairs({
  { "Section", "section" }, { "Article", "article" }, { "Navigation", "nav" },
  { "Aside", "aside" }, { "Header", "header" }, { "Footer", "footer" },
  { "Main", "main" }, { "Code", "code" }, { "Preformatted", "pre" },
}) do
  local label, tag = item[1], item[2]
  menu.add(ROOT .. "/Semantic/" .. label, function() wrap_tag(tag) end)
end
