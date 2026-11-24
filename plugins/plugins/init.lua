-- The plugin list: every plugin in the plugin folders, loaded or not, and enables or disables the one the user chooses (OpenECS DESIGN 9.10).
-- It is built with the ui standard plugin, as the panel list is.

local ecs = require("ecs")

local ui = require("ui")

-- ui's own styles, which the first-party views share so they look alike
local STYLES = ui.styles()

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

    -- an enabled plugin keeps the plugins it depends on enabled, which the list fades
    for _, dependency in ipairs(entry.enabled and entry.depends or {}) do
      if byName[dependency] then
        byName[dependency].needed = true
      end
    end
  end

  table.sort(plugins, function(a, b)
    return a.name < b.name
  end)

  return plugins
end

local function stateText(entry)
  return entry.failed and "failed" or entry.enabled and "enabled" or "disabled"
end

-- each panel's state is an object of Plugins, whose functions are methods
local Plugins = {}
Plugins.__index = Plugins

-- the plugins that match the list's search, by name, description, state or the plugins they depend on
function Plugins:refresh()
  local chosen = self.plugins[self.selected]
  self.plugins = {}

  for _, entry in ipairs(readPlugins()) do
    local texts = { entry.name, entry.description or "", entry.version or "", stateText(entry), table.concat(entry.depends or {}, " ") }

    if ui.matches(self.query, table.concat(texts, "\n")) then
      self.plugins[#self.plugins + 1] = entry
    end
  end

  self.selected = math.min(math.max(self.selected, 1), math.max(#self.plugins, 1))

  for i, entry in ipairs(self.plugins) do
    if chosen and entry.name == chosen.name then
      self.selected = i
    end
  end

  self.panel:redraw()
end

-- the core makes the change when the current pass of the main loop ends, then emits ecs.pluginsChanged, which refreshes every list
local function toggle(entry)
  if entry and not (entry.enabled and entry.needed) then
    (entry.enabled and ecs.plugins.disable or ecs.plugins.enable)(entry.name)
  end
end

-- the list's elements: the title, then a row for each plugin with its state, version, description and the plugins it depends on;
-- a click, or Return on the chosen one, toggles it, unless an enabled plugin needs it, which fades its row
function Plugins:view()
  local rows = {}

  for _, entry in ipairs(self.plugins) do
    local faded = entry.enabled and entry.needed
    local depends = entry.depends or {}

    local children = {
      ui.row({ gap = 12 }, {
        ui.text(entry.name, { color = faded and "textDim" or nil }),
        ui.text(entry.version, { style = STYLES.note }),
        ui.text(stateText(entry), { style = entry.enabled and not faded and STYLES.heading or STYLES.note }),
      }),
    }

    if entry.description ~= "" then
      children[#children + 1] = ui.text(entry.description, { style = STYLES.note, wrap = true })
    end

    if #depends > 0 then
      children[#children + 1] = ui.text("Depends on " .. table.concat(depends, ", "), { style = faded and STYLES.note or STYLES.heading, wrap = true })
    end

    rows[#rows + 1] = ui.column({ style = STYLES.item, gap = 6 }, children)
  end

  if #rows == 0 then
    local empty = self.query ~= "" and "No plugin matches the search." or "No other plugin is in the plugin folders."
    rows[1] = ui.text(empty, { color = "textDim", selectable = false })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("Plugins", { style = STYLES.title }),
    ui.text("Return enables or disables the chosen plugin. A faded plugin stays enabled, as an enabled plugin needs it.", { style = STYLES.note }),
    ui.search(self.query, {
      placeholder = "Search plugins",
      results = "list",
      onChange = function(query)
        self.query = query
        self:refresh()
      end,
      -- Return toggles the plugin the self chose, as in the self
      onSubmit = function()
        toggle(self.plugins[self.selected])
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
        toggle(self.plugins[index])
      end,
    }, rows),
  })
end

function Plugins.create(panel, saved, version)
  local list = setmetatable({ panel = panel, plugins = {}, selected = 1, query = "" }, Plugins)
  lists[panel] = list
  list:refresh()

  -- the list has the keys from the start, so Up, Down and Return work at once
  ui.edit(panel, "list")

  -- the plugin chosen last time is chosen again
  if version == STATE_VERSION and saved and saved.selected then
    for i, entry in ipairs(list.plugins) do
      if entry.name == saved.selected then
        list.selected = i
      end
    end
  end

  return list
end

function Plugins:destroy()
  ui.forget(self.panel)
  lists[self.panel] = nil
end

function Plugins:saveState()
  local chosen = self.plugins[self.selected]
  return { selected = chosen and chosen.name or nil }
end

function Plugins:draw(surface)
  ui.show(self.panel, surface, self:view())
end

function Plugins:event(event)
  -- plugins may have been installed since the list was drawn
  if event.type == "shown" then
    self:refresh()
  end

  ui.event(self.panel, event)
end

ecs.panel.registerType({
  name = "list",
  title = "Plugins",
  stateVersion = STATE_VERSION,
  create = Plugins.create,
  destroy = Plugins.destroy,
  saveState = Plugins.saveState,
  draw = Plugins.draw,
  event = Plugins.event,
})

ecs.event.subscribe("ecs.pluginsChanged", function()
  for _, list in pairs(lists) do
    list:refresh()
  end
end)

local services = {}

-- opens the plugin list, or shows the one that is open
function services.open()
  local openList = next(lists)

  if openList then
    lists[openList]:refresh()
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("plugins.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the plugin list.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open the plugin list", fn = services.open },
})

-- its key after the prefix, and its entry in the tool's menu
ecs.key.bind("prefix", ";", "plugins.open")
ecs.app.addMenuEntry("plugins.open")
