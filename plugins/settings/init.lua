-- The settings window: lists every setting with its value, and changes it in the user's settings file (DESIGN 12).
-- It is built with the ui standard plugin.

local ecs = require("ecs")
local ui = require("ui")

-- ui's own styles, which the first-party views share so they look alike
local STYLES = ui.styles()

-- version of a window's saved state: the setting that was chosen, and the owners whose sections are closed
local STATE_VERSION = 1

local windows = {} -- every open window's state, by its panel handle

-- the core's settings first, then each plugin's, each by name
local function readSettings()
  local settings = {}

  for _, settingName in ipairs(ecs.setting.list() or {}) do
    settings[#settings + 1] = ecs.setting.explain(settingName)
  end

  table.sort(settings, function(a, b)
    if (a.owner == "ecs") ~= (b.owner == "ecs") then
      return a.owner == "ecs"
    end

    return a.name < b.name
  end)

  return settings
end

-- a list or table as a Lua table, which the user can type back
local function literal(value)
  if type(value) == "string" then
    return ("%q"):format(value)
  elseif type(value) ~= "table" then
    return tostring(value)
  end

  local parts = {}

  for _, item in ipairs(value) do
    parts[#parts + 1] = literal(item)
  end

  local keys = {}

  for key in pairs(value) do
    if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > #value then
      keys[#keys + 1] = key
    end
  end

  table.sort(keys, function(a, b)
    return tostring(a) < tostring(b)
  end)

  for _, key in ipairs(keys) do
    local shown = type(key) == "string" and key:match("^[%a_][%w_]*$") and key or ("[%s]"):format(literal(key))
    parts[#parts + 1] = shown .. " = " .. literal(value[key])
  end

  return #parts == 0 and "{}" or "{ " .. table.concat(parts, ", ") .. " }"
end

-- a value as the text of its field
local function show(value)
  if type(value) == "table" then
    return literal(value)
  elseif type(value) == "number" and value % 1 ~= 0 then
    return ("%g"):format(value)
  end

  return tostring(value)
end

-- reads typed text as a value of a setting's type
-- returns the value, or nil and what the value must be
local function read(setting, typed)
  if setting.type == "integer" then
    -- a number is whole when it has no fraction, as LuaJIT's numbers are all doubles
    local value = tonumber(typed)
    value = value and value % 1 == 0 and value or nil
    return value, value == nil and "a whole number" or nil
  elseif setting.type == "number" then
    local value = tonumber(typed)
    return value, value == nil and "a number" or nil
  elseif setting.type == "color" then
    local valid = typed:match("^#%x%x%x%x%x%x$") or typed:match("^#%x%x%x%x%x%x%x%x$")
    return valid and typed or nil, "a colour, such as #18191C or #18191CFF"
  elseif setting.type == "list" or setting.type == "table" then
    -- a Lua table, read without access to anything
    local chunk = load("return " .. typed, "=" .. setting.name, "t", {})
    local ok, value = false, nil

    if chunk then
      ok, value = pcall(chunk)
    end

    if ok and type(value) == "table" then
      return value
    end

    return nil, "a Lua table, such as { 1, 2 }"
  end

  return typed
end

-- each panel's state is an object of Settings, whose functions are methods
local Settings = {}
Settings.__index = Settings

-- whether a setting matches the window's search, by its name, description, owner or value
function Settings:matches(setting)
  return ui.matches(self.query, table.concat({ setting.name, setting.description or "", setting.owner or "", show(setting.value) }, "\n"))
end

-- whether an owner's section shows open: while the window searches, every section with a setting that matches does
function Settings:isOpen(owner)
  return self.query ~= "" or not self.closed[owner]
end

-- reads the settings again; the window's items are the settings that match the search in the open sections, which Up and Down choose
function Settings:refresh()
  local chosen = self.items[self.selected]
  self.settings = {}
  self.items = {}

  for _, setting in ipairs(readSettings()) do
    if self:matches(setting) then
      self.settings[#self.settings + 1] = setting
    end
  end

  for _, setting in ipairs(self.settings) do
    if self:isOpen(setting.owner) then
      self.items[#self.items + 1] = setting
    end
  end

  self.selected = math.min(math.max(self.selected, 1), math.max(#self.items, 1))

  for i, setting in ipairs(self.items) do
    if chosen and setting.name == chosen.name then
      self.selected = i
    end
  end

  self.panel:redraw()
end

function Settings:set(setting, value)
  local ok, message = ecs.setting.set(setting.name, value)

  if not ok then
    ecs.log.warn(message or ("'%s' is not changed."):format(setting.name))
  end

  self.invalid[setting.name] = nil
  self:refresh()
end

-- the next value of a setting one step forward or back, or nil if the window does not step its type
local function step(setting, direction)
  local value = setting.value

  if setting.type == "bool" then
    return not value
  elseif setting.type == "choice" then
    local choices = setting.choices or {}

    for i, choice in ipairs(choices) do
      if choice == value then
        return choices[(i - 1 + direction) % #choices + 1]
      end
    end

    return choices[1]
  elseif setting.type == "integer" then
    return value + direction
  elseif setting.type == "number" then
    return value + direction
  end
end

-- the control that shows and changes a setting's value
function Settings:control(setting)
  local id = "value:" .. setting.name

  if setting.type == "bool" then
    return ui.toggle(setting.value, { id = id, look = "switch", onChange = function(on) self:set(setting, on) end })
  elseif setting.type == "choice" then
    local selected = 0

    for i, choice in ipairs(setting.choices) do
      selected = choice == setting.value and i or selected
    end

    return ui.dropdown(selected, setting.choices, { id = id, grow = 1, onSelect = function(choice) self:set(setting, setting.choices[choice]) end })
  elseif setting.type == "key" then
    return ui.key(setting.value, { id = id, grow = 1, onChange = function(key) self:set(setting, key) end })
  elseif setting.type == "path" then
    -- a path is typed, or chosen with the file dialog, which gives it as if it were typed
    return ui.path(setting.value, {
      id = id,
      grow = 1,
      onSubmit = function(path)
        self:set(setting, path)
      end,
    })
  end

  -- the other types are typed; a text that cannot be read shows as not valid, and Return saves one that can
  local fieldElement = ui.field(show(setting.value), {
    id = id,
    grow = 1,
    valid = not self.invalid[setting.name],
    onChange = function(typed)
      self.invalid[setting.name] = read(setting, typed) == nil
    end,
    onSubmit = function(typed)
      local value, expected = read(setting, typed)

      -- a text that cannot be read is reported, and typing goes on
      if value == nil then
        ecs.log.warn(("'%s' needs %s."):format(setting.name, expected))
        self.invalid[setting.name] = true
        return false
      end

      self:set(setting, value)
    end,
  })

  -- a colour has a picker beside its field
  if setting.type == "color" then
    local picker = ui.color(setting.value, {
      id = "color:" .. setting.name,
      alpha = #setting.value == 9,
      onChange = function(color)
        self:set(setting, color)
      end,
    })
    return ui.row({ gap = 6, grow = 1 }, { picker, fieldElement })
  end

  return fieldElement
end

-- the value a setting has without the user's: the preset's, the theme's or the default
local function valueBelowUser(setting)
  local layers = setting.layers or {}

  if layers.preset ~= nil then
    return layers.preset
  elseif layers.theme ~= nil then
    return layers.theme
  end

  return layers.default
end

function Settings:settingRow(setting)
  -- a setting that the user changed shows its name in the accent, and a button that takes the change away
  local changed = setting.layer == "user"
  local reset = changed and ui.button("Reset", {
    id = "reset:" .. setting.name,
    tooltip = "Back to " .. show(valueBelowUser(setting)),
    onClick = function()
      self:set(setting, nil)
    end,
  }) or ui.space({ width = 0, grow = 0 })

  return ui.row({ id = "row:" .. setting.name, padding = 6, gap = 12 }, {
    -- both columns start from no width and share the row, so every row's control has the same place
    ui.column({ width = 0, grow = 3, gap = 2 }, {
      ui.text(setting.name, { color = changed and "accent" or nil }),
      -- the description wraps within its column, so it never runs under the control
      ui.text(setting.description, { style = STYLES.note, wrap = true }),
    }),
    ui.row({ width = 0, grow = 2, gap = 6 }, { self:control(setting), reset }),
  })
end

-- changes the chosen setting one step, in the user's settings file
function Settings:change(direction)
  local setting = self.items[self.selected]
  local value = setting and step(setting, direction)

  if value ~= nil then
    self:set(setting, value)
  end
end

-- toggles or cycles the chosen setting, or starts typing its value
function Settings:edit()
  local setting = self.items[self.selected]

  if not setting then
    return
  elseif setting.type == "bool" or setting.type == "choice" then
    self:change(1)
  else
    ui.edit(self.panel, "value:" .. setting.name)
  end
end

-- the window's elements: the title, then the settings, each owner's in a section of its name, which a click opens and closes
function Settings:view()
  local sections, rows = {}, {}

  for i, setting in ipairs(self.settings) do
    rows[#rows + 1] = self:settingRow(setting)

    -- a section takes its children when it is made, so each owner's is made once its rows are
    if i == #self.settings or setting.owner ~= self.settings[i + 1].owner then
      local owner = setting.owner

      sections[#sections + 1] = ui.section(owner == "ecs" and "Core" or owner, {
        id = "owner:" .. owner,
        open = self:isOpen(owner),
        onToggle = function(open)
          self.closed[owner] = not open or nil
          self:refresh()
        end,
      }, rows)
      rows = {}
    end
  end

  if #sections == 0 then
    sections[1] = ui.text("No setting matches the search.", { style = STYLES.note })
  end

  -- a press on a row chooses its setting, and the row's control gets the press too; Up and Down choose a setting
  local list = ui.list({
    id = "list",
    gap = 4,
    selected = self.selected,
    onSelect = function(index)
      self.selected = index
      self.panel:redraw()
    end,
  }, sections)

  return ui.column({ style = STYLES.view }, {
    ui.text("Settings", { style = STYLES.title }),
    ui.search(self.query, {
      placeholder = "Search settings",
      results = "list",
      onChange = function(query)
        self.query = query
        self:refresh()
      end,
    }),
    list,
  })
end

function Settings.create(panel, saved, version)
  local window = setmetatable({ panel = panel, settings = {}, items = {}, selected = 1, invalid = {}, closed = {}, query = "" }, Settings)
  windows[panel] = window

  if version == STATE_VERSION and saved then
    for _, owner in ipairs(saved.closed or {}) do
      window.closed[owner] = true
    end
  end

  window:refresh()

  -- the list has the keys from the start, so Up, Down and Return work at once
  ui.edit(panel, "list")

  if version == STATE_VERSION and saved and saved.selected then
    for i, setting in ipairs(window.items) do
      if setting.name == saved.selected then
        window.selected = i
      end
    end
  end

  return window
end

function Settings:destroy()
  windows[self.panel] = nil
end

function Settings:saveState()
  local chosen, closed = self.items[self.selected], {}

  for owner in pairs(self.closed) do
    closed[#closed + 1] = owner
  end

  table.sort(closed)
  return { selected = chosen and chosen.name or nil, closed = closed }
end


function Settings:event(event)
  if event.type == "shown" then
    self:refresh()
  end

  -- Return edits the chosen setting, when no element of the window takes it
  if event.type == "keyDown" and (event.key == "Return" or event.key == "Keypad Enter") and #self.items > 0 then
    self:edit()
  end
end

ecs.panel.registerType(ui.panelType({
  name = "window",
  title = "Settings",
  stateVersion = STATE_VERSION,
  create = Settings.create,
  destroy = Settings.destroy,
  saveState = Settings.saveState,
  view = Settings.view,
  event = Settings.event,
}))

-- a setting that another plugin or the user's file changes, and the settings of a plugin that is enabled or disabled, show at once
local function refreshAll()
  for _, window in pairs(windows) do
    window:refresh()
  end
end

ecs.event.subscribe("ecs.settingChanged", refreshAll)
ecs.event.subscribe("ecs.pluginsChanged", refreshAll)

local services = {}

-- opens the settings window, or shows the one that is open
function services.open()
  local openWindow = next(windows)

  if openWindow then
    ecs.layout.focus(openWindow)
    return
  end

  local panel, message = ecs.layout.open("settings.window")

  if not panel then
    ecs.log.warn(message or "Cannot open the settings window.")
  end
end

-- each function acts on the settings window it gets, unless one of its fields has the keys
local function forWindow(act)
  return function(panel)
    local window = windows[panel]

    if window and #window.items > 0 and not ui.focus(panel) then
      act(window)
    end
  end
end

services.previous = forWindow(function(window)
  window:change(-1)
end)

services.next = forWindow(function(window)
  window:change(1)
end)

ecs.service.register({
  open = { sig = "void()", doc = "Open the settings", fn = services.open },
  previous = {
    sig = "void(handle<ecs.panel> panel)",
    doc = "Change the chosen setting one step back\n"
        .. "@param panel The settings window, as its keys pass the focused panel; nothing happens while one of its fields has the keys",
    fn = services.previous,
  },
  next = {
    sig = "void(handle<ecs.panel> panel)",
    doc = "Change the chosen setting one step forward\n"
        .. "@param panel The settings window, as its keys pass the focused panel; nothing happens while one of its fields has the keys",
    fn = services.next,
  },
})

-- its key after the prefix, and its entry in the title bar's menu Tools
ecs.key.bind("prefix", ",", "settings.open")
ecs.app.addMenuEntry("settings.open")

-- default keys for the window; presets and the user's settings can change them
for service, key in pairs({ previous = "Left", next = "Right" }) do
  ecs.key.bind("settings.window", key, "settings." .. service)
end
