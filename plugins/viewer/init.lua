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

local function load(state, path)
  if state.voice then
    mixer.stop(state.voice)
  end
  state.path, state.kind, state.lines, state.sound, state.voice, state.error = path, nil, nil, nil, nil, nil
  state.panel.title = path and path:match("[^/\\]*$") or "Viewer"
  if not path then
    return
  end
  local info = fs.info(path)
  if not info or info.type ~= "file" then
    state.error = "No such file."
    return
  end
  state.kind = kindOf(path)
  state.size = info.size
  if state.kind == "image" then
    -- Only the image's size is needed here: ui reads the file itself.
    local loaded = image.load(path, 1)
    if loaded then
      state.imageWidth, state.imageHeight = loaded:size()
      loaded:free()
    end
  end
  if state.kind == "sound" then
    local sound, err = audio.load(path)
    state.sound, state.error = sound, not sound and tostring(err) or nil
  elseif state.kind == "text" then
    state.lines = textLines(path)
    if not state.lines then
      state.kind = "other"
    end
  end
  state.panel:redraw()
end

local function draw(state, surface)
  local children = {}
  if not state.path then
    children[1] = ui.text("No file is open.", { style = STYLES.note })
  else
    children[#children + 1] = ui.text(("%s, %d bytes"):format(state.path, state.size or 0), { style = STYLES.note })
    if state.error then
      children[#children + 1] = ui.text(state.error, { color = "#e06c75", wrap = true })
    elseif state.kind == "image" then
      -- The image fills the panel: a big one whole, and a small one by a whole number of times, at most 16.
      local w, h = state.imageWidth or 0, state.imageHeight or 0
      local roomW, roomH = surface.width / surface.scale - 16, surface.height / surface.scale - 48
      local props = {}
      if w > 0 and h > 0 and roomW > 0 and roomH > 0 then
        local fit = math.min(roomW / w, roomH / h)
        fit = fit >= 1 and math.min(16, math.floor(fit)) or fit
        props = { width = w * fit, height = h * fit }
        children[1] = ui.text(("%s, %d bytes, %d × %d, shown at %d%%"):format(state.path, state.size or 0, w, h, math.floor(fit * 100 + 0.5)), { style = STYLES.note })
      end
      children[#children + 1] = ui.image(state.path, props)
    elseif state.kind == "sound" then
      children[#children + 1] = ui.row({ gap = 8, align = "center" }, {
        ui.button("Play", { id = "play", onClick = function()
          if state.voice then
            mixer.stop(state.voice)
          end
          state.voice = state.sound:play(1, false)
        end }),
        ui.button("Stop", { id = "stop", onClick = function()
          if state.voice then
            mixer.stop(state.voice)
            state.voice = nil
          end
        end }),
        ui.text(("%.2f seconds"):format(state.sound:length()), {}),
      })
    elseif state.kind == "text" then
      local items = {}
      for i, spans in ipairs(state.lines) do
        items[i] = ui.text(#spans > 0 and spans or " ", { font = "mono" })
      end
      children[#children + 1] = ui.scroll({ id = "text" }, items)
    else
      children[#children + 1] = ui.text("The viewer shows images, sounds and texts.", { style = STYLES.note })
    end
  end
  ui.show(state.panel, surface, ui.column({ padding = 8, gap = 6 }, children))
end

ecs.panel.registerType({
  name = "file",
  title = "Viewer",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local state = { panel = panel }
    panels[panel] = state
    if version == STATE_VERSION and type(saved) == "table" and saved.path then
      load(state, saved.path)
    end
    return state
  end,
  draw = draw,
  event = function(state, event)
    ui.event(state.panel, event)
  end,
  saveState = function(state)
    return { path = state.path }
  end,
  destroy = function(state)
    if state.voice then
      mixer.stop(state.voice)
    end
    ui.forget(state.panel)
    panels[state.panel] = nil
  end,
})

ecs.service.register({
  open = { sig = "void(string path)", doc = "Show a file in a viewer: the one that shows it, the first one open, or a new one\n@param path The file's path", fn = function(path)
    local free
    for panel, state in pairs(panels) do
      if state.path == path then
        load(state, path)
        ecs.layout.focus(panel)
        return
      end
      free = free or state
    end
    if free then
      load(free, path)
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
      load(free, path)
    end
  end },
  reload = { sig = "void(string path)", doc = "Read a file again in the viewers that show it\n@param path The file's path", fn = function(path)
    for _, state in pairs(panels) do
      if state.path == path then
        load(state, path)
      end
    end
  end },
})
