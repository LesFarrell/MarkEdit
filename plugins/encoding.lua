-- Encoding and checksum commands, on the Plugins menu:
--   Encode      Base64, hexadecimal, URL (percent) and HTML entity encoding
--               and decoding
--   Checksum    CRC-32, CRC-16 (several variants) or NMEA/AIS XOR checksum
--               of the selected text, in a box to copy from
--
-- The Encode commands work on the selection, or on the whole document when
-- nothing is selected. Decoders refuse input that isn't valid, or whose
-- result isn't UTF-8 text, and leave the document unchanged.
--
-- Uses only the plugin API documented in README.md in this folder.

-- The selection as start, finish, or the whole document when nothing is
-- selected.
local function target_range()
  local start, finish = editor.get_selection_range()
  if finish < start then return 1, editor.length() end
  return start, finish
end

-- Replaces the target text with convert(text). convert returns the new text,
-- or nil and a message saying why it can't, which is shown instead. The
-- result is left selected, so commands can be chained.
local function convert_target(convert)
  local start, finish = target_range()
  local result, problem = convert(editor.get_range(start, finish))
  if not result then
    ui.message(problem, nil, "warning")
    return
  end
  editor.replace_range(start, finish, result)
  editor.set_selection(start, start + #result - 1)
end

-- Decoded bytes become document text only if they are UTF-8.
local function as_text(bytes, what)
  if not utf8.len(bytes) then
    return nil, what .. " decodes to binary data, not UTF-8 text, so it wasn't inserted."
  end
  return bytes
end

-- ---- Base64 ----

local BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

-- Value of each Base64 character, accepting the URL-safe - and _ as well.
local BASE64_VALUE = { ["-"] = 62, ["_"] = 63 }
for i = 1, #BASE64 do BASE64_VALUE[BASE64:sub(i, i)] = i - 1 end

local function base64_encode(bytes)
  local out = {}
  for i = 1, #bytes, 3 do
    local a, b, c = bytes:byte(i, i + 2)
    local n = (a << 16) | ((b or 0) << 8) | (c or 0)
    local quad = {}
    for k = 1, 4 do
      local index = (n >> (6 * (4 - k))) & 63
      quad[k] = BASE64:sub(index + 1, index + 1)
    end
    if not b then quad[3] = "=" end
    if not c then quad[4] = "=" end
    out[#out + 1] = table.concat(quad)
  end
  return table.concat(out)
end

-- Returns the decoded bytes, or nil and a message. Whitespace (such as line
-- breaks in wrapped Base64) is ignored, and the = padding is optional.
-- A short final group carries a few bits beyond its last byte; requiring
-- them to be zero rejects non-canonical text (such as "AB==") instead of
-- silently decoding it to something that encodes differently.
local function base64_decode(text)
  local clean = text:gsub("%s", ""):gsub("=+$", "")
  if clean:find("=") then return nil, "Padding (=) can only come at the end of Base64 text." end
  if #clean % 4 == 1 then return nil, "This isn't complete Base64: it has one character too many or too few." end
  local out = {}
  for i = 1, #clean, 4 do
    local n, count = 0, 0
    for k = i, math.min(i + 3, #clean) do
      local value = BASE64_VALUE[clean:sub(k, k)]
      if not value then
        return nil, string.format("%q isn't a Base64 character.", clean:sub(k, k))
      end
      n = (n << 6) | value
      count = count + 1
    end
    if n & ((1 << (8 - 2 * count)) - 1) ~= 0 then
      return nil, "This isn't valid Base64: its last character has bits that don't belong to any byte."
    end
    n = n << (6 * (4 - count))
    local bytes = string.char((n >> 16) & 255, (n >> 8) & 255, n & 255)
    out[#out + 1] = bytes:sub(1, count - 1)
  end
  return table.concat(out)
end

-- ---- hexadecimal ----

-- Bytes as pairs of hex digits separated by spaces, e.g. "48 69".
local function hex_encode(bytes)
  return (bytes:gsub(".", function(c) return string.format("%02x ", c:byte()) end):gsub(" $", ""))
end

-- Accepts hex digits in either case, with or without spaces, line breaks,
-- or 0x prefixes between bytes.
local function hex_decode(text)
  local clean = text:gsub("0[xX](%x%x)", "%1"):gsub("%s", "")
  if clean:find("%X") then return nil, "Only hex digits (0-9, a-f) and spaces can be decoded." end
  if #clean % 2 == 1 then return nil, "Hex text needs an even number of digits, two for each byte." end
  return (clean:gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end))
end

-- ---- URL (percent) encoding ----

-- Encodes everything except the characters RFC 3986 leaves unreserved, so
-- the result is safe in any part of a URL, including a query value.
local function url_encode(bytes)
  return (bytes:gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end

local function url_decode(text)
  if (text:gsub("%%%x%x", "")):find("%", 1, true) then
    return nil, "Each % must be followed by two hex digits."
  end
  return (text:gsub("%%(%x%x)", function(pair) return string.char(tonumber(pair, 16)) end))
end

-- ---- HTML entities ----

local HTML_ESCAPES = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;", ["'"] = "&#39;" }

local HTML_NAMED = {
  amp = "&", lt = "<", gt = ">", quot = '"', apos = "'", nbsp = "\u{A0}",
  copy = "\u{A9}", reg = "\u{AE}", trade = "\u{2122}", hellip = "\u{2026}",
  mdash = "\u{2014}", ndash = "\u{2013}", lsquo = "\u{2018}", rsquo = "\u{2019}",
  ldquo = "\u{201C}", rdquo = "\u{201D}", bull = "\u{2022}", middot = "\u{B7}",
  deg = "\u{B0}", times = "\u{D7}", divide = "\u{F7}", euro = "\u{20AC}",
  pound = "\u{A3}", yen = "\u{A5}", cent = "\u{A2}", sect = "\u{A7}", para = "\u{B6}",
}

local function html_escape(text)
  return (text:gsub("[&<>\"']", HTML_ESCAPES))
end

-- Decodes numeric entities (&#169; and &#xA9;) and the common named ones.
-- Anything else that looks like an entity is left as it is.
local function html_unescape(text)
  return (text:gsub("&(#?[xX]?)(%w+);", function(kind, body)
    local code
    if kind == "#" then code = tonumber(body, 10)
    elseif kind == "#x" or kind == "#X" then code = tonumber(body, 16)
    elseif kind == "" then return HTML_NAMED[body] end
    -- NUL, surrogates and values past U+10FFFF aren't decoded.
    if code and code > 0 and code <= 0x10FFFF and not (code >= 0xD800 and code <= 0xDFFF) then
      return utf8.char(code)
    end
  end))
end

-- ---- checksums ----

local CRC32_TABLE = {}
for n = 0, 255 do
  local c = n
  for _ = 1, 8 do
    c = (c & 1 == 1) and (0xEDB88320 ~ (c >> 1)) or (c >> 1)
  end
  CRC32_TABLE[n] = c
end

local function crc32(bytes)
  local crc = 0xFFFFFFFF
  for i = 1, #bytes do
    crc = CRC32_TABLE[(crc ~ bytes:byte(i)) & 255] ~ (crc >> 8)
  end
  return string.format("%08x", crc ~ 0xFFFFFFFF)
end

-- The common CRC-16 variants, by their names in the CRC catalogue
-- (reveng.sourceforge.io), with the result each gives for "123456789".
-- Reflected variants process bits least significant first, so their
-- polynomial is given bit-reversed (0x8005 is 0xA001, 0x1021 is 0x8408).
local CRC16_VARIANTS = {
  { name = "CRC-16/ARC", poly = 0xA001, reflected = true, init = 0x0000, xorout = 0x0000 },          -- bb3d
  { name = "CRC-16/MODBUS", poly = 0xA001, reflected = true, init = 0xFFFF, xorout = 0x0000 },       -- 4b37
  { name = "CRC-16/CCITT-FALSE", poly = 0x1021, reflected = false, init = 0xFFFF, xorout = 0x0000 }, -- 29b1
  { name = "CRC-16/XMODEM", poly = 0x1021, reflected = false, init = 0x0000, xorout = 0x0000 },      -- 31c3
  { name = "CRC-16/KERMIT", poly = 0x8408, reflected = true, init = 0x0000, xorout = 0x0000 },       -- 2189
  { name = "CRC-16/X-25", poly = 0x8408, reflected = true, init = 0xFFFF, xorout = 0xFFFF },         -- 906e
}

-- Each variant's lookup table, built the first time it's used.
local function crc16_table(variant)
  if variant.table then return variant.table end
  local t = {}
  for n = 0, 255 do
    local c = variant.reflected and n or n << 8
    for _ = 1, 8 do
      if variant.reflected then
        c = (c & 1 == 1) and (variant.poly ~ (c >> 1)) or (c >> 1)
      else
        c = ((c & 0x8000 ~= 0) and ((c << 1) ~ variant.poly) or (c << 1)) & 0xFFFF
      end
    end
    t[n] = c
  end
  variant.table = t
  return t
end

local function crc16(variant, bytes)
  local t, crc = crc16_table(variant), variant.init
  for i = 1, #bytes do
    if variant.reflected then
      crc = t[(crc ~ bytes:byte(i)) & 255] ~ (crc >> 8)
    else
      crc = (t[((crc >> 8) ~ bytes:byte(i)) & 255] ~ (crc << 8)) & 0xFFFF
    end
  end
  return string.format("%04x", crc ~ variant.xorout)
end

-- The XOR of the bytes, as two hex digits, as NMEA 0183 sentences and AIS
-- messages use. A whole sentence can be selected: the leading $ or ! is
-- left out, and so is everything from the * (or the end of the line) on.
local function nmea_checksum(bytes)
  local body, sum = bytes:match("^[$!]?([^*\r\n]*)"), 0
  for i = 1, #body do
    sum = sum ~ body:byte(i)
  end
  return string.format("%02X", sum)
end

-- The selected text, or nil after saying a selection is needed.
local function checksum_text(name)
  local start, finish = editor.get_selection_range()
  if finish < start then
    ui.message("Select the text to calculate the " .. name .. " of first.")
    return nil
  end
  return editor.get_range(start, finish)
end

-- Shows text's checksum in a text box, already selected, so Ctrl+C copies
-- it. Whatever the user does with the box, the document isn't changed.
local function show_checksum(name, text, checksum)
  ui.prompt(string.format("%s of the selection (%d bytes, as UTF-8):", name, #text), checksum, name)
end

-- ---- Plugins > Encode ----

menu.add("Encode/Base64 Encode", function()
  convert_target(base64_encode)
end)

menu.add("Encode/Base64 Decode", function()
  convert_target(function(text)
    local bytes, problem = base64_decode(text)
    if not bytes then return nil, problem end
    return as_text(bytes, "This Base64")
  end)
end)

menu.separator("Encode")

menu.add("Encode/Hex Encode", function()
  convert_target(hex_encode)
end)

menu.add("Encode/Hex Decode", function()
  convert_target(function(text)
    local bytes, problem = hex_decode(text)
    if not bytes then return nil, problem end
    return as_text(bytes, "This hex")
  end)
end)

menu.separator("Encode")

menu.add("Encode/URL Encode", function()
  convert_target(url_encode)
end)

menu.add("Encode/URL Decode", function()
  convert_target(function(text)
    local bytes, problem = url_decode(text)
    if not bytes then return nil, problem end
    return as_text(bytes, "This URL text")
  end)
end)

menu.separator("Encode")

menu.add("Encode/HTML Escape", function()
  convert_target(html_escape)
end)

menu.add("Encode/HTML Unescape", function()
  convert_target(html_unescape)
end)

-- ---- Plugins > Checksum ----

menu.add("Checksum/CRC-32", function()
  local text = checksum_text("CRC-32")
  if text then show_checksum("CRC-32", text, crc32(text)) end
end)

menu.add("Checksum/CRC-16...", function()
  local text = checksum_text("CRC-16")
  if not text then return end
  local names = {}
  for i, variant in ipairs(CRC16_VARIANTS) do names[i] = variant.name end
  local chosen = ui.choose(names, "CRC-16 variant:", "CRC-16")
  if not chosen then return end
  local variant = CRC16_VARIANTS[chosen]
  show_checksum(variant.name, text, crc16(variant, text))
end)

menu.add("Checksum/NMEA (XOR)", function()
  local text = checksum_text("NMEA checksum")
  if text then show_checksum("NMEA checksum", text, nmea_checksum(text)) end
end)
