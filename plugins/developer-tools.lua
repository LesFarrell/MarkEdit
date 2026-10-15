-- NOTE: every command here duplicates one in encoding.lua (Plugins >
-- Encode), which also handles whole documents, URL-safe Base64 and more
-- HTML entities, and now rejects non-canonical Base64 padding too. Removing
-- them would leave this plugin empty, so whether to keep it is pending.
--
-- Small, dependency-free transforms for text commonly handled by developers.
-- Commands intentionally require a selection so they cannot rewrite a whole
-- document by accident.

local function selection_or_warn()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select some text first.", nil, "warning")
    return nil
  end
  return text
end

local base64_alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64_encode(text)
  local output = {}
  for position = 1, #text, 3 do
    local a = text:byte(position) or 0
    local b = text:byte(position + 1) or 0
    local c = text:byte(position + 2) or 0
    local value = a * 65536 + b * 256 + c
    local remaining = #text - position + 1

    output[#output + 1] = base64_alphabet:sub(value // 262144 + 1, value // 262144 + 1)
    output[#output + 1] = base64_alphabet:sub(value // 4096 % 64 + 1, value // 4096 % 64 + 1)
    output[#output + 1] = remaining >= 2
      and base64_alphabet:sub(value // 64 % 64 + 1, value // 64 % 64 + 1) or "="
    output[#output + 1] = remaining >= 3
      and base64_alphabet:sub(value % 64 + 1, value % 64 + 1) or "="
  end
  return table.concat(output)
end

local base64_value = {}
for index = 1, #base64_alphabet do
  base64_value[base64_alphabet:sub(index, index)] = index - 1
end

local function base64_decode(text)
  text = text:gsub("%s", "")
  if #text == 0 or #text % 4 ~= 0 or text:find("[^%w%+/%=]") then
    return nil, "The selection is not valid Base64."
  end

  local output = {}
  for position = 1, #text, 4 do
    local block = text:sub(position, position + 3)
    local a, b = base64_value[block:sub(1, 1)], base64_value[block:sub(2, 2)]
    local third, fourth = block:sub(3, 3), block:sub(4, 4)
    local c, d = base64_value[third], base64_value[fourth]
    local final_block = position + 3 == #text

    if not a or not b or (third ~= "=" and not c) or (fourth ~= "=" and not d)
      or (third == "=" and fourth ~= "=")
      or ((third == "=" or fourth == "=") and not final_block)
      -- Padding discards low-order bits. Requiring those bits to be zero
      -- rejects non-canonical encodings instead of silently changing them.
      or (third == "=" and b % 16 ~= 0)
      or (fourth == "=" and third ~= "=" and c % 4 ~= 0) then
      return nil, "The selection is not valid Base64."
    end

    local value = a * 262144 + b * 4096 + (c or 0) * 64 + (d or 0)
    output[#output + 1] = string.char(value // 65536 % 256)
    if third ~= "=" then output[#output + 1] = string.char(value // 256 % 256) end
    if fourth ~= "=" then output[#output + 1] = string.char(value % 256) end
  end
  return table.concat(output)
end

local function url_encode(text)
  return (text:gsub("([^A-Za-z0-9%-%._~])", function(character)
    return string.format("%%%02X", character:byte())
  end))
end

local function url_decode(text)
  local position = 1
  while true do
    local percent = text:find("%", position, true)
    if not percent then break end
    if not text:sub(percent + 1, percent + 2):match("^%x%x$") then
      return nil, "The selection contains an invalid URL escape."
    end
    position = percent + 3
  end
  return (text:gsub("%%(%x%x)", function(hex)
    return string.char(tonumber(hex, 16))
  end))
end

local function hex_encode(text)
  return (text:gsub(".", function(byte)
    return string.format("%02X", byte:byte())
  end))
end

local function hex_decode(text)
  local compact = text:gsub("%s", "")
  if compact == "" or #compact % 2 ~= 0 or compact:find("[^%x]") then
    return nil, "The selection is not valid hexadecimal text."
  end
  return (compact:gsub("%x%x", function(pair)
    return string.char(tonumber(pair, 16))
  end))
end

local function html_encode(text)
  return (text:gsub("&", "&amp;")
    :gsub("<", "&lt;")
    :gsub(">", "&gt;")
    :gsub('"', "&quot;")
    :gsub("'", "&#39;"))
end

local HTML_ENTITIES = { quot = '"', apos = "'", lt = "<", gt = ">", amp = "&" }

-- A single pass, so a decoded "&" never starts another entity: "&#38;lt;"
-- becomes "&lt;", not "<". Unknown or invalid entities are left as they are.
local function html_decode(text)
  return (text:gsub("&(#?%w+);", function(name)
    local code
    if name:match("^#[xX]%x+$") then code = tonumber(name:sub(3), 16)
    elseif name:match("^#%d+$") then code = tonumber(name:sub(2), 10)
    else return HTML_ENTITIES[name] end
    if not code or code > 0x10FFFF or (code >= 0xD800 and code <= 0xDFFF) then return nil end
    return utf8.char(code)
  end))
end

menu.add("Developer Tools/Base64 Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(base64_encode(text)) end
end)

-- Replaces the selection with what `decode` makes of it. Decoded bytes
-- become document text only if they are UTF-8, as in encoding.lua.
local function replace_with_decoded(decode, what)
  local text = selection_or_warn()
  if not text then return end
  local decoded, message = decode(text)
  if decoded and not utf8.len(decoded) then
    decoded, message = nil, what .. " decodes to binary data, not UTF-8 text, so it wasn't inserted."
  end
  if not decoded then
    ui.message(message, nil, "warning")
    return
  end
  editor.replace_selection(decoded)
end

menu.add("Developer Tools/Base64 Decode", function() replace_with_decoded(base64_decode, "This Base64") end)

menu.separator("Developer Tools")

menu.add("Developer Tools/Hex Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(hex_encode(text)) end
end)

menu.add("Developer Tools/Hex Decode", function() replace_with_decoded(hex_decode, "This hex") end)

menu.separator("Developer Tools")

menu.add("Developer Tools/URL Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(url_encode(text)) end
end)

menu.add("Developer Tools/URL Decode", function() replace_with_decoded(url_decode, "This URL text") end)

menu.separator("Developer Tools")

menu.add("Developer Tools/HTML Entity Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(html_encode(text)) end
end)

menu.add("Developer Tools/HTML Entity Decode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(html_decode(text)) end
end)
