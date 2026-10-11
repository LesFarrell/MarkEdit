local assembler = {}

local function trim(text) return (text:match("^%s*(.-)%s*$")) end
local function lower(text) return trim(text):lower() end

local function fail(line, message)
  error(("Line %d: %s"):format(line, message), 0)
end

local function strip_comment(line)
  local quote, escaped = nil, false
  for i = 1, #line do
    local c = line:sub(i, i)
    if escaped then escaped = false
    elseif quote and c == "\\" then escaped = true
    elseif quote and c == quote then quote = nil
    elseif not quote and (c == '"' or (c == "'" and not line:sub(i - 1, i - 1):match("[%w_]"))) then quote = c
    elseif not quote and c == ";" then return line:sub(1, i - 1) end
  end
  return line
end

local function operands(text, line)
  if trim(text) == "" then return {} end
  local result, start, depth, quote, escaped = {}, 1, 0, nil, false
  for i = 1, #text do
    local c = text:sub(i, i)
    if escaped then escaped = false
    elseif quote and c == "\\" then escaped = true
    elseif quote and c == quote then quote = nil
    elseif not quote and (c == '"' or (c == "'" and not text:sub(i - 1, i - 1):match("[%w_]"))) then quote = c
    elseif not quote and c == "(" then depth = depth + 1
    elseif not quote and c == ")" then depth = depth - 1
    elseif not quote and depth == 0 and c == "," then
      result[#result + 1] = trim(text:sub(start, i - 1)); start = i + 1
    end
    if depth < 0 then fail(line, "unmatched ')' in operands") end
  end
  if quote then fail(line, "unterminated string") end
  if depth ~= 0 then fail(line, "unmatched '(' in operands") end
  result[#result + 1] = trim(text:sub(start))
  return result
end

local function expression(text, symbols, pc, line, allow_undefined)
  local tokens, position = {}, 1
  local function add(kind, value) tokens[#tokens + 1] = { kind, value } end
  while position <= #text do
    local rest, c = text:sub(position), text:sub(position, position)
    local spaces = rest:match("^%s+")
    if spaces then position = position + #spaces
    elseif rest:match("^0[xX][%da-fA-F]+") then
      local value = rest:match("^0[xX]([%da-fA-F]+)"); add("number", tonumber(value, 16)); position = position + #value + 2
    elseif c == "$" and rest:sub(2):match("^[%da-fA-F]") then
      local value = rest:match("^%$([%da-fA-F]+)"); add("number", tonumber(value, 16)); position = position + #value + 1
    elseif c == "%" and rest:sub(2):match("^[01]") then
      local value = rest:match("^%%([01]+)"); add("number", tonumber(value, 2)); position = position + #value + 1
    elseif rest:match("^%d+") then
      local value = rest:match("^(%d+)"); add("number", tonumber(value)); position = position + #value
    elseif c == "'" then
      local value, consumed
      if rest:sub(2, 2) == "\\" then
        local escapes = { n = 10, r = 13, t = 9, ["0"] = 0, ["\\"] = 92, ["'"] = 39 }
        value, consumed = escapes[rest:sub(3, 3)], 4
      else value, consumed = rest:byte(2), 3 end
      if value == nil or rest:sub(consumed, consumed) ~= "'" then fail(line, "invalid character literal") end
      add("number", value); position = position + consumed
    elseif rest:match("^[A-Za-z_%.][A-Za-z0-9_%.]*") then
      local name = rest:match("^([A-Za-z_%.][A-Za-z0-9_%.]*)")
      add("name", name:lower()); position = position + #name
    elseif rest:sub(1, 2) == "<<" or rest:sub(1, 2) == ">>" or
           rest:sub(1, 2) == "==" or rest:sub(1, 2) == "!=" or
           rest:sub(1, 2) == "<=" or rest:sub(1, 2) == ">=" or rest:sub(1, 2) == "<>" then
      add("op", rest:sub(1, 2)); position = position + 2
    elseif c == "(" or c == ")" then add(c, c); position = position + 1
    elseif c:match("[+%-%*/%%&|%^~<>=]") then add("op", c); position = position + 1
    elseif c == "$" then add("current", pc); position = position + 1
    else fail(line, "invalid expression near '" .. rest:sub(1, 12) .. "'") end
  end
  add("end", nil)
  local at, unresolved = 1, false
  local parse_or, parse_expression
  local function primary()
    local token = tokens[at]
    if token[1] == "number" then at = at + 1; return token[2]
    elseif token[1] == "current" then at = at + 1; return pc
    elseif token[1] == "name" then
      at = at + 1
      local value = symbols[token[2]]
      if value == nil then
        if not allow_undefined then fail(line, "unknown symbol '" .. token[2] .. "'") end
        unresolved = true; return 0
      end
      return value
    elseif token[1] == "(" then
      at = at + 1; local value = parse_expression()
      if tokens[at][1] ~= ")" then fail(line, "expected ')' in expression") end
      at = at + 1; return value
    end
    fail(line, "expected a value in expression")
  end
  local function unary()
    local op = tokens[at][1] == "op" and tokens[at][2]
    if op == "+" or op == "-" or op == "~" then
      at = at + 1; local value = unary()
      return op == "-" and -value or op == "~" and ~value or value
    end
    return primary()
  end
  local function binary(next_level, accepted)
    local value = next_level()
    while tokens[at][1] == "op" and accepted[tokens[at][2]] do
      local op = tokens[at][2]; at = at + 1; local right = next_level()
      if op == "*" then value = value * right
      elseif op == "/" then if right == 0 then fail(line, "division by zero") end; value = value // right
      elseif op == "%" then if right == 0 then fail(line, "division by zero") end; value = value % right
      elseif op == "+" then value = value + right
      elseif op == "-" then value = value - right
      elseif op == "<<" then value = value << right
      elseif op == ">>" then value = value >> right
      elseif op == "&" then value = value & right
      elseif op == "^" then value = value ~ right
      elseif op == "|" then value = value | right end
    end
    return value
  end
  local mul = function() return binary(unary, { ["*"]=true, ["/"]=true, ["%"]=true }) end
  local sum = function() return binary(mul, { ["+"]=true, ["-"]=true }) end
  local shift = function() return binary(sum, { ["<<"]=true, [">>"]=true }) end
  local band = function() return binary(shift, { ["&"]=true }) end
  local bxor = function() return binary(band, { ["^"]=true }) end
  parse_or = function() return binary(bxor, { ["|"]=true }) end
  local function comparison()
    local value = parse_or()
    while tokens[at][1] == "op" and
          ({["="]=true,["=="]=true,["!="]=true,["<>"]=true,
            ["<"]=true,["<="]=true,[">"]=true,[">="]=true})[tokens[at][2]] do
      local op = tokens[at][2]; at = at + 1; local right = parse_or()
      local result = (op == "=" or op == "==") and value == right
        or (op == "!=" or op == "<>") and value ~= right
        or op == "<" and value < right or op == "<=" and value <= right
        or op == ">" and value > right or op == ">=" and value >= right
      value = result and 1 or 0
    end
    return value
  end
  parse_expression = comparison
  local value = parse_expression()
  if tokens[at][1] ~= "end" then fail(line, "unexpected token in expression") end
  return value, unresolved
end

local function byte(value, line, what)
  if value < -128 or value > 255 then fail(line, (what or "byte") .. " is outside -128..255") end
  return value & 0xff
end
local function word(value, line)
  if value < -32768 or value > 0xffff then fail(line, "word is outside -32768..65535") end
  value = value & 0xffff; return value & 0xff, value >> 8
end
local function emit_word(prefix, value, line)
  local lo, hi = word(value, line); prefix[#prefix + 1], prefix[#prefix + 2] = lo, hi; return prefix
end

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
  local compact = lower(operand):gsub("%s+", "")
  local index, delta = compact:match("^%((i[xy])([%+%-].+)%)$")
  if not index then index = compact:match("^%((i[xy])%)$"); delta = "0" end
  if not index then return nil end
  local sign = delta:sub(1, 1)
  local value = expression((sign == "+" or sign == "-") and delta:sub(2) or delta,
                           symbols, pc, line, allow)
  if sign == "-" then value = -value end
  if value < -128 or value > 127 then fail(line, "index displacement is outside -128..127") end
  return index, value & 0xff
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
    if a == "a" then inner = b:match("^%((.+)%)$"); if inner then return emit_word({0x3a}, immediate(inner,symbols,pc,line,allow),line) end end
    if b == "a" then inner = a:match("^%((.+)%)$"); if inner then return emit_word({0x32}, immediate(inner,symbols,pc,line,allow),line) end end
    if a == "hl" then inner = b:match("^%((.+)%)$"); if inner then return emit_word({0x2a}, immediate(inner,symbols,pc,line,allow),line) end end
    if b == "hl" then inner = a:match("^%((.+)%)$"); if inner then return emit_word({0x22}, immediate(inner,symbols,pc,line,allow),line) end end
    for _, index in ipairs({"ix","iy"}) do
      if a == index then inner = b:match("^%((.+)%)$"); if inner then return emit_word({PREFIX[index],0x2a}, immediate(inner,symbols,pc,line,allow),line) end end
      if b == index then inner = a:match("^%((.+)%)$"); if inner then return emit_word({PREFIX[index],0x22}, immediate(inner,symbols,pc,line,allow),line) end end
    end
    if a == "bc" or a == "de" or a == "sp" then
      inner = b:match("^%((.+)%)$")
      if inner then return emit_word({0xed, 0x4b + RR[a] * 0x10}, immediate(inner,symbols,pc,line,allow),line) end
    end
    if b == "bc" or b == "de" or b == "sp" then
      inner = a:match("^%((.+)%)$")
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
    local port = b:match("^%((.+)%)$")
    if a == "a" and port and lower(port) ~= "c" then return {0xdb,byte(immediate(port,symbols,pc,line,allow),line,"port")} end
    if b == "(c)" and R8[a] and a ~= "(hl)" then return {0xed,0x40 + R8[a] * 8} end
    if a == "f" and b == "(c)" then return {0xed,0x70} end
  elseif mnemonic == "out" then
    need(2)
    local port = a:match("^%((.+)%)$")
    if b == "a" and port and lower(port) ~= "c" then return {0xd3,byte(immediate(port,symbols,pc,line,allow),line,"port")} end
    if a == "(c)" and R8[b] and b ~= "(hl)" then return {0xed,0x41 + R8[b] * 8} end
    if a == "(c)" and b == "0" then return {0xed,0x71} end
  end

  fail(line, "unknown or unsupported instruction '" .. mnemonic:upper() .. "'")
end

local function quoted_string(value, line)
  local quote = value:sub(1,1)
  if (quote ~= "'" and quote ~= '"') or value:sub(-1) ~= quote then return nil end
  local out, i = {}, 2
  while i < #value do
    local c = value:sub(i,i)
    if c == "\\" then
      i = i + 1; c = value:sub(i,i)
      local escapes = { n="\n", r="\r", t="\t", ["0"]="\0", ["\\"]="\\", ["'"]="'", ['"']='"' }
      c = escapes[c] or c
    end
    out[#out + 1] = c; i = i + 1
  end
  return table.concat(out)
end

local function qualify_locals(text, scope, line)
  local out, i, quote, escaped = {}, 1, nil, false
  while i <= #text do
    local c = text:sub(i, i)
    if quote then
      out[#out + 1] = c
      if escaped then escaped = false
      elseif c == "\\" then escaped = true
      elseif c == quote then quote = nil end
      i = i + 1
    elseif c == '"' or (c == "'" and not text:sub(i - 1, i - 1):match("[%w_]")) then
      quote = c; out[#out + 1] = c; i = i + 1
    elseif c == "." and not text:sub(i - 1, i - 1):match("[A-Za-z0-9_%.]") then
      local name = text:sub(i):match("^(%.[A-Za-z_][A-Za-z0-9_%.]*)")
      if name then
        if not scope then fail(line, "local symbol '" .. name .. "' has no enclosing label") end
        out[#out + 1] = scope .. name; i = i + #name
      else out[#out + 1] = c; i = i + 1 end
    else out[#out + 1] = c; i = i + 1 end
  end
  return table.concat(out)
end

local function parse_source(source)
  local lines = {}
  source = source:gsub("\r\n", "\n"):gsub("\r", "\n")
  if source:sub(-1) ~= "\n" then source = source .. "\n" end
  local number, scope = 0, nil
  for raw in source:gmatch("(.-)\n") do
    number = number + 1
    local text = trim(strip_comment(raw)); local label
    local candidate, rest = text:match("^([A-Za-z_%.][A-Za-z0-9_%.]*)%s*:%s*(.*)$")
    local colon_label = candidate ~= nil
    if candidate then label, text = candidate:lower(), rest end
    local equ_name, equ_expr = text:match("^([A-Za-z_%.][A-Za-z0-9_%.]*)%s+[Ee][Qq][Uu]%s+(.+)$")
    if equ_name and not label then label, text = equ_name:lower(), "equ " .. equ_expr end
    if label then
      if label:sub(1, 1) == "." then
        if not scope then fail(number, "local symbol '" .. label .. "' has no enclosing label") end
        label = scope .. label
      elseif colon_label then scope = label end
    end
    local mnemonic, rest_text = text:match("^(%S+)%s*(.-)%s*$")
    rest_text = qualify_locals(rest_text or "", scope, number)
    lines[#lines + 1] = { number=number, label=label, mnemonic=mnemonic and mnemonic:lower(),
      args=rest_text }
  end
  return lines
end

function assembler.assemble(source)
  local lines = parse_source(source)
  local definitions, equations = {}, {}
  for _, item in ipairs(lines) do
    if item.label then
      if definitions[item.label] then fail(item.number, "duplicate symbol '" .. item.label .. "'") end
      definitions[item.label] = item.number
      if item.mnemonic == "equ" then equations[item.label] = item end
    end
  end

  -- Resolve constant-only EQU chains before layout so forward constants can
  -- be used by ORG, DS, ALIGN and IF. Equations involving address labels are
  -- completed after the first layout pass.
  local seeded = {}
  for _ = 1, #lines do
    local changed = false
    for name, item in pairs(equations) do
      if seeded[name] == nil and not item.args:find("%$") then
        local value, unresolved = expression(item.args, seeded, 0, item.number, true)
        if not unresolved then seeded[name], changed = value, true end
      end
    end
    if not changed then break end
  end

  local symbols, equ_pcs, origin, final_size = {}, {}, nil, nil
  for name, value in pairs(seeded) do symbols[name] = value end
  for pass = 1, 2 do
    local pc, output, stopped, active = 0x8000, {}, false, true
    local base, entry, condition_stack = nil, nil, {}
    for _, item in ipairs(lines) do
      if not stopped then
        local line = item.number
        local mnemonic = item.mnemonic
        if mnemonic == "if" then
          local args = operands(item.args, line)
          if #args ~= 1 then fail(line, "IF expects one expression") end
          local parent, condition = active, false
          if parent then
            local value, unresolved = expression(args[1], symbols, pc, line, pass == 1)
            if unresolved then fail(line, "IF expression must be resolved before layout") end
            condition = value ~= 0
          end
          condition_stack[#condition_stack + 1] = {parent=parent, condition=condition, seen_else=false}
          active = parent and condition
        elseif mnemonic == "else" then
          if trim(item.args) ~= "" then fail(line, "ELSE takes no operands") end
          local state = condition_stack[#condition_stack]
          if not state then fail(line, "ELSE without IF") end
          if state.seen_else then fail(line, "duplicate ELSE") end
          state.seen_else = true; active = state.parent and not state.condition
        elseif mnemonic == "endif" then
          if trim(item.args) ~= "" then fail(line, "ENDIF takes no operands") end
          local state = table.remove(condition_stack)
          if not state then fail(line, "ENDIF without IF") end
          active = state.parent
        elseif active then
          if item.label and mnemonic ~= "equ" then symbols[item.label] = pc end
          if mnemonic then
            local args = operands(item.args, line)
            if mnemonic == "equ" then
            if not item.label then fail(line, "EQU needs a symbol name") end
            if pass == 1 then equ_pcs[item.label] = pc end
            local value, unresolved = expression(item.args,symbols,pc,line,pass==1)
            if not unresolved then symbols[item.label] = value end
            elseif mnemonic == "org" then
            if #args ~= 1 then fail(line, "ORG expects one address") end
            local address, unresolved = expression(args[1],symbols,pc,line,pass==1)
            if unresolved then fail(line, "ORG cannot use a forward reference") end
            if address < 0 or address > 0xffff then fail(line, "ORG address is outside 0..65535") end
            if base and address < pc then fail(line, "ORG cannot move backwards") end
            if pass == 2 and base then for _ = pc, address - 1 do output[#output + 1] = 0 end end
            pc = address; base = base or address
            elseif mnemonic == "db" or mnemonic == "defb" or mnemonic == "defm" then
            if #args == 0 then fail(line, "DB expects at least one value") end
            base = base or pc
            for _, arg in ipairs(args) do
              local text = quoted_string(arg,line)
              if text then for i=1,#text do if pass==2 then output[#output+1]=text:byte(i) end; pc=pc+1 end
              else local value=byte(expression(arg,symbols,pc,line,pass==1),line,"DB value"); if pass==2 then output[#output+1]=value end; pc=pc+1 end
            end
            elseif mnemonic == "dw" or mnemonic == "defw" then
            if #args == 0 then fail(line, "DW expects at least one value") end
            for _,arg in ipairs(args) do local lo,hi=word(expression(arg,symbols,pc,line,pass==1),line); if pass==2 then output[#output+1]=lo;output[#output+1]=hi end;pc=pc+2 end
            base = base or (pc - (#args * 2))
            elseif mnemonic == "ds" or mnemonic == "defs" or mnemonic == "fill" then
            if #args < 1 or #args > 2 then fail(line, "DS expects a count and optional fill byte") end
            local count, unresolved=expression(args[1],symbols,pc,line,pass==1)
            if unresolved then fail(line, "DS size cannot use a forward reference") end
            if count < 0 or count > 0xffff then fail(line, "DS size is outside 0..65535") end
            local fill=args[2] and byte(expression(args[2],symbols,pc,line,pass==1),line,"DS fill") or 0
            if pass==2 then for _=1,count do output[#output+1]=fill end end; base=base or pc;pc=pc+count
            elseif mnemonic == "align" then
              if #args < 1 or #args > 2 then fail(line, "ALIGN expects a boundary and optional fill byte") end
              local boundary, unresolved = expression(args[1],symbols,pc,line,pass==1)
              if unresolved then fail(line, "ALIGN boundary cannot use an unresolved reference") end
              if boundary < 1 or boundary > 0x10000 then fail(line, "ALIGN boundary is outside 1..65536") end
              local fill = args[2] and byte(expression(args[2],symbols,pc,line,pass==1),line,"ALIGN fill") or 0
              local count = (-pc) % boundary
              if pass == 2 then for _=1,count do output[#output+1]=fill end end
              base=base or pc; pc=pc+count
            elseif mnemonic == "assert" then
              if #args < 1 or #args > 2 then fail(line, "ASSERT expects an expression and optional message") end
              local value, unresolved = expression(args[1],symbols,pc,line,pass==1)
              if pass == 2 and (unresolved or value == 0) then
                local message = args[2] and quoted_string(args[2],line) or nil
                fail(line, message or "assertion failed")
              end
            elseif mnemonic == "entry" or mnemonic == "end" then
              if #args > 1 then fail(line, mnemonic:upper() .. " expects at most one address") end
              if #args == 1 then
                local value, unresolved = expression(args[1],symbols,pc,line,pass==1)
                if pass == 2 and unresolved then fail(line, "entry point is unresolved") end
                if not unresolved then
                  if value < 0 or value > 0xffff then fail(line, "entry point is outside 0..65535") end
                  entry = value
                end
              end
              if mnemonic == "end" then stopped = true end
            else
            local bytes=encode(mnemonic,args,symbols,pc,line,pass==1)
            base=base or pc; if pass==2 then for _,value in ipairs(bytes) do output[#output+1]=value end end; pc=pc+#bytes
            end
          end
        end
        if pc > 0x10000 then fail(line, "assembled output extends past address 65535") end
      end
    end
    if #condition_stack ~= 0 then fail(lines[#lines].number, "IF without ENDIF") end
    if pass == 1 then
      origin, final_size = base or 0x8000, pc - (base or pc)
      -- Address labels are now known; finish any EQU chain that refers to one.
      for _ = 1, #lines do
        local changed = false
        for name, item in pairs(equations) do
          local value, unresolved = expression(item.args,symbols,equ_pcs[name] or 0,item.number,true)
          if not unresolved and symbols[name] ~= value then symbols[name], changed = value, true end
        end
        if not changed then break end
      end
      for name, item in pairs(equations) do
        local _, unresolved = expression(item.args,symbols,equ_pcs[name] or 0,item.number,true)
        if unresolved then fail(item.number, "unresolved or circular EQU '" .. name .. "'") end
      end
    else
      if #output ~= final_size then error("Internal assembler pass-size mismatch.",0) end
      local chunks={}; for i=1,#output do chunks[i]=string.char(output[i]) end
      local names={}; for name in pairs(symbols) do names[#names+1]=name end; table.sort(names)
      return table.concat(chunks), origin, symbols, {
        entry=entry or origin, end_address=origin + #output,
        symbol_count=#names,
      }
    end
  end
end

return assembler
