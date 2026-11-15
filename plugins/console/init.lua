-- The console: a log of lines that other plugins add. A line may name a file and a line, such as "components/coin.lua:12",
-- and a click on it runs the command of the setting console.editor, which opens the file there in the user's code editor.

local ecs = require("ecs")
local ui = require("ui")
local process = require("process")

local STYLES = ui.styles()
local LIMIT = 2000

local lines = {} ---@type {text: string, level: string, file: string?, line: integer?}[]
local panels = {} ---@type table<ecs.Panel, table>
local folder = "" -- the folder that the files of the lines are in

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
  for line in (tostring(text) .. "\n"):gmatch("(.-)\r?\n") do
    local file, number = line:match("([%w_%-./\\]+%.%a+):(%d+)")
    lines[#lines + 1] = { text = line, level = level or "info", file = file, line = tonumber(number) }
  end
  while #lines > LIMIT do
    table.remove(lines, 1)
  end
  redraw()
end

ecs.panel.registerType({
  name = "log",
  title = "Console",
  create = function(panel)
    local state = { panel = panel, follow = true }
    panels[panel] = state
    return state
  end,
  draw = function(state, surface)
    local items = {}
    for i, line in ipairs(lines) do
      local color = line.level == "error" and "#e06c75" or line.level == "warn" and "#e5c07b" or nil
      ---@type ui.Text
      local text = line.text
      if line.file then
        text = { { text = line.text, link = tostring(i), color = color } }
      end
      items[i] = ui.text(text, {
        id = "line" .. i, font = "mono", color = color,
        onLink = function()
          openFile(line.file, line.line)
        end,
      })
    end
    if #items == 0 then
      items[1] = ui.text("Nothing was printed yet.", { style = STYLES.note })
    end
    ui.show(state.panel, surface, ui.column({ padding = 8, gap = 6 }, {
      ui.row({ gap = 8 }, {
        ui.text(("%d lines"):format(#lines), { style = STYLES.note, grow = 1 }),
        ui.button("Clear", { id = "clear", onClick = function()
          lines = {}
          redraw()
        end }),
      }),
      ui.scroll({ id = "lines" }, items),
    }))
    if state.follow and #lines > 0 then
      state.follow = false
      ui.reveal(state.panel, "lines", "line" .. #lines)
    end
  end,
  event = function(state, event)
    ui.event(state.panel, event)
  end,
  destroy = function(state)
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  add = { sig = "void(string text, string level)", doc = "Add lines to the console; the level is info, warn or error", fn = add },
  clear = { sig = "void()", doc = "Clear the console", fn = function()
    lines = {}
    redraw()
  end },
  lines = { sig = "value()", doc = "The console's lines, each with its text and level", fn = function()
    return lines
  end },
  setFolder = { sig = "void(string folder)", doc = "The folder that the files of the lines are in, ending with a slash", fn = function(path)
    folder = path
  end },
  open = { sig = "void(string file, int line)", doc = "Open a file at a line in the code editor", fn = openFile },
})
