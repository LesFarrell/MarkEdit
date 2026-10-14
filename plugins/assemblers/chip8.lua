-- CHIP-8 and SUPER-CHIP instruction encoding for the shared assembler in
-- core.lua, using the common mnemonics (Cowgod's reference): CLS, RET, JP,
-- CALL, SE, SNE, LD, ADD, OR, AND, XOR, SUB, SHR, SUBN, SHL, RND, DRW, SKP,
-- SKNP, SYS, and SUPER-CHIP 1.1's SCD, SCR, SCL, EXIT, LOW, HIGH, LD HF,Vx,
-- LD R,Vx and LD Vx,R (DRW Vx,Vy,0 draws its 16x16 sprite). Every
-- instruction is two bytes, high byte first. Programs load at $200.

local core = require("core")
local fail, lower, expression = core.fail, core.lower, core.expression

-- V0..VF, or nil.
local function register(text)
  local digit = lower(text):match("^v(%x)$")
  return digit and tonumber(digit, 16)
end

local function value(text, symbols, pc, line, allow, maximum, what)
  local result, unresolved = expression(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  if result < 0 or result > maximum then
    fail(line, ("%s is outside 0..%d"):format(what, maximum))
  end
  return result
end

local function address(text, symbols, pc, line, allow)
  return value(text, symbols, pc, line, allow, 0xfff, "address")
end

-- Bytes may be written signed, as in ADD V0,-1.
local function byte(text, symbols, pc, line, allow)
  local result, unresolved = expression(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  if result < -128 or result > 255 then fail(line, "byte is outside -128..255") end
  return result & 0xff
end

local function instruction(opcode)
  return { opcode >> 8, opcode & 0xff }
end

-- The Vx,Vy register-to-register operations, by their low nibble.
local REGISTER_OPS = { ["or"]=0x1, ["and"]=0x2, xor=0x3, sub=0x5, subn=0x7 }
local SHIFTS = { shr=0x6, shl=0xe }
-- LD forms with a named operand: LD Vx,DT and LD DT,Vx and so on. HF (the
-- big font) and R (the RPL flags) are SUPER-CHIP's.
local LOAD_FROM = { dt=0x07, k=0x0a, ["[i]"]=0x65, ["(i)"]=0x65, r=0x85 }
local LOAD_TO = { dt=0x15, st=0x18, f=0x29, b=0x33, ["[i]"]=0x55, ["(i)"]=0x55,
                  hf=0x30, r=0x75 }
-- Instructions with no operands.
local FIXED = { cls=0x00e0, ret=0x00ee,
                scr=0x00fb, scl=0x00fc, exit=0x00fd, low=0x00fe, high=0x00ff }

local function encode(mnemonic, args, symbols, pc, line, allow)
  local count = #args
  local a, b = args[1] and lower(args[1]), args[2] and lower(args[2])
  local x, y = args[1] and register(args[1]), args[2] and register(args[2])
  local function addr(text) return address(text, symbols, pc, line, allow) end
  local function kk(text) return byte(text, symbols, pc, line, allow) end

  if FIXED[mnemonic] and count == 0 then return instruction(FIXED[mnemonic]) end
  if mnemonic == "scd" and count == 1 then
    return instruction(0x00c0 | value(args[1], symbols, pc, line, allow, 15, "scroll distance"))
  end
  if mnemonic == "sys" and count == 1 then return instruction(addr(args[1])) end
  if mnemonic == "jp" then
    if count == 1 then return instruction(0x1000 | addr(args[1])) end
    if count == 2 and a == "v0" then return instruction(0xb000 | addr(args[2])) end
  end
  if mnemonic == "call" and count == 1 then return instruction(0x2000 | addr(args[1])) end

  if (mnemonic == "se" or mnemonic == "sne") and count == 2 and x then
    if y then return instruction((mnemonic == "se" and 0x5000 or 0x9000) | x << 8 | y << 4) end
    return instruction((mnemonic == "se" and 0x3000 or 0x4000) | x << 8 | kk(args[2]))
  end

  if mnemonic == "ld" and count == 2 then
    if x and y then return instruction(0x8000 | x << 8 | y << 4) end
    if x and LOAD_FROM[b] then return instruction(0xf000 | x << 8 | LOAD_FROM[b]) end
    if y and LOAD_TO[a] then return instruction(0xf000 | y << 8 | LOAD_TO[a]) end
    if a == "i" then return instruction(0xa000 | addr(args[2])) end
    if x then return instruction(0x6000 | x << 8 | kk(args[2])) end
  end

  if mnemonic == "add" and count == 2 then
    if a == "i" and y then return instruction(0xf01e | y << 8) end
    if x and y then return instruction(0x8004 | x << 8 | y << 4) end
    if x then return instruction(0x7000 | x << 8 | kk(args[2])) end
  end

  if REGISTER_OPS[mnemonic] and count == 2 and x and y then
    return instruction(0x8000 | x << 8 | y << 4 | REGISTER_OPS[mnemonic])
  end
  -- SHR Vx and SHL Vx shift Vx; the optional Vy is the original CHIP-8's
  -- source register.
  if SHIFTS[mnemonic] and x and (count == 1 or (count == 2 and y)) then
    return instruction(0x8000 | x << 8 | (y or x) << 4 | SHIFTS[mnemonic])
  end

  if mnemonic == "rnd" and count == 2 and x then return instruction(0xc000 | x << 8 | kk(args[2])) end
  if mnemonic == "drw" and count == 3 and x and y then
    local rows = value(args[3], symbols, pc, line, allow, 15, "sprite height")
    return instruction(0xd000 | x << 8 | y << 4 | rows)
  end
  if mnemonic == "skp" and count == 1 and x then return instruction(0xe09e | x << 8) end
  if mnemonic == "sknp" and count == 1 and x then return instruction(0xe0a1 | x << 8) end

  fail(line, "unknown or unsupported instruction '" .. mnemonic:upper() ..
    (count > 0 and " " .. table.concat(args, ",") or "") .. "'")
end

return {
  name = "CHIP-8",
  default_origin = 0x200,
  memory_size = 0x1000,
  big_endian = true,
  encode = encode,
}
