-- The viewer: it shows a file as what it is, with the standard loaders: an image with image, through ui,
-- a sound with audio and mixer, and a text with syntax. It knows nothing of the tool that uses it.

local ecs = require("ecs")
local ui = require("ui")
local fs = require("fs")
local audio = require("audio")
local image = require("image")
local mixer = require("mixer")
local syntax = require("syntax")

local STYLES = ui.styles()
local STATE_VERSION = 1
local IMAGES = { png = true, jpg = true, jpeg = true, bmp = true, tga = true, gif = true, webp = true }
local SOUNDS = { wav = true, ogg = true, mp3 = true, flac = true }
-- A text of more lines shows its start only.
local LINES = 2000

local panels = {} ---@type table<ecs.Panel, table>

local function kindOf(path)
  local ending = (path:match("%.(%w+)$") or ""):lower()
  return IMAGES[ending] and "image" or SOUNDS[ending] and "sound" or "text"
end

-- A text's lines as spans, in syntax's colours when syntax knows its language; nothing for a file that is not text.
local function textLines(path)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local text = file:read(1024 * 1024) or ""
  file:close()
  if text:find("\0", 1, true) then
    return nil
  end
  text = text:gsub("\r", ""):gsub("\t", "  ")
  local language = syntax.languageOf(path)
  local lines = language and syntax.spans(language, text)
  if not lines then
    lines = {}
    for line in (text .. "\n"):gmatch("(.-)\n") do
      lines[#lines + 1] = line ~= "" and { { text = line } } or {}
    end
  end
  for i = #lines, LINES + 1, -1 do
    lines[i] = nil
  end
  return lines
end

-- each panel's state is an object of Viewer, whose functions are methods
local Viewer = {}
Viewer.__index = Viewer

function Viewer:load(path)
  if self.voice then
    mixer.stop(self.voice)
  end
  self.path, self.kind, self.lines, self.sound, self.voice, self.error = path, nil, nil, nil, nil, nil
  self.panel.title = path and path:match("[^/\\]*$") or "Viewer"
  if not path then
    return
  end
  local info = fs.info(path)
  if not info or info.type ~= "file" then
    self.error = "No such file."
    return
  end
  self.kind = kindOf(path)
  self.size = info.size
  if self.kind == "image" then
    -- Only the image's size is needed here: ui reads the file itself.
    local loaded = image.load(path, 1)
    if loaded then
      self.imageWidth, self.imageHeight = loaded:size()
      loaded:free()
    end
  end
  if self.kind == "sound" then
    local sound, err = audio.load(path)
    self.sound, self.error = sound, not sound and tostring(err) or nil
  elseif self.kind == "text" then
    self.lines = textLines(path)
    if not self.lines then
      self.kind = "other"
    end
  end
  self.panel:redraw()
end

function Viewer:draw(surface)
  local children = {}
  if not self.path then
    children[1] = ui.text("No file is open.", { style = STYLES.note })
  else
    children[#children + 1] = ui.text(("%s, %d bytes"):format(self.path, self.size or 0), { style = STYLES.note, wrap = true })
    if self.error then
      children[#children + 1] = ui.text(self.error, { color = "#e06c75", wrap = true })
    elseif self.kind == "image" then
      -- The image fills the panel: a big one whole, and a small one by a whole number of times, at most 16.
      local w, h = self.imageWidth or 0, self.imageHeight or 0
      local roomW, roomH = surface.width / surface.scale - 16, surface.height / surface.scale - 48
      local props = {}
      if w > 0 and h > 0 and roomW > 0 and roomH > 0 then
        local fit = math.min(roomW / w, roomH / h)
        fit = fit >= 1 and math.min(16, math.floor(fit)) or fit
        props = { width = w * fit, height = h * fit }
        children[1] = ui.text(("%s, %d bytes, %d × %d, shown at %d%%"):format(self.path, self.size or 0, w, h, math.floor(fit * 100 + 0.5)), { style = STYLES.note, wrap = true })
      end
      children[#children + 1] = ui.image(self.path, props)
    elseif self.kind == "sound" then
      children[#children + 1] = ui.row({ gap = 8, align = "center" }, {
        ui.button("Play", { id = "play", onClick = function()
          if self.voice then
            mixer.stop(self.voice)
          end
          self.voice = self.sound:play(1, false)
        end }),
        ui.button("Stop", { id = "stop", onClick = function()
          if self.voice then
            mixer.stop(self.voice)
            self.voice = nil
          end
        end }),
        ui.text(("%.2f seconds"):format(self.sound:length()), {}),
      })
    elseif self.kind == "text" then
      local items = {}
      for i, spans in ipairs(self.lines) do
        items[i] = ui.text(#spans > 0 and spans or " ", { font = "mono", wrap = true })
      end
      children[#children + 1] = ui.scroll({ id = "text" }, items)
    else
      children[#children + 1] = ui.text("The viewer shows images, sounds and texts.", { style = STYLES.note })
    end
  end
  ui.show(self.panel, surface, ui.column({ padding = 8, gap = 6 }, children))
end

function Viewer.create(panel, saved, version)
  local state = setmetatable({ panel = panel }, Viewer)
  panels[panel] = state
  if version == STATE_VERSION and type(saved) == "table" and saved.path then
    state:load(saved.path)
  end
  return state
end

function Viewer:event(event)
  ui.event(self.panel, event)
end

function Viewer:saveState()
  return { path = self.path }
end

function Viewer:destroy()
  if self.voice then
    mixer.stop(self.voice)
  end
  ui.forget(self.panel)
  panels[self.panel] = nil
end

ecs.panel.registerType({
  name = "file",
  title = "Viewer",
  stateVersion = STATE_VERSION,
  create = Viewer.create,
  draw = Viewer.draw,
  event = Viewer.event,
  saveState = Viewer.saveState,
  destroy = Viewer.destroy,
})

ecs.service.register({
  open = { sig = "void(string path)", doc = "Show a file in a viewer: the one that shows it, the first one open, or a new one\n@param path The file's path", fn = function(path)
    local free
    for panel, state in pairs(panels) do
      if state.path == path then
        state:load(path)
        ecs.layout.focus(panel)
        return
      end
      free = free or state
    end
    if free then
      free:load(path)
      ecs.layout.focus(free.panel)
    else
      ecs.layout.open("viewer.file", { path = path })
    end
  end },
  show = { sig = "void(string path)", doc = "Show a file in the viewer that shows it, or the first one open, as a preview; with no viewer open, do nothing\n@param path The file's path", fn = function(path)
    local free
    for _, state in pairs(panels) do
      if state.path == path then
        return
      end
      free = free or state
    end
    if free then
      free:load(path)
    end
  end },
  reload = { sig = "void(string path)", doc = "Read a file again in the viewers that show it\n@param path The file's path", fn = function(path)
    for _, state in pairs(panels) do
      if state.path == path then
        state:load(path)
      end
    end
  end },
})
