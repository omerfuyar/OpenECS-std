-- The panel list: every panel type of the tool, by the plugin that registered it, and opens the one the user chooses (DESIGN 14).
-- It also gives the blank panel, which holds a place in the layout until a panel goes there.
-- It is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

-- ui's own styles, which the first-party views share so they look alike
local STYLES = ui.styles()

-- version of a list's saved state: the panel type that was chosen, and the plugins whose sections are closed
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

-- each panel's state is an object of Panels, whose functions are methods
local Panels = {}
Panels.__index = Panels

-- whether a plugin's section shows open: while the list searches, every section with a type that matches does
function Panels:isOpen(plugin)
  return self.query ~= "" or not self.closed[plugin]
end

-- reads the types again, those that match the search by title, name, plugin or description; the list's items are the types of the open
-- sections, which Up and Down choose
function Panels:refresh()
  local chosen = self.items[self.selected]
  self.types = {}
  self.items = {}

  for _, entry in ipairs(readTypes()) do
    if ui.matches(self.query, table.concat({ entry.title or "", entry.name, entry.plugin or "", entry.description or "" }, "\n")) then
      self.types[#self.types + 1] = entry
    end
  end

  for _, entry in ipairs(self.types) do
    if self:isOpen(entry.plugin) then
      self.items[#self.items + 1] = entry
    end
  end

  self.selected = math.min(math.max(self.selected, 1), math.max(#self.items, 1))

  for i, entry in ipairs(self.items) do
    if chosen and entry.name == chosen.name then
      self.selected = i
    end
  end

  self.panel:redraw()
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

-- the list's elements: the title, then for each plugin a section of its name, which a click opens and closes, with its description and a row
-- for each of its panel types, which a click, or Return on the chosen one, opens
function Panels:view()
  local sections, rows = {}, nil

  -- a section takes its children when it is made, so each plugin's is made once its rows are
  local function addSection(plugin)
    sections[#sections + 1] = ui.section(plugin, {
      id = "plugin:" .. plugin,
      open = self:isOpen(plugin),
      onToggle = function(open)
        self.closed[plugin] = not open or nil
        self:refresh()
      end,
    }, rows)
  end

  for i, entry in ipairs(self.types) do
    if i == 1 or entry.plugin ~= self.types[i - 1].plugin then
      rows = {}

      if entry.description ~= "" then
        rows[1] = ui.text(entry.description, { style = STYLES.note, wrap = true, selectable = false })
      end
    end

    rows[#rows + 1] = ui.row({ style = STYLES.item }, {
      ui.text(entry.title),
      ui.text(entry.name, { style = STYLES.note }),
    })

    if i == #self.types or entry.plugin ~= self.types[i + 1].plugin then
      addSection(entry.plugin)
    end
  end

  if #sections == 0 then
    local empty = self.query ~= "" and "No panel type matches the search." or "The tool's plugins have no panel types."
    sections[1] = ui.text(empty, { color = "textDim", selectable = false })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("Open a panel", { style = STYLES.title }),
    ui.search(self.query, {
      placeholder = "Search panels",
      results = "list",
      onChange = function(query)
        self.query = query
        self:refresh()
      end,
      -- Return opens the type the self chose, so a search and Return open a panel
      onSubmit = function()
        open(self.items[self.selected])
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
        open(self.items[index])
      end,
    }, sections),
  })
end

function Panels.create(panel, saved, version)
  local list = setmetatable({ panel = panel, types = {}, items = {}, selected = 1, closed = {}, query = "" }, Panels)
  lists[panel] = list

  if version == STATE_VERSION and saved then
    for _, plugin in ipairs(saved.closed or {}) do
      list.closed[plugin] = true
    end
  end

  list:refresh()

  -- the list has the keys from the start, so Up, Down and Return work at once
  ui.edit(panel, "list")

  -- the type chosen last time is chosen again
  if version == STATE_VERSION and saved and saved.selected then
    for i, entry in ipairs(list.items) do
      if entry.name == saved.selected then
        list.selected = i
      end
    end
  end

  return list
end

function Panels:destroy()
  ui.forget(self.panel)
  lists[self.panel] = nil
end

function Panels:saveState()
  local chosen, closed = self.items[self.selected], {}

  for plugin in pairs(self.closed) do
    closed[#closed + 1] = plugin
  end

  table.sort(closed)
  return { selected = chosen and chosen.name or nil, closed = closed }
end

function Panels:draw(surface)
  ui.show(self.panel, surface, self:view())
end

function Panels:event(event)
  -- plugins may have registered types since the list was drawn
  if event.type == "shown" then
    self:refresh()
  end

  ui.event(self.panel, event)
end

ecs.panel.registerType({
  name = "list",
  title = "Panels",
  stateVersion = STATE_VERSION,
  create = Panels.create,
  destroy = Panels.destroy,
  saveState = Panels.saveState,
  draw = Panels.draw,
  event = Panels.event,
})

-- the blank panel shows nothing but a dim hint, centred, so a user arranging the layout keeps a place, such as a split's other side, for a
-- panel to open there later
local Blank = {}
Blank.__index = Blank

function Blank.create(panel)
  return setmetatable({ panel = panel }, Blank)
end

function Blank:destroy()
  ui.forget(self.panel)
end

function Blank:draw(surface)
  ui.show(
    self.panel,
    surface,
    ui.column({ style = STYLES.view, align = "center" }, {
      ui.column({ grow = 1 }, {}),
      ui.text("Open a panel here from the panel list.", { color = "textDim", wrap = true, selectable = false }),
      ui.column({ grow = 1 }, {}),
    })
  )
end

function Blank:event(event)
  ui.event(self.panel, event)
end

ecs.panel.registerType({
  name = "blank",
  title = "Blank",
  create = Blank.create,
  destroy = Blank.destroy,
  draw = Blank.draw,
  event = Blank.event,
})

-- the panel types of a plugin that is enabled or disabled show at once
ecs.event.subscribe("ecs.pluginsChanged", function()
  for _, list in pairs(lists) do
    list:refresh()
  end
end)

local services = {}

-- opens the panel list, or shows the one that is open; the panel that had the focus is where a chosen type opens
function services.open()
  local focus = ecs.layout.getFocus()

  if focus and not lists[focus] then
    target = focus
  end

  local openList = next(lists)

  if openList then
    lists[openList]:refresh()
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("panels.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the panel list.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open the panel list", fn = services.open },
})

-- its key after the prefix, and its entry in the title bar's menu Tools
ecs.key.bind("prefix", ".", "panels.open")
ecs.app.addMenuEntry("panels.open")

