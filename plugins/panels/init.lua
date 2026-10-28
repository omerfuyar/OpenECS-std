-- The panel list: every panel type of the tool, by the plugin that registered it, and opens the one the user chooses (DESIGN 14).
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

-- a size of the ui plugin's settings, in layout units, which every first-party view shares
local function size(name)
  return ecs.setting.get("ui." .. name)
end

-- version of a list's saved state: the panel type that was chosen
local STATE_VERSION = 1

local lists = {} -- every open list's state, by its panel handle
local target -- the panel that had the focus when the list was opened; a chosen type opens in its group

-- every panel type but the list's own, by plugin, then by title
local function readTypes()
  local types = {}

  for _, entry in ipairs(ecs.panel.listTypes()) do
    if entry.name ~= "panels.list" then
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

-- the list's elements: the title, then for each plugin a heading with its description and a row for each of its panel types, which a click,
-- or Return on the chosen one, opens
local function view(list)
  local rows = {}
  local plugin

  for _, entry in ipairs(list.types) do
    if entry.plugin ~= plugin then
      plugin = entry.plugin
      rows[#rows + 1] = ui.column({ padding = size("rowPadding") / 2, selectable = false }, {
        ui.text(plugin, { size = size("noteSize") }),
        entry.description ~= "" and ui.text(entry.description, { size = size("noteSize"), color = "textDim" }) or ui.space({ height = 0 }),
      })
    end

    rows[#rows + 1] = ui.row({ padding = size("rowPadding"), gap = size("margin") / 2 }, {
      ui.text(entry.title),
      ui.text(entry.name, { size = size("noteSize"), color = "textDim" }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("The tool's plugins have no panel types.", { color = "textDim", selectable = false })
  end

  return ui.column({ padding = size("margin"), gap = size("margin") / 2, background = "background" }, {
    ui.text("Open a panel", { size = size("titleSize") }),
    ui.list({
      id = "list",
      selected = list.selected,
      onSelect = function(index)
        list.selected = index
        list.panel:redraw()
      end,
      onActivate = function(index)
        open(list.types[index])
      end,
    }, rows),
  })
end

ecs.panel.registerType({
  name = "list",
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
  destroy = function(state)
    ui.forget(state.panel)
    lists[state.panel] = nil
  end,
  saveState = function(state)
    local chosen = state.types[state.selected]
    return { selected = chosen and chosen.name or nil }
  end,
  draw = function(state, surface)
    ui.show(state.panel, surface, view(state))
  end,
  event = function(state, event)
    -- plugins may have registered types since the list was drawn
    if event.type == "shown" then
      refresh(state)
    end

    ui.event(state.panel, event)
  end,
})

local services = {}

-- opens the panel list, or shows the one that is open; the panel that had the focus is where a chosen type opens
function services.open()
  local focus = ecs.layout.getFocus()

  if focus and not lists[focus] then
    target = focus
  end

  local openList = next(lists)

  if openList then
    refresh(lists[openList])
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("panels.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the panel list.")
  end
end

assert(ecs.service.register({
  open = { sig = "void()", doc = "Open panels", fn = services.open },
}))

