-- Game Boy cartridge images (.gb) for the SM83 assembler.
--
-- The image is a 32 KB ROM with no memory bank controller (cartridge type
-- $00), which every emulator and flash cartridge runs. The header at
-- $0100-$014F is written here: the entry point (NOP, JP entry), the Nintendo
-- logo the boot ROM checks, the title, and both checksums. Program code may
-- use $0000-$00FF (the RST and interrupt vectors) and $0150 upwards.

local gbrom = {}

gbrom.SIZE = 0x8000
gbrom.HEADER_FIRST, gbrom.HEADER_LAST = 0x0100, 0x014f

-- The boot ROM compares this with $0104-$0133 and stops if it differs.
local LOGO = {
  0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
  0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e, 0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
  0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc, 0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,
}

-- The title field: up to 15 upper-case letters, digits and spaces.
local function title_bytes(name)
  local title = (name or ""):upper():gsub("[^A-Z0-9 ]", ""):sub(1, 15)
  return title
end

-- The ROM image for program `bytes` assembled at `origin`, starting at
-- `entry`, titled from `name`. Raises an error when the program doesn't fit
-- or overlaps the header.
function gbrom.image(name, bytes, origin, entry)
  local finish = origin + #bytes
  if finish > gbrom.SIZE then
    error(("The program ends at $%04X, past the 32 KB ($8000) a cartridge without a memory bank controller holds.")
      :format(finish), 0)
  end
  if entry < 0 or entry >= gbrom.SIZE then
    error(("The entry point $%04X is outside the cartridge ROM ($0000-$7FFF)."):format(entry), 0)
  end
  local rom = {}
  for address = 0, gbrom.SIZE - 1 do rom[address] = 0 end
  for index = 1, #bytes do
    local address = origin + index - 1
    local value = bytes:byte(index)
    -- Gaps that ORG fills are zeros; anything else here is code or data.
    if address >= gbrom.HEADER_FIRST and address <= gbrom.HEADER_LAST and value ~= 0 then
      error(("The program puts code or data at $%04X, in the cartridge header ($0100-$014F). Start it at $0150 or later; vectors can go below $0100.")
        :format(address), 0)
    end
    rom[address] = value
  end

  rom[0x100], rom[0x101], rom[0x102], rom[0x103] = 0x00, 0xc3, entry & 0xff, (entry >> 8) & 0xff
  for index, value in ipairs(LOGO) do rom[0x103 + index] = value end
  local title = title_bytes(name)
  for index = 1, #title do rom[0x133 + index] = title:byte(index) end
  -- $0143 $00: a Game Boy (DMG) game; $0147 $00: ROM only; $0148 $00: 32 KB;
  -- $0149 $00: no RAM; $014A $01: sold outside Japan; $014B $00: no licensee.
  rom[0x14a] = 0x01

  local check = 0
  for address = 0x134, 0x14c do check = (check - rom[address] - 1) & 0xff end
  rom[0x14d] = check
  local sum = 0
  for address = 0, gbrom.SIZE - 1 do
    if address ~= 0x14e and address ~= 0x14f then sum = (sum + rom[address]) & 0xffff end
  end
  rom[0x14e], rom[0x14f] = (sum >> 8) & 0xff, sum & 0xff

  local chunks = {}
  for start = 0, gbrom.SIZE - 1, 4096 do
    local piece = {}
    for address = start, start + 4095 do piece[#piece + 1] = string.char(rom[address]) end
    chunks[#chunks + 1] = table.concat(piece)
  end
  return table.concat(chunks)
end

return gbrom
