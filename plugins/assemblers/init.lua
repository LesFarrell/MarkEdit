-- Assemblers for the Z80 (ZX Spectrum TAP), CHIP-8 / SUPER-CHIP (.ch8),
-- 6502 (Commodore 64 PRG or raw binary) and Game Boy SM83 (.gb ROM), on the
-- Tools menu. Each assembles
-- the active document in two passes with the shared core.lua; the CPU
-- modules only encode instructions. README.md describes the syntax.

local core = require("core")
local z80 = require("z80")
local chip8 = require("chip8")
local mos6502 = require("mos6502")
local tap = require("tap")
local sm83 = require("sm83")
local gbrom = require("gbrom")

local function output_name(path, extension)
  if not path then return "program" .. extension end
  local changed, count = path:gsub("%.[^%.\\/]+$", extension)
  return count > 0 and changed or path .. extension
end

-- The file name without its folder or extension, for the name stored in a
-- Spectrum tape or the title in a Game Boy header.
local function base_name(path)
  local name = (path or "program"):match("([^\\/]+)$") or "program"
  return (name:gsub("%.[^%.]+$", ""))
end

local function word(value)
  return string.char(value & 0xff, (value >> 8) & 0xff)
end

-- Assembles the active document for `cpu`. On an error, reports it and
-- selects the failing line; returns nil then, or when nothing was produced.
local function assemble_source(cpu)
  local title = cpu.name .. " assembly failed"
  local source, path = editor.get_text(), editor.file_path()
  local ok, bytes, origin, symbols, info = pcall(core.assemble, source, cpu, editor.read_relative_file)
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

local function save(cpu, result, image, extension, what, open_after_save, application, arguments)
  if ui.save_binary(output_name(result.path, extension), image, "Save " .. what,
      open_after_save or false, application, arguments) and not open_after_save then
    ui.message(("Created %d-byte %s: %d code bytes, origin $%04X, entry $%04X, end $%04X, %d symbols.")
      :format(#image, what, #result.bytes, result.origin, result.info.entry,
        result.info.end_address, result.info.symbol_count), cpu.name .. " assembler")
  end
end

local function build_z80(open_after_save)
  local result = assemble_source(z80)
  if not result then return end
  local tap_ok, image = pcall(tap.code, base_name(result.path), result.bytes,
    result.origin, result.info.entry)
  if not tap_ok then
    ui.message(tostring(image), "Z80 TAP creation failed", "error")
    return
  end
  local application = open_after_save and config.get("z80_emulator_path") or nil
  local arguments = application and config.get("z80_emulator_arguments") or nil
  save(z80, result, image, ".tap", "ZX Spectrum TAP", open_after_save,
    application, arguments)
end

menu.add("/Tools/Assemblers/Z80 Assembler/Compile", function()
  build_z80(false)
end)

menu.add("/Tools/Assemblers/Z80 Assembler/Compile and Run", function()
  build_z80(true)
end)

menu.add("/Tools/Assemblers/Z80 Assembler/Configure Emulator...", function()
  local current = config.get("z80_emulator_path")
  local choice = ui.choose({
    "Use the Windows .tap file association",
    "Choose an emulator executable...",
  }, "How should Compile and Run open TAP files?", "Configure Z80 Emulator")
  if not choice then return end
  if choice == 1 then
    config.set("z80_emulator_path", nil)
    config.set("z80_emulator_arguments", nil)
    ui.status("Z80 Compile and Run will use the Windows .tap association.")
    return
  end
  local application = ui.pick_file("Select ZX Spectrum emulator")
  if not application then return end
  local arguments = ui.prompt(
    "Arguments placed before the TAP file (optional):",
    current == application and config.get("z80_emulator_arguments", "") or "",
    "Configure Z80 Emulator")
  if arguments == nil then return end
  config.set("z80_emulator_path", application)
  config.set("z80_emulator_arguments", arguments ~= "" and arguments or nil)
  ui.status("Z80 emulator configured for Compile and Run.")
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

-- A C64 .prg is the load address followed by the bytes; start it with
-- SYS <entry> after loading.
menu.add("/Tools/Assemblers/6502 Assembler/Compile C64 PRG", function()
  local result = assemble_source(mos6502)
  if result then save(mos6502, result, word(result.origin) .. result.bytes, ".prg", "C64 PRG") end
end)

menu.add("/Tools/Assemblers/6502 Assembler/Compile Raw Binary", function()
  local result = assemble_source(mos6502)
  if result then save(mos6502, result, result.bytes, ".bin", "6502 binary") end
end)

-- A Game Boy ROM: 32 KB with the cartridge header filled in (see gbrom.lua).
-- Code starts at $0150 unless ORG says otherwise; ENTRY, or the first byte
-- of code, is where the header's JP goes.
menu.add("/Tools/Assemblers/Game Boy Assembler/Compile ROM", function()
  local result = assemble_source(sm83)
  if not result then return end
  local ok, image = pcall(gbrom.image, base_name(result.path), result.bytes,
    result.origin, result.info.entry)
  if not ok then
    ui.message(tostring(image), "Game Boy ROM creation failed", "error")
    return
  end
  save(sm83, result, image, ".gb", "Game Boy ROM")
end)
