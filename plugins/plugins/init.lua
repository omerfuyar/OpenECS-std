-- The plugin list: every plugin in the plugin folders, loaded or not, and enables or disables the one the user chooses (OpenECS DESIGN 9.10).
-- It is built with the ui standard plugin, as the panel list is.

local ecs = require("ecs")

local ui = require("ui")

-- a size of the ui plugin's settings, in layout units, which every first-party view shares
local function size(name)
  return ecs.setting.get("ui." .. name)
end

-- version of a list's saved state: the plugin that was chosen
local STATE_VERSION = 1

local lists = {} -- every open list's state, by its panel handle

-- every plugin but the list's own and the ones it needs, through others too, as disabling them would close the list itself; by name
local function readPlugins()
  local all, byName = ecs.plugins.list(), {}

  for _, entry in ipairs(all) do
    byName[entry.name] = entry
  end

  local needed = {}

  local function need(name)
    if not needed[name] then
      needed[name] = true

      for _, dependency in ipairs(byName[name] and byName[name].depends or {}) do
        need(dependency)
      end
    end
  end

  need(ecs.plugin.name)
  local plugins = {}

  for _, entry in ipairs(all) do
    if not needed[entry.name] then
      plugins[#plugins + 1] = entry
    end
  end

  table.sort(plugins, function(a, b)
    return a.name < b.name
  end)

  return plugins
end

local function refresh(list)
  local chosen = list.plugins[list.selected]
  list.plugins = readPlugins()
  list.selected = math.min(math.max(list.selected, 1), math.max(#list.plugins, 1))

  for i, entry in ipairs(list.plugins) do
    if chosen and entry.name == chosen.name then
      list.selected = i
    end
  end

  list.panel:redraw()
end

-- the core makes the change when the current pass of the main loop ends, then emits ecs.pluginsChanged, which refreshes every list
local function toggle(entry)
  if entry then
    (entry.enabled and ecs.plugins.disable or ecs.plugins.enable)(entry.name)
  end
end

local function stateText(entry)
  return entry.failed and "failed" or entry.enabled and "enabled" or "disabled"
end

-- the list's elements: the title, then a row for each plugin with its state, version and description; a click, or Return on the chosen one, toggles it
local function view(list)
  local rows = {}

  for _, entry in ipairs(list.plugins) do
    rows[#rows + 1] = ui.column({ padding = size("rowPadding"), gap = size("margin") / 4 }, {
      ui.row({ gap = size("margin") / 2 }, {
        ui.text(entry.name),
        ui.text(entry.version, { size = size("noteSize"), color = "textDim" }),
        ui.text(stateText(entry), { size = size("noteSize"), color = entry.enabled and "accent" or "textDim" }),
      }),
      entry.description ~= "" and ui.text(entry.description, { size = size("noteSize"), color = "textDim" }) or ui.space({ height = 0 }),
    })
  end

  if #rows == 0 then
    rows[1] = ui.text("No other plugin is in the plugin folders.", { color = "textDim", selectable = false })
  end

  return ui.column({ padding = size("margin"), gap = size("margin") / 2, background = "background" }, {
    ui.text("Plugins", { size = size("titleSize") }),
    ui.text("Return enables or disables the chosen plugin, with the plugins that need it.", { size = size("noteSize"), color = "textDim" }),
    ui.list({
      id = "list",
      selected = list.selected,
      onSelect = function(index)
        list.selected = index
        list.panel:redraw()
      end,
      onActivate = function(index)
        toggle(list.plugins[index])
      end,
    }, rows),
  })
end

ecs.panel.registerType({
  name = "list",
  title = "Plugins",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local list = { panel = panel, plugins = {}, selected = 1 }
    lists[panel] = list
    refresh(list)

    -- the plugin chosen last time is chosen again
    if version == STATE_VERSION and saved and saved.selected then
      for i, entry in ipairs(list.plugins) do
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
    local chosen = state.plugins[state.selected]
    return { selected = chosen and chosen.name or nil }
  end,
  draw = function(state, surface)
    ui.show(state.panel, surface, view(state))
  end,
  event = function(state, event)
    -- plugins may have been installed since the list was drawn
    if event.type == "shown" then
      refresh(state)
    end

    ui.event(state.panel, event)
  end,
})

ecs.event.subscribe("ecs.pluginsChanged", function()
  for _, list in pairs(lists) do
    refresh(list)
  end
end)

local services = {}

-- opens the plugin list, or shows the one that is open
function services.open()
  local openList = next(lists)

  if openList then
    refresh(lists[openList])
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("plugins.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the plugin list.")
  end
end

assert(ecs.service.register({
  open = { sig = "void()", doc = "Enable or disable plugins", fn = services.open },
}))
