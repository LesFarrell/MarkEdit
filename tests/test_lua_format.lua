-- Tests for the bundled lua-format plugin in plugins/lua-format.

-- Tinker Editor gives each plugin a require() that loads from its own folder;
-- this stands in for it.
local loaded = {}
function require(name)
  if loaded[name] == nil then loaded[name] = assert(loadfile("plugins/lua-format/" .. name .. ".lua"))() end
  return loaded[name]
end

local format = require("format")

-- Checks the output and that formatting it again changes nothing.
local function expect(input, expected, label)
  local got = format.format(input)
  assert(got == expected, ("%s\n--- input ---\n%q\n--- expected ---\n%q\n--- got ---\n%q")
    :format(label or "case", input, expected, got))
  assert(format.format(got) == got, (label or "case") .. ": not idempotent")
end

-- ---- Block structure ----

expect("function f()\nreturn 1\nend\n", "function f()\n  return 1\nend\n", "function")

expect("if a then\nx()\nelseif b then\ny()\nelse\nz()\nend\n",
  "if a then\n  x()\nelseif b then\n  y()\nelse\n  z()\nend\n", "if/elseif/else")

expect("while a do\nb()\nend\nfor i = 1, 3 do\nc()\nend\nrepeat\nd()\nuntil e\n",
  "while a do\n  b()\nend\nfor i = 1, 3 do\n  c()\nend\nrepeat\n  d()\nuntil e\n", "loops")

expect("do\nlocal x = 1\nend\n", "do\n  local x = 1\nend\n", "do block")

expect("if a then\nif b then\nc()\nend\nd()\nend\n",
  "if a then\n  if b then\n    c()\n  end\n  d()\nend\n", "nested")

expect("if a then b() else c() end\n", "if a then b() else c() end\n", "one line if")

expect("if a then\nx()\nelse y()\nz()\nend\n",
  "if a then\n  x()\nelse y()\n  z()\nend\n", "else with body")

-- ---- Table braces ----

expect("local t = {\na = 1,\nb = {\nc = 2,\n},\n}\n",
  "local t = {\n  a = 1,\n  b = {\n    c = 2,\n  },\n}\n", "tables")

expect("local t = { a = 1, b = 2 }\n", "local t = { a = 1, b = 2 }\n", "inline table")

expect("local t = {\nf = function()\nreturn 1\nend,\n}\n",
  "local t = {\n  f = function()\n    return 1\n  end,\n}\n", "function value in table")

expect("local t = {\nfunction()\nreturn 1\nend\n}\n",
  "local t = {\n  function()\n    return 1\n  end\n}\n", "brace after end")

-- ---- Strings and comments must not move the depth ----

expect('local s = "then end do function"\nlocal q = \'elseif until\'\n',
  'local s = "then end do function"\nlocal q = \'elseif until\'\n', "keywords in strings")

expect('local s = "a--b"\nif x then\ny()\nend\n',
  'local s = "a--b"\nif x then\n  y()\nend\n', "comment marker in string")

expect("local s = 'it\\'s'\nif x then\ny()\nend\n",
  "local s = 'it\\'s'\nif x then\n  y()\nend\n", "escaped quote")

expect("local v = t[1]\nif x then\ny()\nend\n",
  "local v = t[1]\nif x then\n  y()\nend\n", "indexing bracket")

expect("if x then -- end\n-- then\ny()\nend\n",
  "if x then -- end\n  -- then\n  y()\nend\n", "short comments")

-- ---- Long strings and long comments are preserved exactly ----

expect("function f()\nlocal s = [[\nthen\nend\n]]\nend\n",
  "function f()\n  local s = [[\nthen\nend\n]]\nend\n", "long string body kept")

expect("if a then\n--[[\nend\n]]\nb()\nend\n",
  "if a then\n  --[[\nend\n]]\n  b()\nend\n", "long comment body kept")

expect("local s = [==[\nif then end\n]==]\nif x then\ny()\nend\n",
  "local s = [==[\nif then end\n]==]\nif x then\n  y()\nend\n", "levelled long bracket")

expect("local s = [[\nkeep   \n]]\n", "local s = [[\nkeep   \n]]\n", "trailing space in long string")

-- ---- Whitespace ----

expect("local x = 1   \nlocal y = 2\t\n", "local x = 1\nlocal y = 2\n", "trailing whitespace")

expect("local  x  =  1\n", "local  x  =  1\n", "internal spacing kept")

expect("if a then\n   \nend\n", "if a then\n\nend\n", "blank line to empty")

expect("", "", "empty document")

expect("::top::\ngoto top\n", "::top::\ngoto top\n", "labels and goto")

-- ---- Line endings, ranges and indent units ----

expect("function f()\r\nreturn 1\r\nend\r\n", "function f()\r\n  return 1\r\nend\r\n", "crlf kept")

local ranged = format.format("a = 1\nif x then\nb = 2\nc = 3\nend\n", { first = 4, last = 4 })
assert(ranged == "a = 1\nif x then\nb = 2\n  c = 3\nend\n", "range only reindents the given lines")

local ranged_code = format.format("if x then\ny = 2\nend\n", { first = 2, last = 2 })
assert(ranged_code == "if x then\n  y = 2\nend\n", "range uses the surrounding depth")

local tabbed = format.format("if a then\nb()\nend\n", { indent = "\t" })
assert(tabbed == "if a then\n\tb()\nend\n", "custom indent unit")

print("All lua-format tests passed.")
