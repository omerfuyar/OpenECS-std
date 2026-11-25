-- The console: a log of entries that other plugins add, each a box with its level's colour, its time and its text. An entry
-- may name a file and a line, such as "components/coin.lua:12", and a click on it runs the command of the setting
-- console.editor, which opens the file there in the user's code editor. An entry equal to the one before it stacks on it.
-- Each entry has a source, such as what a tool's program prints or a plugin whose log line it is: the core's event ecs.logged
-- brings every line of the log. Each panel's Show dropdown shows the entries from a level up, and its checkboxes some sources only.

local ecs = require("ecs")
local ui = require("ui")
local process = require("process")

local STYLES = ui.styles()
local LIMIT = 2000
-- the levels that the Show dropdown offers, from the highest, each showing the entries of its level and the levels above
local LEVELS = { "error", "warn", "info", "debug" }
local NAMES = { "Errors", "Warnings and up", "Info and up", "Everything" }
local RANKS = { error = 1, warn = 2, info = 3, debug = 4 }
local COLORS = { debug = "textDim", warn = "warning", error = "error" }

---@class console.Entry
---@field text string what was added, which may hold many lines
---@field level string debug, info, warn or error
---@field source string where it comes from: what the plugin that added it says, such as "game", or the plugin whose log line it is
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
local added = 0 -- grows with each add, so a panel knows when its filtered entries are old
local panels = {} ---@type table<ecs.Panel, table>
local folder = "" -- the folder that the files that entries name are in
local DEFAULT_SOURCE = "output" -- the source of an entry whose adder names none
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
    if process.start(command, "", {}) then
      return
    end
  end
  -- Without the code editor, the system opens the file with the program that it has for such files.
  local opener = isWindows and { "cmd", "/c", "start", "", file } or { "xdg-open", file }
  local started, err = process.start(opener, "", {})
  if not warned then
    warned = true
    add(("The code editor did not start: %s. The setting console.editor names its command."):format(tostring(command[1])), "warn")
  end
  if not started then
    add("The system did not open the file either: " .. tostring(err), "error")
  end
end

function add(text, level, links, source, time)
  text = tostring(text):gsub("\r?\n$", "")
  links = type(links) == "table" and #links > 0 and links or nil
  level = RANKS[level] and level or "info"
  source = source and source ~= "" and source or DEFAULT_SOURCE
  time = time or tostring(os.date("%H:%M:%S"))
  local last = entries[#entries]
  if last and last.text == text and last.level == level and last.source == source then
    last.count, last.time = last.count + 1, time
    last.links = links or last.links
  else
    local file, number = text:match("([%w_%-./\\]+%.%a+):(%d+)")
    entries[#entries + 1] = { text = text, level = level, source = source, time = time, count = 1, file = file, line = tonumber(number), links = links }
  end
  added += 1
  while #entries > LIMIT do
    table.remove(entries, 1)
  end
  redraw()
end

-- every line of the log, which the core's setting ecs.log chooses, with its plugin as its source; the Show dropdown hides the
-- lower levels
ecs.event.subscribe("ecs.logged", function(_, line)
  add(line.message, line.level, nil, line.plugin, (line.time:gsub("%.%d+$", "")))
end)

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
  return ui.column({ id = "entry" .. i, padding = 4, gap = 2, background = chosen and "selected" or "tabRow", border = color or "frame" }, {
    ui.row({ gap = 8 }, {
      ui.text(entry.time, { style = STYLES.note }),
      ui.text(entry.source, { style = STYLES.note, tooltip = "Where it comes from" }),
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

-- each panel's state is an object of Console, whose functions are methods
local Console = {}
Console.__index = Console

-- A click on an entry chooses it, and a second click on it soon after, a double click, goes to what it is about.
function Console:activate(entry)
  if self.clicked == entry then
    self.clicked = nil
    goTo(entry)
  else
    self.clicked = entry
    ecs.timer.start(0.4, false, function()
      if self.clicked == entry then
        self.clicked = nil
      end
    end)
  end
end

-- The menu of a right click on an entry, which the click chose.
function Console:menu(entry, x, y)
  self.chosen = entry
  ui.menu(self.panel, x, y, {
    { label = "Go to", enabled = entry.file ~= nil or entry.links ~= nil, onClick = function()
      goTo(entry)
    end },
    { label = "Copy", keys = "Ctrl+C", onClick = function()
      copy(entry)
    end },
  })
end

function Console.create(panel, saved)
  local state = setmetatable({ panel = panel, follow = true, show = "info", hiddenSources = {} }, Console)
  if type(saved) == "table" and RANKS[saved.show] then
    state.show = saved.show
  end
  if type(saved) == "table" and type(saved.hiddenSources) == "table" then
    state.hiddenSources = saved.hiddenSources
  end
  panels[panel] = state
  return state
end

-- the entries that the panel's Show dropdown, checkboxes and search show, with their numbers, how many each level has, and the sources
-- in order of their first entry, with how many each has; they are kept until an entry is added, the entries are cleared, or the
-- search, the level or a checkbox changes, as a draw that only a hover asks for filters the same entries
function Console:filter()
  local rank = RANKS[self.show]
  local key = self.show .. "\0" .. (self.search or "")
  for source in pairs(self.hiddenSources) do
    key = key .. "\0" .. source
  end
  local kept = self.filtered

  if kept and kept.entries == entries and kept.added == added and kept.key == key then
    return kept
  end

  kept = { entries = entries, added = added, key = key, shown = {}, numbers = {}, places = {}, counts = { debug = 0, info = 0, warn = 0, error = 0 }, sources = {}, bySource = {} }
  local shown, counts, sources, bySource = kept.shown, kept.counts, kept.sources, kept.bySource

  for i, entry in ipairs(entries) do
    counts[entry.level] = (counts[entry.level] or 0) + entry.count
    if not bySource[entry.source] then
      sources[#sources + 1], bySource[entry.source] = entry.source, 0
    end
    bySource[entry.source] += entry.count
    if RANKS[entry.level] <= rank and not self.hiddenSources[entry.source] and ui.matches(self.search or "", entry.text) then
      shown[#shown + 1], kept.numbers[#shown + 1] = entry, i
      kept.places[entry] = #shown
    end
  end

  self.filtered = kept
  return kept
end

function Console:draw(surface)
  local filtered = self:filter()
  local counts, shown, numbers = filtered.counts, filtered.shown, filtered.numbers
  local items, selected = {}, self.chosen and filtered.places[self.chosen]
  -- a long console builds only the entries in view
  local first, last = ui.range(self.panel, "entries", #shown)
  for index = first, last do
    items[#items + 1] = entryBox(numbers[index], shown[index], shown[index] == self.chosen)
  end
  self.shown, self.built = #shown, #items
  if #shown == 0 then
    items[1] = ui.text(#entries == 0 and "Nothing was printed yet." or "The filters hide every entry.", { style = STYLES.note, selectable = false })
  end
  -- the search shows only the entries that hold each word typed
  local header = {}
  header[#header + 1] = ui.search(self.search or "", { width = 200, placeholder = "Search entries", onChange = function(text)
    self.search = text ~= "" and text or nil
  end })
  header[#header + 1] = ui.space()
  -- the Show dropdown shows the entries from a level up, and how many each choice shows
  local choices, total = {}, 0
  for i, level in ipairs(LEVELS) do
    total += counts[level] or 0
    choices[i] = ("%s (%d)"):format(NAMES[i], total)
  end
  header[#header + 1] = ui.text("Show", { style = STYLES.note })
  header[#header + 1] = ui.dropdown(RANKS[self.show], choices, {
    id = "show", width = 170, tooltip = "Show the entries of this level and the levels above", onSelect = function(choice)
      self.show = LEVELS[choice]
    end,
  })
  header[#header + 1] = ui.button("Clear", { id = "clear", tooltip = "Take out every entry", onClick = function()
    entries = {}
    redraw()
  end })
  -- a checkbox for each source shows its entries or hides them
  local sources = { ui.text("Sources", { style = STYLES.note }) }
  for _, source in ipairs(filtered.sources) do
    sources[#sources + 1] = ui.toggle(not self.hiddenSources[source], {
      id = "source:" .. source, look = "check", label = ("%s %d"):format(source, filtered.bySource[source]), tooltip = "Show the entries from here",
      onChange = function(on)
        self.hiddenSources[source] = not on or nil
      end,
    })
  end
  if #filtered.sources == 0 then
    sources[#sources + 1] = ui.text("none yet", { style = STYLES.note })
  end
  -- the search, the Show dropdown and Clear are a bar along the top, over the sources and the entries
  ui.show(self.panel, surface, ui.column({}, {
    ui.row({ style = STYLES.bar, gap = 8, align = "center", wrap = true }, header),
    ui.row({ style = STYLES.bar, gap = 8, align = "center", wrap = true }, sources),
    -- a click chooses an entry, a double click goes to what it is about, and a right click opens its menu
    ui.list({ id = "entries", padding = 8, gap = 4, grow = 1, count = #shown, first = first, selected = selected, onSelect = function(index)
      self.chosen = shown[index]
    end, onActivate = function(index)
      self:activate(shown[index])
    end, onMenu = function(index, x, y)
      if index and shown[index] then
        self:menu(shown[index], x, y)
      end
    end }, items),
  }))
  if self.follow and #shown > 0 then
    self.follow = false
    ui.revealItem(self.panel, "entries", #shown)
  end
end

function Console:event(event)
  ui.event(self.panel, event)
end

-- the level it shows from, the sources it hides, and how many entries show and are built, and each source's count, which a test reads
function Console:saveState()
  local filtered = self:filter()
  return { show = self.show, hiddenSources = self.hiddenSources, shown = self.shown, built = self.built, chosen = self.chosen and self.chosen.text or nil,
    sources = filtered.bySource }
end

function Console:destroy()
  ui.forget(self.panel)
  panels[self.panel] = nil
end

ecs.panel.registerType({
  name = "log",
  title = "Console",
  create = Console.create,
  draw = Console.draw,
  event = Console.event,
  saveState = Console.saveState,
  destroy = Console.destroy,
})

ecs.service.register({
  add = { sig = "void(string text, string level, value links, string source)", doc = "Add an entry to the console, which stacks on the last one when it is equal, from the same source\n@param text The entry's text, which may hold many lines\n@param level debug, info, warn or error; info when it is none of them\n@param links Lua only, or nil: more links after the place it names, each a table of its text, tooltip and onClick (console.Link)\n@param source Where it comes from, such as \"game\", which the console's checkboxes show or hide; \"output\" when empty or nil", fn = function(text, level, links, source)
    add(text, level, links, source)
  end },
  clear = { sig = "void()", doc = "Clear the console", fn = function()
    entries = {}
    redraw()
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
