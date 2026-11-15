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
local handlers = {}

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

local function draw(state, surface)
  local children = {}
  if folder == "" then
    children[1] = ui.text("No folder is open.", { style = STYLES.note })
  else
    local kinds = { "Folder" }
    for _, kind in ipairs(handlers.kinds or {}) do
      kinds[#kinds + 1] = kind.label
    end
    kinds[#kinds + 1] = "File"
    children[#children + 1] = ui.row({ gap = 4, align = "center" }, {
      ui.field(state.name, { id = "name", grow = 1, tooltip = "A name, for a new file or a rename", onChange = function(text)
        state.name = text
      end }),
      ui.dropdown(math.min(state.kind, #kinds), kinds, { id = "kind", tooltip = "What New makes", onSelect = function(choice)
        state.kind = choice
      end }),
      ui.button("New", { id = "new", tooltip = "Make a new file of this name and kind", onClick = function()
        local choice = math.min(state.kind, #kinds)
        if state.name == "" then
          return say(state, false, "Type a name first.")
        end
        local path = folderOf(state) .. state.name
        if choice == 1 then
          say(state, fs.makeFolder(folder .. path))
        elseif choice == #kinds then
          if fs.info(folder .. path) then
            return say(state, false, path .. " is there already.")
          end
          local file = io.open(folder .. path, "wb")
          if file then
            file:close()
          end
          say(state, file ~= nil, "The file could not be made.")
        else
          say(state, handlers.kinds[choice - 1].make(path))
        end
        ecs.event.emit("files.changed")
      end }),
    })
    children[#children + 1] = ui.row({ gap = 4 }, {
      ui.button("Rename", { id = "rename", tooltip = "Give the chosen file the name; a name with folders moves it", onClick = function()
        if state.selected and state.name ~= "" then
          local to = state.name:find("/", 1, true) and state.name or ((state.selected:match("^(.*/)[^/]*$") or "") .. state.name)
          rename(state, to)
        end
      end }),
      ui.button("Delete", { id = "delete", onClick = function()
        delete(state)
      end }),
    })
    if state.message then
      children[#children + 1] = ui.text(tostring(state.message), { color = "#e06c75", wrap = true })
    end
    children[#children + 1] = ui.tree(nodes(""), {
      id = "tree", open = state.open, selected = state.selected, grow = 1,
      onSelect = function(id)
        state.selected, state.pressed = id, id
        if handlers.choose and not isFolder(id) then
          handlers.choose(id)
        end
      end,
      onActivate = function(id)
        if isFolder(id) then
          state.open[id] = not state.open[id] or nil
        elseif state.clicked == id then
          -- a double click opens a file: the second click, soon after the first
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
      end,
      onToggle = function(id, open)
        state.open[id] = open or nil
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
    local state = { panel = panel, open = {}, name = "", kind = 1 }
    if version == STATE_VERSION and type(saved) == "table" then
      state.open = saved.open or {}
    end
    panels[panel] = state
    return state
  end,
  draw = draw,
  event = function(state, event)
    -- A file is dragged out of the tree as a list of paths, as another application's files come.
    if event.type == "pointerDown" then
      state.pressed, state.pressX, state.pressY = nil, event.x, event.y
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
    return { open = state.open }
  end,
  destroy = function(state)
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  setFolder = { sig = "void(string folder)", doc = "Show a folder, by its path ending with a slash; an empty text shows none", fn = setFolder },
  folder = { sig = "string()", doc = "The shown folder", fn = function()
    return folder
  end },
  setHandlers = { sig = "void(value handlers)", doc = "Say how files open, which files New makes, and what a rename and a delete do (files.Handlers)", fn = function(given)
    handlers = given or {}
  end },
  selected = { sig = "string(handle<ecs.panel> panel)", doc = "The chosen file of a files panel, by its path in the folder", fn = function(panel)
    return panels[panel] and panels[panel].selected or ""
  end },
})
