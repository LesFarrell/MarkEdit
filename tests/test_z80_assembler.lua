local assembler = assert(loadfile("plugins/z80-assembler/assembler.lua"))()
local tap = assert(loadfile("plugins/z80-assembler/tap.lua"))()

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

print("All Z80 assembler tests passed.")
