-- Calculate Selection, on the Plugins menu: adds the value after each
-- selected line of arithmetic, with math functions and constants,
-- hex/octal/binary numbers and bitwise operations.
--
-- Uses only the plugin API documented in README.md in this folder.

-- The names an expression can use, in lower case: math functions without
-- the "math." prefix, constants, and hex(), oct() and bin() to show the
-- value in another base. Names are matched ignoring case, so PI works too.
local CALCULATOR = {}
for _, name in ipairs({ "abs", "ceil", "floor", "sqrt", "exp", "log", "sin", "cos", "tan",
                        "asin", "acos", "atan", "min", "max", "fmod", "deg", "rad", "pi", "huge" }) do
  CALCULATOR[name] = math[name]
end
CALCULATOR.tau = 2 * math.pi
CALCULATOR.e = math.exp(1)
CALCULATOR.phi = (1 + math.sqrt(5)) / 2
CALCULATOR.inf = math.huge
CALCULATOR.maxint = math.maxinteger
CALCULATOR.minint = math.mininteger
setmetatable(CALCULATOR, { __index = function(names, name) return rawget(names, name:lower()) end })

-- A whole number to show in a base: what hex(), oct() and bin() return.
local BASED = {}

local function in_base(base, prefix)
  return function(value)
    local whole = math.tointeger(value)
    if not whole then error("only whole numbers can be shown in another base") end
    return setmetatable({ value = whole, base = base, prefix = prefix }, BASED)
  end
end

CALCULATOR.hex = in_base(16, "0x")
CALCULATOR.oct = in_base(8, "0o")
CALCULATOR.bin = in_base(2, "0b")
CALCULATOR.dec = function(value) return math.tointeger(value) or value end

-- Negative numbers show as their 64-bit two's complement, as in a
-- programmer's calculator. >> shifts in zeros, so the loop always ends.
local function format_based(based)
  local bits = based.base == 16 and 4 or based.base == 8 and 3 or 1
  local digits, value = {}, based.value
  repeat
    local digit = value & (based.base - 1)
    table.insert(digits, 1, ("0123456789ABCDEF"):sub(digit + 1, digit + 1))
    value = value >> bits
  until value == 0
  return based.prefix .. table.concat(digits)
end

local function format_number(value)
  if getmetatable(value) == BASED then return format_based(value) end
  if math.type(value) == "float" and value == math.floor(value) and math.abs(value) < 2 ^ 53 then
    return string.format("%d", value)
  end
  return math.type(value) == "integer" and tostring(value) or string.format("%.15g", value)
end

-- Words that stand for bitwise operators, matched ignoring case. Lua's own
-- and, or and not are logical, which means nothing to a calculator.
local WORD_OPERATORS = { ["and"] = "&", ["or"] = "|", xor = "~", ["not"] = "~", shl = "<<", shr = ">>" }

-- Rewrites what Lua doesn't read itself: binary (0b101) and octal (0o17)
-- numbers, and the word operators. Lua reads hex (0xFF) as it is.
local function to_lua_expression(expression)
  return (expression
    :gsub("%f[%w_.]0[bB]([01]+)%f[^%w_.]", function(digits) return tostring(tonumber(digits, 2)) end)
    :gsub("%f[%w_.]0[oO]([0-7]+)%f[^%w_.]", function(digits) return tostring(tonumber(digits, 8)) end)
    :gsub("%f[%w_]([%a_][%w_]*)", function(word) return WORD_OPERATORS[word:lower()] end))
end

-- One line of arithmetic with " = " and its value added, or just the value
-- added when the line already ends in "=". Returns nil and the reason when
-- the line can't be calculated.
local function calculate_line(line)
  local expression, equals = line:match("^(.-)(%s*=%s*)$")
  expression = expression or line
  -- Only numbers, operators, brackets and names: no quotes, so no strings,
  -- and only the names in CALCULATOR mean anything.
  if not expression:find("^[%w%s%.%+%-%*/%%%^%(%),_&|~<>]+$") then
    return nil, "Only numbers, + - * / % ^ // & | ~ << >>, brackets, and functions such as sqrt() can be calculated."
  end
  local chunk = load("return " .. to_lua_expression(expression), "=expression", "t", CALCULATOR)
  local ok, value = false, nil
  if chunk then ok, value = pcall(chunk) end
  if not ok or (type(value) ~= "number" and getmetatable(value) ~= BASED) then
    return nil, "\"" .. expression .. "\" isn't an expression that can be calculated."
  end
  local result = format_number(value)
  if not equals then return line:match("^(.-)%s*$") .. " = " .. result end
  return line .. (equals:find(" $") and "" or " ") .. result
end

-- Adds the value after each selected line of arithmetic, such as
-- "(2 + 3) * 4" or "sqrt(2) / 2", giving "(2 + 3) * 4 = 20". Blank lines
-- are left alone, and nothing changes if any line can't be calculated.
-- Expressions use Lua's operators: ^ is a power, % the remainder, //
-- division rounded down, and & | ~ << >> are bitwise and, or, xor (or not,
-- before a number), and shifts. and, or, xor, not, shl and shr can be
-- written as words. Numbers can be written in hex (0xFF), octal (0o17) or
-- binary (0b101), and hex(), oct() and bin() show the answer in that base.
-- Trigonometry is in radians: rad() converts degrees, deg() converts back.
menu.add("Calculate Selection", function()
  local start = editor.get_selection_range()
  local selected = editor.get_selection()
  if not selected:find("%S") then
    ui.message("Select an expression to calculate, such as (2 + 3) * 4.")
    return
  end
  local problem
  local text = selected:gsub("[^\r\n]+", function(line)
    if problem or not line:find("%S") then return nil end
    local result, reason = calculate_line(line)
    problem = problem or reason
    return result
  end)
  if problem then
    ui.message(problem, nil, "warning")
    return
  end
  editor.replace_selection(text)
  editor.set_selection(start, start + #text - 1)
end)
