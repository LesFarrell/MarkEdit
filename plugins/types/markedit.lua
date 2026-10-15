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
---Raises an error for the run.* commands, which plugins can't execute.
---@param name string
---@return boolean queued
function commands.execute(name) end

events = {}
---@param name 'before_save'|'save'|'after_save'|'open'|'close'|'tab_changed'|'text_changed'|'selection_changed'|'workspace_changed'|'theme_changed'|'shutdown'
---@param fn fun(path: string|nil)
---@return integer subscription
function events.on(name, fn) end
---@param subscription integer
---@return boolean removed
function events.off(subscription) end

config = {}
---@generic T
---@param key string
---@param default? T
---@return T?
function config.get(key, default) end
---@param key string
---@param value string|number|boolean|table|nil
function config.set(key, value) end

app = {}
---@return string
function app.date() end
---@return string
function app.time() end
---@return string
function app.date_time() end
---@return string
function app.version() end
---@return integer
function app.api_version() end
---@return 'windows'
function app.platform() end
---@return integer
function app.memory_used() end
---@return string?
function app.workspace_path() end
---@return table<string, boolean>
function app.capabilities() end
---@param milliseconds integer 0 to 60000.
---@param fn fun()
---@return integer token
function app.defer(milliseconds, fn) end
---@param token integer
---@return boolean
function app.cancel(token) end

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

---@param default_name string
---@param data string Binary data; embedded NUL bytes are preserved.
---@param title? string
---@return boolean saved False when the user cancels the Save As dialog.
function ui.save_binary(default_name, data, title) end
---@param text string
---@param title? string
---@return boolean
function ui.confirm(text, title) end
---@param text string
function ui.status(text) end
---@param text string
function ui.log(text) end
---@param title? string
---@return string?
function ui.pick_file(title) end
---@param title? string
---@return string?
function ui.pick_folder(title) end

documents = {}
---@alias MarkEditEncoding 'utf-8'|'utf-8-bom'|'utf-16le'|'utf-16be'|'ansi'
---@return integer
function documents.count() end
---@return integer
function documents.active() end
---@param index integer
---@return boolean
function documents.activate(index) end
---@return boolean
function documents.new() end
---Asks the user first unless the file is already open in a tab.
---@param path string
---@return boolean
function documents.open(path) end
---@param save_as? boolean
---@return boolean
function documents.save(save_as) end
---@param index? integer
---@return boolean
function documents.close(index) end
---@param index? integer
---@return string?
function documents.path(index) end
---@param index? integer
---@return boolean
function documents.modified(index) end
---Gets or changes whether the active document automatically follows external file updates.
---Transient access/read failures remain pending and are retried quietly.
---@param enabled? boolean
---@return boolean following
function documents.follow(enabled) end
---@overload fun(): MarkEditEncoding
---@param value MarkEditEncoding
---@return boolean success
function documents.encoding(value) end

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
---@alias MarkEditLineEnding '\r\n'|'\n'|'\r'
---@overload fun(): MarkEditLineEnding
---@param value MarkEditLineEnding
---@return MarkEditLineEnding
function editor.eol(value) end
---@return integer
function editor.tab_width() end
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
---@return boolean
function editor.modified() end
---@param value? boolean
---@return boolean
function editor.read_only(value) end
---@return integer start, integer finish
function editor.visible_range() end
---@param position integer
function editor.scroll_to(position) end
---@class MarkEditFindOptions
---@field match_case boolean|nil
---@field whole_word boolean|nil
---@field word_start boolean|nil
---@field regex boolean|nil

---@param text string
---@param start? integer
---@param options? MarkEditFindOptions
---@return integer? start, integer? finish
function editor.find(text, start, options) end
function editor.begin_undo() end
function editor.end_undo() end
---@param line integer
---@param text? string
---@param style? integer
function editor.annotation(line, text, style) end
function editor.clear_annotations() end
---@param line integer
---@param marker? integer 8 to 19.
---@param enabled? boolean
function editor.marker(line, marker, enabled) end
---@param marker? integer 8 to 19; omit to clear every plugin marker.
function editor.clear_markers(marker) end
---@param start integer
---@param finish integer
---@param number integer 8 to 19.
---@param colour? integer RGB value, for example 0xffc107.
function editor.indicator(start, finish, number, colour) end
---@param number integer
---@param start? integer
---@param finish? integer
function editor.clear_indicator(number, start, finish) end
---@param line integer
---@param expanded? boolean
---@return boolean
function editor.fold(line, expanded) end
---@param position integer
---@return integer
function editor.style_at(position) end
---@param text? string When supplied, replaces the clipboard text.
---@return string?
function editor.clipboard(text) end
