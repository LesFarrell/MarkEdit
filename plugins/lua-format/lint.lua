-- Lua syntax linting with the interpreter MarkEdit already embeds. Compiling
-- a chunk validates all Lua grammar without executing any of the document.

local lint = {}

function lint.check(text, name)
  if type(text) ~= "string" then error("Lua source must be a string", 2) end
  local chunk_name = "@" .. (name or "document.lua")
  local chunk, message = load(text, chunk_name, "t", {})
  if chunk then return nil end

  message = tostring(message or "Invalid Lua syntax")
  local line, detail = message:match(":(%d+):%s*(.*)$")
  return {
    line = tonumber(line) or 1,
    message = detail and detail ~= "" and detail or message,
  }
end

return lint
