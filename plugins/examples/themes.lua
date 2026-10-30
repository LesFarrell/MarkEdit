-- Example themes.lua: Solarized colours for Tinker Editor's light and dark themes.
--
-- To use it, copy this file to %APPDATA%\tinkeredit\themes.lua (or paste the
-- parts you want into the one Plugins > Customize Theme Colours creates),
-- then choose Plugins > Reload Plugins. Delete that file to go back to the
-- built-in colours.
--
-- A themes.lua returns { light = { ... }, dark = { ... } }, each mapping a
-- colour name to "#RRGGBB". Any colour left out keeps its built-in value, so
-- a file can change just one or two. The names, and what each one paints,
-- are listed in the file Customize Theme Colours writes.
--
-- The file is ordinary Lua with the base and string libraries, so colours
-- can be computed. This one builds both themes from the same function and
-- blends a few in-between shades with mix().

-- The Solarized palette (https://ethanschoonover.com/solarized/).
local base03, base02 = "#002B36", "#073642"
local base01, base00 = "#586E75", "#657B83"
local base0, base1 = "#839496", "#93A1A1"
local base2, base3 = "#EEE8D5", "#FDF6E3"
local yellow, orange, red = "#B58900", "#CB4B16", "#DC322F"
local magenta, violet, blue = "#D33682", "#6C71C4", "#268BD2"
local cyan, green = "#2AA198", "#859900"

local function channels(colour)
  return tonumber(colour:sub(2, 3), 16), tonumber(colour:sub(4, 5), 16), tonumber(colour:sub(6, 7), 16)
end

-- The colour `amount` of the way from `from` to `to` (0 gives `from`, 1 `to`).
local function mix(from, to, amount)
  local r1, g1, b1 = channels(from)
  local r2, g2, b2 = channels(to)
  local function blend(a, b) return (a + (b - a) * amount + 0.5) // 1 end
  return string.format("#%02X%02X%02X", blend(r1, r2), blend(g1, g2), blend(b1, b2))
end

-- Solarized's light and dark modes use the same accents; only the four
-- background and text tones swap places.
local function solarized(background, highlight, text, emphasis, muted)
  return {
    -- Window chrome
    bg = background,
    fg = text,
    border = mix(background, text, 0.25),
    accent = blue,
    subtleBg = highlight,
    pressedBg = mix(highlight, text, 0.15),
    highlightBg = highlight,
    selectionBg = mix(background, blue, 0.3),
    selectionFg = emphasis,
    disabledFg = muted,
    menuDisabledFg = muted,
    scrollThumb = mix(background, text, 0.3),
    scrollThumbHot = mix(background, text, 0.5),
    scrollThumbDrag = mix(background, text, 0.7),
    tabBg = highlight,
    tabSelectedBg = background,
    tabFg = muted,
    tabSelectedFg = emphasis,
    changedMarker = yellow,
    minimapFrame = muted,
    previewTextFg = text,

    -- Editor
    lineNumber = muted,
    faint = mix(background, text, 0.2),
    braceBg = mix(background, cyan, 0.25),
    error = red,
    bookmarkFore = blue,
    bookmarkBack = mix(background, blue, 0.6),
    breakpointFore = background,
    breakpointBack = red,
    debugLineBack = mix(background, yellow, 0.35),
    debugArrowBack = orange,
    lineChanged = yellow,
    lineSaved = green,

    -- Markdown
    heading = blue,
    link = violet,
    code = cyan,
    quote = muted,
    list = magenta,

    -- Code
    comment = muted,
    keyword = green,
    string = cyan,
    number = magenta,
    functionName = blue,
    variable = orange,
    tag = blue,
    attribute = yellow,
    operatorColour = text,
  }
end

return {
  light = solarized(base3, base2, base00, base01, base1),
  dark = solarized(base03, base02, base0, base1, base01),
}
