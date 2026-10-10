---@meta
-- MarkEdit Lua API definitions for Lua Language Server. This file documents
-- globals supplied by MarkEdit; it is not loaded as an extension.

---@class MarkEditSelection
---@field start integer
---@field finish integer
---@field caret integer
---@field anchor integer

---@class MarkEditMenu
menu = {}
---@param label string
---@param fn fun()
---@param shortcut? string Ctrl/Alt plus a letter, digit, F1-F24, Tab, Space, Insert or Delete.
function menu.add(label, fn, shortcut) end
---@param label string
---@param key string
---@param default boolean
---@param fn fun(checked:boolean)
---@param shortcut? string
function menu.checkbox(label, key, default, fn, shortcut) end
---@param path? string
function menu.separator(path) end

commands = {}
---@param name string
---@param fn fun()
---@param title? string
function commands.register(name, fn, title) end

events = {}
---@param name 'save'|'open'|'tab_changed'
---@param fn fun(path:string?)
function events.on(name, fn) end

config = {}
---@generic T
---@param key string
---@param default? T
---@return T?
function config.get(key, default) end
---@param key string
---@param value string|number|boolean|nil
function config.set(key, value) end

app = {}
---@return string
function app.date() end
---@return string
function app.time() end
---@return string
function app.date_time() end

ui = {}
---@param text string
---@param title? string
---@param kind? 'info'|'warning'|'error'
function ui.message(text, title, kind) end
---@param label string
---@param default? string
---@param title? string
---@return string?
function ui.prompt(label, default, title) end
---@param items string[]
---@param label? string
---@param title? string
---@return integer?, string?
function ui.choose(items, label, title) end

editor = {}
---@return string
function editor.get_text() end
---@param start integer
---@param finish integer
---@return string
function editor.get_range(start, finish) end
---@param n? integer
---@return string
function editor.get_selection(n) end
---@param n integer
---@return string
function editor.get_line(n) end
---@return integer
function editor.length() end
---@return string
function editor.eol() end
---@param text string
---@param n? integer
function editor.replace_selection(text, n) end
---@param start integer
---@param finish integer
---@param text string
function editor.replace_range(start, finish, text) end
---@param text string
---@param position? integer
function editor.insert(text, position) end
---@param text string
function editor.set_text(text) end
---@param n? integer
---@return integer start, integer finish
function editor.get_selection_range(n) end
---@param start integer
---@param finish integer
---@param n? integer
function editor.set_selection(start, finish, n) end
---@return integer
function editor.selection_count() end
---@return MarkEditSelection[]
function editor.get_selections() end
---@param texts string|string[]
function editor.replace_selections(texts) end
---@param start integer
---@param finish integer
---@return integer
function editor.add_selection(start, finish) end
---@param n integer
function editor.drop_selection(n) end
---@return integer
function editor.get_main_selection() end
---@param n integer
function editor.set_main_selection(n) end
---@param n? integer
---@return integer
function editor.get_caret(n) end
---@param position integer
---@param n? integer
function editor.set_caret(position, n) end
---@return integer
function editor.line_count() end
---@return integer
function editor.current_line() end
---@param position integer
---@return integer
function editor.line_at(position) end
---@param n integer
---@return integer start, integer finish
function editor.line_range(n) end
---@return string?
function editor.file_path() end
---@return string
function editor.language() end
