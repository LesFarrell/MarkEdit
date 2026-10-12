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

local function source_range()
  local first, last = editor.get_selection_range()
  if last < first then return editor.get_text(), 1, editor.length() end
  return editor.get_range(first, last), first, last
end

local function tokenize(text)
  local tokens, position = {}, 1
  local function add(kind, value) tokens[#tokens + 1] = { kind = kind, value = value } end
  while position <= #text do
    local character, next_character = text:sub(position, position), text:sub(position + 1, position + 1)
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
      local close = character == "[" and "]" or character
      local start = position
      position = position + 1
      while position <= #text do
        if text:sub(position, position) == close then
          if text:sub(position + 1, position + 1) == close and close ~= "]" then position = position + 2
          else position = position + 1; break end
        else position = position + 1 end
      end
      if text:sub(position - 1, position - 1) ~= close then return nil, "Unterminated quoted value." end
      add(character == "'" and "string" or "identifier", text:sub(start, position - 1))
    elseif character:match("[%a_]") then
      local finish = text:find("[^%w_$]", position) or (#text + 1)
      local value = text:sub(position, finish - 1)
      add(keywords[value:lower()] and "keyword" or "word", value); position = finish
    elseif character:match("%d") then
      local finish = text:find("[^%w_%.]", position) or (#text + 1)
      add("number", text:sub(position, finish - 1)); position = finish
    else
      local pair = text:sub(position, position + 1)
      if pair == "<>" or pair == "!=" or pair == "<=" or pair == ">=" or pair == "||" or pair == "::" then
        add("symbol", pair); position = position + 2
      else add("symbol", character); position = position + 1 end
    end
  end
  return tokens
end

local function word_like(token)
  return token and (token.kind == "word" or token.kind == "keyword" or token.kind == "number"
    or token.kind == "string" or token.kind == "identifier")
end

local function normalize_tokens(tokens, keyword_case)
  local output = {}
  for index, token in ipairs(tokens) do
    local value = token.kind == "keyword" and
      (keyword_case == "lower" and token.value:lower() or token.value:upper()) or token.value
    local previous = tokens[index - 1]
    if #output > 0 then
      local no_space_before = token.value == "," or token.value == ";" or token.value == ")"
        or token.value == "("
        or token.value == "." or token.value == "::"
      local no_space_after = previous and (previous.value == "(" or previous.value == "."
        or previous.value == "::")
      if not no_space_before and not no_space_after then output[#output + 1] = " " end
    end
    output[#output + 1] = value
  end
  return table.concat(output)
end

local clause_starts = {
  select=true, from=true, where=true, having=true, limit=true, offset=true, returning=true,
  insert=true, update=true, delete=true, values=true, set=true, union=true, with=true,
  join=true, inner=true, left=true, right=true, full=true, cross=true,
  create=true, alter=true, drop=true, begin=true, commit=true, rollback=true
}

local function format_tokens(tokens)
  local pieces, line, depth = {}, "", 0
  local function flush()
    if line ~= "" then pieces[#pieces + 1] = string.rep("  ", depth) .. line; line = "" end
  end
  local function append(value)
    local no_space_before = value == "," or value == ";" or value == ")" or value == "("
      or value == "." or value == "::"
    local tail = line:sub(-1)
    local no_space_after = tail == "(" or tail == "." or line:sub(-2) == "::"
    if line ~= "" and not no_space_before and not no_space_after then line = line .. " " end
    line = line .. value
  end
  local index = 1
  while index <= #tokens do
    local token = tokens[index]
    local lower = token.value:lower()
    local phrase = lower
    if token.kind == "keyword" and (lower == "group" or lower == "order") and
       tokens[index + 1] and tokens[index + 1].value:lower() == "by" then
      phrase = lower .. " by"; index = index + 1
    end
    if token.value == ")" then depth = math.max(0, depth - 1) end
    if token.kind == "keyword" and clause_starts[lower] then flush() end
    if token.value == "," then
      append(",")
    elseif token.value == ";" then
      append(";")
      flush()
    elseif token.value == "(" then
      append("(")
      depth = depth + 1
    else
      append(token.kind == "keyword" and phrase:upper() or token.value)
    end
    if token.kind == "comment" then flush() end
    index = index + 1
  end
  flush()
  return table.concat(pieces, editor.eol())
end

local function transform(mode)
  local text, first, last = source_range()
  local tokens, problem = tokenize(text)
  if not tokens then ui.message("SQL error: " .. problem, nil, "warning"); return end
  local result = mode == "format" and format_tokens(tokens)
    or normalize_tokens(tokens, mode == "lower" and "lower" or "upper")
  editor.replace_range(first, last, result)
  editor.set_selection(first, first + #result - 1)
end

menu.add("/Tools/SQL/Format SQL", function() transform("format") end)
menu.add("/Tools/SQL/Minify SQL", function() transform("minify") end)
menu.separator("/Tools/SQL")
menu.add("/Tools/SQL/Uppercase Keywords", function() transform("upper") end)
menu.add("/Tools/SQL/Lowercase Keywords", function() transform("lower") end)
