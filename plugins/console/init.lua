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

-- Whether a command's program is there: a path as it is, and a name in the folders of PATH.
-- On Windows a name may lack its ending, so the command is tried as it is.
local function found(program)
  if package.config:sub(1, 1) == "\\" then
    return true
  end
  local function isFile(path)
    local file = io.open(path, "rb")
    if file then
      file:close()
    end
    return file ~= nil
  end
  if program:find("/", 1, true) then
    return isFile(program)
  end
  for dir in (os.getenv("PATH") or ""):gmatch("[^:]+") do
    if isFile(dir .. "/" .. program) then
      return true
    end
  end
  return false
end

---Opens a file at a line in the user's code editor.
---@param file string a path; a relative one starts at the console's folder
---@param line integer?
local function openFile(file, line)
  if not file:match("^/") and not file:match("^%a:[/\\]") then
    file = folder .. file
  end
  local command = {}
  for word in tostring(ecs.setting.get("console.editor")):gmatch("%S+") do
    command[#command + 1] = (word:gsub("{file}", function()
      return file
    end):gsub("{line}", tostring(line or 1)))
  end
  if #command > 0 and found(command[1]) and process.start(command, "") then
    return
  end
  -- Without the code editor, the system opens the file with the program that it has for such files.
  local isWindows = package.config:sub(1, 1) == "\\"
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

function add(text, level)
  text = tostring(text):gsub("\r?\n$", "")
  level = level or "info"
  local time = os.date("%H:%M:%S")
  local last = entries[#entries]
  if last and last.text == text and last.level == level then
    last.count, last.time = last.count + 1, time
  else
    local file, number = text:match("([%w_%-./\\]+%.%a+):(%d+)")
    entries[#entries + 1] = { text = text, level = level, time = time, count = 1, file = file, line = tonumber(number) }
  end
  while #entries > LIMIT do
    table.remove(entries, 1)
  end
  redraw()
end

-- An entry's box: its time, the place it names as a link, and its count, above its text, framed in its level's colour.
local function entryBox(i, entry)
  local color = COLORS[entry.level]
  return ui.column({ id = "entry" .. i, padding = 4, gap = 2, background = "tabRow", border = color or "tabShown" }, {
    ui.row({ gap = 8 }, {
      ui.text(entry.time, { style = STYLES.note }),
      entry.file and ui.text({ { text = entry.file .. ":" .. (entry.line or 1), link = "open" } }, {
        style = STYLES.note, tooltip = "Open the file at the line in the code editor", onLink = function()
          openFile(entry.file, entry.line)
        end,
      }) or false,
      ui.space(),
      entry.count > 1 and ui.text("×" .. entry.count, { style = STYLES.note, tooltip = "Added this many times in a row" }) or false,
    }),
    ui.text(entry.text, { font = "mono", color = color, wrap = true }),
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
    local items = {}
    for i, entry in ipairs(entries) do
      counts[entry.level] = (counts[entry.level] or 0) + entry.count
      if not state.hidden[entry.level] then
        items[#items + 1] = entryBox(i, entry)
      end
    end
    if #items == 0 then
      items[1] = ui.text(#entries == 0 and "Nothing was printed yet." or "The filters hide every entry.", { style = STYLES.note })
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
      ui.scroll({ id = "entries", gap = 4 }, items),
    }))
    if state.follow and #items > 0 and items[#items].id then
      state.follow = false
      ui.reveal(state.panel, "entries", items[#items].id)
    end
  end,
  event = function(state, event)
    ui.event(state.panel, event)
  end,
  saveState = function(state)
    return { hidden = state.hidden }
  end,
  destroy = function(state)
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  add = { sig = "void(string text, string level)", doc = "Add an entry to the console, which stacks on the last one when it is equal\n@param text The entry's text, which may hold many lines\n@param level info, warn or error", fn = add },
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
})
