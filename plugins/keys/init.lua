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

-- the bindings that run, in their scopes' order: the whole tool, the prefix, then the panel types by name; each scope's by key
local function readBindings(list)
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

  list.functions, list.titles, list.all = functions, titles, bindings
end

-- the text a search matches: the key, the function's name and description, and the scope
local function searchText(list, binding)
  local about = binding.about
  return table.concat({ binding.key, tostring(binding["function"]), about and about.description or "", scopeTitle(binding.scope, list.titles) }, "\n")
end

local function refresh(list)
  readBindings(list)
  list.bindings = {}

  for _, binding in ipairs(list.all) do
    if ui.matches(list.query, searchText(list, binding)) then
      list.bindings[#list.bindings + 1] = binding
    end
  end

  list.panel:redraw()
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
local function rebind(list, binding, key)
  if key ~= binding.key then
    setKey(binding.scope, key, binding["function"])
    unbind(binding)
  end

  refresh(list)
end

local function bindingRow(list, binding)
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
        refresh(list)
      end,
    }) or nil,
    changed and ui.button("Reset", {
      id = "reset:" .. id,
      tooltip = "Back to the key of the preset or the defaults",
      onClick = function()
        setKey(binding.scope, binding.key, nil)
        refresh(list)
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
          rebind(list, binding, key)
        end,
      }) or ui.text(binding.key, { grow = 1, color = "textDim" }),
      buttons[1] or false,
      buttons[2] or false,
    }),
  })
end

-- the scopes a new key may go to: the whole tool, the prefix, and the panel types
local function scopeChoices(list)
  local scopes, names = { TOOL, PREFIX }, {}

  for name in pairs(list.titles) do
    names[#names + 1] = name
  end

  table.sort(names)

  for _, name in ipairs(names) do
    scopes[#scopes + 1] = name
  end

  local texts = {}

  for i, scope in ipairs(scopes) do
    texts[i] = scopeTitle(scope, list.titles)
  end

  return scopes, texts
end

-- the row that adds a key: a function, a scope, then a key, which binds it at once
local function addRow(list)
  local names = {}

  for name in pairs(list.functions) do
    names[#names + 1] = name
  end

  table.sort(names)
  local scopes, scopeTexts = scopeChoices(list)
  local chosen = list.newFunction and names[list.newFunction]

  return ui.group({ title = "Add a key", gap = 6 }, {
    ui.row({ gap = 6 }, {
      ui.dropdown(list.newFunction or 0, names, {
        id = "new:function",
        width = 0,
        grow = 3,
        placeholder = "Function",
        onSelect = function(selected)
          list.newFunction = selected
        end,
      }),
      ui.dropdown(list.newScope or 1, scopeTexts, {
        id = "new:scope",
        width = 0,
        grow = 2,
        onSelect = function(selected)
          list.newScope = selected
        end,
      }),
      ui.key("", {
        id = "new:key",
        width = 0,
        grow = 1,
        tooltip = chosen and "Click, then press the key for " .. chosen or "Choose a function first",
        onChange = function(key)
          if chosen then
            setKey(scopes[list.newScope or 1], key, chosen)
            list.newFunction = nil
            refresh(list)
          end
        end,
      }),
    }),
  })
end

-- the list's elements: the title, the search, the row that adds a key, then a section for each scope, which a click opens and closes
local function view(list)
  local sections, rows = {}, {}

  for i, binding in ipairs(list.bindings) do
    rows[#rows + 1] = bindingRow(list, binding)

    if i == #list.bindings or binding.scope ~= list.bindings[i + 1].scope then
      local scope = binding.scope

      sections[#sections + 1] = ui.section(scopeTitle(scope, list.titles), {
        id = "scope:" .. scope,
        open = list.query ~= "" or not list.closed[scope],
        onToggle = function(open)
          list.closed[scope] = not open or nil
          list.panel:redraw()
        end,
      }, rows)
      rows = {}
    end
  end

  if #sections == 0 then
    sections[1] = ui.text(list.query ~= "" and "No key matches the search." or "The tool binds no keys.", { style = STYLES.note })
  end

  return ui.column({ style = STYLES.view }, {
    ui.text("Keys", { style = STYLES.title }),
    ui.search(list.query, {
      placeholder = "Search keys",
      onChange = function(query)
        list.query = query
        refresh(list)
      end,
    }),
    addRow(list),
    ui.scroll({ id = "bindings", gap = 4, grow = 1 }, sections),
  })
end

ecs.panel.registerType({
  name = "list",
  title = "Keys",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local list = { panel = panel, bindings = {}, all = {}, functions = {}, titles = {}, closed = {}, query = "" }
    lists[panel] = list

    if version == STATE_VERSION and saved then
      for _, scope in ipairs(saved.closed or {}) do
        list.closed[scope] = true
      end
    end

    refresh(list)
    return list
  end,
  destroy = function(state)
    ui.forget(state.panel)
    lists[state.panel] = nil
  end,
  saveState = function(state)
    local closed = {}

    for scope in pairs(state.closed) do
      closed[#closed + 1] = scope
    end

    table.sort(closed)
    return { closed = closed }
  end,
  draw = function(state, surface)
    ui.show(state.panel, surface, view(state))
  end,
  event = function(state, event)
    -- plugins may have registered functions and panel types since the list was drawn
    if event.type == "shown" then
      refresh(state)
    end

    ui.event(state.panel, event)
  end,
})

-- the keys of a plugin that is enabled or disabled, and a new prefix, show at once
local function refreshAll()
  for _, list in pairs(lists) do
    refresh(list)
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
    refresh(lists[shown])
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
