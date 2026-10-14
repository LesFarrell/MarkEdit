-- Integer base and Roman-numeral conversion for every current selection.

local integer_to_roman

local function roman_to_integer(text)
  local values = { I = 1, V = 5, X = 10, L = 50, C = 100, D = 500, M = 1000 }
  text = text:upper()
  if text == "" or text:find("[^IVXLCDM]") then return nil end
  local total, previous = 0, 0
  for index = #text, 1, -1 do
    local value = values[text:sub(index, index)]
    total = total + (value < previous and -value or value)
    previous = value
  end
  if integer_to_roman and integer_to_roman(total) ~= text then return nil end
  return total
end

integer_to_roman = function(value)
  if value < 1 or value > 3999 then return nil end
  local result = {}
  for _, item in ipairs({ {1000,"M"}, {900,"CM"}, {500,"D"}, {400,"CD"},
      {100,"C"}, {90,"XC"}, {50,"L"}, {40,"XL"}, {10,"X"}, {9,"IX"},
      {5,"V"}, {4,"IV"}, {1,"I"} }) do
    while value >= item[1] do result[#result + 1], value = item[2], value - item[1] end
  end
  return table.concat(result)
end

local function parse_integer(text)
  text = text:match("^%s*(.-)%s*$")
  local sign, body = text:match("^([+-]?)(.+)$")
  if not body then return nil end
  local base = 10
  if body:match("^0[xX]") then base, body = 16, body:sub(3)
  elseif body:match("^0[bB]") then base, body = 2, body:sub(3)
  elseif body:match("^0[oO]") then base, body = 8, body:sub(3)
  elseif body:match("^[IVXLCDMivxlcdm]+$") then
    local value = roman_to_integer(body)
    if not value then return nil end
    return sign == "-" and -value or value
  end
  if body == "" or (base == 10 and body:find("[^0-9]"))
    or (base == 16 and body:find("[^0-9A-Fa-f]"))
    or (base == 8 and body:find("[^0-7]")) or (base == 2 and body:find("[^01]")) then
    return nil
  end
  -- Accumulate as a negative number so math.mininteger is reachable and any
  -- value outside the 64-bit range is rejected instead of wrapping.
  local limit = sign == "-" and math.mininteger or -math.maxinteger
  local value = 0
  for index = 1, #body do
    local digit = tonumber(body:sub(index, index), base)
    if value < (limit + digit + base - 1) // base then return nil end
    value = value * base - digit
  end
  return sign == "-" and value or -value
end

-- Digits of the magnitude of value; works on the negative side so that
-- math.mininteger, whose magnitude has no positive integer, converts too.
local function digits(value, base)
  if value == 0 then return "0" end
  local output = {}
  if value > 0 then value = -value end
  while value < 0 do
    local digit = -(value % -base)
    output[#output + 1] = ("0123456789ABCDEF"):sub(digit + 1, digit + 1)
    value = (value + digit) // base
  end
  return table.concat(output):reverse()
end

local function convert(format)
  local selections, replacements = editor.get_selections(), {}
  for index = 1, #selections do
    local text = editor.get_selection(index)
    if text == "" then ui.message("Select one or more integers first.", nil, "warning"); return end
    local value = parse_integer(text)
    if not value then ui.message("Not a supported whole number: " .. text, nil, "warning"); return end
    if format == "decimal" then replacements[index] = tostring(value)
    elseif format == "hex" then replacements[index] = (value < 0 and "-0x" or "0x") .. digits(value, 16)
    elseif format == "binary" then replacements[index] = (value < 0 and "-0b" or "0b") .. digits(value, 2)
    elseif format == "octal" then replacements[index] = (value < 0 and "-0o" or "0o") .. digits(value, 8)
    else
      replacements[index] = integer_to_roman(value)
      if not replacements[index] then
        ui.message("Roman numerals support values from 1 to 3999.", nil, "warning"); return
      end
    end
  end
  editor.replace_selections(replacements)
end

local ROOT = "/Tools/Number Converter"
menu.add(ROOT .. "/To Decimal", function() convert("decimal") end)
menu.add(ROOT .. "/To Hexadecimal", function() convert("hex") end)
menu.add(ROOT .. "/To Binary", function() convert("binary") end)
menu.add(ROOT .. "/To Octal", function() convert("octal") end)
menu.add(ROOT .. "/To Roman Numerals", function() convert("roman") end)
