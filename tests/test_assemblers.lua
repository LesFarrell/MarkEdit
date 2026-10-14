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

print("All assembler tests passed.")
