# Lua formatting

Reindent Lua from **Tools → Interpreters → Lua**:

- **Format Document** reindents the whole active document.
- **Format Selection** reindents only the selected lines, leaving the rest of the
  file alone, and keeps those lines selected. With nothing selected it asks
  you to select the lines first.
- **Check Syntax** compiles the document without running it and reports the
  first syntax error, or "No syntax errors.", in the output panel. Errors read
  `path:line: message`, so double-clicking one in a saved document goes to the
  line. It finds syntax errors only, not lint warnings such as unused
  variables. A `#!` first line is skipped, as the `lua` interpreter does.
- **Check Selection Syntax** does the same for only the selected text, such as
  one function or a Lua block inside another kind of document. Errors still
  give document line numbers, and a clean result names the lines checked
  ("Lines 4-9: No syntax errors."). The selection must be complete Lua on its
  own: selecting half of an `if` reports the missing `end`.

The formatter is deliberately conservative. It rewrites only the whitespace at
the start of each line and removes trailing spaces and tabs. Code, strings and
comments are copied through byte for byte, so running it can never change what
a document means. Everything else a plugin's time budget allows is left to
more thorough tools.

## What it does

- Indents block bodies one level for `function`, `then`, `do` and `repeat`, and
  closes them with `end` and `until`. `else` and `elseif` line up with their
  `if`.
- Indents a line that ends with `{` by one level and pulls a line that starts
  with `}` back, so table lists read the way people write them.
- Leaves the body of a multi-line long string or long comment (`[[ ... ]]` or
  `--[[ ... ]]`) exactly as it is, including trailing spaces, because that
  whitespace is part of the value.
- Keeps the document's line endings (`\r\n`, `\n` or `\r`) and the caret's line
  and column.

It does **not** wrap long lines, change spacing inside a line, align
continuation lines, sort requires, or add or remove parentheses. Those are
formatting decisions this plugin leaves untouched.

## Example

```lua
-- Before
if x then
y = 1
else
z = {
a = 1,
}
end
```

```lua
-- After (Tools > Interpreters > Lua > Format Document)
if x then
  y = 1
else
  z = {
    a = 1,
  }
end
```

## Files

| File | Contents |
|---|---|
| `init.lua` | The **Tools → Interpreters → Lua → Format Document**, **Format Selection**, **Check Syntax** and **Check Selection Syntax** commands, and caret handling |
| `format.lua` | The tokenizer and reindenter, which has no editor dependency |

`format.format(text, options)` is a pure function: it takes a string and
returns the formatted string. `options.first` and `options.last` limit the
1-based lines that change, and `options.indent` sets the indentation unit
(default two spaces). The tests are in `tests/test_lua_format.lua`.
