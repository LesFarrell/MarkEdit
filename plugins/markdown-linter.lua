-- Offline Markdown diagnostics for the Problems panel.

local SOURCE = "Markdown Linter"
local LIVE_LIMIT = 1024 * 1024

local function markdown_document()
  return editor.language() == "markdown"
end

local function anchor_for(text)
  text = text:gsub("!?%[(.-)%]%b()", "%1"):gsub("[*`~]", "")
  return (text:lower():gsub("\226\128[\147\148\152\153\156\157\166]", "")
    :gsub("[^%w%s%-_\128-\255]", ""):gsub("%s", "-"))
end

local function location(line, column)
  local path = editor.file_path()
  return path and string.format("%s:%d:%d", path, line, column or 1)
    or string.format("Line %d, column %d", line, column or 1)
end

local function lint()
  if not markdown_document() then
    problems.clear(SOURCE)
    return 0
  end

  local issues, headings, links = {}, {}, {}
  local seen_heading, last_level = {}, 0
  local fence, front_matter = nil, false
  local list_marker, list_indent = nil, nil
  for line = 1, editor.line_count() do
    local text = editor.get_line(line)
    if line == 1 and text == "---" then front_matter = true
    elseif front_matter then
      if text == "---" or text == "..." then front_matter = false end
    else
      local marker = text:match("^ ? ? ?(```+)") or text:match("^ ? ? ?(~~~+)")
      if fence then
        if marker and marker:sub(1, 1) == fence:sub(1, 1) and #marker >= #fence and
            not text:find("[^`~%s]") then fence = nil end
      elseif marker then
        fence = marker
      else
        local whitespace = text:match("[ \t]+$")
        if whitespace then
          issues[#issues + 1] = location(line, #text - #whitespace + 1) .. ": Trailing whitespace."
        end

        local hashes, heading = text:match("^ ? ? ?(#+)%s+(.-)%s*#*%s*$")
        if hashes and #hashes <= 6 and heading ~= "" then
          local level, anchor = #hashes, anchor_for(heading)
          if last_level > 0 and level > last_level + 1 then
            issues[#issues + 1] = location(line) .. string.format(
              ": Heading level jumps from %d to %d.", last_level, level)
          end
          last_level = level
          local occurrence = seen_heading[anchor] or 0
          if occurrence > 0 then
            issues[#issues + 1] = location(line) .. ": Duplicate heading anchor #" .. anchor .. "."
          end
          seen_heading[anchor] = occurrence + 1
          headings[occurrence == 0 and anchor or (anchor .. "-" .. occurrence)] = true
        end

        if text:match("^[ \t]*[-*+] %[[^ xX]%]") then
          issues[#issues + 1] = location(line) .. ": Task marker must be [ ], [x], or [X]."
        end

        local indent, bullet = text:match("^([ \t]*)([-*+])%s+")
        if bullet then
          if list_indent == indent and list_marker and list_marker ~= bullet then
            issues[#issues + 1] = location(line) .. ": Inconsistent bullet marker in this list."
          end
          list_indent, list_marker = indent, bullet
        elseif text:find("%S") and not text:match("^[ \t]+") then
          list_indent, list_marker = nil, nil
        end

        for anchor in text:gmatch("%b[]%(%s*#([^%)%s]+)%s*%)") do
          links[#links + 1] = { line = line, anchor = anchor }
        end
      end
    end
  end

  for _, link in ipairs(links) do
    if not headings[link.anchor] then
      issues[#issues + 1] = location(link.line) .. ": Internal link does not resolve: #" .. link.anchor .. "."
    end
  end

  if #issues == 0 then problems.clear(SOURCE)
  else problems.report(SOURCE, table.concat(issues, "\n")) end
  return #issues
end

local function check_now()
  local count = lint()
  ui.status(count == 0 and "No Markdown lint problems found." or
    (count .. " Markdown lint problem" .. (count == 1 and "" or "s") .. " found."))
end

local function live_check()
  if config.get("live", false) and editor.length() <= LIVE_LIMIT then lint()
  elseif not markdown_document() then problems.clear(SOURCE) end
end

menu.add("/Tools/Markdown/Lint/Check Document", check_now, nil, markdown_document)
menu.add("/Tools/Markdown/Lint/Clear Problems", function() problems.clear(SOURCE) end)
menu.checkbox("/Tools/Markdown/Lint/Check as You Type", "live", false, function(checked)
  if checked then live_check() else problems.clear(SOURCE) end
end)

events.on("text_changed", live_check)
events.on("open", live_check)
events.on("tab_changed", live_check)
