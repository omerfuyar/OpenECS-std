-- The ui standard plugin: user-interface elements with layout (DESIGN 5).
-- A panel describes its elements in draw, each time, and ui lays them out, draws them with the draw plugin, and calls their callbacks on events.
-- Its elements are generic, so that plugins build their own views from them.

local ecs = require("ecs")
local draw = require("draw")
local image = require("image")

-- sizes in layout units, which the user can change as settings of the plugin
local SIZES = {
  { "paddingX",       10, "Room left and right of the text of buttons, fields and lists" },
  { "paddingY",       5,  "Room above and below the text of buttons, fields and lists" },
  { "checkSize",      16, "Size of a toggle's box, and of a slider's knob" },
  { "fieldWidth",     120, "Width of a field or slider that is not stretched" },
  { "scrollbarWidth", 8,  "Width of a scroll bar" },
  { "thumbMin",       24, "Shortest length of a scroll bar's thumb" },
  { "wheelStep",      48, "Distance a wheel step scrolls" },
  { "margin",         24, "Room around a view of a first-party plugin, such as the launcher" },
  { "rowPadding",     8,  "Room around a row of a list" },
  { "titleSize",      22, "Font size of a view's title" },
  { "noteSize",       13, "Font size of notes and headings" },
}

local size = {}

local function readSizes()
  for _, entry in ipairs(SIZES) do
    size[entry[1]] = ecs.settings.get("ui." .. entry[1]) or entry[2]
  end
end

for _, entry in ipairs(SIZES) do
  assert(ecs.settings.declare({ name = entry[1], type = "number", default = entry[2], description = entry[3] }))
end

readSizes()

-- the colour of a field whose text is not valid
local INVALID = "#E0505A"

-- the elements that take the pointer
local INTERACTIVE = { button = true, toggle = true, selector = true, dropdown = true, slider = true, field = true, key = true }

-- every panel's state, by its panel handle: the last layout, the element under the pointer, the pressed element, the focused field and the scroll offsets
local panels = {}

local function panelState(panel)
  local state = panels[panel]

  if not state then
    state = { scroll = {}, scale = 1 }
    panels[panel] = state
  end

  return state
end

local function fontSize()
  return ecs.settings.get("ecs.fontSize") or 14
end

-- the fonts of the current show, by name; draw keeps the fonts themselves
local fonts = {}

local function fontOf(node)
  local name = node.font or "ui"
  local font = fonts[name]

  if font == nil then
    font = draw.font(name) or draw.font("ui")
    fonts[name] = font
  end

  return font
end

local function measureText(font, text, textSize)
  local width = draw.measure(font, text, textSize)
  return width
end

local function lineHeight(font, textSize)
  local _, line = draw.measure(font, "Ag", textSize)
  return line
end

-- the text that fits a width, ended with an ellipsis when it is cut
local function fit(font, text, textSize, width)
  if measureText(font, text, textSize) <= width then
    return text
  end

  local low, high = 0, utf8.len(text) or #text

  while low < high do
    local middle = (low + high + 1) // 2
    local cut = text:sub(1, (utf8.offset(text, middle + 1) or #text + 1) - 1) .. "…"

    if measureText(font, cut, textSize) <= width then
      low = middle
    else
      high = middle - 1
    end
  end

  return text:sub(1, (utf8.offset(text, low + 1) or #text + 1) - 1) .. "…"
end

-- the lines of a text that wraps at a width: it breaks at spaces, and at its own new lines; a word wider than the width gets a line of its own
local function wrap(font, text, textSize, width)
  local lines = {}

  for paragraph in (text .. "\n"):gmatch("(.-)\n") do
    local current = nil

    for word in paragraph:gmatch("%S+") do
      local joined = current and current .. " " .. word or word

      if current and measureText(font, joined, textSize) > width then
        lines[#lines + 1] = current
        current = word
      else
        current = joined
      end
    end

    lines[#lines + 1] = current or ""
  end

  return lines
end

-- images read once for each path and scale, so that they are drawn sharp
local images = {}

local function picture(path, scale)
  local key = path .. "@" .. scale
  local found = images[key]

  if found == nil then
    found = image.load(path, scale) or false
    images[key] = found
  end

  return found or nil
end

-- element constructors -------------------------------------------------------

local function element(kind, props, children)
  local node = {}

  for key, value in pairs(props or {}) do
    node[key] = value
  end

  node.kind = kind
  node.children = children
  return node
end

local elements = {}

function elements.column(props, children)
  return element("column", props, children or {})
end

function elements.row(props, children)
  return element("row", props, children or {})
end

function elements.scroll(props, children)
  local node = element("scroll", props, children or {})
  assert(node.id, "a scroll element needs an id, which keeps its offset")

  if node.grow == nil then
    node.grow = 1
  end

  return node
end

-- a scroll element whose children are items to choose; children with selectable = false, such as headings, are not items
function elements.list(props, children)
  local node = elements.scroll(props, children)
  node.items = {}

  for _, child in ipairs(node.children) do
    if child.selectable ~= false then
      local index = #node.items + 1
      node.items[index] = child
      child.list, child.item = node, index

      -- an item takes the list's whole width, so its highlight does
      if child.stretch == nil then
        child.stretch = true
      end

      if index == node.selected and child.background == nil then
        child.background = "selected"
      end

      if node.onActivate then
        child.hoverBackground = child.hoverBackground or "tab"
        child.onClick = child.onClick or function()
          node.onActivate(index)
        end
      end
    end
  end

  return node
end

-- a column with a frame and a title
function elements.group(props, children)
  local node = element("column", props, {})
  node.padding = node.padding or size.rowPadding
  node.gap = node.gap or size.rowPadding / 2
  node.border = node.border or "tabShown"

  if node.title then
    node.children[1] = elements.text(node.title, { size = size.noteSize, color = "textDim" })
  end

  for _, child in ipairs(children or {}) do
    node.children[#node.children + 1] = child
  end

  return node
end

function elements.separator(props)
  return element("separator", props)
end

function elements.text(text, props)
  local node = element("text", props)
  node.text = tostring(text)
  return node
end

function elements.button(label, props)
  local node = element("button", props)
  node.label = tostring(label)
  return node
end

-- on or off: a check box, or a switch with look = "switch"
function elements.toggle(on, props)
  local node = element("toggle", props)
  node.on = on == true
  return node
end

-- a number between min and max, 0 and 1 if missing, which a drag changes; step rounds it
function elements.slider(value, props)
  local node = element("slider", props)
  node.min, node.max = node.min or 0, node.max or 1
  node.value = math.min(math.max(tonumber(value) or node.min, node.min), node.max)
  return node
end

-- one of a few choices, side by side
function elements.selector(value, choices, props)
  local node = element("selector", props)
  node.value, node.choices = value, choices or {}
  return node
end

-- one of a list of choices, which a click opens in a popup
function elements.dropdown(value, choices, props)
  local node = element("dropdown", props)
  node.value, node.choices = value, choices or {}
  return node
end

function elements.field(text, props)
  local node = element("field", props)
  node.text = tostring(text or "")
  assert(node.id, "a field needs an id, which keeps what is typed")
  return node
end

-- a number that the user types, kept from min to max if they are given and rounded to step if it is given;
-- Return gives the typed number to onChange, and Up and Down change it by step, or by 1
function elements.number(value, props)
  local node = elements.field("", props)
  local onChange = node.onChange or function(_) end
  node.value = tonumber(value) or 0
  node.onChange, node.onSubmit = nil, nil

  -- a number from min to max, rounded to step from min, or from 0
  local function settle(number)
    if node.step and node.step > 0 then
      local from = node.min or 0
      number = from + math.floor((number - from) / node.step + 0.5) * node.step
    end

    return math.min(math.max(number, node.min or -math.huge), node.max or math.huge)
  end

  local function show(number)
    return math.type(number) == "integer" and tostring(number) or ("%g"):format(number)
  end

  node.value = settle(node.value)
  node.text = show(node.value)

  -- a text that is not a number keeps the field typing
  node.onSubmit = function(text)
    local number = tonumber(text)

    if number == nil then
      return false
    end

    onChange(settle(number))
  end

  -- Up and Down change the typed number, or the value if what is typed is not a number, and give the new one at once
  node.change = function(state, up)
    local number = settle((tonumber(state.typed) or node.value) + (up and 1 or -1) * ((node.step and node.step > 0) and node.step or 1))
    state.typed = show(number)
    state.at = #state.typed + 1
    onChange(number)
  end

  return node
end

function elements.key(combination, props)
  local node = element("key", props)
  node.text = tostring(combination or "")
  assert(node.id, "a key element needs an id")
  return node
end

function elements.image(path, props)
  local node = element("image", props)
  node.path = path
  return node
end

function elements.space(props)
  local node = element("space", props)

  if node.grow == nil then
    node.grow = 1
  end

  return node
end

-- layout ---------------------------------------------------------------------

local CONTAINERS = { column = true, row = true, scroll = true }

-- elements that take the whole width of a column when nothing says otherwise
local WIDE = { field = true, key = true, dropdown = true, selector = true, slider = true, separator = true }

-- the axis of a container: true for a row
local function horizontal(node)
  return node.kind == "row"
end

local function padding(node)
  return node.padding or 0
end

-- whether an element takes the whole cross size of its parent when nothing says otherwise:
-- containers always, separators always, and wide elements across a column
local function stretches(node, row)
  if node.stretch ~= nil then
    return node.stretch
  end

  return CONTAINERS[node.kind] or node.kind == "separator" or (not row and WIDE[node.kind])
end

-- the widths of a selector's choices
local function segments(node, font, textSize)
  local widths = {}

  for i, choice in ipairs(node.choices) do
    widths[i] = measureText(font, tostring(choice), textSize) + 2 * size.paddingX
  end

  return widths
end

-- the size an element needs, without growing; containers measure their children first
-- room is the width a column gives its children, which text that wraps breaks at; nil in a row
local function measure(node, state, room)
  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local line = lineHeight(font, textSize)
  local boxed = line + 2 * size.paddingY
  local width, height

  if node.kind == "text" and node.wrap and (node.width or room) then
    node.lines = wrap(font, node.text, textSize, node.width or room)
    width, height = 0, #node.lines * line

    for _, text in ipairs(node.lines) do
      width = math.max(width, measureText(font, text, textSize))
    end
  elseif node.kind == "text" then
    node.lines = nil
    width, height = measureText(font, node.text, textSize), line
  elseif node.kind == "button" then
    width, height = measureText(font, node.label, textSize) + 2 * size.paddingX, boxed
  elseif node.kind == "dropdown" then
    local widest = 0

    for _, choice in ipairs(node.choices) do
      widest = math.max(widest, measureText(font, tostring(choice), textSize))
    end

    width, height = widest + 3 * size.paddingX + line / 2, boxed
  elseif node.kind == "selector" then
    width, height = 0, boxed

    for _, segment in ipairs(segments(node, font, textSize)) do
      width = width + segment
    end
  elseif node.kind == "toggle" then
    local box = node.look == "switch" and 2 * size.checkSize or size.checkSize
    width, height = box, math.max(size.checkSize, line)

    if node.label then
      width = width + size.paddingX + measureText(font, node.label, textSize)
    end
  elseif node.kind == "slider" then
    width, height = size.fieldWidth, math.max(size.checkSize, line)
  elseif node.kind == "field" or node.kind == "key" then
    width, height = size.fieldWidth, boxed
  elseif node.kind == "image" then
    node.picture = picture(node.path, state.scale)

    if node.picture then
      width, height = draw.imageSize(node.picture)
      width, height = width / state.scale, height / state.scale
    else
      width, height = 0, 0
    end
  elseif node.kind == "separator" then
    width, height = 1, 1
  elseif node.kind == "space" then
    width, height = 0, 0
  else
    local across, along = 0, 0
    local count = 0

    -- a column's children may take its width less its padding, and a scroll element's less its scroll bar too; a row's take what they need
    local inner = nil

    if not horizontal(node) and (node.width or room) then
      inner = (node.width or room) - 2 * padding(node) - (node.kind == "scroll" and size.scrollbarWidth + 4 or 0)
    end

    for _, child in ipairs(node.children) do
      local childWidth, childHeight = measure(child, state, inner)
      local childAlong, childAcross = childHeight, childWidth

      if horizontal(node) then
        childAlong, childAcross = childWidth, childHeight
      end

      along = along + childAlong
      across = math.max(across, childAcross)
      count = count + 1
    end

    along = along + math.max(0, count - 1) * (node.gap or 0)
    local pad = padding(node)
    width, height = across + 2 * pad, along + 2 * pad

    if horizontal(node) then
      width, height = along + 2 * pad, across + 2 * pad
    end

    -- a scroll element shows a part of its content, so it needs little room
    if node.kind == "scroll" then
      node.contentHeight = height
      height = 0
    end
  end

  node.naturalWidth = node.width or width
  node.naturalHeight = node.height or height
  return node.naturalWidth, node.naturalHeight
end

-- places an element and its children in a rectangle; measure ran first
local function arrange(node, x, y, width, height, state)
  node.x, node.y, node.w, node.h = x, y, width, height

  if not CONTAINERS[node.kind] then
    return
  end

  local pad = padding(node)
  local innerX, innerY = x + pad, y + pad
  local innerWidth, innerHeight = width - 2 * pad, height - 2 * pad
  local gap = node.gap or 0
  local row = horizontal(node)

  -- the content of a scroll element is as tall as it needs, and moves up by the offset
  if node.kind == "scroll" then
    local content = node.contentHeight - 2 * pad
    local shown = innerHeight
    node.shown, node.total = height, node.contentHeight
    local offset = math.max(0, math.min(state.scroll[node.id] or 0, node.contentHeight - height))
    state.scroll[node.id] = offset
    node.offset = offset

    if node.contentHeight > height then
      innerWidth = innerWidth - size.scrollbarWidth - 4
    end

    innerY = innerY - offset
    innerHeight = math.max(content, shown)
  end

  -- the main axis: the natural sizes, then the room that is left, shared by the growing children
  local used, grows = 0, 0

  for i, child in ipairs(node.children) do
    used = used + (row and child.naturalWidth or child.naturalHeight) + (i > 1 and gap or 0)
    grows = grows + (child.grow == true and 1 or child.grow or 0)
  end

  local room = (row and innerWidth or innerHeight) - used
  local along = row and innerX or innerY

  for _, child in ipairs(node.children) do
    local grow = child.grow == true and 1 or child.grow or 0
    local childSize = row and child.naturalWidth or child.naturalHeight

    if grows > 0 and grow > 0 and node.kind ~= "scroll" then
      childSize = math.max(0, childSize + room * grow / grows)
    end

    -- the cross axis: the whole room, or the natural size aligned in it
    local crossRoom = row and innerHeight or innerWidth
    local cross = row and child.naturalHeight or child.naturalWidth
    local crossAt = row and innerY or innerX

    if stretches(child, row) and not (row and child.height or not row and child.width) then
      cross = crossRoom
    else
      local align = node.align or (row and "center" or "start")
      crossAt = crossAt + (align == "center" and (crossRoom - cross) / 2 or align == "end" and crossRoom - cross or 0)
    end

    -- a separator in a row stands up
    child.vertical = row

    if row then
      arrange(child, along, crossAt, childSize, cross, state)
    else
      arrange(child, crossAt, along, cross, childSize, state)
    end

    along = along + childSize + gap
  end
end

-- drawing --------------------------------------------------------------------

local function contains(node, x, y)
  return x >= node.x and x < node.x + node.w and y >= node.y and y < node.y + node.h
end

-- the rectangle of a scroll element's thumb, along its height
local function thumb(node)
  local length = math.max(math.min(size.thumbMin, node.h), node.h * node.shown / node.total)
  local travel = node.total - node.shown
  local start = travel > 0 and (node.h - length) * node.offset / travel or 0
  return node.y + start, length
end

-- the part of a rectangle inside a clip rectangle, if there is one
local function intersect(rect, clip)
  if not clip then
    return rect
  end

  local right, bottom = math.min(rect.x + rect.w, clip.x + clip.w), math.min(rect.y + rect.h, clip.y + clip.h)
  local x, y = math.max(rect.x, clip.x), math.max(rect.y, clip.y)
  return { x = x, y = y, w = math.max(0, right - x), h = math.max(0, bottom - y) }
end

local function restoreClip(surface, clip)
  if clip then
    draw.clip(surface, clip.x, clip.y, clip.w, clip.h)
  else
    draw.unclip(surface)
  end
end

-- the left edges and widths of a selector's choices, shared out over its width
local function segmentRects(node)
  local widths = segments(node, fontOf(node), node.size or fontSize())
  local total = 0

  for _, width in ipairs(widths) do
    total = total + width
  end

  local rects, x = {}, node.x

  for i, width in ipairs(widths) do
    local shared = total > 0 and width * node.w / total or 0
    rects[i] = { x = x, w = shared }
    x = x + shared
  end

  return rects
end

-- the slider's value at a position across it
local function sliderValue(node, x)
  local radius = size.checkSize / 2
  local travel = node.w - 2 * radius
  local t = travel > 0 and math.min(math.max((x - node.x - radius) / travel, 0), 1) or 0
  local value = node.min + t * (node.max - node.min)

  if node.step and node.step > 0 then
    value = node.min + math.floor((value - node.min) / node.step + 0.5) * node.step
  end

  return math.min(math.max(value, node.min), node.max)
end

-- a small triangle that points down, centred on a point
local function arrow(surface, x, y, half, color)
  draw.triangles(surface, string.pack("ffffff", x - half, y - half / 2, x + half, y - half / 2, x, y + half / 2), "", color)
end

local function paint(node, surface, state, clip)
  local function inside(x, y)
    return not clip or (x >= clip.x and x < clip.x + clip.w and y >= clip.y and y < clip.y + clip.h)
  end

  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local line = lineHeight(font, textSize)
  local textY = node.y + (node.h - line) / 2
  local hover = state.hover == node and inside(state.pointerX or 0, state.pointerY or 0)
  local pressed = state.pressed == node

  local background = hover and node.hoverBackground or node.background

  if background then
    draw.fill(surface, node.x, node.y, node.w, node.h, background)
  end

  if node.border then
    draw.outline(surface, node.x, node.y, node.w, node.h, 1, node.border)
  end

  -- the element that Tab gave the keys shows a ring, which fields and key elements show as their outline
  if state.ring and state.target == node.id and node.kind ~= "field" and node.kind ~= "key" then
    draw.outline(surface, node.x - 2, node.y - 2, node.w + 4, node.h + 4, 2, "accent")
  end

  if node.kind == "text" and node.lines then
    for i, text in ipairs(node.lines) do
      draw.text(surface, font, fit(font, text, textSize, node.w), node.x, node.y + (i - 1) * line, textSize, node.color or "text")
    end
  elseif node.kind == "text" then
    draw.text(surface, font, fit(font, node.text, textSize, node.w), node.x, textY, textSize, node.color or "text")
  elseif node.kind == "button" then
    draw.fill(surface, node.x, node.y, node.w, node.h, pressed and "accent" or hover and "tabShown" or "tab")
    local shown = fit(font, node.label, textSize, node.w - 2 * size.paddingX)
    local width = measureText(font, shown, textSize)
    draw.text(surface, font, shown, node.x + (node.w - width) / 2, textY, textSize, node.color or "text")
  elseif node.kind == "dropdown" then
    draw.fill(surface, node.x, node.y, node.w, node.h, (pressed or hover) and "tabShown" or "tab")
    local room = node.w - 3 * size.paddingX - line / 2
    draw.text(surface, font, fit(font, tostring(node.value or ""), textSize, room), node.x + size.paddingX, textY, textSize, node.color or "text")
    arrow(surface, node.x + node.w - size.paddingX - line / 4, node.y + node.h / 2, line / 4, "textDim")
  elseif node.kind == "selector" then
    for i, rect in ipairs(segmentRects(node)) do
      local choice = node.choices[i]
      local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.w
      draw.fill(surface, rect.x, node.y, rect.w, node.h, choice == node.value and "accent" or over and "tabShown" or "tab")
      local shown = fit(font, tostring(choice), textSize, rect.w - 2 * size.paddingX)
      local width = measureText(font, shown, textSize)
      draw.text(surface, font, shown, rect.x + (rect.w - width) / 2, textY, textSize, node.color or "text")
    end
  elseif node.kind == "toggle" then
    local box = size.checkSize
    local boxY = node.y + (node.h - box) / 2
    local labelX = node.x + box

    if node.look == "switch" then
      draw.fill(surface, node.x, boxY, 2 * box, box, node.on and "accent" or "tabRow")
      draw.outline(surface, node.x, boxY, 2 * box, box, 1, hover and "accent" or "textDim")
      draw.fill(surface, node.x + (node.on and box or 0) + 3, boxY + 3, box - 6, box - 6, "text")
      labelX = node.x + 2 * box
    else
      draw.fill(surface, node.x, boxY, box, box, "tabRow")
      draw.outline(surface, node.x, boxY, box, box, 1, (node.on or hover) and "accent" or "textDim")

      if node.on then
        draw.fill(surface, node.x + 4, boxY + 4, box - 8, box - 8, "accent")
      end
    end

    if node.label then
      draw.text(surface, font, fit(font, node.label, textSize, node.x + node.w - labelX - size.paddingX), labelX + size.paddingX, textY, textSize, node.color or "text")
    end
  elseif node.kind == "slider" then
    local radius = size.checkSize / 2
    local centerY = node.y + node.h / 2
    local span = node.max - node.min
    local knobX = node.x + radius + (span > 0 and (node.value - node.min) / span or 0) * (node.w - 2 * radius)
    draw.fill(surface, node.x + radius, centerY - 2, node.w - 2 * radius, 4, "tabRow")
    draw.fill(surface, node.x + radius, centerY - 2, knobX - node.x - radius, 4, "accent")
    draw.circle(surface, knobX, centerY, radius, 0, (hover or state.sliding == node.id) and "accent" or "text")
  elseif node.kind == "field" or node.kind == "key" then
    local focused = state.focus == node.id
    local text = node.text

    if focused and node.kind == "field" then
      text = state.typed
    elseif focused then
      text = "Press a key"
    end

    draw.fill(surface, node.x, node.y, node.w, node.h, "tabRow")
    local width = measureText(font, text, textSize)
    local room = node.w - 2 * size.paddingX
    local before = (focused and node.kind == "field") and measureText(font, text:sub(1, state.at - 1), textSize) or width

    -- the text moves left when the cursor would leave the field, so the cursor stays in view
    local textX = node.x + size.paddingX - ((focused and before > room) and before - room or 0)
    local inner = intersect({ x = node.x + 1, y = node.y + 1, w = node.w - 2, h = node.h - 2 }, clip)
    draw.clip(surface, inner.x, inner.y, inner.w, inner.h)
    draw.text(surface, font, (focused or width <= room) and text or fit(font, text, textSize, room), textX, textY, textSize,
      (focused and node.kind == "key") and "textDim" or node.color or "text")

    if focused and node.kind == "field" then
      draw.fill(surface, textX + before, textY, 2, line, "accent")
      state.cursor = { x = textX + before, y = textY, h = line }
    end

    restoreClip(surface, clip)
    -- a number element marks a typed text that is not a number
    local valid = node.valid ~= false and not (focused and node.change and tonumber(state.typed) == nil)
    draw.outline(surface, node.x, node.y, node.w, node.h, 1, not valid and INVALID or focused and "accent" or "tabShown")
  elseif node.kind == "image" then
    if node.picture then
      draw.image(surface, node.picture, node.x, node.y, node.w, node.h)
    end
  elseif node.kind == "separator" then
    if node.vertical then
      draw.fill(surface, node.x + node.w / 2 - 0.5, node.y, 1, node.h, node.color or "tabShown")
    else
      draw.fill(surface, node.x, node.y + node.h / 2 - 0.5, node.w, 1, node.color or "tabShown")
    end
  elseif node.kind == "scroll" then
    -- the children draw inside the scroll element only
    local inner = intersect({ x = node.x, y = node.y, w = node.w, h = node.h }, clip)

    draw.clip(surface, inner.x, inner.y, inner.w, inner.h)

    for _, child in ipairs(node.children) do
      if child.y + child.h > node.y and child.y < node.y + node.h then
        paint(child, surface, state, inner)
      end
    end

    restoreClip(surface, clip)

    if node.total > node.shown then
      local start, length = thumb(node)
      local x = node.x + node.w - size.scrollbarWidth
      draw.fill(surface, x, node.y, size.scrollbarWidth, node.h, "tabRow")
      draw.fill(surface, x, start, size.scrollbarWidth, length, state.dragging == node.id and "accent" or "tabShown")
    end

    return
  end

  for _, child in ipairs(node.children or {}) do
    paint(child, surface, state, clip)
  end
end

-- the innermost element at a point that passes a test; scrolled content counts only where it shows
local function find(node, x, y, test)
  if not contains(node, x, y) then
    return nil
  end

  for i = #(node.children or {}), 1, -1 do
    local found = find(node.children[i], x, y, test)

    if found then
      return found
    end
  end

  return test(node) and node or nil
end

-- the elements that take the pointer: buttons, fields and the like, and containers with onClick
local function interactive(node)
  return INTERACTIVE[node.kind] == true or (CONTAINERS[node.kind] == true and node.onClick ~= nil)
end

local function scrollable(node)
  return node.kind == "scroll" and node.total > node.shown
end

local function listItem(node)
  return node.item ~= nil
end

local function byId(node, id)
  if node.id == id then
    return node
  end

  for _, child in ipairs(node.children or {}) do
    local found = byId(child, id)

    if found then
      return found
    end
  end
end

-- events ---------------------------------------------------------------------

-- calls a callback of an element, if it has one
local function call(callback, ...)
  if callback then
    callback(...)
  end
end

local function blur(panel, state)
  if state.focus then
    state.focus, state.typed, state.cursor, state.at = nil, nil, nil, nil
    panel:setTextInput(false)
    panel:redraw()
  end
end

-- the text of a key press as a key combination, such as "Ctrl+Shift+P"
local function combination(event)
  local parts = {}

  for _, modifier in ipairs({ "ctrl", "shift", "alt", "super" }) do
    if event[modifier] then
      parts[#parts + 1] = modifier:sub(1, 1):upper() .. modifier:sub(2)
    end
  end

  parts[#parts + 1] = event.key
  return table.concat(parts, "+")
end

local MODIFIER_KEYS = { ["Left Ctrl"] = true, ["Right Ctrl"] = true, ["Left Shift"] = true, ["Right Shift"] = true, ["Left Alt"] = true, ["Right Alt"] = true, ["Left GUI"] = true, ["Right GUI"] = true }

-- opens a dropdown's choices in a popup below it; a click or Return chooses one
local function openDropdown(panel, state, node)
  local choices = node.choices

  if #choices == 0 then
    return
  end

  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local rowHeight = lineHeight(font, textSize) + 2 * size.paddingY
  local width, height = node.w, rowHeight * #choices
  local scale = state.scale
  local hover = 1
  local popup

  for i, choice in ipairs(choices) do
    if choice == node.value then
      hover = i
    end
  end

  local function choose(index)
    popup:close()
    call(node.onChange, choices[index])
    panel:redraw()
  end

  local function at(y)
    local index = math.floor(y / scale / rowHeight) + 1
    return choices[index] ~= nil and index or nil
  end

  local message
  popup, message = panel:openPopup({
    anchor = { x = node.x * state.scale, y = node.y * state.scale, width = node.w * state.scale, height = node.h * state.scale },
    width = width,
    height = height,
    draw = function(surface)
      scale = surface.scale
      draw.fill(surface, 0, 0, width, height, "tabRow")

      for i, choice in ipairs(choices) do
        local y = (i - 1) * rowHeight

        if i == hover then
          draw.fill(surface, 0, y, width, rowHeight, "tabShown")
        end

        draw.text(surface, font, fit(font, tostring(choice), textSize, width - 2 * size.paddingX), size.paddingX, y + size.paddingY, textSize,
          choice == node.value and "accent" or "text")
      end

      draw.outline(surface, 0, 0, width, height, 1, "tabShown")
    end,
    event = function(event)
      if event.type == "pointerMove" then
        local index = at(event.y)

        if index and index ~= hover then
          hover = index
          popup:redraw()
        end
      elseif event.type == "pointerUp" and event.button == 1 and at(event.y) then
        choose(at(event.y))
      elseif event.type == "keyDown" and (event.key == "Up" or event.key == "Down") then
        hover = math.min(math.max(hover + (event.key == "Up" and -1 or 1), 1), #choices)
        popup:redraw()
      elseif event.type == "keyDown" and (event.key == "Return" or event.key == "Keypad Enter") then
        choose(hover)
      end
    end,
  })

  if not popup then
    ecs.log.warn(message or "Cannot open the choices.")
  end
end

local function activate(panel, state, node, x)
  if node.kind == "button" or CONTAINERS[node.kind] then
    call(node.onClick)
  elseif node.kind == "toggle" then
    call(node.onChange, not node.on)
  elseif node.kind == "selector" then
    for i, rect in ipairs(segmentRects(node)) do
      if x >= rect.x and x < rect.x + rect.w and node.choices[i] ~= node.value then
        call(node.onChange, node.choices[i])
      end
    end
  elseif node.kind == "dropdown" then
    openDropdown(panel, state, node)
  elseif node.kind == "field" or node.kind == "key" then
    if state.focus ~= node.id then
      blur(panel, state)
      state.focus, state.typed, state.at = node.id, node.text, #node.text + 1
      panel:setTextInput(node.kind == "field")
    end
  end
end

-- handles a key or text event while a field or key element has the focus
local function typing(panel, state, event)
  local node = state.root and byId(state.root, state.focus)

  if not node then
    blur(panel, state)
    return false
  end

  if node.kind == "key" then
    if event.type ~= "keyDown" or MODIFIER_KEYS[event.key] then
      return event.type == "keyUp" or event.type == "text"
    end

    if event.key ~= "Escape" then
      call(node.onChange, combination(event))
    end

    blur(panel, state)
    return true
  end

  -- the cursor is a byte position before which text goes; a character may take several bytes
  local typed, at = state.typed, state.at

  if event.type == "text" then
    state.typed = typed:sub(1, at - 1) .. event.text .. typed:sub(at)
    state.at = at + #event.text
    call(node.onChange, state.typed)
  elseif event.type == "keyDown" and event.key == "Backspace" and at > 1 then
    local previous = utf8.offset(typed, -1, at)
    state.typed, state.at = typed:sub(1, previous - 1) .. typed:sub(at), previous
    call(node.onChange, state.typed)
  elseif event.type == "keyDown" and event.key == "Delete" and at <= #typed then
    local next = utf8.offset(typed, 2, at)
    state.typed = typed:sub(1, at - 1) .. typed:sub(next)
    call(node.onChange, state.typed)
  elseif event.type == "keyDown" and event.key == "Left" then
    state.at = at > 1 and utf8.offset(typed, -1, at) or 1
  elseif event.type == "keyDown" and event.key == "Right" then
    state.at = at <= #typed and utf8.offset(typed, 2, at) or at
  elseif event.type == "keyDown" and event.key == "Home" then
    state.at = 1
  elseif event.type == "keyDown" and event.key == "End" then
    state.at = #typed + 1
  elseif event.type == "keyDown" and (event.key == "Return" or event.key == "Keypad Enter") then
    -- onSubmit gives false to keep typing, such as for a text it cannot read
    if not (node.onSubmit and node.onSubmit(state.typed) == false) then
      blur(panel, state)
    end
  elseif event.type == "keyDown" and event.key == "Escape" then
    blur(panel, state)
  elseif event.type == "keyDown" and node.change and (event.key == "Up" or event.key == "Down") then
    node.change(state, event.key == "Up")
  elseif event.type ~= "keyUp" and event.type ~= "keyDown" then
    return false
  end

  panel:redraw()
  return true
end

-- the interactive elements in the order of the tree, which Tab goes through
local function order(node, list)
  if interactive(node) then
    list[#list + 1] = node
  end

  for _, child in ipairs(node.children or {}) do
    order(child, list)
  end

  return list
end

-- the innermost scroll element that holds an element, or nil
local function scrollOf(node, id, found)
  if node.id == id then
    return found
  end

  for _, child in ipairs(node.children or {}) do
    local inner = scrollOf(child, id, node.kind == "scroll" and node or found)

    if inner ~= nil then
      return inner
    end
  end
end

-- gives the keys to the next interactive element, or to the one before; a field or key element takes typing then
local function step(panel, state, back)
  local list = order(state.root, {})

  if #list == 0 then
    return false
  end

  local index = 0

  for i, node in ipairs(list) do
    index = node.id == state.target and i or index
  end

  if back then
    index = index <= 1 and #list or index - 1
  else
    index = index % #list + 1
  end

  local node = list[index]
  blur(panel, state)
  state.target, state.ring = node.id, true

  if node.kind == "field" or node.kind == "key" then
    activate(panel, state, node, node.x)
  end

  local scroll = scrollOf(state.root, node.id, nil)

  if scroll and scroll.id ~= node.id then
    local top = node.y - scroll.y + scroll.offset
    local offset = state.scroll[scroll.id] or 0
    state.scroll[scroll.id] = math.min(math.max(offset, top + node.h - scroll.h), top)
  end

  panel:redraw()
  return true
end

-- the next choice of a selector, or the next value of a slider, one way along it
local function nudge(node, forward)
  if node.kind == "selector" then
    local index = 0

    for i, choice in ipairs(node.choices) do
      index = choice == node.value and i or index
    end

    local choice = node.choices[math.min(math.max(index + (forward and 1 or -1), 1), #node.choices)]

    if choice ~= nil and choice ~= node.value then
      call(node.onChange, choice)
    end
  else
    local amount = (node.step and node.step > 0) and node.step or (node.max - node.min) / 20
    local value = math.min(math.max(node.value + (forward and amount or -amount), node.min), node.max)

    if value ~= node.value then
      call(node.onChange, value)
    end
  end
end

-- handles a key press for the element that has the keys, when no field types: Tab moves them, Return or Space activates the element,
-- Left and Right change a selector or a slider, and Escape takes the keys away
local function press(panel, state, event)
  if event.key == "Tab" then
    return step(panel, state, event.shift)
  end

  local node = state.target and byId(state.root, state.target)

  if not node then
    return false
  end

  local enter = event.key == "Return" or event.key == "Keypad Enter" or event.key == "Space"

  if event.key == "Escape" then
    state.target, state.ring = nil, false
  elseif (node.kind == "selector" or node.kind == "slider") and (event.key == "Left" or event.key == "Right") then
    nudge(node, event.key == "Right")
  elseif enter and node.kind ~= "selector" and node.kind ~= "slider" then
    activate(panel, state, node, node.x)
  else
    return false
  end

  panel:redraw()
  return true
end

-- sets a slider from the pointer, and calls onChange if its value changes
local function slide(node, x)
  local value = sliderValue(node, x)

  if value ~= node.value then
    node.value = value
    call(node.onChange, value)
  end
end

local function handle(panel, event)
  local state = panelState(panel)
  local root = state.root

  if not root then
    return false
  end

  -- Tab leaves a field too
  if state.focus and (event.type == "text" or event.type == "keyDown" or event.type == "keyUp") and not (event.type == "keyDown" and event.key == "Tab") then
    return typing(panel, state, event)
  end

  if event.type == "keyDown" then
    return press(panel, state, event)
  end

  -- pointer positions come in surface pixels, and the layout is in layout units
  local x, y = (event.x or 0) / state.scale, (event.y or 0) / state.scale

  if event.type == "pointerMove" then
    state.pointerX, state.pointerY = x, y

    if state.dragging then
      local node = byId(root, state.dragging)
      local _, length = thumb(node)
      local travel = node.h - length

      if travel > 0 then
        state.scroll[node.id] = (y - node.y - state.grab) / travel * (node.total - node.shown)
      end

      panel:redraw()
      return true
    end

    if state.sliding then
      local node = byId(root, state.sliding)

      if node then
        slide(node, x)
      end

      panel:redraw()
      return true
    end

    local hover = find(root, x, y, interactive)

    if hover ~= state.hover or (hover and hover.kind == "selector") then
      state.hover = hover
      panel:redraw()
    end

    return hover ~= nil
  elseif event.type == "pointerDown" and event.button == 1 then
    local scroll = find(root, x, y, scrollable)

    -- a press on a scrollbar grabs its thumb, or moves the thumb there first
    if scroll and x >= scroll.x + scroll.w - size.scrollbarWidth then
      local start, length = thumb(scroll)

      if y < start or y >= start + length then
        local travel = scroll.h - length
        state.scroll[scroll.id] = travel > 0 and (y - scroll.y - length / 2) / travel * (scroll.total - scroll.shown) or 0
        start = scroll.y + (y - scroll.y - length / 2)
      end

      state.dragging, state.grab = scroll.id, y - start
      panel:redraw()
      return true
    end

    -- a press on a list's item chooses it; the element under the pointer still gets the press
    local item = find(root, x, y, listItem)

    if item and item.item ~= item.list.selected then
      call(item.list.onSelect, item.item)
    end

    local node = find(root, x, y, interactive)

    if not node or (node.kind ~= "field" and node.kind ~= "key") then
      blur(panel, state)
    end

    -- the pressed element gets the keys, without a ring, so Tab goes on from it
    state.pressed = node
    state.target, state.ring = node and node.id, false

    if node and (node.kind == "field" or node.kind == "key") then
      activate(panel, state, node, x)
    elseif node and node.kind == "slider" then
      state.sliding = node.id
      slide(node, x)
    end

    if node or item then
      panel:redraw()
    end

    return node ~= nil or item ~= nil
  elseif event.type == "pointerUp" and event.button == 1 then
    local pressed = state.pressed
    state.pressed, state.dragging, state.sliding = nil, nil, nil

    -- a click is a press and a release on the same element
    local node = find(root, x, y, interactive)

    if pressed and node and node.id == pressed.id and node.kind == pressed.kind and node.kind ~= "field" and node.kind ~= "key" and node.kind ~= "slider" then
      activate(panel, state, node, x)
    end

    panel:redraw()
    return pressed ~= nil
  elseif event.type == "wheel" then
    local scroll = find(root, x, y, scrollable)

    if scroll then
      state.scroll[scroll.id] = (state.scroll[scroll.id] or 0) - event.wheelY * size.wheelStep
      panel:redraw()
      return true
    end
  elseif event.type == "unfocused" or event.type == "hidden" then
    blur(panel, state)
  end

  return false
end

-- services -------------------------------------------------------------------

local services = {}

-- lays out the elements in the panel's surface and draws them; call it from the panel's draw
function services.show(panel, surface, root)
  local state = panelState(panel)
  local width, height = surface.width / surface.scale, surface.height / surface.scale
  state.scale = surface.scale
  state.cursor = nil
  fonts = {}
  readSizes()

  -- ids that elements leave out come from their places, so they stay the same from one draw to the next
  local function name(node, path)
    node.id = node.id or path

    for i, child in ipairs(node.children or {}) do
      name(child, path .. "." .. i)
    end
  end

  name(root, "root")
  measure(root, state, width)
  arrange(root, 0, 0, width, height, state)

  -- the element under the pointer and the pressed element are found again in the new layout
  state.hover = state.hover and state.pointerX and find(root, state.pointerX, state.pointerY, interactive) or nil
  state.pressed = state.pressed and byId(root, state.pressed.id) or nil
  state.root = root

  draw.unclip(surface)
  draw.fill(surface, 0, 0, width, height, "background")
  paint(root, surface, state, nil)
  draw.unclip(surface)

  if state.focus and state.cursor then
    local scale = surface.scale
    panel:setTextInput(true, state.cursor.x * scale, state.cursor.y * scale, 2 * scale, state.cursor.h * scale)
  end
end

-- handles a panel's event: calls the callbacks of the elements it reaches; call it from the panel's event
function services.event(panel, event)
  return handle(panel, event)
end

-- forgets a panel's state; call it from the panel's destroy
function services.forget(panel)
  panels[panel] = nil
end

-- the rectangle of an element in the last layout, in layout units
function services.rect(panel, id)
  local root = panelState(panel).root
  local node = root and byId(root, id)

  if not node then
    return false, 0, 0, 0, 0
  end

  return true, node.x, node.y, node.w, node.h
end

-- the id of the field or key element that has the focus, or nil
function services.focus(panel)
  return panelState(panel).focus
end

-- gives a field or key element the focus, as a click on it does
function services.edit(panel, id)
  local state = panelState(panel)
  local node = state.root and byId(state.root, id)

  if node and (node.kind == "field" or node.kind == "key") then
    activate(panel, state, node, 0)
    panel:redraw()
  end
end

-- scrolls a scroll element so that one of its children shows
function services.reveal(panel, scrollId, childId)
  local state = panelState(panel)
  local scroll = state.root and byId(state.root, scrollId)
  local child = scroll and byId(scroll, childId)

  if child then
    local top = child.y - scroll.y + scroll.offset
    local offset = state.scroll[scrollId] or 0
    state.scroll[scrollId] = math.min(math.max(offset, top + child.h - scroll.h), top)
    panel:redraw()
  end
end

-- chooses the item of a list a number of items away from the chosen one, and scrolls it into view
function services.move(panel, listId, step)
  local state = panelState(panel)
  local list = state.root and byId(state.root, listId)

  if not list or not list.items or #list.items == 0 then
    return
  end

  local index = list.selected and list.selected + step or (step > 0 and 1 or #list.items)
  index = math.min(math.max(index, 1), #list.items)
  call(list.onSelect, index)
  services.reveal(panel, listId, list.items[index].id)
end

-- activates a list's chosen item, as a click on it does
function services.activate(panel, listId)
  local state = panelState(panel)
  local list = state.root and byId(state.root, listId)

  if list and list.selected and list.items and list.items[list.selected] then
    call(list.onActivate, list.selected)
  end
end

assert(ecs.service.register({
  show = { sig = "void(handle<ecs.panel> panel, handle<ecs.surface> surface, value root)", doc = "Lay out elements in a panel and draw them", fn = services.show },
  event = { sig = "bool(handle<ecs.panel> panel, value event)", doc = "Handle a panel's event, and call the callbacks of the elements it reaches", fn = services.event },
  forget = { sig = "void(handle<ecs.panel> panel)", doc = "Forget a panel's state", fn = services.forget },
  rect = { sig = "bool(handle<ecs.panel> panel, string id, out float x, out float y, out float width, out float height)", doc = "Give an element's rectangle in the last layout", fn = services.rect },
  focus = { sig = "string(handle<ecs.panel> panel)", doc = "Give the id of the focused field", fn = services.focus },
  edit = { sig = "void(handle<ecs.panel> panel, string id)", doc = "Give a field the focus", fn = services.edit },
  reveal = { sig = "void(handle<ecs.panel> panel, string scroll, string child)", doc = "Scroll so a child shows", fn = services.reveal },
  move = { sig = "void(handle<ecs.panel> panel, string list, int step)", doc = "Choose the item of a list a number of items away, and scroll it into view; onSelect gets it", fn = services.move },
  activate = { sig = "void(handle<ecs.panel> panel, string list)", doc = "Activate a list's chosen item; onActivate gets it", fn = services.activate },
  column = { sig = "value(value props, value children)", doc = "Elements below each other", fn = elements.column },
  row = { sig = "value(value props, value children)", doc = "Elements beside each other", fn = elements.row },
  scroll = { sig = "value(value props, value children)", doc = "Elements below each other, which scroll", fn = elements.scroll },
  list = { sig = "value(value props, value children)", doc = "Items below each other, which scroll; selected is the chosen one, a press chooses one for onSelect, and a click gives it to onActivate", fn = elements.list },
  group = { sig = "value(value props, value children)", doc = "Elements below each other in a frame, under a title", fn = elements.group },
  separator = { sig = "value(value props)", doc = "A line across a column, or up a row", fn = elements.separator },
  text = { sig = "value(string text, value props)", doc = "A line of text", fn = elements.text },
  button = { sig = "value(string label, value props)", doc = "A button; onClick runs when it is clicked", fn = elements.button },
  toggle = { sig = "value(bool on, value props)", doc = "On or off: a check box, or a switch with look = \"switch\"; onChange gets the new state", fn = elements.toggle },
  slider = { sig = "value(double value, value props)", doc = "A number from min to max, which a drag changes, rounded to step; onChange gets it", fn = elements.slider },
  selector = { sig = "value(string value, value choices, value props)", doc = "One of a few choices, side by side; onChange gets the clicked one", fn = elements.selector },
  dropdown = { sig = "value(string value, value choices, value props)", doc = "One of a list of choices, which a click opens; onChange gets the chosen one", fn = elements.dropdown },
  field = { sig = "value(string text, value props)", doc = "A text field; onChange gets each edit and onSubmit the text when Return is pressed", fn = elements.field },
  number = { sig = "value(double value, value props)", doc = "A number that the user types, from min to max, rounded to step; Return or Up and Down give it to onChange", fn = elements.number },
  key = { sig = "value(string combination, value props)", doc = "A key combination, which a click and a key press change; onChange gets it", fn = elements.key },
  image = { sig = "value(string path, value props)", doc = "An image file", fn = elements.image },
  space = { sig = "value(value props)", doc = "Empty room, which grows by default", fn = elements.space },
}))
