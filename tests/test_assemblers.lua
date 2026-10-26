-- Tests for the bundled assemblers in plugins/assemblers.

-- MarkEdit gives each plugin a require() that loads from its own folder;
-- this stands in for it.
local loaded = {}
function require(name)
  if loaded[name] == nil then loaded[name] = assert(loadfile("plugins/assemblers/" .. name .. ".lua"))() end
  return loaded[name]
end

local core = require("core")
local z80 = require("z80")
local tap = require("tap")

-- ---- Z80 ----

local assembler = { assemble = function(source) return core.assemble(source, z80) end }

local function hex(data)
  return (data:gsub(".", function(c) return ("%02X"):format(c:byte()) end))
end

local function expect_bytes(source, expected)
  local bytes = assembler.assemble(source)
  assert(hex(bytes) == expected, ("expected %s, got %s\n%s"):format(expected, hex(bytes), source))
end

local function expect_error(source, message)
  local ok, problem = pcall(assembler.assemble, source)
  assert(not ok and tostring(problem):find(message, 1, true), "expected error containing " .. message)
end

local opcode_cases = {
  {"NOP\nHALT\nDI\nEI\nEXX\nDAA\nCPL\nCCF\nSCF\nRLCA\nRLA\nRRCA\nRRA",
   "0076F3FBD9272F3F3707170F1F"},
  {"NEG\nRETI\nRETN\nRRD\nRLD\nLDI\nLDIR\nLDD\nLDDR",
   "ED44ED4DED45ED67ED6FEDA0EDB0EDA8EDB8"},
  {"CPI\nCPIR\nCPD\nCPDR\nINI\nINIR\nIND\nINDR\nOUTI\nOTIR\nOUTD\nOTDR",
   "EDA1EDB1EDA9EDB9EDA2EDB2EDAAEDBAEDA3EDB3EDABEDBB"},
  {"LD B,C\nLD (HL),A\nLD A,(BC)\nLD (DE),A\nLD SP,HL\nLD A,I\nLD R,A",
   "41770A12F9ED57ED4F"},
  {"LD A,($1234)\nLD ($1234),HL\nLD BC,($1234)\nLD ($1234),SP\nLD B,42\nLD DE,$1234\nLD IX,$1234",
   "3A3412223412ED4B3412ED733412062A113412DD213412"},
  {"INC A\nDEC (HL)\nINC SP\nDEC IX\nINC (IY-1)", "3C3533DD2BFD34FF"},
  {"ADD A,B\nADC A,C\nSUB D\nSBC A,E\nAND H\nXOR L\nOR (HL)\nCP 1",
   "8089929BA4ADB6FE01"},
  {"ADD HL,SP\nADC HL,DE\nSBC HL,BC\nADD IX,IX", "39ED5AED42DD29"},
  {"JP $1234\nJP NZ,$1234\nCALL $1234\nCALL Z,$1234\nRET\nRET C\nRST 56",
   "C33412C23412CD3412CC3412C9D8FF"},
  {"PUSH AF\nPOP BC\nPUSH IY\nEX AF,AF'\nEX DE,HL\nEX (SP),IX\nIM 2",
   "F5C1FDE508EBDDE3ED5E"},
  {"RLC B\nRR (HL)\nBIT 7,A\nRES 2,C\nSET 0,(HL)", "CB00CB1ECB7FCB91CBC6"},
  {"IN A,($fe)\nIN B,(C)\nOUT ($fe),A\nOUT (C),D", "DBFEED40D3FEED51"},
}
for _, case in ipairs(opcode_cases) do expect_bytes("ORG 32768\n" .. case[1], case[2]) end

expect_bytes("ORG 32768\nJR $\nDJNZ $", "18FE10FE")
expect_bytes("ORG 32768\nCALL here\nhere EQU $\nRET", "CD0380C9")
expect_bytes("ORG 32768\nIN (C)\nIN F,(C)\nOUT (C),0", "ED70ED70ED71")
expect_bytes("ORG 32768\nSLL B\nSL1 (HL)", "CB30CB36")
expect_bytes("ORG 32768\nLD IXH,$12\nLD A,IXL\nLD IYH,A\nINC IXH\nDEC IYL\nADD A,IXH",
  "DD2612DD7DFD67DD24FD2DDD84")
expect_bytes("ORG 32768\nRLC (IX+1),B\nSET 3,(IY-2),A", "DDCB0100FDCBFEDF")

local bytes, origin, symbols, info = assembler.assemble([[
COUNT EQU LATER
BASE EQU $8000
LATER EQU 3
ORG BASE
start: DB 1
.loop: DJNZ .loop
ALIGN 8,$ff
FILL COUNT,$aa
IF COUNT = 3
DEFM "OK"
ELSE
DB 0
ENDIF
ASSERT COUNT = 3,"constant resolution failed"
ENTRY start.loop
END
DB $ee
]])
assert(origin == 0x8000 and info.entry == 0x8001 and info.end_address == 0x800d)
assert(hex(bytes) == "0110FEFFFFFFFFFFAAAAAA4F4B", hex(bytes))
assert(symbols["start.loop"] == 0x8001 and info.symbol_count == 5)

expect_error("ORG 32768\nASSERT 1 = 2,\"bad layout\"", "bad layout")
expect_error("A EQU B\nB EQU A\nDB 0", "unresolved or circular EQU")
expect_error("ORG 32768\nIF 1\nDB 0", "IF without ENDIF")
expect_error("ORG 32768\nJR 33000", "relative jump target is out of range")

local image = tap.code("demo", string.char(0xc9), 0x8000, 0x8001)
assert(#image > 50 and image:find(string.char(0xfd), 1, true), "TAP loader has no CLEAR token")
assert(image:find("32767", 1, true) and image:find("32769", 1, true),
  "TAP loader does not encode CLEAR and entry addresses")

-- ---- CHIP-8 and 6502 ----

local chip8 = require("chip8")
local mos6502 = require("mos6502")

local function expect_cpu_bytes(cpu, source, expected)
  local output = core.assemble(source, cpu)
  assert(hex(output) == expected, ("%s: expected %s, got %s\n%s"):format(cpu.name, expected, hex(output), source))
end

local function expect_cpu_error(cpu, source, message)
  local ok, problem = pcall(core.assemble, source, cpu)
  assert(not ok and tostring(problem):find(message, 1, true),
    ("%s: expected an error containing %q, got %s"):format(cpu.name, message, tostring(problem)))
end

expect_cpu_bytes(chip8, [[
start:  CLS
        LD V0,5
        LD I,sprite
        DRW V0,V1,5
        ADD V0,-1
        SNE V0,V1
        LD VA,DT
        LD DT,VA
        LD [I],VF
        LD VF,[I]
        SHR V3
        SHL V3,V4
        JP start
        JP V0,$300
        CALL start
        RET
sprite: DB $F0,$90
]], "00E06005A220D01570FF9010FA07FA15FF55FF658336834E1200B300220000EEF090")
expect_cpu_bytes(chip8, "SE V1,$22\nSE V1,V2\nOR V1,V2\nAND V1,V2\nXOR V1,V2\nADD V1,V2\nSUB V1,V2\nSUBN V1,V2",
  "31225120812181228123812481258127")
expect_cpu_bytes(chip8, "RND V5,$0F\nSKP V5\nSKNP V5\nLD V5,K\nLD ST,V5\nLD F,V5\nLD B,V5\nADD I,V5\nSYS $123",
  "C50FE59EE5A1F50AF518F529F533F51E0123")
-- SUPER-CHIP, and DW stores the high byte first.
expect_cpu_bytes(chip8, "HIGH\nLOW\nSCD 4\nSCR\nSCL\nEXIT\nLD HF,V2\nLD R,V7\nLD V7,R\nDRW V1,V2,0\nDW $1234",
  "00FF00FE00C400FB00FC00FDF230F775F785D1201234")
expect_cpu_error(chip8, "LD V0,256", "byte is outside")
expect_cpu_error(chip8, "JP $1000", "address is outside 0..4095")
expect_cpu_error(chip8, "DRW V0,V1,16", "sprite height is outside 0..15")
expect_cpu_error(chip8, "SCD 16", "scroll distance is outside 0..15")
expect_cpu_error(chip8, "LD V0", "unknown or unsupported instruction 'LD V0'")

expect_cpu_bytes(mos6502, [[
        LDA #$10
        LDA $10
        LDA $1234
        LDA $10,X
        LDA $1234,Y
        LDA ($10,X)
        LDA ($10),Y
        STA $0200,X
        JMP ($1234)
        JMP $C000
        ASL
        ASL A
        LSR $10
        LDX $10,Y
        INX
        RTS
]], "A910A510AD3412B510B93412A110B1109D00026C34124C00C00A0A4610B610E860")
expect_cpu_bytes(mos6502, "loop: DEX\nBNE loop\nBEQ done\nNOP\ndone: RTS", "CAD0FDF001EA60")
expect_cpu_bytes(mos6502, "LDA #<$1234\nLDX #>$1234\nLDY #-1", "A934A212A0FF")
-- A label defined further down gets the absolute form even when it turns
-- out to be in zero page, so both passes agree on the instruction's size.
expect_cpu_bytes(mos6502, "ORG $0000\nLDA data\nRTS\ndata: DB 1", "AD04006001")
expect_cpu_bytes(mos6502, "ORG $C000\nLDA data\nRTS\ndata: DB 1\nDW $1234", "AD04C060013412")
expect_cpu_error(mos6502, "STX $1234,X", "STX has no address,X form")
expect_cpu_error(mos6502, "LDA ($1234),Y", "zero-page address is outside 0..255")
expect_cpu_error(mos6502, "BNE $D000", "branch target is out of range")
expect_cpu_error(mos6502, "LDA #300", "immediate value is outside -128..255")
expect_cpu_error(mos6502, "FOO", "unknown instruction 'FOO'")

-- ---- Game Boy (SM83) ----

local sm83 = require("sm83")
local gbrom = require("gbrom")

local sm83_cases = {
  {"NOP\nHALT\nSTOP\nDI\nEI\nDAA\nCPL\nCCF\nSCF\nRLCA\nRLA\nRRCA\nRRA\nRETI",
   "00761000F3FB272F3F3707170F1FD9"},
  {"LD B,C\nLD (HL),A\nLD A,(BC)\nLD (DE),A\nLD SP,HL", "41770A12F9"},
  {"LD A,(HL+)\nLD A,(HLD)\nLD (HLI),A\nLD (HL-),A\nLDI A,(HL)\nLDD (HL),A", "2A3A22322A32"},
  {"LD A,[HL+]\nLD [HL],A\nLD A,[$C000]\nLD [$C000],A", "2A77FA00C0EA00C0"},
  {"LDH ($FF44),A\nLDH A,($44)\nLDH A,(C)\nLDH (C),A\nLD A,(C)\nLD ($FF00+C),A", "E044F044F2E2F2E2"},
  {"LD BC,$1234\nLD SP,$FFFE\nLD ($C000),SP\nLD HL,SP+5\nLD HL,SP-2\nADD SP,-2",
   "013412 31FEFF 0800C0 F805 F8FE E8FE"},
  {"INC A\nDEC (HL)\nINC SP\nDEC BC\nADD HL,DE", "3C35330B19"},
  {"ADD A,B\nADC A,C\nSUB D\nSBC A,E\nAND H\nXOR A\nOR (HL)\nCP $90", "8089929BA4AFB6FE90"},
  {"JP $1234\nJP NZ,$1234\nJP HL\nCALL $1234\nCALL C,$1234\nRET\nRET NC\nRST $38",
   "C33412C23412E9CD3412DC3412C9D0FF"},
  {"PUSH AF\nPOP HL\nPUSH BC\nPOP DE", "F5E1C5D1"},
  {"RLC B\nRR (HL)\nSWAP A\nSRL B\nBIT 7,H\nRES 2,C\nSET 0,(HL)", "CB00CB1ECB37CB38CB7CCB91CBC6"},
}
for _, case in ipairs(sm83_cases) do
  expect_cpu_bytes(sm83, "ORG $0150\n" .. case[1], (case[2]:gsub(" ", "")))
end
expect_cpu_bytes(sm83, "ORG $0150\nJR $\nJR NZ,$", "18FE20FE")
expect_cpu_error(sm83, "EXX", "EXX is a Z80 instruction the Game Boy doesn't have")
expect_cpu_error(sm83, "DJNZ $", "Z80 instruction")
expect_cpu_error(sm83, "LD HL,($C000)", "can only load A from or to an address")
expect_cpu_error(sm83, "LD B,[$C000]", "can only load A from or to an address")
expect_cpu_error(sm83, "JP PE,$0150", "unknown or unsupported instruction 'JP'")
expect_cpu_error(sm83, "LDH ($1234),A", "LDH address must be $FF00..$FFFF")
expect_cpu_error(sm83, "ADD SP,200", "SP offset is outside -128..127")

-- The ROM: the header is filled in and both checksums verify, as an
-- emulator's boot ROM checks them.
local program, rom_origin, _, rom_info = core.assemble([[
        ORG $0040
vblank: RETI
        ORG $0150
start:  DI
.loop:  JR .loop
        ENTRY start
]], sm83)
local rom = gbrom.image("hello world!", program, rom_origin, rom_info.entry)
assert(#rom == 0x8000, "a Game Boy ROM is 32 KB")
local function at(address) return rom:byte(address + 1) end
assert(at(0x40) == 0xd9 and at(0x150) == 0xf3 and at(0x151) == 0x18, "code lands at its addresses")
assert(at(0x100) == 0x00 and at(0x101) == 0xc3 and at(0x102) == 0x50 and at(0x103) == 0x01,
  "the entry point jumps to ENTRY")
assert(at(0x104) == 0xce and at(0x133) == 0x3e, "the Nintendo logo is in place")
assert(rom:sub(0x135, 0x13f) == "HELLO WORLD", "the title is upper case without punctuation")
local header = 0
for address = 0x134, 0x14c do header = (header - at(address) - 1) & 0xff end
assert(at(0x14d) == header, "the header checksum verifies")
local sum = 0
for address = 0, 0x7fff do
  if address ~= 0x14e and address ~= 0x14f then sum = (sum + at(address)) & 0xffff end
end
assert(at(0x14e) == sum >> 8 and at(0x14f) == sum & 0xff, "the global checksum verifies")
local overlap = core.assemble("ORG $0100\nNOP\nJP $0150", sm83)
local ok, problem = pcall(gbrom.image, "x", overlap, 0x100, 0x100)
assert(not ok and problem:find("cartridge header", 1, true), "code in the header is refused")
ok, problem = pcall(gbrom.image, "x", string.rep("\0", 0x8000), 0x150, 0x150)
assert(not ok and problem:find("32 KB", 1, true), "programs past 32 KB are refused")

-- Every bundled example assembles.
local function read_example(name)
  return read_file("plugins/assemblers/examples/" .. name)
end
for name, cpu in pairs({ ["z80-hello.asm"] = z80, ["chip8-hello.asm"] = chip8,
                         ["c64-hello.asm"] = mos6502, ["gb-hello.asm"] = sm83 }) do
  local ok_example, example = pcall(core.assemble, read_example(name), cpu)
  assert(ok_example and #example > 0, name .. " assembles: " .. tostring(example))
end
local hello, hello_origin, _, hello_info = core.assemble(read_example("gb-hello.asm"), sm83)
assert(#gbrom.image("gb-hello", hello, hello_origin, hello_info.entry) == 0x8000,
  "the Game Boy example makes a ROM")

print("All assembler tests passed.")
