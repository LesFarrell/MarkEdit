-- Dependency-free SQL formatting for selections or the whole document.
-- The tokenizer preserves quoted text and comments and only changes known
-- keywords, so identifiers and string contents are never case-folded.

local keywords = {}
for word in ("select from where group by order having limit offset fetch union all distinct " ..
    "insert into values update set delete create alter drop table view index as join inner " ..
    "left right full outer cross on and or not null is in exists between like case when then " ..
    "else end asc desc with recursive returning begin commit rollback primary key foreign " ..
    "references constraint default check unique true false over partition rows range current " ..
    "row preceding following cast collate database schema grant revoke"):gmatch("%S+") do
  keywords[word] = true
end

-- Multi-character operators, longest first so the tokenizer matches greedily.
local operators = {
  "->>", "#>>", "<->", "<=>", "!~*", "<<=", ">>=", "||/",
  "->", "#>", "#-", "::", "||", "<=", ">=", "<>", "!=", "<<", ">>", "@>", "<@", "&&",
  "?|", "?&", "@@", "!~", "~*", ":=", "=>", "|/", "^@"
}

local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

-- Position just past the quoted value whose opening quote is at `position`,
-- or nil when it is never closed. Doubled quotes are escapes, except in [].
local function quoted_end(text, position)
  local open = text:sub(position, position)
  local close = open == "[" and "]" or open
  position = position + 1
  while position <= #text do
    if text:sub(position, position) == close then
      if text:sub(position + 1, position + 1) == close and close ~= "]" then position = position + 2
      else return position + 1 end
    else position = position + 1 end
  end
  return nil
end

-- Tokens are held as parallel arrays (kind[i], value[i], first[i], last[i],
-- with n tokens in all) rather than one table per token: a table costs far
-- more than the four small fields it would hold, and at one table per
-- token - keywords, punctuation and all - that overhead alone could run
-- past the plugin memory limit well before a large SQL file's text does.
-- first/last record where a token starts and ends in `text`, so keyword
-- case can be changed in place without disturbing the layout around it.
local function tokenize(text)
  local tokens = { kind = {}, value = {}, first = {}, last = {}, n = 0 }
  local position, start = 1, 1
  local function add(kind, value)
    local n = tokens.n + 1
    tokens.kind[n] = kind
    tokens.value[n] = value
    tokens.first[n] = start
    tokens.last[n] = start + #value - 1
    tokens.n = n
  end
  while position <= #text do
    local character, next_character = text:sub(position, position), text:sub(position + 1, position + 1)
    start = position
    if character:match("%s") then
      position = position + 1
    elseif character == "-" and next_character == "-" then
      local finish = text:find("[\r\n]", position + 2) or (#text + 1)
      add("comment", text:sub(position, finish - 1)); position = finish
    elseif character == "/" and next_character == "*" then
      local finish = text:find("*/", position + 2, true)
      if not finish then return nil, "Unterminated block comment." end
      add("comment", text:sub(position, finish + 1)); position = finish + 2
    elseif character == "'" or character == '"' or character == "`" or character == "[" then
      local finish = quoted_end(text, position)
      if not finish then return nil, "Unterminated quoted value." end
      add(character == "'" and "string" or "identifier", text:sub(position, finish - 1)); position = finish
    elseif character:match("[NnEeBbXx]") and next_character == "'" then
      -- Prefixed string literals (N'', E'', B'', X'') are one token.
      local finish = quoted_end(text, position + 1)
      if not finish then return nil, "Unterminated quoted value." end
      add("string", text:sub(position, finish - 1)); position = finish
    elseif text:find("^[$@:?]@?[%w_]", position)
        and not text:sub(position - 1, position - 1):match("[%w_:]") then
      -- Parameters and variables ($1, @name, @@version, :name, ?1) are one
      -- token, so nothing gets spaced apart from its prefix. A bare @@ (text
      -- search) is an operator.
      local _, finish = text:find("^[$@:?]@?[%w_$]+", position)
      add("word", text:sub(position, finish)); position = finish + 1
    elseif character:match("[%a_\128-\255]") then
      -- Bytes 128-255 are word characters, so non-ASCII names stay whole.
      local finish = text:find("[^%w_$\128-\255]", position) or (#text + 1)
      local value = text:sub(position, finish - 1)
      add(keywords[value:lower()] and "keyword" or "word", value); position = finish
    elseif character:match("%d") or (character == "." and next_character:match("%d")
        and not text:sub(position - 1, position - 1):match("[%w_\"`%]]")) then
      -- digits[.digits][e[+-]digits] or .digits; other forms such as 0x1F
      -- fall back to a run of word characters.
      local _, finish = text:find("^%d*%.?%d*[eE][+-]?%d+", position)
      if not finish then finish = (text:find("[^%w_%.]", position) or (#text + 1)) - 1 end
      add("number", text:sub(position, finish)); position = finish + 1
    else
      local operator = character
      for _, candidate in ipairs(operators) do
        if text:sub(position, position + #candidate - 1) == candidate then operator = candidate; break end
      end
      add("symbol", operator); position = position + #operator
    end
  end
  return tokens
end

local function normalize_tokens(tokens, keyword_case)
  local output = {}
  for index = 1, tokens.n do
    local kind, raw_value = tokens.kind[index], tokens.value[index]
    local value = kind == "keyword" and
      (keyword_case == "lower" and raw_value:lower() or raw_value:upper()) or raw_value
    local previous = index - 1
    if previous >= 1 and tokens.kind[previous] == "comment" and tokens.value[previous]:sub(1, 2) == "--" then
      -- A line comment runs to the end of the line, so keep the break after it.
      output[#output + 1] = editor.eol()
    elseif #output > 0 then
      local no_space_before = raw_value == "," or raw_value == ";" or raw_value == ")"
        or raw_value == "("
        or raw_value == "." or raw_value == "::"
      local previous_value = previous >= 1 and tokens.value[previous]
      local no_space_after = previous_value and (previous_value == "(" or previous_value == "."
        or previous_value == "::")
      if not no_space_before and not no_space_after then output[#output + 1] = " " end
    end
    output[#output + 1] = value
  end
  return table.concat(output)
end

local clause_starts = {
  select=true, from=true, where=true, having=true, limit=true, offset=true, returning=true,
  insert=true, update=true, delete=true, values=true, set=true, union=true, with=true,
  join=true, group=true, order=true,
  create=true, alter=true, drop=true, begin=true, commit=true, rollback=true
}

-- Words that may precede JOIN; the whole phrase (LEFT OUTER JOIN) is one clause.
local join_qualifiers = { inner=true, left=true, right=true, full=true, cross=true, outer=true }

local function is_qualifier(tokens, index)
  return index >= 1 and index <= tokens.n and tokens.kind[index] == "keyword"
    and join_qualifiers[tokens.value[index]:lower()]
end

-- True when the qualifier at index begins a join phrase, not e.g. LEFT(name, 2).
local function starts_join(tokens, index)
  if is_qualifier(tokens, index - 1) then return false end
  while is_qualifier(tokens, index) do index = index + 1 end
  return index <= tokens.n and tokens.kind[index] == "keyword" and tokens.value[index]:lower() == "join"
end

local function format_tokens(tokens)
  local pieces, line, line_tail, depth, line_depth = {}, {}, "", 0, 0
  local function flush()
    if #line > 0 then
      pieces[#pieces + 1] = string.rep("  ", line_depth) .. table.concat(line)
      line, line_tail = {}, ""
    end
  end
  local function append(value)
    local no_space_before = value == "," or value == ";" or value == ")" or value == "("
      or value == "." or value == "::"
    local no_space_after = line_tail:sub(-1) == "(" or line_tail:sub(-1) == "."
      or line_tail == "::"
    if #line > 0 and not no_space_before and not no_space_after then line[#line + 1] = " " end
    if #line == 0 then line_depth = depth end
    line[#line + 1] = value
    line_tail = value:sub(-2)
  end
  local index = 1
  while index <= tokens.n do
    local kind, value = tokens.kind[index], tokens.value[index]
    local lower = value:lower()
    local phrase = lower
    local clause_start = kind == "keyword" and (clause_starts[lower] or join_qualifiers[lower])
    if clause_start then
      if lower == "group" or lower == "order" then
        clause_start = index + 1 <= tokens.n and tokens.value[index + 1]:lower() == "by"
        if clause_start then phrase = lower .. " by"; index = index + 1 end
      elseif lower == "join" then
        clause_start = not is_qualifier(tokens, index - 1)
      elseif join_qualifiers[lower] then
        clause_start = starts_join(tokens, index)
      end
    end
    if value == ")" then depth = math.max(0, depth - 1) end
    if clause_start then flush() end
    if value == "," then
      append(",")
    elseif value == ";" then
      append(";")
      flush()
    elseif value == "(" then
      append("(")
      depth = depth + 1
    else
      append(kind == "keyword" and phrase:upper() or value)
    end
    if kind == "comment" then flush() end
    index = index + 1
  end
  flush()
  return table.concat(pieces, editor.eol())
end

-- Changes the case of keywords only, leaving every other byte - line breaks
-- and indentation included - exactly as it was.
local function recase_keywords(text, tokens, keyword_case)
  local pieces, position = {}, 1
  for index = 1, tokens.n do
    if tokens.kind[index] == "keyword" then
      pieces[#pieces + 1] = text:sub(position, tokens.first[index] - 1)
      pieces[#pieces + 1] = keyword_case == "lower" and tokens.value[index]:lower() or tokens.value[index]:upper()
      position = tokens.last[index] + 1
    end
  end
  pieces[#pieces + 1] = text:sub(position)
  return table.concat(pieces)
end

local function transform(mode)
  local text, first, last = source_range()
  local tokens, problem = tokenize(text)
  if not tokens then ui.message("SQL error: " .. problem, nil, "warning"); return end
  if mode == "upper" or mode == "lower" then
    local result = recase_keywords(text, tokens, mode)
    editor.replace_range(first, last, result)
    editor.set_selection(first, first + #result - 1)
    return
  end
  local result = mode == "format" and format_tokens(tokens)
    or normalize_tokens(tokens, "upper")
  -- Keep the line ending that closed the source text.
  local trailing = text:match("\r\n$") or text:match("[\r\n]$")
  if trailing and result ~= "" then result = result .. trailing end
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

menu.add("/Tools/SQL/Format SQL", function() transform("format") end)
menu.add("/Tools/SQL/Minify SQL", function() transform("minify") end)
menu.separator("/Tools/SQL")
menu.add("/Tools/SQL/Uppercase Keywords", function() transform("upper") end)
menu.add("/Tools/SQL/Lowercase Keywords", function() transform("lower") end)
