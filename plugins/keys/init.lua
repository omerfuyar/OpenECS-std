-- The key list: every key binding of the tool, by scope, to change, remove or add one in the user's settings (DESIGN 33).
-- It reads and changes the bindings with ecs.key (OpenECS DESIGN 7.8), and is built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

local STYLES = ui.styles()

-- version of a list's saved state: the scopes whose sections are closed
local STATE_VERSION = 1

local PREFIX = "prefix"
local TOOL = "" -- the whole tool's scope, in tables keyed by scope; ecs.key takes nil for it

local lists = {} -- every open list's state, by its panel handle

-- the scope that ecs.key takes
local function keyScope(scope)
  return scope ~= TOOL and scope or nil
end

-- a section's title: the whole tool, the keys after the core prefix, or a panel type's title
local function scopeTitle(scope, titles)
  if scope == TOOL then
    return "Everywhere"
  elseif scope == PREFIX then
    return ("After the prefix, %s"):format(tostring(ecs.setting.get("ecs.prefix")))
  end

  return titles[scope] and ("%s (%s)"):format(titles[scope], scope) or scope
end

-- each panel's state is an object of Keys, whose functions are methods
local Keys = {}
Keys.__index = Keys

-- the bindings that run, in their scopes' order: the whole tool, the prefix, then the panel types by name; each scope's by key
function Keys:readBindings()
  local functions, titles = {}, {}

  for _, entry in ipairs(ecs.key.functions()) do
    functions[entry.name] = entry
  end

  for _, entry in ipairs(ecs.panel.listTypes()) do
    titles[entry.name] = entry.title
  end

  local bindings = {}

  for _, binding in ipairs(ecs.key.list()) do
    if binding.active then
      bindings[#bindings + 1] = {
        key = binding.key,
        ["function"] = binding["function"],
        scope = binding.scope or TOOL,
        layer = binding.layer,
        about = functions[binding["function"]],
      }
    end
  end

  local function order(scope)
    return scope == TOOL and "0" or scope == PREFIX and "1" or "2" .. scope
  end

  table.sort(bindings, function(a, b)
    if a.scope ~= b.scope then
      return order(a.scope) < order(b.scope)
    end

    return a.key < b.key
  end)

  self.functions, self.titles, self.all = functions, titles, bindings
end

-- the text a search matches: the key, the function's name and description, and the scope
function Keys:searchText(binding)
  local about = binding.about
  return table.concat({ binding.key, tostring(binding["function"]), about and about.description or "", scopeTitle(binding.scope, self.titles) }, "\n")
end

function Keys:refresh()
  self:readBindings()
  self.bindings = {}

  for _, binding in ipairs(self.all) do
    if ui.matches(self.query, self:searchText(binding)) then
      self.bindings[#self.bindings + 1] = binding
    end
  end

  self.panel:redraw()
end

-- changes a key in the user's settings, and says why when the file cannot be written
local function setKey(scope, key, value)
  local ok, message = ecs.key.set(keyScope(scope), key, value)

  if not ok then
    ecs.log.warn(message or ("The key '%s' cannot be set."):format(key))
  end
end

-- the function that runs a key of a scope now, or false or nil
local function boundTo(scope, key)
  for _, binding in ipairs(ecs.key.list()) do
    if binding.active and (binding.scope or TOOL) == scope and binding.key == key then
      return binding["function"]
    end
  end
end

-- takes a key away from its function: the user's own binding goes, and a key that a layer below still binds to the function is removed
local function unbind(binding)
  if binding.layer == "user" then
    setKey(binding.scope, binding.key, nil)
  end

  if boundTo(binding.scope, binding.key) == binding["function"] then
    setKey(binding.scope, binding.key, false)
  end
end

-- gives a binding's function another key in its scope
function Keys:rebind(binding, key)
  if key ~= binding.key then
    setKey(binding.scope, key, binding["function"])
    unbind(binding)
  end

  self:refresh()
end

function Keys:bindingRow(binding)
  local func = binding["function"]
  local changed = binding.layer == "user"
  local id = binding.scope .. ":" .. binding.key
  local title, note

  if func == false then
    title, note = "Removed", "The key does nothing here, as a binding below it is taken away"
  else
    title = binding.about and binding.about.description ~= "" and binding.about.description or func
    note = binding.about and func or func .. ", which no plugin registers"
  end

  local buttons = {
    func ~= false and ui.button("Remove", {
      id = "remove:" .. id,
      tooltip = "Take the key away from the function",
      onClick = function()
        unbind(binding)
        self:refresh()
      end,
    }) or nil,
    changed and ui.button("Reset", {
      id = "reset:" .. id,
      tooltip = "Back to the key of the preset or the defaults",
      onClick = function()
        setKey(binding.scope, binding.key, nil)
        self:refresh()
      end,
    }) or nil,
  }

  return ui.row({ id = "row:" .. id, padding = 6, gap = 12 }, {
    ui.column({ width = 0, grow = 3, gap = 2 }, {
      ui.text(title, { color = changed and "accent" or nil }),
      ui.text(note, { style = STYLES.note }),
    }),
    ui.row({ width = 0, grow = 2, gap = 6 }, {
      func ~= false and ui.key(binding.key, {
        id = "key:" .. id,
        grow = 1,
        tooltip = "Click, then press the new key",
        onChange = function(key)
          self:rebind(binding, key)
        end,
      }) or ui.text(binding.key, { grow = 1, color = "textDim" }),
      buttons[1] or false,
      buttons[2] or false,
    }),
  })
end

-- the scopes a new key may go to: the whole tool, the prefix, and the panel types
function Keys:scopeChoices()
  local scopes, names = { TOOL, PREFIX }, {}

  for name in pairs(self.titles) do
    names[#names + 1] = name
  end

  table.sort(names)

  for _, name in ipairs(names) do
    scopes[#scopes + 1] = name
  end

  local texts = {}

  for i, scope in ipairs(scopes) do
    texts[i] = scopeTitle(scope, self.titles)
  end

  return scopes, texts
end

-- the row that adds a key: a function, a scope, then a key, which binds it at once
function Keys:addRow()
  local names = {}

  for name in pairs(self.functions) do
    names[#names + 1] = name
  end

  table.sort(names)
  local scopes, scopeTexts = self:scopeChoices()
  local chosen = self.newFunction and names[self.newFunction]

  return ui.group({ title = "Add a key", gap = 6 }, {
    ui.row({ gap = 6 }, {
      ui.dropdown(self.newFunction or 0, names, {
        id = "new:function",
        width = 0,
        grow = 3,
        placeholder = "Function",
        -- the functions' names are in folders by their dots, as ecs.workspace.switch2
        folders = ".",
        onSelect = function(selected)
          self.newFunction = selected
        end,
      }),
      ui.dropdown(self.newScope or 1, scopeTexts, {
        id = "new:scope",
        width = 0,
        grow = 2,
        onSelect = function(selected)
          self.newScope = selected
        end,
      }),
      ui.key("", {
        id = "new:key",
        width = 0,
        grow = 1,
        tooltip = chosen and "Click, then press the key for " .. chosen or "Choose a function first",
        onChange = function(key)
          if chosen then
            setKey(scopes[self.newScope or 1], key, chosen)
            self.newFunction = nil
            self:refresh()
          end
        end,
      }),
    }),
  })
end

-- the list's elements: the title, the search, the row that adds a key, then a section for each scope, which a click opens and closes
function Keys:view()
  local sections, rows = {}, {}

  for i, binding in ipairs(self.bindings) do
    rows[#rows + 1] = self:bindingRow(binding)

    if i == #self.bindings or binding.scope ~= self.bindings[i + 1].scope then
      local scope = binding.scope

      sections[#sections + 1] = ui.section(scopeTitle(scope, self.titles), {
        id = "scope:" .. scope,
        open = self.query ~= "" or not self.closed[scope],
        onToggle = function(open)
          self.closed[scope] = not open or nil
          self.panel:redraw()
        end,
      }, rows)
      rows = {}
    end
  end

  if #sections == 0 then
    sections[1] = ui.text(self.query ~= "" and "No key matches the search." or "The tool binds no keys.", { style = STYLES.note })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("Keys", { style = STYLES.title }),
    ui.search(self.query, {
      placeholder = "Search keys",
      onChange = function(query)
        self.query = query
        self:refresh()
      end,
    }),
    self:addRow(),
    ui.scroll({ id = "bindings", gap = 4, grow = 1 }, sections),
  })
end

function Keys.create(panel, saved, version)
  local list = setmetatable({ panel = panel, bindings = {}, all = {}, functions = {}, titles = {}, closed = {}, query = "" }, Keys)
  lists[panel] = list

  if version == STATE_VERSION and saved then
    for _, scope in ipairs(saved.closed or {}) do
      list.closed[scope] = true
    end
  end

  list:refresh()
  return list
end

function Keys:destroy()
  ui.forget(self.panel)
  lists[self.panel] = nil
end

function Keys:saveState()
  local closed = {}

  for scope in pairs(self.closed) do
    closed[#closed + 1] = scope
  end

  table.sort(closed)
  return { closed = closed }
end

function Keys:draw(surface)
  ui.show(self.panel, surface, self:view())
end

function Keys:event(event)
  -- plugins may have registered functions and panel types since the list was drawn
  if event.type == "shown" then
    self:refresh()
  end

  ui.event(self.panel, event)
end

ecs.panel.registerType({
  name = "list",
  title = "Keys",
  stateVersion = STATE_VERSION,
  create = Keys.create,
  destroy = Keys.destroy,
  saveState = Keys.saveState,
  draw = Keys.draw,
  event = Keys.event,
})

-- the keys of a plugin that is enabled or disabled, and a new prefix, show at once
local function refreshAll()
  for _, list in pairs(lists) do
    list:refresh()
  end
end

ecs.event.subscribe("ecs.pluginsChanged", refreshAll)

ecs.event.subscribe("ecs.settingChanged", function(_, value)
  if value and value.name == "ecs.prefix" then
    refreshAll()
  end
end)

-- opens the key list, or shows the one that is open
local function open()
  local shown = next(lists)

  if shown then
    lists[shown]:refresh()
    ecs.layout.focus(shown)
    return
  end

  local panel, message = ecs.layout.open("keys.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the key list.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open the key list", fn = open },
})

-- its key after the prefix, and its entry in the tool's menu
ecs.key.bind("prefix", "K", "keys.open")
ecs.app.addMenuEntry("keys.open")

-- the list of the keys after the prefix: while the core waits for one, a popup over the focused panel lists them with what they do now,
-- in sections, from the core's event ecs.prefix (OpenECS DESIGN 7.5)
local draw = require("draw")

local PREFIX_SECTIONS = { "Navigation", "Panel", "Layout", "Session", "More" }

-- the sections of the core's functions, by the start of their names; the core's other functions act on the panel, and the plugins' are More
local SECTION_STARTS = {
  { "ecs.layout.focus", 1 },
  { "ecs.layout.moveToWorkspace", 1 },
  { "ecs.layout.move", 1 },
  { "ecs.layout.nextTab", 1 },
  { "ecs.workspace.", 1 },
  { "ecs.layout.closeGroup", 3 },
  { "ecs.layout.lock", 3 },
  { "ecs.layout.reset", 3 },
  { "ecs.session.", 4 },
  { "ecs.app.", 4 },
}

-- functions that share a line when they run on the four arrows with the same modifiers, and those of each workspace, from the first's key to the last's
local ARROW_FAMILIES = { { "ecs.layout.focus", "Focus to panel" }, { "ecs.layout.move", "Move panel to" } }
local ARROWS = { "Left", "Right", "Up", "Down" }
local WORKSPACE_FAMILIES = { { "ecs.workspace.switch", "Switch to workspace" }, { "ecs.layout.moveToWorkspace", "Move panel to workspace" } }

local prefixPopup

local function sectionOf(name)
  for _, start in ipairs(SECTION_STARTS) do
    if name:sub(1, #start[1]) == start[1] then
      return start[2]
    end
  end

  return name:sub(1, 4) == "ecs." and 2 or 5
end

-- a key's modifiers, as written, and its name: "Shift+Left" gives "Shift+" and "Left", and "Ctrl++" gives "Ctrl+" and "+"
local function splitKey(key)
  local last = key:sub(1, -2):match(".*()%+")
  return last and key:sub(1, last) or "", last and key:sub(last + 1) or key
end

-- folds a key into one line with its arrow family: "Shift+Arrows"; or nil
local function foldArrows(keys, index, listed)
  local modifiers = splitKey(keys[index].key)

  for _, family in ipairs(ARROW_FAMILIES) do
    local members = {}

    for i, arrow in ipairs(ARROWS) do
      for j, entry in ipairs(keys) do
        if not listed[j] and entry["function"] == family[1] .. arrow and entry.key:lower() == (modifiers .. arrow):lower() then
          members[i] = j
        end
      end
    end

    if #members == 4 and (members[1] == index or members[2] == index or members[3] == index or members[4] == index) then
      for _, j in ipairs(members) do
        listed[j] = true
      end

      return modifiers .. "Arrows", family[2]
    end
  end
end

-- folds a workspace family's keys into one line: "1...0", or "Shift+1...0"
local function foldWorkspaces(keys, start, listed)
  local first, last

  for i, entry in ipairs(keys) do
    local number = entry["function"]:sub(1, #start) == start and tonumber(entry["function"]:sub(#start + 1))

    if number then
      listed[i] = true
      first = (not first or number < first.number) and { number = number, key = entry.key } or first
      last = (not last or number > last.number) and { number = number, key = entry.key } or last
    end
  end

  if first.number == last.number then
    return first.key
  end

  local modifiers, name = splitKey(last.key)
  return first.key .. "..." .. (first.key:sub(1, #modifiers) == modifiers and name or last.key)
end

-- the list's lines: a heading for each section that has keys, then each key with what it does, and Escape last
local function prefixLines(keys)
  local lines, listed = {}, {}

  for section, heading in ipairs(PREFIX_SECTIONS) do
    local start = #lines
    lines[#lines + 1] = { heading = heading }

    for i, entry in ipairs(keys) do
      local name = entry["function"]

      if not listed[i] and sectionOf(name) == section then
        local key, text

        for _, family in ipairs(WORKSPACE_FAMILIES) do
          if not key and name:sub(1, #family[1]) == family[1] then
            key, text = foldWorkspaces(keys, family[1], listed), family[2]
          end
        end

        if not key then
          key, text = foldArrows(keys, i, listed)
        end

        listed[i] = true
        lines[#lines + 1] = { key = key or entry.key, text = text or entry.text }
      end
    end

    if #lines == start + 1 then
      lines[#lines] = nil
    end
  end

  lines[#lines + 1] = { key = "Escape", text = "Cancel" }
  return lines
end

local function showPrefixKeys(value)
  if prefixPopup then
    prefixPopup:close()
    prefixPopup = nil
  end

  local panel = value.waiting and value.panel and ecs.layout.findPanel(value.panel)

  if not panel then
    return
  end

  local font = draw.font("ui")
  local size = ecs.setting.get("ecs.fontSize") or 13
  local lines = prefixLines(value.keys or {})
  local margin, padding, gap = 12, 12, 16
  local keyWidth, textWidth, lineHeight = 0, 0, 0

  for _, line in ipairs(lines) do
    local width, height = draw.measure(font, line.heading or line.text, size, 1)
    lineHeight = math.max(lineHeight, height)

    if line.heading then
      textWidth = math.max(textWidth, width)
    else
      keyWidth = math.max(keyWidth, (draw.measure(font, line.key, size, 1)))
      textWidth = math.max(textWidth, width)
    end
  end

  -- a heading has room above it, apart from the first
  local step, above = lineHeight + 3, 6
  local tops, y = {}, padding

  for i, line in ipairs(lines) do
    y = y + ((line.heading and i > 1) and above or 0)
    tops[i] = y
    y = y + step
  end

  local boxWidth = 2 * padding + keyWidth + gap + textWidth
  local boxHeight = y + padding
  local radius = ecs.setting.get("ui.radius") or 3

  -- the popup's margin stays clear, so the list stands off the panel's corner
  local popup
  popup = panel:openPopup({
    type = "tooltip",
    anchor = { x = 0, y = 0, width = 0, height = 0 },
    width = boxWidth + margin,
    height = boxHeight + margin,
    draw = function(surface)
      draw.rounded(surface, margin, margin, boxWidth, boxHeight, radius + 1, "overlay")
      draw.roundedOutline(surface, margin, margin, boxWidth, boxHeight, radius + 1, 1, "frame")

      for i, line in ipairs(lines) do
        local y = margin + tops[i]

        if line.heading then
          draw.text(surface, font, line.heading, margin + padding, y, size, "textDim")
        else
          draw.text(surface, font, line.key, margin + padding, y, size, "accent")
          draw.text(surface, font, line.text, margin + padding + keyWidth + gap, y, size, "text")
        end
      end
    end,
    closed = function()
      prefixPopup = prefixPopup ~= popup and prefixPopup or nil
    end,
  })
  prefixPopup = popup
end

ecs.event.subscribe("ecs.prefix", function(_, value)
  showPrefixKeys(value)
end)
