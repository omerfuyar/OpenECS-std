-- The panel list: every panel type of the tool, by the plugin that registered it, and opens the one the user chooses (DESIGN 14).
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

-- whether a plugin's section shows open: while the list searches, every section with a type that matches does
local function isOpen(list, plugin)
  return list.query ~= "" or not list.closed[plugin]
end

-- reads the types again, those that match the search by title, name, plugin or description; the list's items are the types of the open
-- sections, which Up and Down choose
local function refresh(list)
  local chosen = list.items[list.selected]
  list.types = {}
  list.items = {}

  for _, entry in ipairs(readTypes()) do
    if ui.matches(list.query, table.concat({ entry.title or "", entry.name, entry.plugin or "", entry.description or "" }, "\n")) then
      list.types[#list.types + 1] = entry
    end
  end

  for _, entry in ipairs(list.types) do
    if isOpen(list, entry.plugin) then
      list.items[#list.items + 1] = entry
    end
  end

  list.selected = math.min(math.max(list.selected, 1), math.max(#list.items, 1))

  for i, entry in ipairs(list.items) do
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

-- the list's elements: the title, then for each plugin a section of its name, which a click opens and closes, with its description and a row
-- for each of its panel types, which a click, or Return on the chosen one, opens
local function view(list)
  local sections, rows = {}, nil

  -- a section takes its children when it is made, so each plugin's is made once its rows are
  local function addSection(plugin)
    sections[#sections + 1] = ui.section(plugin, {
      id = "plugin:" .. plugin,
      open = isOpen(list, plugin),
      onToggle = function(open)
        list.closed[plugin] = not open or nil
        refresh(list)
      end,
    }, rows)
  end

  for i, entry in ipairs(list.types) do
    if i == 1 or entry.plugin ~= list.types[i - 1].plugin then
      rows = {}

      if entry.description ~= "" then
        rows[1] = ui.text(entry.description, { style = STYLES.note, wrap = true, selectable = false })
      end
    end

    rows[#rows + 1] = ui.row({ style = STYLES.item }, {
      ui.text(entry.title),
      ui.text(entry.name, { style = STYLES.note }),
    })

    if i == #list.types or entry.plugin ~= list.types[i + 1].plugin then
      addSection(entry.plugin)
    end
  end

  if #sections == 0 then
    local empty = list.query ~= "" and "No panel type matches the search." or "The tool's plugins have no panel types."
    sections[1] = ui.text(empty, { color = "textDim", selectable = false })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("Open a panel", { style = STYLES.title }),
    ui.search(list.query, {
      results = "list",
      onChange = function(query)
        list.query = query
        refresh(list)
      end,
      -- Return opens the type the list chose, so a search and Return open a panel
      onSubmit = function()
        open(list.items[list.selected])
      end,
    }),
    ui.list({
      id = "list",
      selected = list.selected,
      onSelect = function(index)
        list.selected = index
        list.panel:redraw()
      end,
      onActivate = function(index)
        open(list.items[index])
      end,
    }, sections),
  })
end

ecs.panel.registerType({
  name = "list",
  title = "Panels",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local list = { panel = panel, types = {}, items = {}, selected = 1, closed = {}, query = "" }
    lists[panel] = list

    if version == STATE_VERSION and saved then
      for _, plugin in ipairs(saved.closed or {}) do
        list.closed[plugin] = true
      end
    end

    refresh(list)

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
  end,
  destroy = function(state)
    ui.forget(state.panel)
    lists[state.panel] = nil
  end,
  saveState = function(state)
    local chosen, closed = state.items[state.selected], {}

    for plugin in pairs(state.closed) do
      closed[#closed + 1] = plugin
    end

    table.sort(closed)
    return { selected = chosen and chosen.name or nil, closed = closed }
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

-- the panel types of a plugin that is enabled or disabled show at once
ecs.event.subscribe("ecs.pluginsChanged", function()
  for _, list in pairs(lists) do
    refresh(list)
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
    refresh(lists[openList])
    ecs.layout.focus(openList)
    return
  end

  local panel, message = ecs.layout.open("panels.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the panel list.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open panels", fn = services.open },
})

