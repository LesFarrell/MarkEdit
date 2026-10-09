-- Run Brainfuck, on the Plugins menu: runs the selected text as a
-- Brainfuck program and inserts what it prints on a new line after the
-- selection, leaving it selected.
--
-- Every character other than the eight commands + - < > . , [ ] is a
-- comment. The program has 30,000 cells of 8 bits each, which wrap around
-- (255 + 1 is 0, 0 - 1 is 255), and starts at the first. If the program
-- reads input (,), a box asks for it before it runs: \n in it is a line
-- break, \t a tab and \\ a backslash. Reading past the end of the input
-- sets the cell to 0.
--
-- A program that runs for too long, or prints too much, is stopped, with a
-- message saying why, and what it printed so far is inserted.
--
-- Uses only the plugin API documented in README.md in this folder.

local TAPE_SIZE = 30000

-- Well inside the 5 second limit commands have, so a program that never
-- finishes gets a clear message instead of a timeout error.
local MAX_STEPS = 20000000

local MAX_OUTPUT = 100000

-- Kinds of instruction. Runs of + and -, and of < and >, become one ADD or
-- MOVE by their total, and [-] (or [+]) becomes CLEAR.
local ADD, MOVE, OUTPUT, INPUT, OPEN, CLOSE, CLEAR = 1, 2, 3, 4, 5, 6, 7

local DELTA = { ["+"] = 1, ["-"] = -1, [">"] = 1, ["<"] = -1 }

-- Returns the program as { kinds = {...}, args = {...} }, or nil and the
-- position in program of an unmatched bracket and a message. An OPEN's arg
-- is the index of its CLOSE, and the reverse.
local function compile(program)
  local kinds, args = {}, {}
  local opens, openPositions = {}, {}
  for position, c in program:gmatch("()([%+%-<>%.,%[%]])") do
    local kind = (c == "+" or c == "-") and ADD or (c == "<" or c == ">") and MOVE
    local last = #kinds
    if kind and kinds[last] == kind then
      args[last] = args[last] + DELTA[c]
    elseif kind then
      kinds[last + 1], args[last + 1] = kind, DELTA[c]
    elseif c == "." then
      kinds[last + 1] = OUTPUT
    elseif c == "," then
      kinds[last + 1] = INPUT
    elseif c == "[" then
      kinds[last + 1] = OPEN
      opens[#opens + 1], openPositions[#opens + 1] = last + 1, position
    else
      local open = opens[#opens]
      if not open then return nil, position, "This ] has no [ before it to match." end
      opens[#opens], openPositions[#opens] = nil, nil
      if last == open + 1 and kinds[last] == ADD and args[last] % 2 == 1 then
        -- An odd step reaches 0 from any 8-bit value.
        kinds[last], args[last] = nil, nil
        kinds[open], args[open] = CLEAR, nil
      else
        kinds[last + 1], args[last + 1] = CLOSE, open
        args[open] = last + 1
      end
    end
  end
  if #opens > 0 then
    return nil, openPositions[#opens], "This [ has no ] after it to match."
  end
  return { kinds = kinds, args = args }
end

-- Runs a compiled program, reading from input. Returns what it printed,
-- and nil, or a message saying why it was stopped.
local function run(code, input)
  local kinds, args = code.kinds, code.args
  local tape = {}
  for i = 1, TAPE_SIZE do tape[i] = 0 end
  local out, printed = {}, 0
  local pointer, pc, steps, read = 1, 1, 0, 0
  local count = #kinds
  while pc <= count do
    steps = steps + 1
    if steps > MAX_STEPS then
      return table.concat(out), string.format("The program was stopped after %d steps, in case it never finishes.", MAX_STEPS)
    end
    local kind = kinds[pc]
    if kind == ADD then
      tape[pointer] = (tape[pointer] + args[pc]) & 255
    elseif kind == MOVE then
      pointer = pointer + args[pc]
      if pointer < 1 then
        return table.concat(out), "The program moved left of the first cell."
      elseif pointer > TAPE_SIZE then
        return table.concat(out), string.format("The program moved right of the last of its %d cells.", TAPE_SIZE)
      end
    elseif kind == OUTPUT then
      if printed == MAX_OUTPUT then
        return table.concat(out), string.format("The program was stopped after printing %d bytes.", MAX_OUTPUT)
      end
      printed = printed + 1
      out[printed] = string.char(tape[pointer])
    elseif kind == INPUT then
      read = read + 1
      tape[pointer] = input:byte(read) or 0
    elseif kind == OPEN then
      if tape[pointer] == 0 then pc = args[pc] end
    elseif kind == CLOSE then
      if tape[pointer] ~= 0 then pc = args[pc] end
    else -- CLEAR
      tape[pointer] = 0
    end
    pc = pc + 1
  end
  return table.concat(out)
end

local INPUT_ESCAPES = { n = "\n", t = "\t", ["\\"] = "\\" }

-- What the program printed, made fit for the document: NUL bytes, and
-- anything that isn't UTF-8 text, become escapes, and line breaks become
-- the document's line ending.
local function as_document_text(bytes)
  if utf8.len(bytes) then
    bytes = bytes:gsub("\0", "\\0")
  else
    bytes = bytes:gsub("[\0\128-\255]", function(c) return string.format("\\x%02X", c:byte()) end)
  end
  return (bytes:gsub("\r?\n", editor.eol()))
end

menu.add("Brainfuck", function()
  local start, finish = editor.get_selection_range()
  if finish < start then
    ui.message("Select the Brainfuck text to run first.")
    return
  end
  local program = editor.get_range(start, finish)

  local code, position, problem = compile(program)
  if not code then
    -- Select the bracket, so it's easy to find.
    local at = start + position - 1
    editor.set_selection(at, at)
    ui.message(string.format("Line %d: %s", editor.line_at(at), problem), nil, "warning")
    return
  end
  if #code.kinds == 0 then
    ui.message("The selection has no Brainfuck commands (+ - < > . , [ ]) in it.")
    return
  end

  local input = ""
  if program:find(",", 1, true) then
    local answer = ui.prompt("Input for the program (\\n for a line break):", "")
    if answer == nil then return end
    input = answer:gsub("\\(.)", INPUT_ESCAPES)
  end

  local output, stopped = run(code, input)
  if output ~= "" then
    local text = as_document_text(output)
    local at = finish + 1 + #editor.eol()
    editor.insert(editor.eol() .. text, finish + 1)
    editor.set_selection(at, at + #text - 1)
  end
  if stopped then
    if output ~= "" then stopped = stopped .. " What it printed so far has been inserted." end
    ui.message(stopped, nil, "warning")
  elseif output == "" then
    ui.message("The program finished without printing anything.")
  end
end)
