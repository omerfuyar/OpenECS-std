-- The settings window: lists every setting with its value, and changes it in the user's settings file (DESIGN 12).
-- It is built with the ui standard plugin.

local ecs = require("ecs")
local ui = require("ui")

-- a size of the ui plugin's settings, in layout units, which every first-party view shares
local function size(name)
  return ecs.setting.get("ui." .. name)
end

-- version of a window's saved state: the setting that was chosen
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
    if math.type(key) ~= "integer" or key < 1 or key > #value then
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
  elseif math.type(value) == "float" then
    return ("%g"):format(value)
  end

  return tostring(value)
end

-- reads typed text as a value of a setting's type
-- returns the value, or nil and what the value must be
local function read(setting, typed)
  if setting.type == "integer" then
    local value = math.tointeger(tonumber(typed))
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

local function refresh(window)
  local chosen = window.settings[window.selected]
  window.settings = readSettings()
  window.selected = math.min(math.max(window.selected, 1), math.max(#window.settings, 1))

  for i, setting in ipairs(window.settings) do
    if chosen and setting.name == chosen.name then
      window.selected = i
    end
  end

  window.panel:redraw()
end

local function set(window, setting, value)
  local ok, message = ecs.setting.set(setting.name, value)

  if not ok then
    ecs.log.warn(message or ("'%s' is not changed."):format(setting.name))
  end

  window.invalid[setting.name] = nil
  refresh(window)
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
    return math.tointeger(value + direction)
  elseif setting.type == "number" then
    return value + direction
  end
end

-- the control that shows and changes a setting's value
local function control(window, setting)
  local id = "value:" .. setting.name

  if setting.type == "bool" then
    return ui.toggle(setting.value, { id = id, look = "switch", onChange = function(on) set(window, setting, on) end })
  elseif setting.type == "choice" then
    return ui.dropdown(setting.value, setting.choices, { id = id, grow = 1, onChange = function(value) set(window, setting, value) end })
  elseif setting.type == "key" then
    return ui.key(setting.value, { id = id, grow = 1, onChange = function(key) set(window, setting, key) end })
  end

  -- the other types are typed; a text that cannot be read shows as not valid, and Return saves one that can
  local fieldElement = ui.field(show(setting.value), {
    id = id,
    grow = 1,
    valid = not window.invalid[setting.name],
    onChange = function(typed)
      window.invalid[setting.name] = read(setting, typed) == nil
    end,
    onSubmit = function(typed)
      local value, expected = read(setting, typed)

      -- a text that cannot be read is reported, and typing goes on
      if value == nil then
        ecs.log.warn(("'%s' needs %s."):format(setting.name, expected))
        window.invalid[setting.name] = true
        return false
      end

      set(window, setting, value)
    end,
  })

  -- a colour shows beside its field
  if setting.type == "color" then
    return ui.row({ gap = 6, grow = 1 }, { ui.column({ width = 18, height = 18, background = setting.value, border = "textDim" }), fieldElement })
  end

  return fieldElement
end

local function settingRow(window, setting)
  return ui.row({ id = "row:" .. setting.name, padding = 6, gap = 12 }, {
    -- both columns start from no width and share the row, so every row's control has the same place
    ui.column({ width = 0, grow = 3, gap = 2 }, {
      ui.text(setting.name),
      ui.text(setting.description, { size = size("noteSize"), color = "textDim" }),
    }),
    ui.row({ width = 0, grow = 2 }, { control(window, setting) }),
  })
end

-- changes the chosen setting one step, in the user's settings file
local function change(window, direction)
  local setting = window.settings[window.selected]
  local value = setting and step(setting, direction)

  if value ~= nil then
    set(window, setting, value)
  end
end

-- toggles or cycles the chosen setting, or starts typing its value
local function edit(window)
  local setting = window.settings[window.selected]

  if not setting then
    return
  elseif setting.type == "bool" or setting.type == "choice" then
    change(window, 1)
  else
    ui.edit(window.panel, "value:" .. setting.name)
  end
end

-- the window's elements: the title, then the settings, each owner's under its name
local function view(window)
  local rows, owner = {}, nil

  for _, setting in ipairs(window.settings) do
    if setting.owner ~= owner then
      owner = setting.owner
      rows[#rows + 1] = ui.text(owner == "ecs" and "Core" or owner, { size = size("noteSize"), color = "accent", selectable = false })
    end

    rows[#rows + 1] = settingRow(window, setting)
  end

  -- a press on a row chooses its setting, and the row's control gets the press too; Up and Down choose a setting
  local list = ui.list({
    id = "list",
    gap = 4,
    selected = window.selected,
    onSelect = function(index)
      window.selected = index
      window.panel:redraw()
    end,
  }, rows)

  return ui.column({ padding = size("margin"), gap = size("margin") / 2, background = "background" }, {
    ui.text("Settings", { size = size("titleSize") }),
    list,
  })
end

ecs.panel.registerType({
  name = "window",
  title = "Settings",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local window = { panel = panel, settings = {}, selected = 1, invalid = {} }
    windows[panel] = window
    refresh(window)

    if version == STATE_VERSION and saved and saved.selected then
      for i, setting in ipairs(window.settings) do
        if setting.name == saved.selected then
          window.selected = i
        end
      end
    end

    return window
  end,
  destroy = function(window)
    ui.forget(window.panel)
    windows[window.panel] = nil
  end,
  saveState = function(window)
    local chosen = window.settings[window.selected]
    return { selected = chosen and chosen.name or nil }
  end,
  draw = function(window, surface)
    ui.show(window.panel, surface, view(window))
  end,
  event = function(window, event)
    if event.type == "shown" then
      refresh(window)
    end

    -- Return edits the chosen setting, when no element of the window takes it
    if not ui.event(window.panel, event) and event.type == "keyDown" and (event.key == "Return" or event.key == "Keypad Enter") and #window.settings > 0 then
      edit(window)
    end
  end,
})

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

    if window and #window.settings > 0 and not ui.focus(panel) then
      act(window)
    end
  end
end

services.previous = forWindow(function(window)
  change(window, -1)
end)

services.next = forWindow(function(window)
  change(window, 1)
end)

assert(ecs.service.register({
  open = { sig = "void()", doc = "Open settings", fn = services.open },
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
}))

-- default keys for the window; presets and the user's settings can change them
for service, key in pairs({ previous = "Left", next = "Right" }) do
  assert(ecs.key.bind("settings.window", key, "settings." .. service))
end
