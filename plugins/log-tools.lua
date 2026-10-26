-- Log cleanup and Compact Log Event Format (CLEF) tools. CLEF commands work
-- on newline-delimited JSON events and understand the standard @t, @m, @mt,
-- @l, @x and @r fields, @@-escaped property names, Serilog message templates,
-- and nested JSON objects and arrays in property values. CLEF > Inspect
-- reports in the output panel.

local function source_bounds()
  local first, last = editor.get_selection_range()
  if last < first then first, last = 1, editor.length() end
  local trailing
  if last >= first then
    local ending = editor.get_range(math.max(first, last - 1), last)
    trailing = ending:match("\r\n$") or ending:match("[\r\n]$")
  end
  return first, last, trailing
end

local function source_range()
  local first, last, trailing = source_bounds()
  return editor.get_range(first, last), first, last, trailing
end

local function replace_source(result, first, last, trailing)
  if trailing and result ~= "" and not result:find("[\r\n]$") then result = result .. trailing end
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

-- Build large results from moderately sized strings. Keeping one table entry
-- per event costs more memory than the text itself when logs have short lines.
local OUTPUT_CHUNK_BYTES = 256 * 1024

local function output_builder(separator)
  local chunks, pieces, bytes, count = {}, {}, 0, 0
  local function append(value)
    count = count + 1
    if count > 1 then
      pieces[#pieces + 1], bytes = separator, bytes + #separator
    end
    pieces[#pieces + 1], bytes = value, bytes + #value
    if bytes >= OUTPUT_CHUNK_BYTES then
      chunks[#chunks + 1], pieces, bytes = table.concat(pieces), {}, 0
    end
  end
  local function finish()
    if #pieces > 0 then chunks[#chunks + 1] = table.concat(pieces) end
    return table.concat(chunks), count
  end
  return append, finish
end

local simple_escapes = { ['"']='"', ["\\"]="\\", ["/"]="/", b="\b", f="\f", n="\n", r="\r", t="\t" }

-- Decodes a quoted JSON string, or returns nil when an escape is invalid.
local function json_string(raw)
  local output, position, closing = {}, 2, #raw
  while position < closing do
    local slash = raw:find("\\", position, true)
    if not slash or slash >= closing then
      output[#output + 1] = raw:sub(position, closing - 1)
      break
    end
    output[#output + 1] = raw:sub(position, slash - 1)
    local escaped = raw:sub(slash + 1, slash + 1)
    if escaped == "u" then
      local hex = raw:match("^%x%x%x%x", slash + 2)
      if not hex then return nil end
      local code, after = tonumber(hex, 16), slash + 6
      if code >= 0xD800 and code <= 0xDBFF then
        local low = raw:match("^\\u(%x%x%x%x)", after)
        low = low and tonumber(low, 16)
        if not low or low < 0xDC00 or low > 0xDFFF then return nil end
        code, after = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00), after + 6
      elseif code >= 0xDC00 and code <= 0xDFFF then
        return nil
      end
      output[#output + 1], position = utf8.char(code), after
    elseif simple_escapes[escaped] then
      output[#output + 1], position = simple_escapes[escaped], slash + 2
    else
      return nil
    end
  end
  return table.concat(output)
end

local function quoted_end(text, position)
  position = position + 1
  while true do
    local found = text:find('["\\]', position)
    if not found then return nil end
    if text:sub(found, found) == '"' then return found + 1 end
    position = found + 2
  end
end

local function skip_space(text, position)
  return text:find("[^ \t\r\n]", position) or (#text + 1)
end

-- A small JSON parser. Values become nodes: { kind = "string", value },
-- { kind = "number" or "literal", text }, { kind = "array", items } and
-- { kind = "object", keys, values, map } where map gives a key's index.
local parse_value

local function parse_container(text, position, depth)
  local opening = text:sub(position, position)
  local closing = opening == "{" and "}" or "]"
  local node = opening == "{" and { kind = "object", keys = {}, values = {}, map = {} } or
    { kind = "array", items = {} }
  position = skip_space(text, position + 1)
  if text:sub(position, position) == closing then return node, position + 1 end
  while true do
    local value
    if opening == "{" then
      if text:sub(position, position) ~= '"' then return nil end
      local key_end = quoted_end(text, position)
      local key = key_end and json_string(text:sub(position, key_end - 1))
      if not key then return nil end
      position = skip_space(text, key_end)
      if text:sub(position, position) ~= ":" then return nil end
      value, position = parse_value(text, skip_space(text, position + 1), depth + 1)
      if not value then return nil end
      if node.map[key] then
        node.values[node.map[key]] = value
      else
        local count = #node.keys + 1
        node.keys[count], node.values[count], node.map[key] = key, value, count
      end
    else
      value, position = parse_value(text, position, depth + 1)
      if not value then return nil end
      node.items[#node.items + 1] = value
    end
    position = skip_space(text, position)
    local separator = text:sub(position, position)
    if separator == closing then return node, position + 1 end
    if separator ~= "," then return nil end
    position = skip_space(text, position + 1)
  end
end

-- Deeper nesting is rejected, so a hostile event can't exhaust the stack.
local JSON_MAX_DEPTH = 100

function parse_value(text, position, depth)
  if depth > JSON_MAX_DEPTH then return nil end
  local character = text:sub(position, position)
  if character == "{" or character == "[" then return parse_container(text, position, depth) end
  if character == '"' then
    local finish = quoted_end(text, position)
    local value = finish and json_string(text:sub(position, finish - 1))
    if not value then return nil end
    return { kind = "string", value = value }, finish
  end
  local number = text:match("^-?%d+%.?%d*[eE]?[+-]?%d*", position)
  if number and tonumber(number) then return { kind = "number", text = number }, position + #number end
  local word = text:match("^%a+", position)
  if word == "true" or word == "false" or word == "null" then
    return { kind = "literal", text = word }, position + #word
  end
end

local function parse_json(text)
  local node, finish = parse_value(text, skip_space(text, 1), 0)
  if not node or skip_space(text, finish) <= #text then return nil end
  return node
end

local quote_escapes = { ['"']='\\"', ["\\"]="\\\\", ["\b"]="\\b", ["\f"]="\\f", ["\n"]="\\n",
  ["\r"]="\\r", ["\t"]="\\t" }

local function json_quote(value)
  return '"' .. value:gsub('[%c"\\]', function(character)
    return quote_escapes[character] or string.format("\\u%04x", character:byte())
  end) .. '"'
end

-- Indented JSON for property listings; lines are joined with "\n".
local function json_text(node, depth)
  if node.kind == "string" then return json_quote(node.value) end
  if node.kind ~= "object" and node.kind ~= "array" then return node.text end
  local object = node.kind == "object"
  local count = object and #node.keys or #node.items
  if count == 0 then return object and "{}" or "[]" end
  local indent, parts = string.rep("  ", depth + 1), {}
  for index = 1, count do
    parts[index] = indent .. (object and
      json_quote(node.keys[index]) .. ": " .. json_text(node.values[index], depth + 1) or
      json_text(node.items[index], depth + 1))
  end
  return (object and "{" or "[") .. "\n" .. table.concat(parts, ",\n") .. "\n" ..
    string.rep("  ", depth) .. (object and "}" or "]")
end

-- Serilog-style rendering for messages: a top-level string is shown as-is,
-- nested strings are quoted, and objects use their $type as a type name.
local function display_value(node, nested)
  if node.kind == "string" then return nested and json_quote(node.value) or node.value end
  if node.kind == "array" then
    local parts = {}
    for index, item in ipairs(node.items) do parts[index] = display_value(item, true) end
    return "[" .. table.concat(parts, ", ") .. "]"
  end
  if node.kind == "object" then
    local parts = {}
    for index, key in ipairs(node.keys) do
      if key ~= "$type" then parts[#parts + 1] = key .. ": " .. display_value(node.values[index], true) end
    end
    local body = #parts > 0 and "{ " .. table.concat(parts, ", ") .. " }" or "{}"
    local type_name = node.map["$type"] and node.values[node.map["$type"]]
    if type_name and type_name.kind == "string" then return type_name.value .. " " .. body end
    return body
  end
  return node.text
end

-- An event keeps the reserved @ fields apart from its properties, so a
-- property can never overwrite a reserved field or the event's own data.
-- Property names that start with @ are written as @@ in CLEF.
local function parse_event(line)
  local node = parse_json(line)
  if not node or node.kind ~= "object" then return nil end
  local event = { line = line, fields = {}, properties = {}, order = {} }
  for index, key in ipairs(node.keys) do
    local value = node.values[index]
    if key:sub(1, 2) == "@@" or key:sub(1, 1) ~= "@" then
      local name = key:sub(1, 2) == "@@" and key:sub(2) or key
      if event.properties[name] == nil then event.order[#event.order + 1] = name end
      event.properties[name] = value
    else
      event.fields[key] = display_value(value, false)
      if key == "@r" and value.kind == "array" then event.renderings = value.items end
    end
  end
  return event
end

-- Visit newline-delimited events without copying the complete document into
-- Lua. A CR at the end of a chunk is retained until the next chunk so CRLF is
-- always treated as one line ending.
local INPUT_CHUNK_BYTES = 256 * 1024

local function each_event(visitor)
  local first, last, trailing = source_bounds()
  local position, pending, number, total = first, "", 0, 0
  while position <= last do
    local finish = math.min(last, position + INPUT_CHUNK_BYTES - 1)
    local text = pending .. editor.get_range(position, finish)
    position, pending = finish + 1, ""
    local start = 1
    while true do
      local ending = text:find("[\r\n]", start)
      if not ending then
        pending = text:sub(start)
        break
      end
      if text:sub(ending, ending) == "\r" and ending == #text and position <= last then
        pending = text:sub(start)
        break
      end
      number = number + 1
      local line = text:sub(start, ending - 1)
      if line:match("%S") then
        local event = parse_event(line)
        if not event then
          ui.message("Invalid CLEF JSON event on line " .. number .. ".", nil, "warning")
          return nil
        end
        total = total + 1
        visitor(event, total)
      end
      start = ending + (text:sub(ending, ending + 1) == "\r\n" and 2 or 1)
    end
  end
  number = number + 1
  if pending:match("%S") then
    local event = parse_event(pending)
    if not event then
      ui.message("Invalid CLEF JSON event on line " .. number .. ".", nil, "warning")
      return nil
    end
    total = total + 1
    visitor(event, total)
  end
  return { first = first, last = last, trailing = trailing, count = total }
end

local level_names = { v="VRB", vrb="VRB", verbose="VRB", trace="VRB", d="DBG", dbg="DBG", debug="DBG",
  i="INF", inf="INF", info="INF", information="INF", w="WRN", wrn="WRN", warn="WRN", warning="WRN",
  e="ERR", err="ERR", error="ERR", f="FTL", ftl="FTL", fatal="FTL", critical="FTL" }
local level_order = { "VRB", "DBG", "INF", "WRN", "ERR", "FTL" }
local level_labels = { "Verbose", "Debug", "Information", "Warning", "Error", "Fatal" }
local level_rank = {}
for rank, level in ipairs(level_order) do level_rank[level] = rank end

local function event_level(event)
  local value = (event.fields["@l"] or "Information"):lower()
  return level_names[value] or value:sub(1, 3):upper()
end

local function aligned(value, alignment)
  local width = alignment and tonumber(alignment)
  if not width or utf8.len(value) == nil then return value end
  local padding = math.abs(width) - utf8.len(value)
  if padding <= 0 then return value end
  return width < 0 and value .. string.rep(" ", padding) or string.rep(" ", padding) .. value
end

-- Renders a message template: {Name}, {@Name} and {$Name} take the property
-- value, ",n" pads it, and {{ and }} are literal braces. A ":format" token
-- uses the event's matching @r rendering when there is one.
local function render_template(template, properties, renderings)
  local output, position, formatted = {}, 1, 0
  while position <= #template do
    local brace = template:find("[{}]", position)
    if not brace then
      output[#output + 1] = template:sub(position)
      break
    end
    output[#output + 1] = template:sub(position, brace - 1)
    local character = template:sub(brace, brace)
    local close = character == "{" and template:find("}", brace + 1, true)
    local token = close and template:sub(brace + 1, close - 1)
    local name, alignment, format
    if token then
      name, alignment, format = token:match("^[@$]?([%w_%.]+),(%-?%d+)(.*)$")
      if not name then name, format = token:match("^[@$]?([%w_%.]+)(.*)$") end
    end
    if template:sub(brace + 1, brace + 1) == character then
      output[#output + 1], position = character, brace + 2
    elseif name and (format == "" or format:sub(1, 1) == ":") then
      local value = properties[name] and display_value(properties[name], false)
      if format ~= "" then
        formatted = formatted + 1
        local rendering = renderings and renderings[formatted]
        if value and rendering and rendering.kind == "string" then value = rendering.value end
      end
      output[#output + 1] = value and aligned(value, alignment) or "{" .. token .. "}"
      position = close + 1
    else
      output[#output + 1], position = character, brace + 1
    end
  end
  return table.concat(output)
end

local function render_message(event)
  if event.fields["@m"] then return event.fields["@m"] end
  if not event.fields["@mt"] then return "(event without @m or @mt)" end
  return render_template(event.fields["@mt"], event.properties, event.renderings)
end

-- Objects, arrays and strings that hold JSON are listed as indented JSON.
local function property_lines(lines, name, node)
  local embedded = node.kind == "string" and node.value:match("^%s*[%[{]") and parse_json(node.value)
  local text = json_text(embedded or node, 0)
  local first = true
  for line in (text .. "\n"):gmatch("(.-)\n") do
    lines[#lines + 1] = first and "    " .. name .. " = " .. line or "    " .. line
    first = false
  end
end

local function format_events(with_properties)
  local add, result = output_builder(editor.eol())
  local source = each_event(function(event)
    local timestamp = event.fields["@t"] or "(no timestamp)"
    add(string.format("%s [%s] %s", timestamp, event_level(event), render_message(event)))
    if with_properties then
      local lines = {}
      for _, name in ipairs(event.order) do
        property_lines(lines, name, event.properties[name])
      end
      for _, line in ipairs(lines) do add(line) end
    end
    if event.fields["@x"] then
      for exception_line in (event.fields["@x"] .. "\n"):gmatch("(.-)\r?\n") do
        add("    " .. exception_line)
      end
    end
  end)
  if not source then return end
  replace_source(result(), source.first, source.last, source.trailing)
end

local function days_from_civil(year, month, day)
  if month <= 2 then year = year - 1 end
  local era = (year >= 0 and year or year - 399) // 400
  local year_of_era = year - era * 400
  local day_of_year = (153 * (month + (month > 2 and -3 or 9)) + 2) // 5 + day - 1
  return era * 146097 + year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
end

-- Returns whole UTC seconds plus a nine-digit fraction string, so events can
-- be ordered at full precision across different offsets and fraction lengths.
local function timestamp_key(value)
  if not value then return nil end
  local year, month, day, hour, minute, second, rest =
    value:match("^(%d%d%d%d)-(%d%d)-(%d%d)[Tt ](%d%d):(%d%d):(%d%d)(.*)$")
  if not year then return nil end
  local fraction, zone = rest:match("^[%.,](%d+)(.*)$")
  if not fraction then fraction, zone = "", rest end
  local offset = 0
  if zone ~= "" and zone ~= "Z" and zone ~= "z" then
    local sign, hours, minutes = zone:match("^([+-])(%d%d):?(%d%d)$")
    if not sign then return nil end
    offset = (tonumber(hours) * 3600 + tonumber(minutes) * 60) * (sign == "-" and -1 or 1)
  end
  local seconds = days_from_civil(tonumber(year), tonumber(month), tonumber(day)) * 86400 +
    tonumber(hour) * 3600 + tonumber(minute) * 60 + tonumber(second) - offset
  return { seconds = seconds, fraction = (fraction .. "000000000"):sub(1, 9) }
end

local function earlier(a, b)
  if a.seconds ~= b.seconds then return a.seconds < b.seconds end
  return a.fraction < b.fraction
end

local function inspect_clef()
  local levels, exceptions, templates, template_order = {}, 0, {}, {}
  local first_event, last_event, first_key, last_key
  local source = each_event(function(event)
    local level = event_level(event)
    levels[level] = (levels[level] or 0) + 1
    if event.fields["@x"] then exceptions = exceptions + 1 end
    local template = event.fields["@mt"] or event.fields["@m"]
    if template then
      if not templates[template] then template_order[#template_order + 1] = template end
      templates[template] = (templates[template] or 0) + 1
    end
    local key = timestamp_key(event.fields["@t"])
    if key then
      if not first_key or earlier(key, first_key) then first_key, first_event = key, event end
      if not last_key or earlier(last_key, key) then last_key, last_event = key, event end
    end
  end)
  if not source then return end
  local counts = {}
  for _, level in ipairs(level_order) do
    if levels[level] then counts[#counts + 1] = level .. ": " .. levels[level] end
  end
  for level, count in pairs(levels) do
    if not level_rank[level] then counts[#counts + 1] = level .. ": " .. count end
  end
  local report = {
    string.format("CLEF events: %d", source.count),
    "Levels: " .. (#counts > 0 and table.concat(counts, ", ") or "none"),
    string.format("Exceptions: %d", exceptions),
  }
  if first_event then
    report[#report + 1] = "First: " .. first_event.fields["@t"]
    report[#report + 1] = "Last: " .. last_event.fields["@t"]
  end
  table.sort(template_order, function(a, b)
    if templates[a] ~= templates[b] then return templates[a] > templates[b] end
    return a < b
  end)
  if #template_order > 0 then
    report[#report + 1] = string.format("Distinct messages: %d", #template_order)
    report[#report + 1] = "Most common:"
    for index = 1, math.min(5, #template_order) do
      local template = template_order[index]:gsub("[\r\n]+", " ")
      if utf8.len(template) and utf8.len(template) > 80 then
        template = template:sub(1, utf8.offset(template, 78) - 1) .. "..."
      end
      report[#report + 1] = string.format("  %d x %s", templates[template_order[index]], template)
    end
  end
  output.report("CLEF Inspection", table.concat(report, "\n"))
end

local function keep_events(predicate)
  local add, result = output_builder(editor.eol())
  local kept = 0
  local source = each_event(function(event)
    if predicate(event) then add(event.line); kept = kept + 1 end
  end)
  if not source then return end
  replace_source(result(), source.first, source.last, source.trailing)
  ui.status(string.format("Kept %d of %d events.", kept, source.count))
end

local function filter_clef(minimum)
  keep_events(function(event) return (level_rank[event_level(event)] or 3) >= minimum end)
end

local function choose_level()
  local minimum = ui.choose(level_labels, "Keep events at or above:")
  if minimum then filter_clef(minimum) end
end

local function search_clef()
  local query = ui.prompt("Keep events containing:")
  if not query or query == "" then return end
  query = query:lower()
  keep_events(function(event)
    return render_message(event):lower():find(query, 1, true) ~= nil or
      event.line:lower():find(query, 1, true) ~= nil
  end)
end

local function sort_clef()
  local events = {}
  local source = each_event(function(event, index)
    events[index] = { line = event.line, index = index, key = timestamp_key(event.fields["@t"]) }
  end)
  if not source then return end
  table.sort(events, function(a, b)
    if a.key and b.key and (a.key.seconds ~= b.key.seconds or a.key.fraction ~= b.key.fraction) then
      return earlier(a.key, b.key)
    end
    if (a.key == nil) ~= (b.key == nil) then return a.key ~= nil end
    return a.index < b.index
  end)
  local add, result = output_builder(editor.eol())
  for _, event in ipairs(events) do add(event.line) end
  replace_source(result(), source.first, source.last, source.trailing)
end

local function extract_exceptions()
  local add, result = output_builder(editor.eol() .. editor.eol())
  local source = each_event(function(event)
    if event.fields["@x"] then
      add((event.fields["@t"] or "(no timestamp)") .. " " .. render_message(event) ..
        editor.eol() .. event.fields["@x"]:gsub("\r?\n", editor.eol()))
    end
  end)
  if not source then return end
  replace_source(result(), source.first, source.last, source.trailing)
end

local function remove_ansi()
  local text, first, last, trailing = source_range()
  local result = text:gsub("\27%][^\7\27]*\7", "")
    :gsub("\27%][^\7\27]*\27\\", "")
    :gsub("\27%[[\48-\63]*[\32-\47]*[\64-\126]", "")
  replace_source(result, first, last, trailing)
end

menu.add("/Tools/Logs/CLEF/Format Events", function() format_events(false) end)
menu.add("/Tools/Logs/CLEF/Format Events with Properties", function() format_events(true) end)
menu.add("/Tools/Logs/CLEF/Inspect", inspect_clef)
menu.separator("/Tools/Logs/CLEF")
menu.add("/Tools/Logs/CLEF/Keep Warnings and Above", function() filter_clef(4) end)
menu.add("/Tools/Logs/CLEF/Keep Errors and Fatal", function() filter_clef(5) end)
menu.add("/Tools/Logs/CLEF/Keep Level and Above...", choose_level)
menu.add("/Tools/Logs/CLEF/Keep Events Containing...", search_clef)
menu.separator("/Tools/Logs/CLEF")
menu.add("/Tools/Logs/CLEF/Sort by Timestamp", sort_clef)
menu.add("/Tools/Logs/CLEF/Extract Exceptions", extract_exceptions)
menu.separator("/Tools/Logs")
menu.add("/Tools/Logs/Remove ANSI Colour Codes", remove_ansi)
