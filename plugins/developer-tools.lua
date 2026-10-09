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
      or ((third == "=" or fourth == "=") and not final_block) then
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

local function html_decode(text)
  return (text:gsub("&#[xX](%x+);", function(value)
      local code = tonumber(value, 16)
      if not code or code > 0x10FFFF or (code >= 0xD800 and code <= 0xDFFF) then
        return "&#x" .. value .. ";"
      end
      return utf8.char(code)
    end)
    :gsub("&#(%d+);", function(value)
      local code = tonumber(value, 10)
      if not code or code > 0x10FFFF or (code >= 0xD800 and code <= 0xDFFF) then
        return "&#" .. value .. ";"
      end
      return utf8.char(code)
    end)
    :gsub("&quot;", '"')
    :gsub("&#39;", "'")
    :gsub("&apos;", "'")
    :gsub("&lt;", "<")
    :gsub("&gt;", ">")
    :gsub("&amp;", "&"))
end

menu.add("Developer Tools/Base64 Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(base64_encode(text)) end
end)

menu.add("Developer Tools/Base64 Decode", function()
  local text = selection_or_warn()
  if not text then return end
  local decoded, message = base64_decode(text)
  if not decoded then
    ui.message(message, nil, "warning")
    return
  end
  editor.replace_selection(decoded)
end)

menu.separator("Developer Tools")

menu.add("Developer Tools/Hex Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(hex_encode(text)) end
end)

menu.add("Developer Tools/Hex Decode", function()
  local text = selection_or_warn()
  if not text then return end
  local decoded, message = hex_decode(text)
  if not decoded then
    ui.message(message, nil, "warning")
    return
  end
  editor.replace_selection(decoded)
end)

menu.separator("Developer Tools")

menu.add("Developer Tools/URL Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(url_encode(text)) end
end)

menu.add("Developer Tools/URL Decode", function()
  local text = selection_or_warn()
  if not text then return end
  local decoded, message = url_decode(text)
  if not decoded then
    ui.message(message, nil, "warning")
    return
  end
  editor.replace_selection(decoded)
end)

menu.separator("Developer Tools")

menu.add("Developer Tools/HTML Entity Encode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(html_encode(text)) end
end)

menu.add("Developer Tools/HTML Entity Decode", function()
  local text = selection_or_warn()
  if text then editor.replace_selection(html_decode(text)) end
end)
