-- MOS 6502 instruction encoding for the shared assembler in core.lua: the
-- 56 documented instructions in every addressing mode.
--
--   LDA #$10        immediate (#<label and #>label take the low/high byte)
--   LDA $10         zero page, or absolute when the address is $100 or more
--   LDA $10,X       zero page,X / absolute,X (and ,Y)
--   LDA ($10,X)     indexed indirect
--   LDA ($10),Y     indirect indexed
--   JMP ($1234)     indirect
--   ASL / ASL A     accumulator
--   BNE label       relative
--
-- The zero-page forms are a byte shorter, so the choice is made in the first
-- pass and kept: an address that isn't known yet (a label defined further
-- down) gets the absolute form, as in most 6502 assemblers.

local core = require("core")
local fail, lower, trim, expression = core.fail, core.lower, core.trim, core.expression

-- Opcodes by addressing mode. imp: implied, acc: accumulator, imm: #value,
-- zp/zpx/zpy: zero page, abs/absx/absy: absolute, ind: (addr),
-- indx: (zp,X), indy: (zp),Y, rel: branch.
local OPS = {
  adc = { imm=0x69, zp=0x65, zpx=0x75, abs=0x6d, absx=0x7d, absy=0x79, indx=0x61, indy=0x71 },
  ["and"] = { imm=0x29, zp=0x25, zpx=0x35, abs=0x2d, absx=0x3d, absy=0x39, indx=0x21, indy=0x31 },
  asl = { acc=0x0a, zp=0x06, zpx=0x16, abs=0x0e, absx=0x1e },
  bcc = { rel=0x90 }, bcs = { rel=0xb0 }, beq = { rel=0xf0 }, bmi = { rel=0x30 },
  bne = { rel=0xd0 }, bpl = { rel=0x10 }, bvc = { rel=0x50 }, bvs = { rel=0x70 },
  bit = { zp=0x24, abs=0x2c },
  brk = { imp=0x00 },
  clc = { imp=0x18 }, cld = { imp=0xd8 }, cli = { imp=0x58 }, clv = { imp=0xb8 },
  cmp = { imm=0xc9, zp=0xc5, zpx=0xd5, abs=0xcd, absx=0xdd, absy=0xd9, indx=0xc1, indy=0xd1 },
  cpx = { imm=0xe0, zp=0xe4, abs=0xec },
  cpy = { imm=0xc0, zp=0xc4, abs=0xcc },
  dec = { zp=0xc6, zpx=0xd6, abs=0xce, absx=0xde },
  dex = { imp=0xca }, dey = { imp=0x88 },
  eor = { imm=0x49, zp=0x45, zpx=0x55, abs=0x4d, absx=0x5d, absy=0x59, indx=0x41, indy=0x51 },
  inc = { zp=0xe6, zpx=0xf6, abs=0xee, absx=0xfe },
  inx = { imp=0xe8 }, iny = { imp=0xc8 },
  jmp = { abs=0x4c, ind=0x6c },
  jsr = { abs=0x20 },
  lda = { imm=0xa9, zp=0xa5, zpx=0xb5, abs=0xad, absx=0xbd, absy=0xb9, indx=0xa1, indy=0xb1 },
  ldx = { imm=0xa2, zp=0xa6, zpy=0xb6, abs=0xae, absy=0xbe },
  ldy = { imm=0xa0, zp=0xa4, zpx=0xb4, abs=0xac, absx=0xbc },
  lsr = { acc=0x4a, zp=0x46, zpx=0x56, abs=0x4e, absx=0x5e },
  nop = { imp=0xea },
  ora = { imm=0x09, zp=0x05, zpx=0x15, abs=0x0d, absx=0x1d, absy=0x19, indx=0x01, indy=0x11 },
  pha = { imp=0x48 }, php = { imp=0x08 }, pla = { imp=0x68 }, plp = { imp=0x28 },
  rol = { acc=0x2a, zp=0x26, zpx=0x36, abs=0x2e, absx=0x3e },
  ror = { acc=0x6a, zp=0x66, zpx=0x76, abs=0x6e, absx=0x7e },
  rti = { imp=0x40 }, rts = { imp=0x60 },
  sbc = { imm=0xe9, zp=0xe5, zpx=0xf5, abs=0xed, absx=0xfd, absy=0xf9, indx=0xe1, indy=0xf1 },
  sec = { imp=0x38 }, sed = { imp=0xf8 }, sei = { imp=0x78 },
  sta = { zp=0x85, zpx=0x95, abs=0x8d, absx=0x9d, absy=0x99, indx=0x81, indy=0x91 },
  stx = { zp=0x86, zpy=0x96, abs=0x8e },
  sty = { zp=0x84, zpx=0x94, abs=0x8c },
  tax = { imp=0xaa }, tay = { imp=0xa8 }, tsx = { imp=0xba },
  txa = { imp=0x8a }, txs = { imp=0x9a }, tya = { imp=0x98 },
}

-- How each kind of operand is written, for error messages.
local SYNTAX = {
  acc = "A", imm = "#value", indx = "(zp,X)", indy = "(zp),Y", ind = "(address)",
  addr = "address", addrx = "address,X", addry = "address,Y",
}

-- The short (zero-page) and long (absolute) form of each address mode.
local ADDRESS_FORMS = {
  addr = { "zp", "abs" }, addrx = { "zpx", "absx" }, addry = { "zpy", "absy" },
}

-- Which kind of operand the instruction has, and the expression inside it.
local function classify(args, opcodes, line)
  if #args == 0 then return "none" end
  local first = trim(args[1])
  if #args == 1 then
    if lower(first) == "a" then return "acc" end
    if first:sub(1, 1) == "#" then return "imm", first:sub(2) end
    local inner = first:match("^%((.*),%s*[Xx]%s*%)$")
    if inner then return "indx", inner end
    inner = first:match("^%((.*)%)$")
    if inner and opcodes.ind then return "ind", inner end
    return "addr", first
  end
  if #args == 2 then
    local index = lower(args[2])
    local inner = first:match("^%((.*)%)$")
    if index == "y" and inner then return "indy", inner end
    if index == "x" then return "addrx", first end
    if index == "y" then return "addry", first end
  end
  fail(line, "operand must be #value, address, address,X, address,Y, (address), (zp,X) or (zp),Y")
end

local function zero_page(text, symbols, pc, line, allow)
  local value, unresolved = expression(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  if value < 0 or value > 0xff then fail(line, "zero-page address is outside 0..255") end
  return value
end

local function encode(mnemonic, args, symbols, pc, line, allow, memo)
  local opcodes = OPS[mnemonic]
  if not opcodes then fail(line, "unknown instruction '" .. mnemonic:upper() .. "'") end
  local kind, text = classify(args, opcodes, line)
  local function missing() fail(line, mnemonic:upper() .. " has no " .. SYNTAX[kind] .. " form") end

  if kind == "none" then
    local code = opcodes.imp or opcodes.acc
    if not code then fail(line, mnemonic:upper() .. " needs an operand") end
    return { code }
  end
  if kind == "acc" then
    if not opcodes.acc then missing() end
    return { opcodes.acc }
  end
  if kind == "imm" then
    if not opcodes.imm then missing() end
    -- #<value and #>value: the low and high byte of a 16-bit value.
    local part = text:match("^%s*([<>])")
    local value, unresolved = expression(part and trim(text):sub(2) or text, symbols, pc, line, allow)
    if part == "<" then value = value & 0xff elseif part == ">" then value = (value >> 8) & 0xff end
    if not unresolved and (value < -128 or value > 255) then fail(line, "immediate value is outside -128..255") end
    return { opcodes.imm, value & 0xff }
  end
  if kind == "indx" or kind == "indy" then
    if not opcodes[kind] then missing() end
    return { opcodes[kind], zero_page(text, symbols, pc, line, allow) }
  end
  if kind == "ind" then
    local value = expression(text, symbols, pc, line, allow)
    if value < 0 or value > 0xffff then fail(line, "address is outside 0..65535") end
    return { opcodes.ind, value & 0xff, value >> 8 }
  end
  if kind == "addr" and opcodes.rel then
    local target, unresolved = expression(text, symbols, pc, line, allow)
    if unresolved then return { opcodes.rel, 0 } end
    local distance = target - (pc + 2)
    if distance < -128 or distance > 127 then fail(line, "branch target is out of range") end
    return { opcodes.rel, distance & 0xff }
  end

  local short, long = table.unpack(ADDRESS_FORMS[kind])
  local value, unresolved = expression(text, symbols, pc, line, allow)
  -- Chosen once, in the first pass, so the instruction's size can't change.
  local form = memo[line]
  if not form then
    local fits = not unresolved and value >= 0 and value <= 0xff
    form = (fits or not opcodes[long]) and short or long
    if not opcodes[form] then form = opcodes[long] and long or short end
    memo[line] = form
  end
  if not opcodes[form] then missing() end
  if unresolved then return form == short and { opcodes[form], 0 } or { opcodes[form], 0, 0 } end
  if form == short then
    if value < 0 or value > 0xff then fail(line, "zero-page address is outside 0..255") end
    return { opcodes[form], value }
  end
  if value < 0 or value > 0xffff then fail(line, "address is outside 0..65535") end
  return { opcodes[form], value & 0xff, value >> 8 }
end

return {
  name = "6502",
  default_origin = 0xc000,
  memory_size = 0x10000,
  big_endian = false,
  encode = encode,
}
