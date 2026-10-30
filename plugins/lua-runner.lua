-- Runs Lua code with the Lua built into MarkEdit, on Tools > Interpreters > Lua:
--   Run Document        the whole document
--   Run Selection       the selected text
--   Evaluate Selection  shows the value of an expression, as a REPL would
-- print(), io.write(), errors, io.read() and io.lines() all use the Terminal
-- tab. Error text includes a stack trace and path:line locations when the
-- document is saved. io.read() and io.lines() take input from the terminal. The
-- script's last partial line ("Name? ") acts as its prompt; Escape or closing
-- terminal session ends the input. Unlike
-- Run (Ctrl+F5), this needs no Lua installed, and the code runs in the same
-- sandbox as plugins: the basic functions and the string, table, math, utf8
-- and coroutine libraries, plus editor, ui, output, problems, terminal and app,
-- so a script can
-- work on the open document. There is no io or os, so no file access, and
-- the 5 second limit on a command applies. Changes a script makes to the
-- document are one undo step, undone if it fails.

local BASIC = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen",
  "rawset", "select", "setmetatable", "getmetatable", "tonumber", "tostring", "type",
  "xpcall", "load", "_VERSION",
}
local LIBRARIES = {
  "string", "table", "math", "utf8", "coroutine", "editor", "ui",
  "output", "problems", "terminal", "app",
}

local function copy(source)
  local result = {}
  for key, value in pairs(source) do result[key] = value end
  return result
end

-- What has been written since the last line break: the question io.read()
-- asks, as in io.write("Name? ").
local partial_line = ""
local input_buffer = ""
local input_eof = false

local function write_text(text)
  terminal.write((text:gsub("\r\n", "\n"):gsub("\n", "\r\n")))
  partial_line = (partial_line .. text):match("[^\n]*$")
end

local function write_values(separator, ...)
  local parts = {}
  for index = 1, select("#", ...) do parts[index] = tostring((select(index, ...))) end
  write_text(table.concat(parts, separator))
end

-- One line from the terminal, or nil when the user cancels (the end of
-- input). terminal.read echoes the answer.
local function terminal_line()
  local question = partial_line:match("^%s*(.-)%s*$")
  local answer = terminal.read(question == "" and "Input for the Lua script: " or nil)
  if answer then partial_line = "" end
  return answer
end

local function fill_input()
  if input_eof then return false end
  local line = terminal_line()
  if not line then
    input_eof = true
    return false
  end
  input_buffer = input_buffer .. line .. "\n"
  return true
end

local function read_line(with_break)
  while true do
    local ending = input_buffer:find("\n", 1, true)
    if ending then
      local value = input_buffer:sub(1, ending - (with_break and 0 or 1))
      input_buffer = input_buffer:sub(ending + 1)
      return value
    end
    if not fill_input() then
      if input_buffer == "" then return nil end
      local value = input_buffer
      input_buffer = ""
      return value
    end
  end
end

local function read_count(count)
  if count < 0 or count % 1 ~= 0 then error("invalid format", 3) end
  while #input_buffer < count and fill_input() do end
  if count == 0 then return "" end
  if input_buffer == "" then return nil end
  local value = input_buffer:sub(1, count)
  input_buffer = input_buffer:sub(#value + 1)
  return value
end

local function read_number()
  while true do
    while not input_buffer:find("%S") and fill_input() do end
    input_buffer = input_buffer:gsub("^%s+", "")
    if input_buffer == "" then return nil end
    local token, rest = input_buffer:match("^(%S+)(.*)$")
    local value = tonumber(token)
    input_buffer = rest or ""
    if value ~= nil then return value end
    -- Interactive numeric input is friendlier when a mistyped token can be
    -- corrected on the next line instead of ending the script immediately.
  end
end

local function read_all()
  while fill_input() do end
  local value = input_buffer
  input_buffer = ""
  return value
end

local function read_one(format)
  if type(format) == "number" then return read_count(format) end
  if type(format) ~= "string" then error("bad argument to 'read' (invalid format)", 3) end
  local kind = format:gsub("^%*", "")
  if kind == "l" then return read_line(false) end
  if kind == "L" then return read_line(true) end
  if kind == "n" then return read_number() end
  if kind == "a" then return read_all() end
  error("bad argument to 'read' (invalid format)", 3)
end

-- io.read supports multiple formats against one shared input buffer, including
-- *a across as many terminal lines as the user enters (Escape ends input).
local function read(...)
  local formats = select("#", ...) > 0 and { ... } or { "l" }
  local values = {}
  for index, format in ipairs(formats) do
    values[index] = read_one(format)
    if values[index] == nil then return table.unpack(values, 1, index) end
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
  local stdout, stderr, stdin = {}, {}, {}
  function stdout:write(...) write_values("", ...); return self end
  function stdout:flush() return true end
  function stderr:write(...) write_values("", ...); return self end
  function stderr:flush() return true end
  function stdin:read(...) return read(...) end
  function stdin:lines(...)
    local formats = table.pack(...)
    return function() return read(table.unpack(formats, 1, formats.n)) end
  end
  env.io = {
    stdin = stdin, stdout = stdout, stderr = stderr,
    write = function(...) return stdout:write(...) end,
    -- Writes reach the terminal grid synchronously, so there is no buffered
    -- stream to drain. Keep standard prompt-and-read scripts compatible.
    flush = function() return true end,
    read = read,
    lines = function(...) return stdin:lines(...) end,
    type = function(value)
      if value == stdin or value == stdout or value == stderr then return "file" end
      return nil
    end,
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
  input_buffer = ""
  input_eof = false
  local env = new_environment()
  local chunk, problem = load(code, chunk_name(), "t", env)
  terminal.begin()
  terminal.clear()
  terminal.write(string.char(27) .. "[1;36m" .. title .. string.char(27) .. "[0m\r\n")
  if not chunk then
    terminal.write(string.char(27) .. "[31m" .. problem .. string.char(27) .. "[0m\r\n\r\n")
    terminal.finish()
    return
  end
  local results = table.pack(xpcall(chunk, function(message)
    local text = tostring(message)
    if text == "interrupted" or text:match(":%d+: interrupted$") then return "interrupted" end
    return app.traceback(text)
  end))
  if not results[1] then
    local message = results[2] == "interrupted" and "[Interrupted]" or results[2]
    terminal.write(string.char(27) .. "[31m" .. message .. string.char(27) .. "[0m\r\n\r\n")
    terminal.finish()
    return
  end
  if results.n > 1 then
    terminal.write("=> ")
    write_values("\t", table.unpack(results, 2, results.n))
    terminal.write("\r\n")
  end
  terminal.write("\r\n")
  terminal.finish()
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
