-- Edit > Convert commands. Keeping the complete submenu in one plugin makes
-- these text transformations replaceable without carrying parallel native
-- command IDs, dispatch cases, or menu entries.

local ROOT = "/Edit/Convert"

local function transform_selections(transform)
  local selections = editor.get_selections()
  local main = editor.get_main_selection()
  local replacements, changed = {}, false
  for index, selection in ipairs(selections) do
    local text = editor.get_selection(index)
    replacements[index] = transform(text)
    if replacements[index] ~= text then changed = true end
  end
  if not changed then return end
  editor.replace_selections(replacements)
  -- Case conversion doesn't change byte lengths, so the original ranges can
  -- be restored exactly after the bulk replacement.
  editor.set_selection(selections[1].start, selections[1].finish)
  for index = 2, #selections do
    editor.add_selection(selections[index].start, selections[index].finish)
  end
  editor.set_main_selection(math.min(editor.selection_count(), main))
end

-- Only ASCII case is changed. Lua's sandboxed string library and Scintilla's
-- former case commands have the same predictable byte-oriented behaviour.
local function uppercase() transform_selections(string.upper) end
local function lowercase() transform_selections(string.lower) end

local function title_case_text(text)
  return (text:gsub("()(%a)(%a*)", function(position, first, rest)
    -- Up to four bytes before the word: one ASCII letter and a three-byte ’.
    local before = text:sub(math.max(position - 4, 1), position - 1)
    local continues = before:find("%a'$") or before:find("%a\u{2019}$")
    return (continues and first:lower() or first:upper()) .. rest:lower()
  end))
end

local function target_lines()
  local start, finish = editor.get_selection_range()
  if finish < start then return nil end
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

local function transform_lines(mode)
  local first, last = target_lines()
  if not first or last <= first then return end
  local lines = {}
  for line = first, last do lines[#lines + 1] = editor.get_line(line) end

  if mode == "unique" then
    local seen, kept = {}, {}
    for _, line in ipairs(lines) do
      if not seen[line] then seen[line], kept[#kept + 1] = true, line end
    end
    lines = kept
  elseif mode == "descending" then
    table.sort(lines, function(a, b) return a > b end)
  else
    table.sort(lines)
  end
  replace_lines(first, last, lines)
end

-- Rewrites leading indentation throughout the document. A tab advances to
-- the next configured tab stop; whitespace inside a line is never touched.
local function convert_indentation(to_spaces)
  local width = editor.tab_width()
  if width < 1 then width = 4 end
  for line = editor.line_count(), 1, -1 do
    local text = editor.get_line(line)
    local indent = text:match("^[ \t]+")
    if indent then
      local column = 0
      for index = 1, #indent do
        if indent:byte(index) == 9 then column = column + width - column % width
        else column = column + 1 end
      end
      local replacement
      if to_spaces then
        replacement = string.rep(" ", column)
      else
        replacement = string.rep("\t", column // width) .. string.rep(" ", column % width)
      end
      if replacement ~= indent then
        local start = editor.line_range(line)
        editor.replace_range(start, start + #indent - 1, replacement)
      end
    end
  end
end

menu.add(ROOT .. "/&UPPERCASE", uppercase)
menu.add(ROOT .. "/&lowercase", lowercase)
menu.add(ROOT .. "/&Title Case", function() transform_selections(title_case_text) end)
menu.separator(ROOT)
menu.add(ROOT .. "/Sort Lines &Ascending", function() transform_lines("ascending") end)
menu.add(ROOT .. "/Sort Lines Descend&ing", function() transform_lines("descending") end)
menu.add(ROOT .. "/Remove &Duplicate Lines", function() transform_lines("unique") end)
menu.separator(ROOT)
menu.add(ROOT .. "/Tabs to &Spaces", function() convert_indentation(true) end)
menu.add(ROOT .. "/Spaces to Ta&bs", function() convert_indentation(false) end)
menu.separator(ROOT)
menu.add(ROOT .. "/Line &Endings/&CRLF (Windows)", function() editor.eol("\r\n") end)
menu.add(ROOT .. "/Line &Endings/&LF (Unix/macOS)", function() editor.eol("\n") end)
menu.add(ROOT .. "/Line &Endings/C&R (Classic Mac)", function() editor.eol("\r") end)
menu.add(ROOT .. "/En&coding/UTF-&8", function() documents.encoding("utf-8") end)
menu.add(ROOT .. "/En&coding/UTF-8 with &BOM", function() documents.encoding("utf-8-bom") end)
menu.add(ROOT .. "/En&coding/UTF-16 &LE", function() documents.encoding("utf-16le") end)
menu.add(ROOT .. "/En&coding/UTF-16 B&E", function() documents.encoding("utf-16be") end)
menu.add(ROOT .. "/En&coding/&ANSI (system code page)", function() documents.encoding("ansi") end)
