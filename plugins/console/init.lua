-- The console: a log of entries that other plugins add, each a box with its level's colour, its time and its text. An entry
-- may name a file and a line, such as "components/coin.lua:12", and a click on it runs the command of the setting
-- console.editor, which opens the file there in the user's code editor. An entry equal to the one before it stacks on it.

local ecs = require("ecs")
local ui = require("ui")
local process = require("process")

local STYLES = ui.styles()
local LIMIT = 2000
local LEVELS = { "info", "warn", "error" }
local NAMES = { info = "Info", warn = "Warnings", error = "Errors" }
local COLORS = { warn = "#e5c07b", error = "#e06c75" }

---@class console.Entry
---@field text string what was added, which may hold many lines
---@field level string info, warn or error
---@field time string when it was added last, as hours, minutes and seconds
---@field count integer how many times it was added in a row
---@field file string? the first file that it names
---@field line integer? the line of that file
---@field links console.Link[]? more links, which the plugin that added it gives

---A link of an entry's, after its place: a text, and what a click on it does.
---@class console.Link
---@field text string
---@field tooltip string?
---@field onClick fun()

local entries = {} ---@type console.Entry[]
local panels = {} ---@type table<ecs.Panel, table>
local folder = "" -- the folder that the files that entries name are in

ecs.setting.declare({
  name = "clearOnRun",
  type = "bool",
  description = "Whether the console clears when the tool starts a run of its program, such as a game that plays",
  default = true,
})

ecs.setting.declare({
  name = "editor",
  type = "string",
  description = "The command that opens a file at a line in a code editor; {file} and {line} stand for them",
  default = "code --goto {file}:{line}",
})

local warned = false -- the user was told that the code editor did not start
local add

local function redraw()
  for panel, state in pairs(panels) do
    state.follow = true
    panel:redraw()
  end
end

local isWindows = package.config:sub(1, 1) == "\\"

local function isFile(path)
  local file = io.open(path, "rb")
  if file then
    file:close()
  end
  return file ~= nil
end

-- The path of a command's program: a path as it is, and a name in the folders of PATH. On Windows, a name may lack its ending,
-- such as code for code.cmd, so each ending of PATHEXT is tried too; Windows starts a .cmd or .bat found so through cmd itself.
local function resolve(program)
  local endings = { "" }
  if isWindows then
    for ending in (os.getenv("PATHEXT") or ".COM;.EXE;.BAT;.CMD"):gmatch("[^;]+") do
      endings[#endings + 1] = ending
    end
  end
  local function withEnding(path)
    for _, ending in ipairs(endings) do
      if isFile(path .. ending) then
        return path .. ending
      end
    end
  end
  if program:find("[/\\]") then
    return withEnding(program)
  end
  for dir in (os.getenv("PATH") or ""):gmatch(isWindows and "[^;]+" or "[^:]+") do
    local path = withEnding(dir .. (isWindows and "\\" or "/") .. program)
    if path then
      return path
    end
  end
end

-- The words of a command, split at spaces; a word in double quotes keeps its spaces, such as a path in Program Files.
local function words(command)
  local out, at = {}, 1
  while true do
    at = command:find("%S", at)
    if not at then
      return out
    end
    local last
    if command:sub(at, at) == '"' then
      last = command:find('"', at + 1, true) or #command + 1
      out[#out + 1] = command:sub(at + 1, last - 1)
    else
      last = (command:find("%s", at) or #command + 1) - 1
      out[#out + 1] = command:sub(at, last)
    end
    at = last + 1
  end
end

---Opens a file at a line in the user's code editor.
---@param file string a path; a relative one starts at the console's folder
---@param line integer?
local function openFile(file, line)
  if not file:match("^/") and not file:match("^%a:[/\\]") then
    file = folder .. file
  end
  local command = {}
  for _, word in ipairs(words(tostring(ecs.setting.get("console.editor")))) do
    command[#command + 1] = (word:gsub("{file}", function()
      return file
    end):gsub("{line}", tostring(line or 1)))
  end
  local program = command[1] and resolve(command[1])
  if program then
    command[1] = program
    if process.start(command, "") then
      return
    end
  end
  -- Without the code editor, the system opens the file with the program that it has for such files.
  local opener = isWindows and { "cmd", "/c", "start", "", file } or { "xdg-open", file }
  local started, err = process.start(opener, "")
  if not warned then
    warned = true
    add(("The code editor did not start: %s. The setting console.editor names its command."):format(tostring(command[1])), "warn")
  end
  if not started then
    add("The system did not open the file either: " .. tostring(err), "error")
  end
end

function add(text, level, links)
  text = tostring(text):gsub("\r?\n$", "")
  links = type(links) == "table" and #links > 0 and links or nil
  level = level or "info"
  local time = tostring(os.date("%H:%M:%S"))
  local last = entries[#entries]
  if last and last.text == text and last.level == level then
    last.count, last.time = last.count + 1, time
    last.links = links or last.links
  else
    local file, number = text:match("([%w_%-./\\]+%.%a+):(%d+)")
    entries[#entries + 1] = { text = text, level = level, time = time, count = 1, file = file, line = tonumber(number), links = links }
  end
  while #entries > LIMIT do
    table.remove(entries, 1)
  end
  redraw()
end

-- The links that the plugin that added an entry gave, in a row.
local function linksOf(entry)
  if not entry.links then
    return false
  end
  local out = {}
  for _, link in ipairs(entry.links) do
    out[#out + 1] = ui.text({ { text = link.text, link = "action" } }, {
      style = STYLES.note, tooltip = link.tooltip, onLink = function()
        link.onClick()
      end,
    })
  end
  return ui.row({ gap = 8 }, out)
end

-- An entry's box: its time, the place it names as a link, and its count, above its text, framed in its level's colour.
local function entryBox(i, entry, chosen)
  local color = COLORS[entry.level]
  return ui.column({ id = "entry" .. i, padding = 4, gap = 2, background = chosen and "selected" or "tabRow", border = color or "tabShown" }, {
    ui.row({ gap = 8 }, {
      ui.text(entry.time, { style = STYLES.note }),
      entry.file and ui.text({ { text = entry.file .. ":" .. (entry.line or 1), link = "open" } }, {
        style = STYLES.note, tooltip = "Open the file at the line in the code editor", onLink = function()
          openFile(entry.file, entry.line)
        end,
      }) or false,
      linksOf(entry),
      ui.space(),
      entry.count > 1 and ui.text("×" .. entry.count, { style = STYLES.note, tooltip = "Added this many times in a row" }) or false,
    }),
    ui.text(entry.text, { font = "mono", color = color, wrap = true }),
  })
end

-- Goes to what an entry is about: the file and line it names, or else its first link, such as the object that raised it.
local function goTo(entry)
  if entry.file then
    openFile(entry.file, entry.line)
  elseif entry.links then
    entry.links[1].onClick()
  end
end

-- Puts an entry's text on the clipboard, with the times it was added in a row when it stacked.
local function copy(entry)
  ecs.clipboard.setText(entry.count > 1 and ("%s (×%d)"):format(entry.text, entry.count) or entry.text)
end

-- A click on an entry chooses it, and a second click on it soon after, a double click, goes to what it is about.
local function activate(state, entry)
  if state.clicked == entry then
    state.clicked = nil
    goTo(entry)
  else
    state.clicked = entry
    ecs.timer.start(0.4, false, function()
      if state.clicked == entry then
        state.clicked = nil
      end
    end)
  end
end

-- The menu of a right click on an entry, which the click chose.
local function menu(state, entry, x, y)
  state.chosen = entry
  ui.menu(state.panel, x, y, {
    { label = "Go to", enabled = entry.file ~= nil or entry.links ~= nil, onClick = function()
      goTo(entry)
    end },
    { label = "Copy", keys = "Ctrl+C", onClick = function()
      copy(entry)
    end },
  })
end

ecs.panel.registerType({
  name = "log",
  title = "Console",
  create = function(panel, saved)
    local state = { panel = panel, follow = true, hidden = {} }
    if type(saved) == "table" and type(saved.hidden) == "table" then
      state.hidden = saved.hidden
    end
    panels[panel] = state
    return state
  end,
  draw = function(state, surface)
    local counts = { info = 0, warn = 0, error = 0 }
    local items, shown, selected = {}, {}, nil
    for i, entry in ipairs(entries) do
      counts[entry.level] = (counts[entry.level] or 0) + entry.count
      if not state.hidden[entry.level] and ui.matches(state.search or "", entry.text) then
        items[#items + 1] = entryBox(i, entry, entry == state.chosen)
        shown[#items] = entry
        selected = entry == state.chosen and #items or selected
      end
    end
    state.shown = #items
    if #items == 0 then
      items[1] = ui.text(#entries == 0 and "Nothing was printed yet." or "The filters hide every entry.", { style = STYLES.note, selectable = false })
    end
    -- the levels' toggles filter the entries, and show how many each has
    local header = {}
    for _, level in ipairs(LEVELS) do
      header[#header + 1] = ui.toggle(not state.hidden[level], {
        id = "show:" .. level, label = ("%s %d"):format(NAMES[level], counts[level] or 0), tooltip = "Show the entries of this level",
        onChange = function(on)
          state.hidden[level] = not on or nil
        end,
      })
    end
    -- the search shows only the entries that hold each word typed
    header[#header + 1] = ui.search(state.search or "", { width = 200, placeholder = "Search entries", onChange = function(text)
      state.search = text ~= "" and text or nil
    end })
    header[#header + 1] = ui.space()
    header[#header + 1] = ui.toggle(ecs.setting.get("console.clearOnRun") == true, {
      id = "clearOnRun", label = "Clear on run", tooltip = "Clear the console when the tool starts a run, such as a game that plays",
      onChange = function(on)
        ecs.setting.set("console.clearOnRun", on)
      end,
    })
    header[#header + 1] = ui.button("Clear", { id = "clear", tooltip = "Take out every entry", onClick = function()
      entries = {}
      redraw()
    end })
    ui.show(state.panel, surface, ui.column({ padding = 8, gap = 6 }, {
      ui.row({ gap = 8, align = "center", wrap = true }, header),
      -- a click chooses an entry, a double click goes to what it is about, and a right click opens its menu
      ui.list({ id = "entries", gap = 4, grow = 1, selected = selected, onSelect = function(index)
        state.chosen = shown[index]
      end, onActivate = function(index)
        activate(state, shown[index])
      end, onMenu = function(index, x, y)
        if index and shown[index] then
          menu(state, shown[index], x, y)
        end
      end }, items),
    }))
    if state.follow and #items > 0 and items[#items].id then
      state.follow = false
      ui.reveal(state.panel, "entries", items[#items].id)
    end
  end,
  event = function(state, event)
    ui.event(state.panel, event)
  end,
  -- the levels it hides, and how many entries show, which a test reads
  saveState = function(state)
    return { hidden = state.hidden, shown = state.shown, chosen = state.chosen and state.chosen.text or nil }
  end,
  destroy = function(state)
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  add = { sig = "void(string text, string level, value links)", doc = "Add an entry to the console, which stacks on the last one when it is equal\n@param text The entry's text, which may hold many lines\n@param level info, warn or error\n@param links Lua only, or nil: more links after the place it names, each a table of its text, tooltip and onClick (console.Link)", fn = add },
  clear = { sig = "void()", doc = "Clear the console", fn = function()
    entries = {}
    redraw()
  end },
  run = { sig = "void()", doc = "Say that the tool starts a run of its program, such as a game that plays: the console clears when the setting console.clearOnRun is on", fn = function()
    if ecs.setting.get("console.clearOnRun") == true then
      entries = {}
      redraw()
    end
  end },
  entries = { sig = "value()", doc = "The console's entries\n@return The entries, each a table of its text, level, time, count, and the file and line it names (console.Entry)", fn = function()
    return entries
  end },
  setFolder = { sig = "void(string folder)", doc = "Say which folder the files that the entries name are in\n@param folder The folder's path, ending with a slash", fn = function(path)
    folder = path
  end },
  open = { sig = "void(string file, int line)", doc = "Open a file at a line in the code editor\n@param file The file's path\n@param line The line, from 1", fn = openFile },
  copy = { sig = "void(handle<ecs.panel> panel)", doc = "Copy the chosen entry's text, as Ctrl+C does in the console\n@param panel The console, as its keys pass the focused panel; nothing happens without a chosen entry or while its search has the keys", fn = function(panel)
    local state = panels[panel]
    if state and state.chosen and not ui.focus(panel) then
      copy(state.chosen)
    end
  end },
})

-- the console's default key; presets and the user's settings can change it
ecs.key.bind("console.log", "Ctrl+C", "console.copy")
