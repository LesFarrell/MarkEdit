# Lua formatting and linting

Reindent the active Lua document from **Tools → Format Lua**, or validate its
syntax with **Tools → Lint Lua**. When lines are selected for formatting, only
those lines are reindented; the rest of the file is left alone.

The formatter is deliberately conservative. It rewrites only the whitespace at
the start of each line and removes trailing spaces and tabs. Code, strings and
comments are copied through byte for byte, so running it can never change what
a document means. Everything else a plugin's time budget allows is left to
more thorough tools.

The linter compiles the document with MarkEdit's embedded Lua parser but never
runs it. A syntax error is reported with the parser's message and its line is
selected in the editor. A valid document produces a short status message. It
checks syntax rather than style or undefined globals, so lint results match the
exact Lua version used to run MarkEdit plugins.

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
-- After (Tools > Format Lua)
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
| `init.lua` | The Format and Lint commands, caret handling and diagnostics |
| `format.lua` | The tokenizer and reindenter, which has no editor dependency |
| `lint.lua` | Non-executing syntax validation and diagnostic parsing |

`format.format(text, options)` is a pure function: it takes a string and
returns the formatted string. `options.first` and `options.last` limit the
1-based lines that change, and `options.indent` sets the indentation unit
(default two spaces). `lint.check(text, name)` returns `nil` for valid Lua or a
table containing `line` and `message` for the syntax error. The tests are in
`tests/test_lua_format.lua`.
