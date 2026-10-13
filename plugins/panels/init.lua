-- The panel list: every panel type of the tool, by the plugin that registered it, and opens the one the user chooses (DESIGN 12).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

local function name(localName)
  return "panels." .. localName
end

-- sizes in layout units
local MARGIN = 24
local TITLE_SIZE = 22
local HEADING_SIZE = 13
local ENTRY_SIZE = 16
local ROW_PADDING = 8

-- the id of the list of entries, which scrolls
local LIST = "list"

-- version of a list's saved state: the panel type that was chosen
local STATE_VERSION = 1

local lists = {} -- every open list's state, by its panel handle
local target -- the panel that had the focus when the list was opened; a chosen type opens in its group

-- every panel type but the list's own, by plugin, then by title
local function readTypes()
  local types = {}

  for _, entry in ipairs(ecs.panel.types()) do
    if entry.name ~= name("list") then
      types[#types + 1] = entry
    end
  end

  return types
end

local function refresh(list)
  local chosen = list.types[list.selected]
  list.types = readTypes()
  list.selected = math.min(math.max(list.selected, 1), math.max(#list.types, 1))

  for i, entry in ipairs(list.types) do
    if chosen and entry.name == chosen.name then
      list.selected = i
    end
  end

  list.panel:redraw()
end

-- opens a panel of a type in the group of the panel that had the focus, or where the core places new panels if that panel is gone
local function open(entry)
  if not entry then
    return
  end

  local ok, panel, message = pcall(ecs.layout.open, entry.name, nil, target, "center")

  if not ok or not panel then
    panel, message = ecs.layout.open(entry.name)
  end

  if not panel then
    ecs.log.warn(message or ("Cannot open a panel of '" .. entry.name .. "'."))
  end
end

local function findList(panel)
  local list = lists[panel]

  if not list then
    ecs.log.warn("That panel is not a panel list.")
  end

  return list
end

local function move(panel, step)
  local list = findList(panel)

  if list and #list.types > 0 then
    list.selected = math.min(math.max(list.selected + step, 1), #list.types)
    ui.reveal(panel, LIST, "entry:" .. list.selected)
    panel:redraw()
  end
end

-- the list's elements: the title, then for each plugin a heading with its description and a row for each of its panel types, which a click opens
local function tree(list)
  local rows = {}
  local plugin

  for i, entry in ipairs(list.types) do
    if entry.plugin ~= plugin then
      plugin = entry.plugin
      rows[#rows + 1] = ui.column({ id = "plugin:" .. plugin, padding = ROW_PADDING / 2 }, {
        ui.text(plugin, { size = HEADING_SIZE }),
        entry.description ~= "" and ui.text(entry.description, { size = HEADING_SIZE, color = "textDim" }) or ui.space({ height = 0 }),
      })
    end

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
      ui.text(entry.name, { size = HEADING_SIZE, color = "textDim" }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("The tool's plugins have no panel types.", { size = ENTRY_SIZE, color = "textDim" })
  end

  return ui.column({ padding = MARGIN, gap = MARGIN / 2, background = "background" }, {
    ui.text("Open a panel", { size = TITLE_SIZE }),
    ui.scroll({ id = LIST }, rows),
  })
end

ecs.panel.registerType({
  name = name("list"),
  title = "Panels",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local list = { panel = panel, types = {}, selected = 1 }
    lists[panel] = list
    refresh(list)

    -- the type chosen last time is chosen again
    if version == STATE_VERSION and saved and saved.selected then
      for i, entry in ipairs(list.types) do
        if entry.name == saved.selected then
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
    local chosen = list.types[list.selected]
    return { selected = chosen and chosen.name or nil }
  end,
  draw = function(list, surface)
    ui.show(list.panel, surface, tree(list))
  end,
  event = function(list, event)
    -- plugins may have registered types since the list was drawn
    if event.type == "shown" then
      refresh(list)
    end

    ui.event(list.panel, event)
  end,
})

local services = {}

-- opens the panel list, or shows the one that is open; the panel that had the focus is where a chosen type opens
function services.open()
  local focus = ecs.layout.getFocus()

  if focus and not lists[focus] then
    target = focus
  end

  local open = next(lists)

  if open then
    refresh(lists[open])
    ecs.layout.focus(open)
    return
  end

  local panel, message = ecs.layout.open(name("list"))

  if not panel then
    ecs.log.warn(message or "Cannot open the panel list.")
  end
end

function services.up(panel)
  move(panel, -1)
end

function services.down(panel)
  move(panel, 1)
end

function services.choose(panel)
  local list = findList(panel)

  if list then
    open(list.types[list.selected])
  end
end

assert(ecs.service.register("panels", {
  open = { sig = "void()", doc = "Open the panel list", fn = services.open },
  up = { sig = "void(handle<ecs.panel> panel)", doc = "Choose the panel type above", fn = services.up },
  down = { sig = "void(handle<ecs.panel> panel)", doc = "Choose the panel type below", fn = services.down },
  choose = { sig = "void(handle<ecs.panel> panel)", doc = "Open a panel of the chosen type", fn = services.choose },
}))

-- default keys for the list; presets and the user's settings can change them
for service, key in pairs({ up = "Up", down = "Down", choose = "Return" }) do
  assert(ecs.input.bind(name("list"), key, name(service)))
end
