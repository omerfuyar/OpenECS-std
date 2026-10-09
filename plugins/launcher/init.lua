-- The launcher: lists the presets, then the saved sessions, and opens the one the user chooses (DESIGN 5).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

local function name(localName)
  return "launcher." .. localName
end

-- sizes in layout units
local MARGIN = 24
local TITLE_SIZE = 22
local HEADING_SIZE = 13
local ENTRY_SIZE = 16
local ROW_PADDING = 8

-- the id of the list of entries, which scrolls
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
    entries[#entries + 1] = { kind = "session", name = session.name, path = session.path, title = session.appName, detail = session.name .. ", " .. saved }
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

local function move(panel, step)
  local list = findList(panel)

  if list and #list.entries > 0 then
    list.selected = math.min(math.max(list.selected + step, 1), #list.entries)
    ui.reveal(panel, LIST, "entry:" .. list.selected)
    panel:redraw()
  end
end

-- the list's elements: the title, then one row for each entry, which a click chooses and opens
local function tree(list)
  local rows = {}

  for i, entry in ipairs(list.entries) do
    rows[#rows + 1] = ui.row({
      id = "entry:" .. i,
      padding = ROW_PADDING,
      gap = MARGIN / 2,
      background = i == list.selected and "selected" or nil,
      hoverBackground = "tab",
      onClick = function()
        list.selected = i
        list.panel:redraw()
        open(entry)
      end,
    }, {
      ui.text(entry.title, { size = ENTRY_SIZE }),
      ui.text(entry.kind == "preset" and "preset" or entry.detail, { size = HEADING_SIZE, color = "textDim" }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("There are no presets and no saved sessions.", { size = ENTRY_SIZE, color = "textDim" })
  end

  return ui.column({ padding = MARGIN, gap = MARGIN / 2, background = "background" }, {
    ui.text("OpenECS", { size = TITLE_SIZE }),
    ui.scroll({ id = LIST }, rows),
  })
end

ecs.panel.registerType({
  name = name("list"),
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
  move(panel, -1)
end

function services.down(panel)
  move(panel, 1)
end

function services.open(panel)
  local list = findList(panel)

  if list then
    open(list.entries[list.selected])
  end
end

assert(ecs.service.register("launcher", {
  up = { sig = "void(handle<ecs.panel>)", doc = "Choose the entry above", fn = services.up },
  down = { sig = "void(handle<ecs.panel>)", doc = "Choose the entry below", fn = services.down },
  open = { sig = "void(handle<ecs.panel>)", doc = "Open the chosen preset or session", fn = services.open },
}))

-- default keys for the list; presets and the user's settings can change them
for service, key in pairs({ up = "Up", down = "Down", open = "Return" }) do
  assert(ecs.input.bind(name("list"), key, name(service)))
end
