-- The files panel: the files of one folder, apart from what starts with a dot. It makes folders and files, renames and
-- moves them, and deletes them after asking. The tool that uses it gives the folder, and says with handlers how a file
-- opens, which files it makes, and what a rename and a delete do besides.

local ecs = require("ecs")
local ui = require("ui")
local fs = require("fs")

local STYLES = ui.styles()
local STATE_VERSION = 1

local folder = "" -- the shown folder, ending with a slash
local watches = {}
local panels = {} ---@type table<ecs.Panel, table>

---What the tool gives the files panel. Every field may be left out.
---@class files.Handlers
---@field open? fun(path: string) opens a file, by its path in the folder, at a double click
---@field choose? fun(path: string) shows a file that a click or a key chose, by its path in the folder, such as in a preview
---@field kinds? {label: string, make: fun(path: string): boolean?, string?}[] the files that New makes: make gets the new file's path in the folder
---@field rename? fun(from: string, to: string): boolean?, string? renames or moves a file in place of the panel
---@field delete? fun(path: string): boolean? deletes a file in place of the panel
---@field deleteNote? fun(path: string): string? a text for the question before a file is deleted, such as the files that name it
---@field icon? fun(path: string): {image: string?, label: string?, color: string?}? a file's icon in the grid: an image by its path in the folder, or a badge's label and colour
local handlers = {}
local menu

ecs.event.declare("changed", "The shown folder's files changed")

local function redraw()
  for panel in pairs(panels) do
    panel:redraw()
  end
end

local function watch(dir)
  if watches[dir] then
    return
  end
  watches[dir] = fs.watch(folder .. dir, function()
    watch(dir)
    redraw()
  end) or true
  for _, entry in ipairs(fs.list(folder .. dir, "") or {}) do
    if entry.type == "folder" and entry.name:sub(1, 1) ~= "." then
      watch(dir == "" and entry.name or (dir .. "/" .. entry.name))
    end
  end
end

local function setFolder(path)
  for _, w in pairs(watches) do
    if w ~= true then
      w:stop()
    end
  end
  watches = {}
  folder = path
  if folder ~= "" then
    watch("")
  end
  redraw()
end

-- The tree's nodes for a folder: folders first, then files, each by name.
local function nodes(dir)
  local list = {}
  local entries = fs.list(folder .. dir, "") or {}
  table.sort(entries, function(a, b)
    if (a.type == "folder") ~= (b.type == "folder") then
      return a.type == "folder"
    end
    return a.name < b.name
  end)
  for _, entry in ipairs(entries) do
    if entry.name:sub(1, 1) ~= "." then
      local path = dir == "" and entry.name or (dir .. "/" .. entry.name)
      list[#list + 1] = { id = path, label = entry.name, children = entry.type == "folder" and nodes(path) or nil }
    end
  end
  return list
end

local function isFolder(path)
  local info = fs.info(folder .. path)
  return info ~= nil and info.type == "folder"
end

-- The folder that a new file goes to: the chosen folder, or the chosen file's.
local function folderOf(state)
  local chosen = state.selected
  if not chosen then
    return ""
  elseif isFolder(chosen) then
    return chosen .. "/"
  end
  return (chosen:match("^(.*/)[^/]*$") or "")
end

local function say(state, ok, message)
  state.message = not ok and message or nil
  state.panel:redraw()
end

local function rename(state, to)
  local from = state.selected
  if not from or to == "" or to == from then
    return
  end
  if fs.info(folder .. to) then
    return say(state, false, to .. " is there already.")
  end
  if handlers.rename then
    say(state, handlers.rename(from, to))
  else
    local parent = to:match("^(.*)/[^/]*$")
    if parent then
      fs.makeFolder(folder .. parent)
    end
    say(state, os.rename(folder .. from, folder .. to))
  end
  state.selected = to
  ecs.event.emit("files.changed")
end

local function delete(state)
  local path = state.selected
  if not path then
    return
  end
  local note = handlers.deleteNote and handlers.deleteNote(path)
  local answer = ecs.dialog.showMessage("Delete " .. path, "Delete " .. path .. "?" .. (note and note ~= "" and ("\n" .. note) or ""),
    { "Delete", "Cancel" })
  if answer ~= 1 then
    return
  end
  if handlers.delete then
    handlers.delete(path)
  else
    os.remove(folder .. path)
  end
  state.selected = nil
  ecs.event.emit("files.changed")
end

-- makes a file of a kind, or renames the chosen file, with the name that the field took
local function finish(state, asking, name)
  if name == "" then
    return say(state, false, "Type a name first.")
  end
  if asking.kind == "rename" then
    local to = name:find("/", 1, true) and name or ((asking.path:match("^(.*/)[^/]*$") or "") .. name)
    state.selected = asking.path
    return rename(state, to)
  end
  local path = asking.folder .. name
  if asking.kind == "folder" then
    say(state, fs.makeFolder(folder .. path))
  elseif asking.kind == "file" then
    if fs.info(folder .. path) then
      return say(state, false, path .. " is there already.")
    end
    local file = io.open(folder .. path, "wb")
    if file then
      file:close()
    end
    say(state, file ~= nil, "The file could not be made.")
  else
    say(state, handlers.kinds[asking.kind].make(path))
  end
  ecs.event.emit("files.changed")
end

-- asks for a name in the field above the tree, which gets the keys
local function ask(state, asking)
  state.asking, state.message = asking, nil
  ui.edit(state.panel, "name")
  state.panel:redraw()
end

-- the menu of a right click: about the file under it, which the click chose, or beside the files about the folder; New makes a
-- file in the chosen folder, or in the chosen file's
menu = function(state, id, x, y, into)
  into = into or id and folderOf(state) or ""
  local entries = {
    { label = "New folder", onClick = function() ask(state, { kind = "folder", folder = into, label = "New folder", text = "" }) end },
  }
  for i, kind in ipairs(handlers.kinds or {}) do
    entries[#entries + 1] = { label = "New " .. kind.label:lower(), onClick = function()
      ask(state, { kind = i, folder = into, label = "New " .. kind.label:lower(), text = "" })
    end }
  end
  entries[#entries + 1] = { label = "New file", onClick = function() ask(state, { kind = "file", folder = into, label = "New file", text = "" }) end }
  if id then
    local isDir = isFolder(id)
    entries[#entries + 1] = { line = true }
    entries[#entries + 1] = { label = "Open", enabled = not isDir and handlers.open ~= nil, onClick = function() handlers.open(id) end }
    entries[#entries + 1] = { label = "Rename", onClick = function()
      ask(state, { kind = "rename", path = id, label = "Rename", text = id:match("[^/]*$") })
    end }
    entries[#entries + 1] = { label = "Delete", onClick = function()
      state.selected = id
      delete(state)
    end }
  end
  ui.menu(state.panel, x, y, entries)
end

-- a click on a file or folder: a folder opens or closes in the tree, or the grid goes into it, and a second click on a file soon
-- after the first, a double click, opens it with the tool's handler
local function activate(state, id)
  if isFolder(id) then
    if state.view == "grid" then
      state.dir, state.selected = id .. "/", nil
    else
      state.open[id] = not state.open[id] or nil
    end
  elseif state.clicked == id then
    state.clicked = nil
    if handlers.open then
      handlers.open(id)
    end
  else
    state.clicked = id
    ecs.timer.start(0.4, false, function()
      if state.clicked == id then
        state.clicked = nil
      end
    end)
  end
end

-- chooses a file, and shows it with the tool's handler
local function choose(state, id)
  state.selected = id
  if handlers.choose and not isFolder(id) then
    handlers.choose(id)
  end
end

local IMAGES = { png = true, jpg = true, jpeg = true, bmp = true, tga = true, gif = true, svg = true }
local TILE = 84
local ICON = 48

-- a file's icon in the grid: the tool's, a picture of an image, or a badge of the file's ending, in a colour of its own
local function icon(path, dir)
  local given = handlers.icon and handlers.icon(path)
  if type(given) == "table" and given.image then
    return ui.image(folder .. given.image, { width = ICON, height = ICON })
  end
  local ending = (path:match("%.(%w+)$") or ""):lower()
  if not given and IMAGES[ending] then
    return ui.image(folder .. path, { width = ICON, height = ICON })
  end
  local label = type(given) == "table" and given.label or dir and "DIR" or ending ~= "" and ending:upper():sub(1, 4) or "FILE"
  local color = type(given) == "table" and given.color or dir and "accent" or "tabShown"
  return ui.column({ width = ICON, height = ICON, background = color, align = "center", padding = 4 }, {
    ui.space(),
    ui.text(label, { size = 12, color = dir and "background" or "text" }),
    ui.space(),
  })
end

-- the grid: the path of the shown folder as a row of folders to click, then a tile for each of its files, in rows that wrap
local function grid(state, children)
  local dir = state.dir
  if dir ~= "" and not isFolder(dir:sub(1, -2)) then
    dir, state.dir = "", ""
  end
  local crumbs = { ui.button(folder:match("([^/\\]+)[/\\]$") or "/", { id = "crumb:", tooltip = "Show this folder", onClick = function()
    state.dir = ""
  end }) }
  local walked = ""
  for part in dir:gmatch("([^/]+)/") do
    walked = walked .. part .. "/"
    local at = walked
    crumbs[#crumbs + 1] = ui.text("›", { style = STYLES.note })
    crumbs[#crumbs + 1] = ui.button(part, { id = "crumb:" .. at, tooltip = "Show this folder", onClick = function()
      state.dir = at
    end })
  end
  children[#children + 1] = ui.row({ gap = 4, align = "center", wrap = true }, crumbs)
  local tiles = {}
  state.tiles = {}
  for _, entry in ipairs(nodes(dir:sub(1, -2))) do
    local id = entry.id
    state.tiles[#state.tiles + 1] = id
    tiles[#tiles + 1] = ui.column({
      id = "tile:" .. id, width = TILE, padding = 4, gap = 4, align = "center", tooltip = entry.label,
      background = state.selected == id and "selected" or nil, hoverBackground = "tab",
      onClick = function()
        activate(state, id)
      end,
      onMenu = function(x, y)
        choose(state, id)
        menu(state, id, x, y)
      end,
    }, {
      icon(id, entry.children ~= nil),
      ui.text(entry.label, { size = 12 }),
    })
  end
  children[#children + 1] = ui.scroll({ id = "grid", grow = 1, onMenu = function(x, y)
    menu(state, nil, x, y, dir)
  end }, {
    #tiles > 0 and ui.row({ gap = 4, wrap = true }, tiles) or ui.text("The folder is empty.", { style = STYLES.note }),
  })
end

local function draw(state, surface)
  state.scale = surface.scale
  local children = {}
  if folder == "" then
    children[1] = ui.text("No folder is open.", { style = STYLES.note })
  else
    -- the name field shows while a menu's New or Rename asks for a name: Return makes the file or renames it
    local asking = state.asking
    if asking then
      children[#children + 1] = ui.row({ gap = 4, align = "center" }, {
        ui.text(asking.label),
        ui.field(asking.text, { id = "name", grow = 1, tooltip = "Return takes the name; a name with folders moves the file", onChange = function(text)
          asking.text = text
        end, onSubmit = function(text)
          state.asking = nil
          finish(state, asking, text)
        end }),
        ui.button("Cancel", { id = "cancel", tooltip = "Close the field", onClick = function()
          state.asking = nil
        end }),
      })
    end
    if state.message then
      children[#children + 1] = ui.text(tostring(state.message), { color = "#e06c75", wrap = true })
    end
    children[#children + 1] = ui.row({}, {
      ui.selector(state.view == "grid" and 2 or 1, { "Tree", "Grid" }, {
        id = "view", tooltip = "The folder as a tree, or one folder at a time as a grid of icons", onSelect = function(choice)
          state.view = choice == 2 and "grid" or "tree"
        end,
      }),
    })
  end
  if folder ~= "" and state.view == "grid" then
    grid(state, children)
  elseif folder ~= "" then
    children[#children + 1] = ui.tree(nodes(""), {
      id = "tree", open = state.open, selected = state.selected, grow = 1, tooltip = "A right click opens a menu",
      onSelect = function(id)
        state.pressed = id
        choose(state, id)
      end,
      onActivate = function(id)
        activate(state, id)
      end,
      onToggle = function(id, open)
        state.open[id] = open or nil
      end,
      onMenu = function(id, x, y)
        menu(state, id, x, y)
      end,
    })
  end
  ui.show(state.panel, surface, ui.column({ padding = 8, gap = 6 }, children))
end

ecs.panel.registerType({
  name = "tree",
  title = "Files",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local state = { panel = panel, open = {}, view = "tree", dir = "", tiles = {} }
    if version == STATE_VERSION and type(saved) == "table" then
      state.open = saved.open or {}
      state.view = saved.view == "grid" and "grid" or "tree"
      state.dir = type(saved.dir) == "string" and saved.dir or ""
    end
    panels[panel] = state
    return state
  end,
  draw = draw,
  event = function(state, event)
    -- A file is dragged out of the tree as a list of paths, as another application's files come.
    if event.type == "pointerDown" then
      state.pressed, state.pressX, state.pressY = nil, event.x, event.y
      -- in the grid, a press chooses the tile under it, which a drag takes
      if state.view == "grid" and event.button == 1 then
        local scale = state.scale or 1
        for _, id in ipairs(state.tiles) do
          local rect = ui.rect(state.panel, "tile:" .. id)
          local x, y = event.x / scale, event.y / scale
          if rect and x >= rect.x and y >= rect.y and x < rect.x + rect.width and y < rect.y + rect.height then
            state.pressed = id
            choose(state, id)
            state.panel:redraw()
          end
        end
      end
    elseif event.type == "pointerMove" and state.pressed then
      if math.abs(event.x - state.pressX) + math.abs(event.y - state.pressY) > 8 then
        state.panel:startDrag("file-list", { folder .. state.pressed })
        state.pressed = nil
      end
    elseif event.type == "pointerUp" then
      state.pressed = nil
    end
    ui.event(state.panel, event)
  end,
  saveState = function(state)
    return { open = state.open, view = state.view, dir = state.dir }
  end,
  destroy = function(state)
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  setFolder = { sig = "void(string folder)", doc = "Show a folder in the files panels\n@param folder The folder's path, ending with a slash; an empty text shows none", fn = setFolder },
  folder = { sig = "string()", doc = "The shown folder\n@return The folder's path, ending with a slash, or an empty text", fn = function()
    return folder
  end },
  setHandlers = { sig = "void(value handlers)", doc = "Say how files open, which files New makes, and what a rename and a delete do\n@param handlers The handlers (files.Handlers)", fn = function(given)
    handlers = given or {}
  end },
  selected = { sig = "string(handle<ecs.panel> panel)", doc = "The chosen file of a files panel\n@param panel The files panel\n@return The file's path in the folder, or an empty text", fn = function(panel)
    return panels[panel] and panels[panel].selected or ""
  end },
})
