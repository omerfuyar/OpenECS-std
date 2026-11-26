-- The launcher: lists the presets, then the saved sessions, and opens the one the user chooses (DESIGN 13).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

-- ui's own styles, which the first-party views share so they look alike
local STYLES = ui.styles()

-- version of a list's saved state: the entry that was chosen, by its type and name
local STATE_VERSION = 2

local lists = {} -- every open list's state, by its panel handle

-- presets of tools used before come first, the most recently used first, then the others by name; sessions are newest first
local function readEntries()
  local presets, sessions = ecs.session.listPresets(), ecs.session.listSessions()

  table.sort(presets, function(a, b)
    if (a.lastUsed ~= nil) ~= (b.lastUsed ~= nil) then
      return a.lastUsed ~= nil
    end

    if a.lastUsed ~= b.lastUsed then
      return a.lastUsed > b.lastUsed
    end

    return a.name < b.name
  end)

  table.sort(sessions, function(a, b)
    if a.saved ~= b.saved then
      return a.saved > b.saved
    end

    return a.name < b.name
  end)

  local entries = {}

  for _, preset in ipairs(presets) do
    entries[#entries + 1] = { type = "preset", name = preset.name, path = preset.path, title = preset.appName }
  end

  for _, session in ipairs(sessions) do
    local saved = os.date("%Y-%m-%d %H:%M", session.saved)
    entries[#entries + 1] = { type = "session", name = session.name, path = session.path, title = session.appName, detail = session.name .. ", " .. saved }
  end

  return entries
end

-- each panel's state is an object of Launcher, whose functions are methods
local Launcher = {}
Launcher.__index = Launcher

-- reads both lists again, the entries that match the search by title, name, type or date, and keeps the chosen entry chosen if it is still there
function Launcher:refresh()
  local chosen = self.entries[self.selected]
  self.entries = {}

  for _, entry in ipairs(readEntries()) do
    if ui.matches(self.query, table.concat({ entry.title or "", entry.name or "", entry.type, entry.detail or "" }, "\n")) then
      self.entries[#self.entries + 1] = entry
    end
  end

  self.selected = math.min(math.max(self.selected, 1), math.max(#self.entries, 1))

  for i, entry in ipairs(self.entries) do
    if chosen and entry.type == chosen.type and entry.name == chosen.name then
      self.selected = i
    end
  end

  self.panel:redraw()
end

-- opens an entry: a preset's tool, or a saved session
local function open(entry)
  if not entry then
    return
  end

  local ok, message

  if entry.type == "preset" then
    ok, message = ecs.session.openPreset(entry.path)
  else
    ok, message = ecs.session.open(entry.path)
  end

  if not ok then
    ecs.log.warn(message or "The entry is not opened.")
  end
end

-- the list's elements: the title, then one row for each entry, which a click, or Return on the chosen one, opens
function Launcher:view()
  local rows = {}

  for _, entry in ipairs(self.entries) do
    rows[#rows + 1] = ui.row({ style = STYLES.item }, {
      ui.text(entry.title),
      ui.text(entry.type == "preset" and "preset" or entry.detail, { style = STYLES.note }),
    })
  end

  if #rows == 0 then
    local empty = self.query ~= "" and "No preset or session matches the search." or "There are no presets and no saved sessions."
    rows[1] = ui.text(empty, { color = "textDim", selectable = false })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("OpenECS", { style = STYLES.title }),
    ui.search(self.query, {
      placeholder = "Search tools and sessions",
      results = "list",
      onChange = function(query)
        self.query = query
        self:refresh()
      end,
      -- Return opens the entry the self chose, so a search and Return open a tool
      onSubmit = function()
        open(self.entries[self.selected])
      end,
    }),
    ui.list({
      id = "list",
      selected = self.selected,
      onSelect = function(index)
        self.selected = index
        self.panel:redraw()
      end,
      onActivate = function(index)
        open(self.entries[index])
      end,
    }, rows),
  })
end

function Launcher.create(panel, saved, version)
  local list = setmetatable({ panel = panel, entries = {}, selected = 1, query = "" }, Launcher)
  lists[panel] = list
  list:refresh()

  -- the list has the keys from the start, so Up, Down and Return work at once
  ui.edit(panel, "list")

  -- the entry chosen last time is chosen again
  if version == STATE_VERSION and saved and saved.selected then
    for i, entry in ipairs(list.entries) do
      if entry.type == saved.selected.type and entry.name == saved.selected.name then
        list.selected = i
      end
    end
  end

  return list
end

function Launcher:destroy()
  lists[self.panel] = nil
end

function Launcher:saveState()
  local chosen = self.entries[self.selected]
  return { selected = chosen and { type = chosen.type, name = chosen.name } or nil }
end

function Launcher:event(event)
  if event.type == "shown" then
    self:refresh()
  end
end

ecs.panel.registerType(ui.panelType({
  name = "list",
  title = "Launcher",
  stateVersion = STATE_VERSION,
  create = Launcher.create,
  destroy = Launcher.destroy,
  saveState = Launcher.saveState,
  view = Launcher.view,
  event = Launcher.event,
}))

local services = {}

-- every tool loads the launcher, so the user can go to another tool from any of them; an open list gets the focus instead of a second one
function services.open()
  local openList = next(lists)

  if openList then
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("launcher.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the launcher.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open the launcher", fn = services.open },
})

-- its key after the prefix, and its entry in the title bar's menu Tools
ecs.key.bind("prefix", "/", "launcher.open")
ecs.app.addMenuEntry("launcher.open")

