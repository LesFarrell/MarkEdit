-- Runs Lua code with the Lua built into MarkEdit, on Tools > Interpreters > Lua:
--   Run Document        the whole document
--   Run Selection       the selected text
--   Evaluate Selection  shows the value of an expression, as a REPL would
-- print() and io.write() go to the output panel, and so do errors, as
-- path:line locations to double-click when the document is saved.
-- io.read() and io.lines() ask for each line in a box, titled with what the
-- script last wrote ("Name? "); Cancel ends the input. Unlike
-- Run (Ctrl+F5), this needs no Lua installed, and the code runs in the same
-- sandbox as plugins: the basic functions and the string, table, math, utf8
-- and coroutine libraries, plus editor, ui, output and app, so a script can
-- work on the open document. There is no io or os, so no file access, and
-- the 5 second limit on a command applies. Changes a script makes to the
-- document are one undo step, undone if it fails.

local BASIC = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen",
  "rawset", "select", "setmetatable", "getmetatable", "tonumber", "tostring", "type",
  "xpcall", "load", "_VERSION",
}
local LIBRARIES = { "string", "table", "math", "utf8", "coroutine", "editor", "ui", "output", "app" }

local function copy(source)
  local result = {}
  for key, value in pairs(source) do result[key] = value end
  return result
end

-- What has been written since the last line break: the question io.read()
-- asks, as in io.write("Name? ").
local partial_line = ""

local function write_text(text)
  output.write(text)
  partial_line = (partial_line .. text):match("[^\n]*$")
end

local function write_values(separator, ...)
  local parts = {}
  for index = 1, select("#", ...) do parts[index] = tostring((select(index, ...))) end
  write_text(table.concat(parts, separator))
end

-- One line from the user, asked for in a box, or nil when they cancel (the
-- end of input). The answer is echoed to the output, as a terminal would.
local function read_line()
  local question = partial_line:match("^%s*(.-)%s*$")
  local answer = ui.prompt(question ~= "" and question or "Input for the Lua script:", "", "Lua Input")
  if answer then write_text(answer .. "\n") end
  return answer
end

-- io.read's formats: "l" a line, "L" a line with its line break, "n" a
-- number, "a" everything left (one line here), or a count of characters.
-- Each argument reads one value, as in standard Lua.
local function read(...)
  local formats = select("#", ...) > 0 and { ... } or { "l" }
  local values = {}
  for index, format in ipairs(formats) do
    local line = read_line()
    -- At the end of input the format that hit it gives nil, as in standard Lua.
    if not line then return table.unpack(values, 1, index) end
    if type(format) == "number" then values[index] = line:sub(1, format)
    else
      local kind = tostring(format):gsub("^%*", ""):sub(1, 1)
      if kind == "n" then values[index] = tonumber(line)
      elseif kind == "L" then values[index] = line .. "\n"
      else values[index] = line end
    end
  end
  return table.unpack(values, 1, #formats)
end

-- A fresh environment for each run, so one script's globals don't leak into
-- the next, and copies of the libraries, so a script can't change the
-- plugin's own.
local function new_environment()
  local env = {}
  for _, name in ipairs(BASIC) do env[name] = _ENV[name] end
  for _, name in ipairs(LIBRARIES) do env[name] = copy(_ENV[name]) end
  env.print = function(...)
    write_values("\t", ...)
    write_text("\n")
  end
  env.io = {
    write = function(...) write_values("", ...) end,
    read = read,
    lines = function() return function() return read_line() end end,
  }
  env._G = env
  return env
end

-- The chunk name errors are reported with: the document's path, so the
-- output panel can open the line, or its tab title when it's unsaved.
local function chunk_name()
  return "=" .. (editor.file_path() or "untitled")
end

-- The line offset for a selection, so error lines match the document.
local function prefix_lines(first)
  local lines = editor.line_at(first) - 1
  return string.rep("\n", lines)
end

local function run(code, title)
  partial_line = ""
  local env = new_environment()
  local chunk, problem = load(code, chunk_name(), "t", env)
  output.write(string.char(27) .. "[1;36m" .. title .. string.char(27) .. "[0m\n")
  output.show()
  if not chunk then
    output.write(string.char(27) .. "[31m" .. problem .. string.char(27) .. "[0m\n\n")
    return
  end
  local results = table.pack(xpcall(chunk, function(message) return tostring(message) end))
  if not results[1] then
    output.write(string.char(27) .. "[31m" .. results[2] .. string.char(27) .. "[0m\n\n")
    return
  end
  if results.n > 1 then
    output.write("=> ")
    write_values("\t", table.unpack(results, 2, results.n))
    output.write("\n")
  end
  output.write("\n")
end

local function selected_code()
  local text = editor.get_selection()
  if text == "" then
    ui.message("Select the Lua code to run first.", nil, "warning")
    return nil
  end
  local first = editor.get_selection_range()
  return prefix_lines(first) .. text
end

menu.add("/Tools/Interpreters/Lua/Run Document", function()
  run(editor.get_text(), "Lua: " .. (editor.file_path() or "untitled"))
end)

menu.add("/Tools/Interpreters/Lua/Run Selection", function()
  local code = selected_code()
  if code then run(code, "Lua: selection") end
end)

-- An expression is tried first, as "return <selection>", so "2^10" shows
-- 1024; anything that isn't one runs as statements.
menu.add("/Tools/Interpreters/Lua/Evaluate Selection", function()
  local code = selected_code()
  if not code then return end
  local body = code:match("^\n*(.-)%s*$")
  local leading = code:match("^\n*")
  local expression = leading .. "return " .. body
  if load(expression, chunk_name(), "t", {}) then code = expression end
  local title = body:gsub("%s+", " ")
  if (utf8.len(title) or 0) > 60 then title = title:sub(1, utf8.offset(title, 61) - 1) end
  run(code, "Lua: " .. title)
end)
