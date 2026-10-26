-- Integer base, Roman-numeral and Unix timestamp/ISO 8601 date conversion
-- for every current selection.

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

-- ---- Unix timestamps and ISO 8601 dates, in UTC ----
-- Plugins have no os library, so the calendar arithmetic is done here:
-- the proleptic Gregorian calendar, as in Howard Hinnant's days_from_civil.

-- Timestamps at least this far from 1970 are in milliseconds: 10^11
-- seconds is the year 5138, while 10^11 milliseconds is March 1973.
local MILLISECOND_THRESHOLD = 100000000000

local function days_from_civil(year, month, day)
  year = month <= 2 and year - 1 or year
  local era = year // 400
  local year_of_era = year - era * 400
  local day_of_year = (153 * (month + (month > 2 and -3 or 9)) + 2) // 5 + day - 1
  local day_of_era = year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
  return era * 146097 + day_of_era - 719468
end

local function civil_from_days(days)
  days = days + 719468
  local era = days // 146097
  local day_of_era = days - era * 146097
  local year_of_era = (day_of_era - day_of_era // 1460 + day_of_era // 36524
    - day_of_era // 146096) // 365
  local day_of_year = day_of_era - (365 * year_of_era + year_of_era // 4 - year_of_era // 100)
  local shifted_month = (5 * day_of_year + 2) // 153
  local day = day_of_year - (153 * shifted_month + 2) // 5 + 1
  local month = shifted_month + (shifted_month < 10 and 3 or -9)
  return year_of_era + era * 400 + (month <= 2 and 1 or 0), month, day
end

local function days_in_month(year, month)
  if month == 2 then
    return (year % 4 == 0 and (year % 100 ~= 0 or year % 400 == 0)) and 29 or 28
  end
  return (month == 4 or month == 6 or month == 9 or month == 11) and 30 or 31
end

-- "2024-03-01T12:30:00Z" for a timestamp in seconds, or with milliseconds,
-- "2024-03-01T12:30:00.250Z", for one in milliseconds.
local function timestamp_to_iso(text)
  local value = math.tointeger(tonumber((text:match("^%s*([+-]?%d+)%s*$"))))
  if not value then return nil end
  local milliseconds
  if math.abs(value) >= MILLISECOND_THRESHOLD then value, milliseconds = value // 1000, value % 1000 end
  local days, seconds = value // 86400, value % 86400
  local year, month, day = civil_from_days(days)
  if year < 0 or year > 9999 then return nil end
  local iso = string.format("%04d-%02d-%02dT%02d:%02d:%02d", year, month, day,
    seconds // 3600, seconds % 3600 // 60, seconds % 60)
  return iso .. (milliseconds and string.format(".%03dZ", milliseconds) or "Z")
end

-- Seconds since 1970 for an ISO 8601 date: "2024-03-01", optionally with a
-- time after a T or space ("12:30", "12:30:05", "12:30:05.25") and a "Z" or
-- "+01:00"/"-0500" offset. A time without an offset is taken as UTC. A time
-- with fractional seconds gives milliseconds instead.
local function iso_to_timestamp(text)
  text = text:match("^%s*(.-)%s*$")
  local year, month, day, rest = text:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)(.*)$")
  if not year then return nil end
  year, month, day = tonumber(year), tonumber(month), tonumber(day)
  if month < 1 or month > 12 or day < 1 or day > days_in_month(year, month) then return nil end
  local hour, minute, second, fraction, offset = 0, 0, 0, nil, 0
  if rest ~= "" then
    local h, m, clock_rest = rest:match("^[Tt ](%d%d):(%d%d)(.*)$")
    if not h then return nil end
    hour, minute, rest = tonumber(h), tonumber(m), clock_rest
    local s, after_seconds = rest:match("^:(%d%d)(.*)$")
    if s then second, rest = tonumber(s), after_seconds end
    local f, after_fraction = rest:match("^[.,](%d+)(.*)$")
    if f then fraction, rest = tonumber((f .. "00"):sub(1, 3)), after_fraction end
    if rest ~= "Z" and rest ~= "z" and rest ~= "" then
      local sign, oh, om = rest:match("^([+-])(%d%d):?(%d%d)$")
      if not sign or tonumber(oh) > 23 or tonumber(om) > 59 then return nil end
      offset = (tonumber(oh) * 3600 + tonumber(om) * 60) * (sign == "-" and -1 or 1)
    end
    if hour > 23 or minute > 59 or second > 59 then return nil end
  end
  local seconds = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60
    + second - offset
  return tostring(fraction and seconds * 1000 + fraction or seconds)
end

local function convert_dates(converter, what)
  local selections, replacements = editor.get_selections(), {}
  for index = 1, #selections do
    local text = editor.get_selection(index)
    if text == "" then ui.message("Select one or more " .. what .. " first.", nil, "warning"); return end
    replacements[index] = converter(text)
    if not replacements[index] then
      ui.message("Not a supported " .. what:gsub("s$", "") .. ": " .. text, nil, "warning"); return
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
menu.separator(ROOT)
menu.add(ROOT .. "/Unix Timestamp to ISO Date (UTC)", function()
  convert_dates(timestamp_to_iso, "Unix timestamps")
end)
menu.add(ROOT .. "/ISO Date to Unix Timestamp", function()
  convert_dates(iso_to_timestamp, "ISO 8601 dates")
end)
