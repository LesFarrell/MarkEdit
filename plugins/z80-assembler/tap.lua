local tap = {}

local function word(value)
  return string.char(value & 0xff, (value >> 8) & 0xff)
end

local function block(flag, payload)
  local checksum = flag
  for i = 1, #payload do checksum = checksum ~ payload:byte(i) end
  local body = string.char(flag) .. payload .. string.char(checksum)
  return word(#body) .. body
end

local function tape_name(filename)
  local name = filename:upper():gsub("[^A-Z0-9 _%-]", "_"):sub(1, 10)
  return name .. string.rep(" ", 10 - #name)
end

local function basic_number(value)
  local printed = tostring(value)
  return printed .. string.char(0x0e, 0, 0, value & 0xff, (value >> 8) & 0xff, 0)
end

local function basic_loader(name, origin, entry)
  -- Line 10: CLEAR origin-1:LOAD "" CODE:RANDOMIZE USR entry
  -- Spectrum BASIC stores integer constants as their printed digits followed by
  -- a number marker and the five-byte small-integer representation.
  local statement = ""
  if origin > 0x5ccb then
    statement = string.char(0xfd) .. basic_number(origin - 1) .. string.char(0x3a)
  end
  statement = statement .. string.char(0xef) .. '""' .. string.char(0xaf, 0x3a, 0xf9, 0xc0) ..
    basic_number(entry) .. string.char(0x0d)
  local program = string.char(0, 10) .. word(#statement) .. statement
  local header = string.char(0) .. name .. word(#program) .. word(10) .. word(#program)
  return block(0x00, header) .. block(0xff, program)
end

function tap.code(filename, data, origin, entry)
  -- A TAP block's 16-bit length also includes its flag and checksum bytes.
  if #data > 0xfffd then error("The assembled program is too large for a TAP CODE block.", 0) end
  entry = entry or origin
  if origin < 0 or origin > 0xffff or entry < 0 or entry > 0xffff then
    error("The TAP origin and entry point must be within 0..65535.", 0)
  end
  if origin + #data > 0x10000 then error("The TAP CODE block extends past address 65535.", 0) end
  local name = tape_name(filename)
  local header = string.char(3) .. name .. word(#data) .. word(origin) .. word(0x8000)
  return basic_loader(name, origin, entry) .. block(0x00, header) .. block(0xff, data)
end

return tap
