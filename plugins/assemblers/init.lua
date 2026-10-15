-- Assemblers for the Z80 (ZX Spectrum TAP), CHIP-8 / SUPER-CHIP (.ch8) and
-- 6502 (Commodore 64 PRG or raw binary), on the Tools menu. Each assembles
-- the active document in two passes with the shared core.lua; the CPU
-- modules only encode instructions. README.md describes the syntax.

local core = require("core")
local z80 = require("z80")
local chip8 = require("chip8")
local mos6502 = require("mos6502")
local tap = require("tap")
local c64_prg = require("c64_prg")

local function output_name(path, extension)
  if not path then return "program" .. extension end
  local changed, count = path:gsub("%.[^%.\\/]+$", extension)
  return count > 0 and changed or path .. extension
end

local function spectrum_name(path)
  local name = (path or "program"):match("([^\\/]+)$") or "program"
  return (name:gsub("%.[^%.]+$", ""))
end

-- Assembles the active document for `cpu`. On an error, reports it and
-- selects the failing line; returns nil then, or when nothing was produced.
local function assemble_source(cpu)
  local title = cpu.name .. " assembly failed"
  local source, path = editor.get_text(), editor.file_path()
  local ok, bytes, origin, symbols, info = pcall(core.assemble, source, cpu)
  if not ok then
    local message = tostring(bytes)
    local line = tonumber(message:match("^Line (%d+):"))
    ui.message(message, title, "error")
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
    ui.message("Failed to assemble the source: no machine code produced.", title, "warning")
    return nil
  end
  return {bytes=bytes, origin=origin, symbols=symbols, info=info, path=path}
end

local function save(cpu, result, image, extension, what)
  if ui.save_binary(output_name(result.path, extension), image, "Save " .. what) then
    ui.message(("Created %d-byte %s: %d code bytes, origin $%04X, entry $%04X, end $%04X, %d symbols.")
      :format(#image, what, #result.bytes, result.origin, result.info.entry,
        result.info.end_address, result.info.symbol_count), cpu.name .. " assembler")
  end
end

menu.add("/Tools/Assemblers/Z80 Assembler/Compile", function()
  local result = assemble_source(z80)
  if not result then return end
  local tap_ok, image = pcall(tap.code, spectrum_name(result.path), result.bytes,
    result.origin, result.info.entry)
  if not tap_ok then
    ui.message(tostring(image), "Z80 TAP creation failed", "error")
    return
  end
  save(z80, result, image, ".tap", "ZX Spectrum TAP")
end)

menu.add("/Tools/Assemblers/CHIP-8 Assembler/Compile", function()
  local result = assemble_source(chip8)
  if not result then return end
  -- A .ch8 file has no header: interpreters load it at $200 and start there.
  if result.origin ~= 0x200 then
    ui.message(("CHIP-8 programs load at $200, but this one starts at $%03X. Use ORG $200.")
      :format(result.origin), "CHIP-8 assembly failed", "error")
    return
  end
  save(chip8, result, result.bytes, ".ch8", "CHIP-8 program")
end)

-- A self-starting C64 .prg loads at BASIC start. RUN executes its SYS stub;
-- the machine-code loader relocates the payload to ORG and jumps to ENTRY.
menu.add("/Tools/Assemblers/6502 Assembler/Compile C64 PRG", function()
  local result = assemble_source(mos6502)
  if not result then return end
  local prg_ok, image = pcall(c64_prg.code, result.bytes, result.origin, result.info.entry)
  if not prg_ok then
    ui.message(tostring(image), "C64 PRG creation failed", "error")
    return
  end
  save(mos6502, result, image, ".prg", "self-starting C64 PRG")
end)

menu.add("/Tools/Assemblers/6502 Assembler/Compile Raw Binary", function()
  local result = assemble_source(mos6502)
  if result then save(mos6502, result, result.bytes, ".bin", "6502 binary") end
end)
