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
