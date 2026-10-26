-- Game Boy (Sharp SM83, "GBZ80") instruction encoding for the shared
-- assembler in core.lua.
--
-- The SM83 is a cut-down Z80 with a few instructions of its own. It has no
-- IX/IY, EX, EXX, DJNZ, IN/OUT, block instructions or ED prefix, and only
-- the NZ, Z, NC and C conditions. It adds LD A,(HL+)/(HL-), LDH for the
-- $FF00-$FFFF I/O page, LD HL,SP+e, ADD SP,e, LD (nn),SP, SWAP and STOP.
--
-- Memory operands can be written Zilog-style, (HL), or RGBDS-style, [HL].
-- (HL+) is also (HLI), and (HL-) is (HLD); LDI and LDD are accepted for
-- LD (HL+) and LD (HL-).

local core = require("core")
local fail, trim, lower = core.fail, core.trim, core.lower
local expression, byte, emit_word = core.expression, core.byte, core.emit_word

local R8 = { b=0, c=1, d=2, e=3, h=4, l=5, ["(hl)"]=6, a=7 }
local RR = { bc=0, de=1, hl=2, sp=3 }
local PUSHPOP = { bc=0, de=1, hl=2, af=3 }
local CC = { nz=0, z=1, nc=2, c=3 }

local FIXED = {
  nop={0x00}, halt={0x76}, stop={0x10,0x00}, di={0xf3}, ei={0xfb},
  daa={0x27}, cpl={0x2f}, ccf={0x3f}, scf={0x37},
  rlca={0x07}, rla={0x17}, rrca={0x0f}, rra={0x1f}, reti={0xd9},
}

-- LD forms between registers and register-addressed memory.
local LD_SPECIAL = {
  ["a,(bc)"]={0x0a}, ["a,(de)"]={0x1a}, ["(bc),a"]={0x02}, ["(de),a"]={0x12},
  ["a,(hl+)"]={0x2a}, ["a,(hli)"]={0x2a}, ["a,(hl-)"]={0x3a}, ["a,(hld)"]={0x3a},
  ["(hl+),a"]={0x22}, ["(hli),a"]={0x22}, ["(hl-),a"]={0x32}, ["(hld),a"]={0x32},
  ["a,(c)"]={0xf2}, ["(c),a"]={0xe2}, ["a,($ff00+c)"]={0xf2}, ["($ff00+c),a"]={0xe2},
  ["sp,hl"]={0xf9},
}

-- [x] is the RGBDS spelling of (x).
local function parenthesised(text)
  local inner = trim(text):match("^%[(.*)%]$")
  return inner and "(" .. inner .. ")" or trim(text)
end

-- An operand lower-cased and without spaces, for matching register forms.
local function form(text)
  return text and lower(text):gsub("%s+", "") or nil
end

local function value(text, symbols, pc, line, allow)
  return expression(text, symbols, pc, line, allow)
end

local function signed_byte(text, symbols, pc, line, allow, what)
  local number, unresolved = value(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  if number < -128 or number > 127 then fail(line, what .. " is outside -128..127") end
  return number & 0xff
end

local function relative(text, symbols, pc, line, allow)
  local target, unresolved = value(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  local distance = target - (pc + 2)
  if distance < -128 or distance > 127 then fail(line, "relative jump target is out of range") end
  return distance & 0xff
end

-- LDH's address: an offset into the $FF00 page, written either as the
-- offset or as the full address.
local function high_page(text, symbols, pc, line, allow)
  local address, unresolved = value(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  if address >= 0xff00 and address <= 0xffff then return address - 0xff00 end
  if address >= 0 and address <= 0xff then return address end
  fail(line, "LDH address must be $FF00..$FFFF (or 0..$FF)")
end

local function encode(mnemonic, args, symbols, pc, line, allow)
  local fixed = FIXED[mnemonic]
  if fixed and #args == 0 then return { table.unpack(fixed) } end
  -- `JP NZ label` as well as `JP NZ,label`, as in z80.lua.
  if (mnemonic == "jp" or mnemonic == "jr" or mnemonic == "call") and #args == 1 then
    local condition, target = args[1]:match("^([A-Za-z]+)%s+(.+)$")
    if condition and CC[condition:lower()] then args = { condition, target } end
  end
  for i, argument in ipairs(args) do
    args[i] = parenthesised(argument:gsub("^([A-Za-z_%.][A-Za-z0-9_%.]*):$", "%1"))
  end
  -- a/b are for matching registers only; values come from args[] so that
  -- character literals keep their case.
  local a, b = form(args[1]), form(args[2])
  local function need(count)
    if #args ~= count then fail(line, mnemonic:upper() .. " expects " .. count .. " operand(s)") end
  end
  local function memory(text) return text:match("^%((.+)%)$") end

  if mnemonic == "ldi" or mnemonic == "ldd" then
    need(2)
    local plus = mnemonic == "ldi"
    if a == "a" and b == "(hl)" then return { plus and 0x2a or 0x3a } end
    if a == "(hl)" and b == "a" then return { plus and 0x22 or 0x32 } end
    fail(line, mnemonic:upper() .. " works between A and (HL)")
  end

  if mnemonic == "ldh" then
    need(2)
    if a == "a" and b == "(c)" then return { 0xf2 } end
    if a == "(c)" and b == "a" then return { 0xe2 } end
    local inner = memory(args[2])
    if a == "a" and inner then return { 0xf0, high_page(inner, symbols, pc, line, allow) } end
    inner = memory(args[1])
    if b == "a" and inner then return { 0xe0, high_page(inner, symbols, pc, line, allow) } end
    fail(line, "LDH works between A and (address) or (C)")
  end

  if mnemonic == "ld" then
    need(2)
    if R8[a] and R8[b] then
      if a == "(hl)" and b == "(hl)" then fail(line, "LD (HL),(HL) is not an instruction") end
      return { 0x40 + R8[a] * 8 + R8[b] }
    end
    local special = LD_SPECIAL[a .. "," .. b]
    if special then return { table.unpack(special) } end
    -- LD HL,SP+e: SP plus a signed offset.
    local offset = args[2]:match("^[Ss][Pp]%s*([%+%-].*)$")
    if a == "hl" and (offset or b == "sp") then
      return { 0xf8, offset and signed_byte(offset, symbols, pc, line, allow, "SP offset") or 0 }
    end
    local inner = memory(args[2])
    if a == "a" and inner then return emit_word({0xfa}, value(inner, symbols, pc, line, allow), line) end
    inner = memory(args[1])
    if inner and b == "a" then return emit_word({0xea}, value(inner, symbols, pc, line, allow), line) end
    if inner and b == "sp" then return emit_word({0x08}, value(inner, symbols, pc, line, allow), line) end
    -- Z80 habits such as LD HL,(nn) and LD B,(nn) would otherwise read the
    -- brackets as grouping and quietly load the address itself.
    if memory(args[1]) or memory(args[2]) then
      fail(line, "the Game Boy can only load A from or to an address; LD " ..
        args[1]:upper() .. "," .. args[2]:upper() .. " is not an instruction")
    end
    if R8[a] then return { 0x06 + R8[a] * 8, byte(value(args[2], symbols, pc, line, allow), line, "immediate") } end
    if RR[a] then return emit_word({ 0x01 + RR[a] * 0x10 }, value(args[2], symbols, pc, line, allow), line) end
    fail(line, "unsupported LD operands")
  end

  if mnemonic == "inc" or mnemonic == "dec" then
    need(1)
    if R8[a] then return { (mnemonic == "inc" and 0x04 or 0x05) + R8[a] * 8 } end
    if RR[a] then return { (mnemonic == "inc" and 0x03 or 0x0b) + RR[a] * 0x10 } end
    fail(line, mnemonic:upper() .. " needs a register or (HL)")
  end

  local alu = { add={0x80,0xc6}, adc={0x88,0xce}, sub={0x90,0xd6}, sbc={0x98,0xde},
                ["and"]={0xa0,0xe6}, xor={0xa8,0xee}, ["or"]={0xb0,0xf6}, cp={0xb8,0xfe} }
  if alu[mnemonic] then
    if mnemonic == "add" and #args == 2 then
      if a == "hl" and RR[b] then return { 0x09 + RR[b] * 0x10 } end
      if a == "sp" then return { 0xe8, signed_byte(args[2], symbols, pc, line, allow, "SP offset") } end
    end
    -- The 8-bit forms take A explicitly or not: ADD A,B and SUB B.
    if #args == 2 then
      if a ~= "a" then fail(line, mnemonic:upper() .. " requires A as its first 8-bit operand") end
      table.remove(args, 1); a = form(args[1])
    else need(1) end
    if R8[a] then return { alu[mnemonic][1] + R8[a] } end
    return { alu[mnemonic][2], byte(value(args[1], symbols, pc, line, allow), line, "immediate") }
  end

  if mnemonic == "jp" then
    if #args == 1 then
      if a == "hl" or a == "(hl)" then return { 0xe9 } end
      return emit_word({0xc3}, value(args[1], symbols, pc, line, allow), line)
    elseif #args == 2 and CC[a] then
      return emit_word({0xc2 + CC[a] * 8}, value(args[2], symbols, pc, line, allow), line)
    end
  elseif mnemonic == "jr" then
    if #args == 1 then return { 0x18, relative(args[1], symbols, pc, line, allow) } end
    if #args == 2 and CC[a] then return { 0x20 + CC[a] * 8, relative(args[2], symbols, pc, line, allow) } end
  elseif mnemonic == "call" then
    if #args == 1 then return emit_word({0xcd}, value(args[1], symbols, pc, line, allow), line) end
    if #args == 2 and CC[a] then
      return emit_word({0xc4 + CC[a] * 8}, value(args[2], symbols, pc, line, allow), line)
    end
  elseif mnemonic == "ret" then
    if #args == 0 then return { 0xc9 } end
    if #args == 1 and CC[a] then return { 0xc0 + CC[a] * 8 } end
  elseif mnemonic == "rst" then
    need(1)
    local target = value(args[1], symbols, pc, line, allow)
    if target < 0 or target > 0x38 or target % 8 ~= 0 then fail(line, "RST target must be 0, 8, ..., 56") end
    return { 0xc7 + target }
  elseif mnemonic == "push" or mnemonic == "pop" then
    need(1)
    if PUSHPOP[a] then return { (mnemonic == "push" and 0xc5 or 0xc1) + PUSHPOP[a] * 0x10 } end
    fail(line, mnemonic:upper() .. " needs BC, DE, HL or AF")
  end

  local rotates = { rlc=0x00, rrc=0x08, rl=0x10, rr=0x18, sla=0x20, sra=0x28, swap=0x30, srl=0x38 }
  if rotates[mnemonic] then
    need(1)
    if R8[a] then return { 0xcb, rotates[mnemonic] + R8[a] } end
    fail(line, mnemonic:upper() .. " needs a register or (HL)")
  elseif mnemonic == "bit" or mnemonic == "res" or mnemonic == "set" then
    need(2)
    local bit = value(args[1], symbols, pc, line, allow)
    if bit < 0 or bit > 7 then fail(line, "bit number must be 0..7") end
    local base = mnemonic == "bit" and 0x40 or mnemonic == "res" and 0x80 or 0xc0
    if R8[b] then return { 0xcb, base + bit * 8 + R8[b] } end
    fail(line, mnemonic:upper() .. " needs a register or (HL)")
  end

  local z80_only = { ex=true, exx=true, djnz=true, ["in"]=true, out=true, im=true, neg=true,
                     ldir=true, lddr=true, cpi=true, cpir=true, cpd=true, cpdr=true, retn=true }
  if z80_only[mnemonic] then fail(line, mnemonic:upper() .. " is a Z80 instruction the Game Boy doesn't have") end
  fail(line, "unknown or unsupported instruction '" .. mnemonic:upper() .. "'")
end

return {
  name = "Game Boy",
  default_origin = 0x0150,
  memory_size = 0x10000,
  big_endian = false,
  encode = encode,
}
