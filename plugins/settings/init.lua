-- The settings window: lists every setting with its value, and changes it in the user's settings file (DESIGN 10).
-- It is built with the ui standard plugin.

local ecs = require("ecs")
local ui = require("ui")

local function name(localName)
  return "settings." .. localName
end

-- sizes in layout units
local MARGIN = 24
local TITLE_SIZE = 22
local SECTION_SIZE = 13
local NOTE_SIZE = 12

-- version of a window's saved state: the setting that was chosen
local STATE_VERSION = 1

-- the id of the list of settings, which scrolls
local LIST = "list"

local windows = {} -- every open window's state, by its panel handle

-- the core's settings first, then each plugin's, each by name
local function readSettings()
  local settings = {}

  for _, settingName in ipairs(ecs.settings.list() or {}) do
    settings[#settings + 1] = ecs.settings.explain(settingName)
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

local function findWindow(panel)
  local window = windows[panel]

  if not window then
    ecs.log.warn("That panel is not a settings window.")
  end

  return window
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
  local ok, message = ecs.settings.set(setting.name, value)

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
    return ui.check(setting.value, { id = id, onChange = function(on) set(window, setting, on) end })
  elseif setting.type == "choice" then
    return ui.choice(setting.value, setting.choices, { id = id, grow = 1, onChange = function(value) set(window, setting, value) end })
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

local function settingRow(window, index, setting)
  local chosen = index == window.selected

  return ui.row({ id = "row:" .. setting.name, padding = 6, gap = 12, background = chosen and "selected" or nil }, {
    -- both columns start from no width and share the row, so every row's control has the same place
    ui.column({ width = 0, grow = 3, gap = 2 }, {
      ui.text(setting.name),
      ui.text(setting.description, { size = NOTE_SIZE, color = "textDim" }),
    }),
    ui.row({ width = 0, grow = 2 }, { control(window, setting) }),
  })
end

local function tree(window)
  local rows, owner = {}, nil

  for i, setting in ipairs(window.settings) do
    if setting.owner ~= owner then
      owner = setting.owner
      rows[#rows + 1] = ui.text(owner == "ecs" and "Core" or owner, { size = SECTION_SIZE, color = "accent" })
    end

    rows[#rows + 1] = settingRow(window, i, setting)
  end

  return ui.column({ padding = MARGIN, gap = MARGIN / 2, background = "background" }, {
    ui.text("Settings", { size = TITLE_SIZE }),
    ui.scroll({ id = LIST, gap = 4 }, rows),
  })
end

-- chooses a setting, and scrolls it into view
local function choose(window, index)
  window.selected = math.min(math.max(index, 1), #window.settings)
  local setting = window.settings[window.selected]

  if setting then
    ui.reveal(window.panel, LIST, "row:" .. setting.name)
  end

  window.panel:redraw()
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

ecs.panel.registerType({
  name = name("window"),
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
    window.scale = surface.scale
    ui.show(window.panel, surface, tree(window))
  end,
  event = function(window, event)
    if event.type == "shown" then
      refresh(window)
    end

    -- a click on a row chooses its setting, and the row's control gets the click too
    if event.type == "pointerDown" and event.button == 1 then
      for i, setting in ipairs(window.settings) do
        local ok, x, y, width, height = ui.rect(window.panel, "row:" .. setting.name)
        local px, py = event.x / (window.scale or 1), event.y / (window.scale or 1)

        if ok and px >= x and px < x + width and py >= y and py < y + height then
          window.selected = i
          window.panel:redraw()
        end
      end
    end

    ui.event(window.panel, event)
  end,
})

local services = {}

-- opens the settings window, or shows the one that is open
function services.open()
  local open = next(windows)

  if open then
    ecs.layout.focus(open)
    return
  end

  local panel, message = ecs.layout.open(name("window"))

  if not panel then
    ecs.log.warn(message or "Cannot open the settings window.")
  end
end

-- each function acts on the settings window it gets, unless one of its fields has the keys
local function forWindow(act)
  return function(panel)
    local window = findWindow(panel)

    if window and #window.settings > 0 and not ui.focus(panel) then
      act(window)
    end
  end
end

services.up = forWindow(function(window)
  choose(window, window.selected - 1)
end)

services.down = forWindow(function(window)
  choose(window, window.selected + 1)
end)

services.previous = forWindow(function(window)
  change(window, -1)
end)

services.next = forWindow(function(window)
  change(window, 1)
end)

-- while a field has the keys, the key of edit saves what is typed, as Return does in the field
function services.edit(panel)
  local window = findWindow(panel)

  if window and ui.focus(panel) then
    ui.event(panel, { type = "keyDown", key = "Return" })
  elseif window and #window.settings > 0 then
    edit(window)
  end
end

assert(ecs.service.register("settings", {
  open = { sig = "void()", doc = "Open the settings window", fn = services.open },
  up = { sig = "void(handle<ecs.panel> panel)", doc = "Choose the setting above", fn = services.up },
  down = { sig = "void(handle<ecs.panel> panel)", doc = "Choose the setting below", fn = services.down },
  previous = { sig = "void(handle<ecs.panel> panel)", doc = "Change the chosen setting one step back", fn = services.previous },
  next = { sig = "void(handle<ecs.panel> panel)", doc = "Change the chosen setting one step forward", fn = services.next },
  edit = { sig = "void(handle<ecs.panel> panel)", doc = "Toggle or cycle the chosen setting, or type its value", fn = services.edit },
}))

-- default keys for the window; presets and the user's settings can change them
for service, key in pairs({ up = "Up", down = "Down", previous = "Left", next = "Right", edit = "Return" }) do
  assert(ecs.input.bind(name("window"), key, name(service)))
end
