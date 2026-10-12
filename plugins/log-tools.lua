-- Log cleanup and Compact Log Event Format (CLEF) tools. CLEF commands work
-- on newline-delimited JSON events and understand the standard @t, @m, @mt,
-- @l and @x fields plus scalar custom properties.

local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

local function replace_source(result, first, last)
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

local function json_string(raw)
  if not raw or raw:sub(1, 1) ~= '"' then return raw end
  local output, position = {}, 2
  while position < #raw do
    local character = raw:sub(position, position)
    if character ~= "\\" then output[#output + 1], position = character, position + 1
    else
      local escaped = raw:sub(position + 1, position + 1)
      local simple = { ['"']='"', ["\\"]="\\", ["/"]="/", b="\b", f="\f", n="\n", r="\r", t="\t" }
      if escaped == "u" then
        local code = tonumber(raw:sub(position + 2, position + 5), 16)
        if not code or (code >= 0xD800 and code <= 0xDFFF) then return raw end
        output[#output + 1], position = utf8.char(code), position + 6
      elseif simple[escaped] then output[#output + 1], position = simple[escaped], position + 2
      else return raw end
    end
  end
  return table.concat(output)
end

local function quoted_end(text, position)
  position = position + 1
  while position <= #text do
    local character = text:sub(position, position)
    if character == "\\" then position = position + 2
    elseif character == '"' then return position + 1
    else position = position + 1 end
  end
end

local function value_end(text, position)
  position = text:find("[^ \t]", position) or (#text + 1)
  if text:sub(position, position) == '"' then return quoted_end(text, position) end
  local opening = text:sub(position, position)
  if opening == "{" or opening == "[" then
    local stack, quoted = { opening == "{" and "}" or "]" }, false
    local cursor = position + 1
    while cursor <= #text do
      local character = text:sub(cursor, cursor)
      if quoted then
        if character == "\\" then cursor = cursor + 1
        elseif character == '"' then quoted = false end
      elseif character == '"' then quoted = true
      elseif character == "{" then stack[#stack + 1] = "}"
      elseif character == "[" then stack[#stack + 1] = "]"
      elseif character == "}" or character == "]" then
        if stack[#stack] ~= character then return nil end
        stack[#stack] = nil
        if #stack == 0 then return cursor + 1 end
      end
      cursor = cursor + 1
    end
    return nil
  end
  return text:find("[,}]", position) or (#text + 1)
end

local function parse_event(line)
  local position = line:find("[^ \t]") or 1
  if line:sub(position, position) ~= "{" then return nil end
  position = position + 1
  local event = { raw = line }
  while true do
    position = line:find("[^ \t]", position) or (#line + 1)
    if line:sub(position, position) == "}" then
      return line:sub(position + 1):match("^%s*$") and event or nil
    end
    if line:sub(position, position) ~= '"' then return nil end
    local key_end = quoted_end(line, position)
    if not key_end then return nil end
    local key = json_string(line:sub(position, key_end - 1))
    position = line:find("[^ \t]", key_end) or (#line + 1)
    if line:sub(position, position) ~= ":" then return nil end
    local start = line:find("[^ \t]", position + 1) or (#line + 1)
    local finish = value_end(line, start)
    if not finish then return nil end
    local raw = line:sub(start, finish - 1):match("^(.-)%s*$")
    event[key] = raw:sub(1, 1) == '"' and json_string(raw) or raw
    position = line:find("[^ \t]", finish) or (#line + 1)
    local separator = line:sub(position, position)
    if separator == "}" then
      return line:sub(position + 1):match("^%s*$") and event or nil
    end
    if separator ~= "," then return nil end
    position = position + 1
  end
end

local function clef_events(text)
  text = text:gsub("\r\n", "\n"):gsub("\r", "\n")
  local events = {}
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:match("%S") then
      local event = parse_event(line)
      if not event then return nil, "Invalid CLEF JSON event on line " .. (#events + 1) .. "." end
      events[#events + 1] = event
    end
  end
  return events
end

local level_names = { v="VRB", vrb="VRB", verbose="VRB", d="DBG", dbg="DBG", debug="DBG",
  i="INF", inf="INF", information="INF", w="WRN", wrn="WRN", warn="WRN", warning="WRN",
  e="ERR", err="ERR", error="ERR", f="FTL", ftl="FTL", fatal="FTL" }
local level_rank = { VRB=1, DBG=2, INF=3, WRN=4, ERR=5, FTL=6 }

local function event_level(event)
  local value = (event["@l"] or "Information"):lower()
  return level_names[value] or value:sub(1, 3):upper()
end

local function display_value(value)
  if not value then return "" end
  if value:sub(1, 1) == '"' then return json_string(value) end
  return value
end

local function render_message(event)
  if event["@m"] then return event["@m"] end
  local template = event["@mt"] or "(event without @m or @mt)"
  return (template:gsub("{(@?[%w_%.]+)[^}]*}", function(name)
    return display_value(event[name:gsub("^@", "")]) ~= "" and
      display_value(event[name:gsub("^@", "")]) or "{" .. name .. "}"
  end))
end

local function format_clef()
  local text, first, last = source_range()
  local events, problem = clef_events(text)
  if not events then ui.message(problem, nil, "warning"); return end
  local lines = {}
  for _, event in ipairs(events) do
    local timestamp = event["@t"] or "(no timestamp)"
    lines[#lines + 1] = string.format("%s [%s] %s", timestamp, event_level(event), render_message(event))
    if event["@x"] then
      for exception_line in (event["@x"] .. "\n"):gmatch("(.-)\n") do
        lines[#lines + 1] = "    " .. exception_line
      end
    end
  end
  replace_source(table.concat(lines, editor.eol()), first, last)
end

local function inspect_clef()
  local text = source_range()
  local events, problem = clef_events(text)
  if not events then ui.message(problem, nil, "warning"); return end
  local levels, exceptions = {}, 0
  for _, event in ipairs(events) do
    local level = event_level(event)
    levels[level] = (levels[level] or 0) + 1
    if event["@x"] then exceptions = exceptions + 1 end
  end
  local counts = {}
  for _, level in ipairs({"VRB","DBG","INF","WRN","ERR","FTL"}) do
    if levels[level] then counts[#counts + 1] = level .. ": " .. levels[level] end
  end
  ui.message(string.format("CLEF events: %d\nLevels: %s\nExceptions: %d", #events,
    #counts > 0 and table.concat(counts, ", ") or "none", exceptions), "Log Tools")
end

local function filter_clef(minimum)
  local text, first, last = source_range()
  local events, problem = clef_events(text)
  if not events then ui.message(problem, nil, "warning"); return end
  local lines = {}
  for _, event in ipairs(events) do
    if (level_rank[event_level(event)] or 3) >= minimum then lines[#lines + 1] = event.raw end
  end
  replace_source(table.concat(lines, editor.eol()), first, last)
end

local function extract_exceptions()
  local text, first, last = source_range()
  local events, problem = clef_events(text)
  if not events then ui.message(problem, nil, "warning"); return end
  local lines = {}
  for _, event in ipairs(events) do
    if event["@x"] then lines[#lines + 1] = event["@x"] end
  end
  replace_source(table.concat(lines, editor.eol() .. editor.eol()), first, last)
end

local function remove_ansi()
  local text, first, last = source_range()
  local result = text:gsub("\27%[[%d;?]*[%a]", "")
  replace_source(result, first, last)
end

menu.add("/Tools/Logs/CLEF/Format Events", format_clef)
menu.add("/Tools/Logs/CLEF/Inspect", inspect_clef)
menu.separator("/Tools/Logs/CLEF")
menu.add("/Tools/Logs/CLEF/Keep Warnings and Above", function() filter_clef(4) end)
menu.add("/Tools/Logs/CLEF/Keep Errors and Fatal", function() filter_clef(5) end)
menu.add("/Tools/Logs/CLEF/Extract Exceptions", extract_exceptions)
menu.separator("/Tools/Logs")
menu.add("/Tools/Logs/Remove ANSI Colour Codes", remove_ansi)
