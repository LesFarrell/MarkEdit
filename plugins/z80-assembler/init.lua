-- Z80 assembler and ZX Spectrum TAP exporter.
-- Source is assembled in two passes; ORG defaults to 32768 ($8000).

local assembler = require("assembler")
local tap = require("tap")

local function output_name(path, extension)
  if not path then return "program" .. extension end
  local changed, count = path:gsub("%.[^%.\\/]+$", extension)
  return count > 0 and changed or path .. extension
end

local function spectrum_name(path)
  local name = (path or "program"):match("([^\\/]+)$") or "program"
  return (name:gsub("%.[^%.]+$", ""))
end

local function assemble_source()
  local source, path = editor.get_text(), editor.file_path()
  local ok, bytes, origin, symbols, info = pcall(assembler.assemble, source)
  if not ok then
    local message = tostring(bytes)
    local line = tonumber(message:match("^Line (%d+):"))
    ui.message(message, "Z80 assembly failed", "error")
    if line and line >= 1 and line <= editor.line_count() then
      -- The diagnostic must still be visible if selection cannot be changed
      -- in an unusual pane state, so highlighting is deliberately best-effort.
      pcall(function()
        local first, last = editor.line_range(line)
        editor.set_selection(first, last)
      end)
    end
    return nil
  end
  if #bytes == 0 then
    ui.message("The source produced no machine code.", "Z80 assembly failed", "warning")
    return nil
  end
  return {bytes=bytes, origin=origin, symbols=symbols, info=info, path=path}
end

menu.add("Z80 Assembler/Compile", function()
  local result = assemble_source()
  if not result then return end
  local tap_ok, image = pcall(tap.code, spectrum_name(result.path), result.bytes,
    result.origin, result.info.entry)
  if not tap_ok then
    ui.message(tostring(image), "Z80 TAP creation failed", "error")
    return
  end
  if ui.save_binary(output_name(result.path, ".tap"), image, "Save ZX Spectrum TAP") then
    ui.message(("Created %d-byte TAP: %d code bytes, origin $%04X, entry $%04X, end $%04X, %d symbols.")
      :format(#image, #result.bytes, result.origin, result.info.entry,
        result.info.end_address, result.info.symbol_count), "Z80 assembler")
  end
end)
