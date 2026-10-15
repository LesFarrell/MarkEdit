-- Self-starting C64 PRG wrapper. The file loads at BASIC start ($0801):
-- line 10 executes a small machine-code relocator, which copies the assembled
-- payload to its ORG address and jumps to ENTRY. This avoids padding a normal
-- $C000 program with almost 47 KiB of zeroes.

local prg = {}

local BASIC_START = 0x0801
local LOADER_START = 0x080d -- immediately after: 10 SYS2061 + BASIC terminator
local PAYLOAD_START = 0x0841 -- LOADER_START plus the fixed 52-byte relocator

local function word(value)
  return string.char(value & 0xff, (value >> 8) & 0xff)
end

local function basic_loader()
  -- $0801: next-line pointer, line 10, SYS token, "2061", line/program ends.
  return string.char(0x0b, 0x08, 0x0a, 0x00, 0x9e) .. "2061" ..
    string.char(0x00, 0x00, 0x00)
end

local function relocator(origin, entry, length)
  local pages, remainder = length >> 8, length & 0xff
  return string.char(
    0xa9, PAYLOAD_START & 0xff,       -- LDA #<payload
    0x85, 0xfb,                       -- STA $FB
    0xa9, (PAYLOAD_START >> 8) & 0xff,-- LDA #>payload
    0x85, 0xfc,                       -- STA $FC
    0xa9, origin & 0xff,              -- LDA #<origin
    0x85, 0xfd,                       -- STA $FD
    0xa9, (origin >> 8) & 0xff,       -- LDA #>origin
    0x85, 0xfe,                       -- STA $FE
    0xa0, 0x00,                       -- LDY #0
    0xa2, pages,                      -- LDX #full pages
    0xf0, 0x0e,                       -- BEQ remainder
    0xb1, 0xfb,                       -- page: LDA ($FB),Y
    0x91, 0xfd,                       -- STA ($FD),Y
    0xc8,                             -- INY
    0xd0, 0xf9,                       -- BNE page
    0xe6, 0xfc,                       -- INC $FC
    0xe6, 0xfe,                       -- INC $FE
    0xca,                             -- DEX
    0xd0, 0xf2,                       -- BNE page
    0xa0, 0x00,                       -- remainder: LDY #0
    0xc0, remainder,                  -- CPY #remaining bytes
    0xf0, 0x07,                       -- BEQ run
    0xb1, 0xfb,                       -- LDA ($FB),Y
    0x91, 0xfd,                       -- STA ($FD),Y
    0xc8,                             -- INY
    0xd0, 0xf5,                       -- BNE remainder
    0x4c, entry & 0xff, (entry >> 8) & 0xff -- run: JMP entry
  )
end

function prg.code(data, origin, entry)
  entry = entry or origin
  if type(data) ~= "string" or #data == 0 then
    error("The C64 PRG payload must contain machine code.", 0)
  end
  if origin < 0 or origin > 0xffff or entry < 0 or entry > 0xffff or
      origin + #data > 0x10000 then
    error("The C64 origin, entry point and payload must fit within 0..65535.", 0)
  end
  if #data > 0xffff then error("The C64 PRG payload is too large.", 0) end
  if PAYLOAD_START + #data > 0x10000 then
    error("The self-starting C64 PRG is too large to load.", 0)
  end
  local destination_end = origin + #data
  local loader_end = PAYLOAD_START
  if origin < loader_end and destination_end > LOADER_START then
    error("The assembled code overlaps the C64 PRG loader at $080D..$0840.", 0)
  end
  if origin > PAYLOAD_START and origin < PAYLOAD_START + #data then
    error("The assembled code overlaps its packed C64 PRG payload; choose a higher ORG.", 0)
  end
  return word(BASIC_START) .. basic_loader() .. relocator(origin, entry, #data) .. data
end

return prg
