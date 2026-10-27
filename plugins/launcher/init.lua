-- The launcher: lists the presets, then the saved sessions, and opens the one the user chooses (DESIGN 13).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

-- a size of the ui plugin's settings, in layout units, which every first-party view shares
local function size(name)
  return ecs.setting.get("ui." .. name)
end

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

-- reads both lists again, and keeps the chosen entry chosen if it is still there
local function refresh(list)
  local chosen = list.entries[list.selected]
  list.entries = readEntries()
  list.selected = math.min(math.max(list.selected, 1), math.max(#list.entries, 1))

  for i, entry in ipairs(list.entries) do
    if chosen and entry.type == chosen.type and entry.name == chosen.name then
      list.selected = i
    end
  end

  list.panel:redraw()
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
local function view(list)
  local rows = {}

  for _, entry in ipairs(list.entries) do
    rows[#rows + 1] = ui.row({ padding = size("rowPadding"), gap = size("margin") / 2 }, {
      ui.text(entry.title),
      ui.text(entry.type == "preset" and "preset" or entry.detail, { size = size("noteSize"), color = "textDim" }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("There are no presets and no saved sessions.", { color = "textDim", selectable = false })
  end

  return ui.column({ padding = size("margin"), gap = size("margin") / 2, background = "background" }, {
    ui.text("OpenECS", { size = size("titleSize") }),
    ui.list({
      id = "list",
      selected = list.selected,
      onSelect = function(index)
        list.selected = index
        list.panel:redraw()
      end,
      onActivate = function(index)
        open(list.entries[index])
      end,
    }, rows),
  })
end

ecs.panel.registerType({
  name = "list",
  title = "Launcher",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local list = { panel = panel, entries = {}, selected = 1 }
    lists[panel] = list
    refresh(list)

    -- the entry chosen last time is chosen again
    if version == STATE_VERSION and saved and saved.selected then
      for i, entry in ipairs(list.entries) do
        if entry.type == saved.selected.type and entry.name == saved.selected.name then
          list.selected = i
        end
      end
    end

    return list
  end,
  destroy = function(state)
    ui.forget(state.panel)
    lists[state.panel] = nil
  end,
  saveState = function(state)
    local chosen = state.entries[state.selected]
    return { selected = chosen and { type = chosen.type, name = chosen.name } or nil }
  end,
  draw = function(state, surface)
    ui.show(state.panel, surface, view(state))
  end,
  event = function(state, event)
    if event.type == "shown" then
      refresh(state)
    end

    ui.event(state.panel, event)
  end,
})

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

assert(ecs.service.register({
  open = { sig = "void()", doc = "Open launcher", fn = services.open },
}))

