-- The files panel: the files of one folder, apart from what starts with a dot. It makes folders and files, renames and
-- moves them, and deletes them after asking. The tool that uses it gives the folder, and says with handlers how a file
-- opens, which files it makes, and what a rename and a delete do besides.

local ecs = require("ecs")
local ui = require("ui")
local fs = require("fs")
local process = require("process")

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

-- The nodes of a search: the files and folders whose name matches it (ui.matches), and the folders above them, which
-- open adds as open; nil when nothing under a node matches.
local function found(list, search, open)
  local kept = {}
  for _, node in ipairs(list) do
    local children = node.children and found(node.children, search, open)
    if children and #children > 0 then
      open[node.id] = true
      kept[#kept + 1] = { id = node.id, label = node.label, children = children }
    elseif ui.matches(search, node.label) then
      kept[#kept + 1] = { id = node.id, label = node.label, children = node.children and {} or nil }
    end
  end
  return kept
end

-- Every file and folder whose name matches a search, under a list of nodes, for the grid's search.
local function flat(list, search, out)
  for _, node in ipairs(list) do
    if ui.matches(search, node.label) then
      out[#out + 1] = node
    end
    flat(node.children or {}, search, out)
  end
  return out
end

-- each panel's state is an object of Files, whose functions are methods
local Files = {}
Files.__index = Files

function Files:say(ok, message)
  self.message = not ok and message or nil
  self.panel:redraw()
end

-- Shows a file or folder in the system's file manager: chosen, through the file manager's own interface on Linux, or else the
-- folder that holds it; with Explorer's /select on Windows.
function Files:reveal(path)
  local full = folder .. path
  local started, err
  if package.config:sub(1, 1) == "\\" then
    started, err = process.start({ "explorer", "/select," .. full:gsub("/", "\\") }, "")
  else
    local out, code = process.run({ "dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1", "--type=method_call",
      "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems", "array:string:file://" .. full, "string:" }, "", "")
    if out and code == 0 then
      return
    end
    started, err = process.start({ "xdg-open", full:match("^(.*/)[^/]*$") or folder }, "")
  end
  if not started then
    self:say(false, "The file manager did not open: " .. tostring(err))
  end
end

local function isFolder(path)
  local info = fs.info(folder .. path)
  return info ~= nil and info.type == "folder"
end

-- The folder that a new file goes to: the chosen folder, or the chosen file's.
function Files:folderOf()
  local chosen = self.selected
  if not chosen then
    return ""
  elseif isFolder(chosen) then
    return chosen .. "/"
  end
  return (chosen:match("^(.*/)[^/]*$") or "")
end

function Files:rename(to)
  local from = self.selected
  if not from or to == "" or to == from then
    return
  end
  if fs.info(folder .. to) then
    return self:say(false, to .. " is there already.")
  end
  if handlers.rename then
    self:say(handlers.rename(from, to))
  else
    local parent = to:match("^(.*)/[^/]*$")
    if parent then
      fs.makeFolder(folder .. parent)
    end
    self:say(os.rename(folder .. from, folder .. to))
  end
  self.selected = to
  ecs.event.emit("files.changed")
end

-- A free path for a file of this name in a folder of the panel's: the name, or with a number before its ending, "apple 2.png".
local function freePath(dir, name)
  local base, ending = name:match("^(.+)(%.[^.]*)$")
  base, ending = base or name, base and ending or ""
  local path, n = dir .. name, 1
  while fs.info(folder .. path) do
    n += 1
    path = ("%s%s %d%s"):format(dir, base, n, ending)
  end
  return path
end

-- Copies a file, or a folder with all in it, from anywhere to a full path.
local function copyAll(from, to)
  local info = fs.info(from)
  if not info then
    return false, from .. " is gone."
  elseif info.type ~= "folder" then
    return fs.copy(from, to)
  end
  local ok, why = fs.makeFolder(to)
  if not ok then
    return ok, why
  end
  for _, entry in ipairs(fs.list(from, "") or {}) do
    local copied, failure = copyAll(from .. "/" .. entry.name, to .. "/" .. entry.name)
    if not copied then
      return copied, failure
    end
  end
  return true
end

-- A path with / for \\, and without the . and .. that it passes through, so a path that leaves the folder is not taken for one in it.
local function normal(path)
  path = path:gsub("\\", "/")
  local lead = path:match("^/") or path:match("^%a:/") or ""
  local parts = {}
  for part in path:sub(#lead + 1):gmatch("[^/]+") do
    if part == ".." and #parts > 0 and parts[#parts] ~= ".." then
      parts[#parts] = nil
    elseif part ~= "." then
      parts[#parts + 1] = part
    end
  end
  return lead .. table.concat(parts, "/")
end

-- Files dropped on a folder of the panel's, dir, which ends with a slash or is empty: a file of the folder moves there, with the
-- tool's rename when it has one, and a file from elsewhere is copied there, under a free name.
function Files:dropFiles(paths, dir)
  if type(paths) ~= "table" or folder == "" then
    return
  end
  local changed = false
  for _, path in ipairs(paths) do
    path = normal(tostring(path))
    local name = path:match("([^/]+)$")
    local root = normal(folder) .. "/"
    if name and path:sub(1, #root) == root then
      local from = path:sub(#root + 1)
      local to = dir .. name
      if to ~= from and dir:sub(1, #from + 1) ~= from .. "/" then
        self.selected = from
        self:rename(to)
        changed = true
      end
    elseif name then
      local to = freePath(dir, name)
      self:say(copyAll(path, folder .. to))
      self.selected = to
      changed = true
    end
  end
  if changed then
    if dir ~= "" then
      self.open[dir:sub(1, -2)] = true
    end
    ecs.event.emit("files.changed")
    self.panel:redraw()
  end
end

-- The folder that a drop on an entry goes into: a folder itself, when the drop is into it or on its tile, or the entry's folder.
local function dropFolder(id, into)
  if not id then
    return ""
  elseif into and isFolder(id) then
    return id .. "/"
  end
  return id:match("^(.*/)[^/]*$") or ""
end

function Files:delete()
  local path = self.selected
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
  self.selected = nil
  ecs.event.emit("files.changed")
end

-- makes a file of a kind, or renames the chosen file, with the name that the field took
function Files:finish(asking, name)
  if name == "" then
    return self:say(false, "Type a name first.")
  end
  if asking.kind == "rename" then
    local to = name:find("/", 1, true) and name or ((asking.path:match("^(.*/)[^/]*$") or "") .. name)
    self.selected = asking.path
    return self:rename(to)
  end
  local path = asking.folder .. name
  if asking.kind == "folder" then
    self:say(fs.makeFolder(folder .. path))
  elseif asking.kind == "file" then
    if fs.info(folder .. path) then
      return self:say(false, path .. " is there already.")
    end
    local file = io.open(folder .. path, "wb")
    if file then
      file:close()
    end
    self:say(file ~= nil, "The file could not be made.")
  else
    self:say(handlers.kinds[asking.kind].make(path))
  end
  ecs.event.emit("files.changed")
end

-- asks for a name in the field above the tree, which gets the keys
function Files:ask(asking)
  self.asking, self.message = asking, nil
  ui.edit(self.panel, "name")
  self.panel:redraw()
end

-- the menu of a right click: about the file under it, which the click chose, or beside the files about the folder; New makes a
-- file in the chosen folder, or in the chosen file's
menu = function(state, id, x, y, into)
  into = into or id and state:folderOf() or ""
  local entries = {
    { label = "New folder", onClick = function() state:ask({ kind = "folder", folder = into, label = "New folder", text = "" }) end },
  }
  for i, kind in ipairs(handlers.kinds or {}) do
    entries[#entries + 1] = { label = "New " .. kind.label:lower(), onClick = function()
      state:ask({ kind = i, folder = into, label = "New " .. kind.label:lower(), text = "" })
    end }
  end
  entries[#entries + 1] = { label = "New file", onClick = function() state:ask({ kind = "file", folder = into, label = "New file", text = "" }) end }
  if id then
    local isDir = isFolder(id)
    entries[#entries + 1] = { line = true }
    entries[#entries + 1] = { label = "Open", enabled = not isDir and handlers.open ~= nil, onClick = function() handlers.open(id) end }
    entries[#entries + 1] = { label = "Show in file manager", onClick = function() state:reveal(id) end }
    entries[#entries + 1] = { label = "Rename", onClick = function()
      state:ask({ kind = "rename", path = id, label = "Rename", text = id:match("[^/]*$") })
    end }
    entries[#entries + 1] = { label = "Delete", onClick = function()
      state.selected = id
      state:delete()
    end }
  end
  ui.menu(state.panel, x, y, entries)
end

-- a click on a file or folder: a folder opens or closes in the tree, or the grid goes into it, and a second click on a file soon
-- after the first, a double click, opens it with the tool's handler
function Files:activate(id)
  if isFolder(id) then
    if self.view == "grid" then
      self.dir, self.selected = id .. "/", nil
    else
      self.open[id] = not self.open[id] or nil
    end
  elseif self.clicked == id then
    self.clicked = nil
    if handlers.open then
      handlers.open(id)
    end
  else
    self.clicked = id
    ecs.timer.start(0.4, false, function()
      if self.clicked == id then
        self.clicked = nil
      end
    end)
  end
end

-- chooses a file, and shows it with the tool's handler
function Files:choose(id)
  self.selected = id
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
function Files:grid(children)
  local dir = self.dir
  if dir ~= "" and not isFolder(dir:sub(1, -2)) then
    dir, self.dir = "", ""
  end
  local crumbs = { ui.button(folder:match("([^/\\]+)[/\\]$") or "/", { id = "crumb:", tooltip = "Show this folder", onClick = function()
    self.dir = ""
  end }) }
  local walked = ""
  for part in dir:gmatch("([^/]+)/") do
    walked ..= part .. "/"
    local at = walked
    crumbs[#crumbs + 1] = ui.text("›", { style = STYLES.note })
    crumbs[#crumbs + 1] = ui.button(part, { id = "crumb:" .. at, tooltip = "Show this folder", onClick = function()
      self.dir = at
    end })
  end
  children[#children + 1] = ui.row({ gap = 4, align = "center", wrap = true }, crumbs)
  local tiles = {}
  self.tiles = {}
  local entries = nodes(dir:sub(1, -2))
  if self.search then
    entries = flat(entries, self.search, {})
  end
  for _, entry in ipairs(entries) do
    local id = entry.id
    self.tiles[#self.tiles + 1] = id
    tiles[#tiles + 1] = ui.column({
      id = "tile:" .. id, width = TILE, padding = 4, gap = 4, align = "center", tooltip = id,
      background = self.selected == id and "selected" or nil, hoverBackground = "tab",
      onClick = function()
        self:activate(id)
      end,
      onMenu = function(x, y)
        self:choose(id)
        menu(self, id, x, y)
      end,
      onDrop = function(_, value)
        self:dropFiles(value, entry.children and dropFolder(id, true) or dir)
      end,
    }, {
      icon(id, entry.children ~= nil),
      ui.text(entry.label, { size = 12 }),
    })
  end
  children[#children + 1] = ui.scroll({ id = "grid", grow = 1, onMenu = function(x, y)
    menu(self, nil, x, y, dir)
  end, onDrop = function(_, value)
    self:dropFiles(value, dir)
  end }, {
    #tiles > 0 and ui.row({ gap = 4, wrap = true }, tiles)
      or ui.text(self.search and "No file's name holds the text." or "The folder is empty.", { style = STYLES.note }),
  })
end

function Files:draw(surface)
  self.scale = surface.scale
  local children = {}
  if folder == "" then
    children[1] = ui.text("No folder is open.", { style = STYLES.note })
  else
    -- the name field shows while a menu's New or Rename asks for a name: Return makes the file or renames it
    local asking = self.asking
    if asking then
      children[#children + 1] = ui.row({ gap = 4, align = "center" }, {
        ui.text(asking.label),
        ui.field(asking.text, { id = "name", grow = 1, tooltip = "Return takes the name; a name with folders moves the file", onChange = function(text)
          asking.text = text
        end, onSubmit = function(text)
          self.asking = nil
          self:finish(asking, text)
        end }),
        ui.button("Cancel", { id = "cancel", tooltip = "Close the field", onClick = function()
          self.asking = nil
        end }),
      })
    end
    if self.message then
      children[#children + 1] = ui.text(tostring(self.message), { color = "#e06c75", wrap = true })
    end
    children[#children + 1] = ui.row({ gap = 6, align = "center" }, {
      ui.selector(self.view == "grid" and 2 or 1, { "Tree", "Grid" }, {
        id = "view", tooltip = "The folder as a tree, or one folder at a time as a grid of icons", onSelect = function(choice)
          self.view = choice == 2 and "grid" or "tree"
        end,
      }),
      ui.toggle(self.locked, { id = "lock", label = "Lock", tooltip = "Stay where it is when the tool shows another file", onChange = function(on)
        self.locked = on
      end }),
      ui.search(self.search or "", { grow = 1, placeholder = "Search files", onChange = function(text)
        self.search = text ~= "" and text or nil
      end }),
    })
  end
  if folder ~= "" and self.view == "grid" then
    self:grid(children)
  elseif folder ~= "" then
    local shown, open = nodes(""), self.open
    if self.search then
      open = {}
      shown = found(shown, self.search, open)
    end
    children[#children + 1] = ui.tree(shown, {
      id = "tree", open = open, selected = self.selected, grow = 1, tooltip = "A right click opens a menu",
      onSelect = function(id)
        self:choose(id)
      end,
      -- a file dragged out of the tree is a list of paths, as another application's files come
      onDrag = function(id)
        return "file-list", { folder .. id }
      end,
      -- files dropped on a folder go into it, and on a file, or beside the files, into its folder
      onDrop = function(id, where, _, value)
        self:dropFiles(value, dropFolder(id, where == "into"))
      end,
      onActivate = function(id)
        self:activate(id)
      end,
      onToggle = function(id, opened)
        if not self.search then
          self.open[id] = opened or nil
        end
      end,
      onMenu = function(id, x, y)
        menu(self, id, x, y)
      end,
    })
  end
  ui.show(self.panel, surface, ui.column({ padding = 8, gap = 6 }, children))
  -- a file that files.reveal showed is scrolled to once the folders above it are laid out open
  if self.revealing then
    local path = self.revealing
    self.revealing = nil
    ui.reveal(self.panel, self.view == "grid" and "grid" or "tree", self.view == "grid" and "tile:" .. path or path)
  end
end

function Files.create(panel, saved, version)
  local state = setmetatable({ panel = panel, open = {}, view = "tree", dir = "", tiles = {}, locked = false }, Files)
  if version == STATE_VERSION and type(saved) == "table" then
    state.locked = saved.locked == true
    state.open = saved.open or {}
    state.view = saved.view == "grid" and "grid" or "tree"
    state.dir = type(saved.dir) == "string" and saved.dir or ""
  end
  panels[panel] = state
  -- files from another application, and those that a files panel drags, come as a list of paths
  panel:acceptDrops({ "file-list" })
  return state
end

function Files:event(event)
  -- A file is dragged out of the grid as a list of paths, as the tree drags it.
  if event.type == "pointerDown" then
    self.pressed, self.pressX, self.pressY = nil, event.x, event.y
    -- in the grid, a press chooses the tile under it, which a drag takes
    if self.view == "grid" and event.button == 1 then
      local scale = self.scale or 1
      for _, id in ipairs(self.tiles) do
        local rect = ui.rect(self.panel, "tile:" .. id)
        local x, y = event.x / scale, event.y / scale
        if rect and x >= rect.x and y >= rect.y and x < rect.x + rect.width and y < rect.y + rect.height then
          self.pressed = id
          self:choose(id)
          self.panel:redraw()
        end
      end
    end
  elseif event.type == "pointerMove" and self.pressed then
    if math.abs(event.x - self.pressX) + math.abs(event.y - self.pressY) > 8 then
      self.panel:startDrag("file-list", { folder .. self.pressed })
      self.pressed = nil
    end
  elseif event.type == "pointerUp" then
    self.pressed = nil
  end
  ui.event(self.panel, event)
end

function Files:saveState()
  return { open = self.open, view = self.view, dir = self.dir, locked = self.locked, selected = self.selected }
end

function Files:destroy()
  ui.forget(self.panel)
  panels[self.panel] = nil
end

ecs.panel.registerType({
  name = "tree",
  title = "Files",
  stateVersion = STATE_VERSION,
  create = Files.create,
  draw = Files.draw,
  event = Files.event,
  saveState = Files.saveState,
  destroy = Files.destroy,
})

-- Shows a file in each files panel that is not locked: chooses it, without the tool's choose, opens the folders above it in the tree,
-- or shows its folder in the grid, and scrolls to it.
local function show(path)
  if folder == "" or not fs.info(folder .. path) then
    return
  end
  for panel, state in pairs(panels) do
    if not state.locked then
      state.selected = path
      local above = ""
      for part in path:gmatch("([^/]+)/") do
        above = above == "" and part or (above .. "/" .. part)
        state.open[above] = true
      end
      state.dir = path:match("^(.*/)[^/]*$") or ""
      if state.search and not ui.matches(state.search, path:match("[^/]*$")) then
        state.search = nil
      end
      state.revealing = path
      panel:redraw()
    end
  end
end

ecs.service.register({
  reveal = { sig = "void(string path)", doc = "Show a file in each files panel that is not locked: choose it, open the folders above it and scroll to it\n@param path The file's path in the folder", fn = show },
  lock = { sig = "void(handle<ecs.panel> panel)", doc = "Lock a files panel, so it stays where it is when the tool shows another file, or unlock it\n@param panel The files panel", fn = function(panel)
    local state = panels[panel]
    if state then
      state.locked = not state.locked
      panel:redraw()
    end
  end },
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
