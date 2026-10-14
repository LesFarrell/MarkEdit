-- Z80 instruction encoding for the shared assembler in core.lua.

local core = require("core")
local fail, trim, lower = core.fail, core.trim, core.lower
local expression, byte, emit_word = core.expression, core.byte, core.emit_word

local R8 = { b=0, c=1, d=2, e=3, h=4, l=5, ["(hl)"]=6, a=7 }
local IREG = {
  ixh={prefix=0xdd,code=4}, ixl={prefix=0xdd,code=5},
  iyh={prefix=0xfd,code=4}, iyl={prefix=0xfd,code=5},
}
local RR = { bc=0, de=1, hl=2, sp=3 }
local QQ = { bc=0, de=1, hl=2, af=3 }
local CC = { nz=0, z=1, nc=2, c=3, po=4, pe=5, p=6, m=7 }
local JRCC = { nz=0x20, z=0x28, nc=0x30, c=0x38 }
local PREFIX = { ix=0xdd, iy=0xfd }

local FIXED = {
  nop={0x00}, halt={0x76}, di={0xf3}, ei={0xfb}, exx={0xd9},
  daa={0x27}, cpl={0x2f}, ccf={0x3f}, scf={0x37},
  rlca={0x07}, rla={0x17}, rrca={0x0f}, rra={0x1f},
  neg={0xed,0x44}, reti={0xed,0x4d}, retn={0xed,0x45}, rrd={0xed,0x67}, rld={0xed,0x6f},
  ldi={0xed,0xa0}, ldir={0xed,0xb0}, ldd={0xed,0xa8}, lddr={0xed,0xb8},
  cpi={0xed,0xa1}, cpir={0xed,0xb1}, cpd={0xed,0xa9}, cpdr={0xed,0xb9},
  ini={0xed,0xa2}, inir={0xed,0xb2}, ind={0xed,0xaa}, indr={0xed,0xba},
  outi={0xed,0xa3}, otir={0xed,0xb3}, outd={0xed,0xab}, otdr={0xed,0xbb},
}

local function indexed(operand, symbols, pc, line, allow)
  -- Keep the displacement's original case (character literals) and its
  -- leading sign: the expression parser handles unary +/- itself, so
  -- `(IX-2+1)` is -1 rather than -(2+1).
  local text = trim(operand)
  local index, delta = text:match("^%(%s*([Ii][XxYy])%s*([%+%-].+)%)$")
  if not index then index = text:match("^%(%s*([Ii][XxYy])%s*%)$"); delta = "0" end
  if not index then return nil end
  local value = expression(delta, symbols, pc, line, allow)
  if value < -128 or value > 127 then fail(line, "index displacement is outside -128..127") end
  return index:lower(), value & 0xff
end

local function immediate(text, symbols, pc, line, allow)
  return expression(text, symbols, pc, line, allow)
end

local function relative(text, symbols, pc, instruction_size, line, allow)
  local target, unresolved = expression(text, symbols, pc, line, allow)
  if unresolved then return 0 end
  local distance = target - (pc + instruction_size)
  if distance < -128 or distance > 127 then fail(line, "relative jump target is out of range") end
  return distance & 0xff
end

local function encode(mnemonic, args, symbols, pc, line, allow)
  local fixed = FIXED[mnemonic]
  if fixed and #args == 0 then return { table.unpack(fixed) } end
  -- Accept the relaxed branch spelling used by several older assemblers:
  -- `JP NZ label` as well as Zilog's `JP NZ,label`. A trailing colon on a
  -- bare label reference is harmless too, even though it is normally only
  -- written on the label definition.
  if (mnemonic == "jp" or mnemonic == "jr" or mnemonic == "call") and #args == 1 then
    local condition, target = args[1]:match("^([A-Za-z]+)%s+(.+)$")
    local conditions = mnemonic == "jr" and JRCC or CC
    if condition and conditions[condition:lower()] then args = { condition, target } end
  end
  for i, argument in ipairs(args) do
    args[i] = argument:gsub("^([A-Za-z_%.][A-Za-z0-9_%.]*):$", "%1")
  end
  -- a/b are lowercased for register matching only; values are evaluated from
  -- args[] so character literals such as 'A' keep their case.
  local a, b = args[1] and lower(args[1]), args[2] and lower(args[2])
  local function need(count) if #args ~= count then fail(line, mnemonic:upper() .. " expects " .. count .. " operand(s)") end end

  if mnemonic == "ld" then
    need(2)
    local ia, ib = IREG[a], IREG[b]
    if ia and ib and ia.prefix == ib.prefix then return {ia.prefix, 0x40 + ia.code * 8 + ib.code} end
    if ia and R8[b] and (R8[b] <= 3 or b == "a") then return {ia.prefix, 0x40 + ia.code * 8 + R8[b]} end
    if ib and R8[a] and (R8[a] <= 3 or a == "a") then return {ib.prefix, 0x40 + R8[a] * 8 + ib.code} end
    if ia then return {ia.prefix, 0x06 + ia.code * 8,
      byte(immediate(args[2], symbols, pc, line, allow), line, "immediate")} end
    if R8[a] and R8[b] then
      if a == "(hl)" and b == "(hl)" then fail(line, "LD (HL),(HL) is not an instruction") end
      return { 0x40 + R8[a] * 8 + R8[b] }
    end
    local bi, bd = indexed(args[2], symbols, pc, line, allow)
    local ai, ad = indexed(args[1], symbols, pc, line, allow)
    if R8[a] and a ~= "(hl)" and bi then return { PREFIX[bi], 0x46 + R8[a] * 8, bd } end
    if ai and R8[b] and b ~= "(hl)" then return { PREFIX[ai], 0x70 + R8[b], ad } end
    if ai then return { PREFIX[ai], 0x36, ad, byte(immediate(args[2], symbols, pc, line, allow), line, "immediate") } end
    local special = {
      ["a,(bc)"]={0x0a}, ["a,(de)"]={0x1a}, ["(bc),a"]={0x02}, ["(de),a"]={0x12},
      ["sp,hl"]={0xf9}, ["a,i"]={0xed,0x57}, ["a,r"]={0xed,0x5f},
      ["i,a"]={0xed,0x47}, ["r,a"]={0xed,0x4f},
      ["sp,ix"]={0xdd,0xf9}, ["sp,iy"]={0xfd,0xf9},
    }
    if special[a .. "," .. b] then return { table.unpack(special[a .. "," .. b]) } end
    local inner
    if a == "a" then inner = args[2]:match("^%((.+)%)$"); if inner then return emit_word({0x3a}, immediate(inner,symbols,pc,line,allow),line) end end
    if b == "a" then inner = args[1]:match("^%((.+)%)$"); if inner then return emit_word({0x32}, immediate(inner,symbols,pc,line,allow),line) end end
    if a == "hl" then inner = args[2]:match("^%((.+)%)$"); if inner then return emit_word({0x2a}, immediate(inner,symbols,pc,line,allow),line) end end
    if b == "hl" then inner = args[1]:match("^%((.+)%)$"); if inner then return emit_word({0x22}, immediate(inner,symbols,pc,line,allow),line) end end
    for _, index in ipairs({"ix","iy"}) do
      if a == index then inner = args[2]:match("^%((.+)%)$"); if inner then return emit_word({PREFIX[index],0x2a}, immediate(inner,symbols,pc,line,allow),line) end end
      if b == index then inner = args[1]:match("^%((.+)%)$"); if inner then return emit_word({PREFIX[index],0x22}, immediate(inner,symbols,pc,line,allow),line) end end
    end
    if a == "bc" or a == "de" or a == "sp" then
      inner = args[2]:match("^%((.+)%)$")
      if inner then return emit_word({0xed, 0x4b + RR[a] * 0x10}, immediate(inner,symbols,pc,line,allow),line) end
    end
    if b == "bc" or b == "de" or b == "sp" then
      inner = args[1]:match("^%((.+)%)$")
      if inner then return emit_word({0xed, 0x43 + RR[b] * 0x10}, immediate(inner,symbols,pc,line,allow),line) end
    end
    if R8[a] then return { 0x06 + R8[a] * 8, byte(immediate(args[2], symbols, pc, line, allow), line, "immediate") } end
    if RR[a] then return emit_word({ 0x01 + RR[a] * 0x10 }, immediate(args[2], symbols, pc, line, allow), line) end
    if PREFIX[a] then return emit_word({ PREFIX[a], 0x21 }, immediate(args[2], symbols, pc, line, allow), line) end
    fail(line, "unsupported LD operands")
  end

  if mnemonic == "inc" or mnemonic == "dec" then
    need(1); local offset = mnemonic == "inc" and 0 or 1
    if IREG[a] then return {IREG[a].prefix,
      (mnemonic == "inc" and 0x04 or 0x05) + IREG[a].code * 8} end
    if R8[a] then return { (mnemonic == "inc" and 0x04 or 0x05) + R8[a] * 8 } end
    if RR[a] then return { (mnemonic == "inc" and 0x03 or 0x0b) + RR[a] * 0x10 } end
    if PREFIX[a] then return { PREFIX[a], mnemonic == "inc" and 0x23 or 0x2b } end
    local ai, ad = indexed(args[1], symbols, pc, line, allow)
    if ai then return { PREFIX[ai], 0x34 + offset, ad } end
  end

  local alu = { add={0x80,0xc6}, adc={0x88,0xce}, sub={0x90,0xd6}, sbc={0x98,0xde},
                ["and"]={0xa0,0xe6}, xor={0xa8,0xee}, ["or"]={0xb0,0xf6}, cp={0xb8,0xfe} }
  if alu[mnemonic] then
    if (mnemonic == "add" or mnemonic == "adc" or mnemonic == "sbc") and #args == 2 then
      if a == "hl" and RR[b] then
        if mnemonic == "add" then return { 0x09 + RR[b] * 0x10 } end
        return { 0xed, (mnemonic == "adc" and 0x4a or 0x42) + RR[b] * 0x10 }
      end
      if (a == "ix" or a == "iy") and (b == "bc" or b == "de" or b == a or b == "sp") and mnemonic == "add" then
        local code = b == "bc" and 0x09 or b == "de" and 0x19 or b == a and 0x29 or 0x39
        return { PREFIX[a], code }
      end
      if a ~= "a" then fail(line, mnemonic:upper() .. " requires A as its first 8-bit operand") end
      table.remove(args, 1); a = lower(args[1])
    else need(1) end
    if R8[a] then return { alu[mnemonic][1] + R8[a] } end
    if IREG[a] then return { IREG[a].prefix, alu[mnemonic][1] + IREG[a].code } end
    local ai, ad = indexed(args[1], symbols, pc, line, allow)
    if ai then return { PREFIX[ai], alu[mnemonic][1] + 6, ad } end
    return { alu[mnemonic][2], byte(immediate(args[1], symbols, pc, line, allow), line, "immediate") }
  end

  if mnemonic == "jp" then
    if #args == 1 then
      if a == "(hl)" then return {0xe9} end
      if a == "(ix)" or a == "(iy)" then return {PREFIX[a:sub(2,3)],0xe9} end
      return emit_word({0xc3}, immediate(args[1],symbols,pc,line,allow), line)
    elseif #args == 2 and CC[a] then return emit_word({0xc2 + CC[a] * 8}, immediate(args[2],symbols,pc,line,allow), line) end
  elseif mnemonic == "jr" then
    if #args == 1 then return {0x18, relative(args[1],symbols,pc,2,line,allow)}
    elseif #args == 2 and JRCC[a] then return {JRCC[a], relative(args[2],symbols,pc,2,line,allow)} end
  elseif mnemonic == "djnz" then need(1); return {0x10, relative(args[1],symbols,pc,2,line,allow)}
  elseif mnemonic == "call" then
    if #args == 1 then return emit_word({0xcd}, immediate(args[1],symbols,pc,line,allow), line)
    elseif #args == 2 and CC[a] then return emit_word({0xc4 + CC[a] * 8}, immediate(args[2],symbols,pc,line,allow), line) end
  elseif mnemonic == "ret" then
    if #args == 0 then return {0xc9} elseif #args == 1 and CC[a] then return {0xc0 + CC[a] * 8} end
  elseif mnemonic == "rst" then
    need(1); local value = immediate(args[1],symbols,pc,line,allow)
    if value < 0 or value > 0x38 or value % 8 ~= 0 then fail(line, "RST target must be 0, 8, ..., 56") end
    return {0xc7 + value}
  end

  if mnemonic == "push" or mnemonic == "pop" then
    need(1); local base = mnemonic == "push" and 0xc5 or 0xc1
    if QQ[a] then return {base + QQ[a] * 0x10} end
    if PREFIX[a] then return {PREFIX[a], mnemonic == "push" and 0xe5 or 0xe1} end
  elseif mnemonic == "ex" then
    need(2); local pair = a .. "," .. b
    local ex = { ["af,af'"]={0x08}, ["de,hl"]={0xeb}, ["(sp),hl"]={0xe3},
                 ["(sp),ix"]={0xdd,0xe3}, ["(sp),iy"]={0xfd,0xe3} }
    if ex[pair] then return {table.unpack(ex[pair])} end
  elseif mnemonic == "im" then
    need(1); local mode = immediate(args[1],symbols,pc,line,allow)
    local code = ({[0]=0x46,[1]=0x56,[2]=0x5e})[mode]
    if code then return {0xed,code} end
    fail(line, "IM mode must be 0, 1 or 2")
  end

  local rotates = { rlc=0x00, rrc=0x08, rl=0x10, rr=0x18, sla=0x20, sra=0x28,
                    sll=0x30, sl1=0x30, srl=0x38 }
  if rotates[mnemonic] then
    if #args ~= 1 and #args ~= 2 then fail(line, mnemonic:upper() .. " expects 1 or 2 operand(s)") end
    if R8[a] then return {0xcb, rotates[mnemonic] + R8[a]} end
    local ai, ad = indexed(args[1],symbols,pc,line,allow)
    if ai then
      local destination = 6
      if #args == 2 then
        local register = lower(args[2])
        if R8[register] == nil or register == "(hl)" then fail(line, "indexed rotate destination must be a register") end
        destination = R8[register]
      end
      return {PREFIX[ai],0xcb,ad,rotates[mnemonic]+destination}
    end
  elseif mnemonic == "bit" or mnemonic == "res" or mnemonic == "set" then
    if #args ~= 2 and #args ~= 3 then fail(line, mnemonic:upper() .. " expects 2 or 3 operand(s)") end
    local bit = immediate(args[1],symbols,pc,line,allow)
    if bit < 0 or bit > 7 then fail(line, "bit number must be 0..7") end
    local base = mnemonic == "bit" and 0x40 or mnemonic == "res" and 0x80 or 0xc0
    if R8[b] then return {0xcb,base + bit * 8 + R8[b]} end
    local bi, bd = indexed(args[2],symbols,pc,line,allow)
    if bi then
      local destination = 6
      if #args == 3 then
        local register = lower(args[3])
        if R8[register] == nil or register == "(hl)" then fail(line, "indexed bit destination must be a register") end
        destination = R8[register]
      end
      return {PREFIX[bi],0xcb,bd,base + bit * 8 + destination}
    end
  elseif mnemonic == "in" then
    if #args == 1 and a == "(c)" then return {0xed,0x70} end
    need(2)
    local port = args[2]:match("^%((.+)%)$")
    if a == "a" and port and lower(port) ~= "c" then return {0xdb,byte(immediate(port,symbols,pc,line,allow),line,"port")} end
    if b == "(c)" and R8[a] and a ~= "(hl)" then return {0xed,0x40 + R8[a] * 8} end
    if a == "f" and b == "(c)" then return {0xed,0x70} end
  elseif mnemonic == "out" then
    need(2)
    local port = args[1]:match("^%((.+)%)$")
    if b == "a" and port and lower(port) ~= "c" then return {0xd3,byte(immediate(port,symbols,pc,line,allow),line,"port")} end
    if a == "(c)" and R8[b] and b ~= "(hl)" then return {0xed,0x41 + R8[b] * 8} end
    if a == "(c)" and b == "0" then return {0xed,0x71} end
  end

  fail(line, "unknown or unsupported instruction '" .. mnemonic:upper() .. "'")
end

return {
  name = "Z80",
  default_origin = 0x8000,
  memory_size = 0x10000,
  big_endian = false,
  encode = encode,
}
