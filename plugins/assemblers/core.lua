-- The CPU-independent half of the bundled assemblers: comments, operands,
-- expressions, labels (including .local ones), EQU, IF/ELSE/ENDIF, the data
-- directives, and the two-pass layout. Each CPU module (z80.lua, chip8.lua,
-- mos6502.lua) supplies a description:
--   name            shown in messages
--   default_origin  the address used when the source has no ORG
--   memory_size     addresses run from 0 to memory_size - 1
--   big_endian      true when DW stores the high byte first
--   encode(mnemonic, args, symbols, pc, line, first_pass, memo)
--                   returns the instruction's bytes as a list; memo is a
--                   table that lasts for one assembly, for decisions that
--                   must not change between the passes (such as choosing a
--                   shorter zero-page form)
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

local function quoted_string(value)
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

function assembler.assemble(source, cpu)
  local lines = parse_source(source)
  local memo, top = {}, cpu.memory_size - 1

  -- Resolve constant-only EQU chains before layout so forward constants can
  -- be used by ORG, DS, ALIGN and IF. Equations involving address labels are
  -- completed after the first layout pass. Only EQUs on lines that IF/ELSE
  -- leaves active are seeded; a block whose IF cannot be resolved yet is
  -- skipped until a later round resolves it. A `$` not followed by a hex digit
  -- is the current address (as in the lexer), which is unknown here.
  local seeded = {}
  for _ = 1, #lines do
    local changed, active, stack = false, true, {}
    for _, item in ipairs(lines) do
      local mnemonic = item.mnemonic
      if mnemonic == "if" then
        local condition = nil
        if active then
          local value, unresolved = expression(item.args, seeded, 0, item.number, true)
          if not unresolved and not (item.args .. " "):find("%$[^%x]") then condition = value ~= 0 end
        end
        stack[#stack + 1] = {parent=active, condition=condition}
        active = active and condition == true
      elseif mnemonic == "else" and #stack > 0 then
        local state = stack[#stack]
        active = state.parent and state.condition == false
      elseif mnemonic == "endif" and #stack > 0 then
        active = table.remove(stack).parent
      elseif active and mnemonic == "equ" and item.label and seeded[item.label] == nil
          and not (item.args .. " "):find("%$[^%x]") then
        local value, unresolved = expression(item.args, seeded, 0, item.number, true)
        if not unresolved then seeded[item.label], changed = value, true end
      end
    end
    if not changed then break end
  end

  local symbols, equ_pcs, origin, final_size = {}, {}, nil, nil
  local definitions, equations = {}, {}
  for name, value in pairs(seeded) do symbols[name] = value end
  for pass = 1, 2 do
    local pc, output, stopped, active = cpu.default_origin, {}, false, true
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
          if item.label and pass == 1 then
            if definitions[item.label] then fail(line, "duplicate symbol '" .. item.label .. "'") end
            definitions[item.label] = line
            if mnemonic == "equ" then equations[item.label] = item end
          end
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
            if address < 0 or address > top then fail(line, ("ORG address is outside 0..%d"):format(top)) end
            if base and address < pc then fail(line, "ORG cannot move backwards") end
            if pass == 2 and base then for _ = pc, address - 1 do output[#output + 1] = 0 end end
            pc = address; base = base or address
            elseif mnemonic == "db" or mnemonic == "defb" or mnemonic == "defm" then
            if #args == 0 then fail(line, "DB expects at least one value") end
            base = base or pc
            for _, arg in ipairs(args) do
              local text = quoted_string(arg)
              if text then for i=1,#text do if pass==2 then output[#output+1]=text:byte(i) end; pc=pc+1 end
              else local value=byte(expression(arg,symbols,pc,line,pass==1),line,"DB value"); if pass==2 then output[#output+1]=value end; pc=pc+1 end
            end
            elseif mnemonic == "dw" or mnemonic == "defw" then
            if #args == 0 then fail(line, "DW expects at least one value") end
            for _,arg in ipairs(args) do
              local lo,hi=word(expression(arg,symbols,pc,line,pass==1),line)
              if cpu.big_endian then lo,hi=hi,lo end
              if pass==2 then output[#output+1]=lo;output[#output+1]=hi end;pc=pc+2
            end
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
                local message = args[2] and quoted_string(args[2]) or nil
                fail(line, message or "assertion failed")
              end
            elseif mnemonic == "entry" or mnemonic == "end" then
              if #args > 1 then fail(line, mnemonic:upper() .. " expects at most one address") end
              if #args == 1 then
                local value, unresolved = expression(args[1],symbols,pc,line,pass==1)
                if pass == 2 and unresolved then fail(line, "entry point is unresolved") end
                if not unresolved then
                  if value < 0 or value > top then fail(line, ("entry point is outside 0..%d"):format(top)) end
                  entry = value
                end
              end
              if mnemonic == "end" then stopped = true end
            else
            local bytes=cpu.encode(mnemonic,args,symbols,pc,line,pass==1,memo)
            base=base or pc; if pass==2 then for _,value in ipairs(bytes) do output[#output+1]=value end end; pc=pc+#bytes
            end
          end
        end
        if pc > cpu.memory_size then fail(line, ("assembled output extends past address %d"):format(top)) end
      end
    end
    if #condition_stack ~= 0 then fail(lines[#lines].number, "IF without ENDIF") end
    if pass == 1 then
      origin, final_size = base or cpu.default_origin, pc - (base or pc)
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

-- Shared with the CPU modules.
assembler.fail, assembler.trim, assembler.lower = fail, trim, lower
assembler.expression, assembler.byte, assembler.word, assembler.emit_word = expression, byte, word, emit_word

return assembler
