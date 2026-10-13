-- The launcher: lists the presets, then the saved sessions, and opens the one the user chooses (DESIGN 13).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

-- a size of the ui plugin's settings, in layout units, which every first-party view shares
local function size(name)
  return ecs.settings.get("ui." .. name)
end

-- the id of the list of entries
local LIST = "list"

-- version of a list's saved state: the entry that was chosen
local STATE_VERSION = 1

local lists = {} -- every open list's state, by its panel handle

-- presets of tools used before come first, the most recently used first, then the others by name; sessions are newest first
local function readEntries()
  local presets, sessions = ecs.session.presets(), ecs.session.sessions()

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
    entries[#entries + 1] = { kind = "preset", name = preset.name, path = preset.path, title = preset.appName }
  end

  for _, session in ipairs(sessions) do
    local saved = os.date("%Y-%m-%d %H:%M", session.saved)
    entries[#entries + 1] = {
      kind = "session",
      name = session.name,
      path = session.path,
      title = session.appName,
      detail =
          session.name .. ", " .. saved
    }
  end

  return entries
end

-- reads both lists again, and keeps the chosen entry chosen if it is still there
local function refresh(list)
  local chosen = list.entries[list.selected]
  list.entries = readEntries()
  list.selected = math.min(math.max(list.selected, 1), math.max(#list.entries, 1))

  for i, entry in ipairs(list.entries) do
    if chosen and entry.kind == chosen.kind and entry.name == chosen.name then
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

  if entry.kind == "preset" then
    ok, message = ecs.session.openPreset(entry.path)
  else
    ok, message = ecs.session.open(entry.path)
  end

  if not ok then
    ecs.log.warn(message or "The entry is not opened.")
  end
end

local function findList(panel)
  local list = lists[panel]

  if not list then
    ecs.log.warn("That panel is not a launcher list.")
  end

  return list
end

-- the list's elements: the title, then one row for each entry, which a click chooses and opens
local function tree(list)
  local rows = {}

  for _, entry in ipairs(list.entries) do
    rows[#rows + 1] = ui.row({ padding = size("rowPadding"), gap = size("margin") / 2 }, {
      ui.text(entry.title),
      ui.text(entry.kind == "preset" and "preset" or entry.detail, { size = size("noteSize"), color = "textDim" }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("There are no presets and no saved sessions.", { color = "textDim", selectable = false })
  end

  return ui.column({ padding = size("margin"), gap = size("margin") / 2, background = "background" }, {
    ui.text("OpenECS", { size = size("titleSize") }),
    ui.list({
      id = LIST,
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
        if entry.kind == saved.selected.kind and entry.name == saved.selected.name then
          list.selected = i
        end
      end
    end

    return list
  end,
  destroy = function(list)
    ui.forget(list.panel)
    lists[list.panel] = nil
  end,
  saveState = function(list)
    local chosen = list.entries[list.selected]
    return { selected = chosen and { kind = chosen.kind, name = chosen.name } or nil }
  end,
  draw = function(list, surface)
    ui.show(list.panel, surface, tree(list))
  end,
  event = function(list, event)
    if event.type == "shown" then
      refresh(list)
    end

    ui.event(list.panel, event)
  end,
})

local services = {}

function services.up(panel)
  if findList(panel) then
    ui.move(panel, LIST, -1)
  end
end

function services.down(panel)
  if findList(panel) then
    ui.move(panel, LIST, 1)
  end
end

function services.open(panel)
  if findList(panel) then
    ui.activate(panel, LIST)
  end
end

assert(ecs.service.register({
  up = { sig = "void(handle<ecs.panel>)", doc = "Choose the entry above", fn = services.up },
  down = { sig = "void(handle<ecs.panel>)", doc = "Choose the entry below", fn = services.down },
  open = { sig = "void(handle<ecs.panel>)", doc = "Open the chosen preset or session", fn = services.open },
}))

-- default keys for the list; presets and the user's settings can change them
for service, key in pairs({ up = "Up", down = "Down", open = "Return" }) do
  assert(ecs.input.bind("launcher.list", key, "launcher." .. service))
end
