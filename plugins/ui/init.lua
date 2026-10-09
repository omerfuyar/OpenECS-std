-- The ui standard plugin: user-interface elements with layout (DESIGN 3).
-- A panel describes its elements in draw, each time, and ui lays them out, draws them with the draw plugin, and calls their callbacks on events.

local ecs = require("ecs")
local draw = require("draw")

-- sizes in layout units
local PADDING_X, PADDING_Y = 10, 5
local CHECK_SIZE = 16
local FIELD_WIDTH = 120
local SCROLLBAR_WIDTH = 8
local THUMB_MIN = 24
local WHEEL_STEP = 48

-- the colour of a field whose text is not valid
local INVALID = "#E0505A"

-- the elements that take the pointer
local INTERACTIVE = { button = true, check = true, choice = true, field = true, key = true }

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

local function lineHeight(size)
  local _, line = draw.measure("Ag", size)
  return line
end

-- the text that fits a width, ended with an ellipsis when it is cut
local function fit(text, size, width)
  if draw.measure(text, size) <= width then
    return text
  end

  local low, high = 0, utf8.len(text) or #text

  while low < high do
    local middle = (low + high + 1) // 2
    local cut = text:sub(1, (utf8.offset(text, middle + 1) or #text + 1) - 1) .. "…"

    if draw.measure(cut, size) <= width then
      low = middle
    else
      high = middle - 1
    end
  end

  return text:sub(1, (utf8.offset(text, low + 1) or #text + 1) - 1) .. "…"
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

function elements.check(on, props)
  local node = element("check", props)
  node.on = on == true
  return node
end

function elements.choice(value, choices, props)
  local node = element("choice", props)
  node.value = value
  node.choices = choices
  return node
end

function elements.field(text, props)
  local node = element("field", props)
  node.text = tostring(text or "")
  assert(node.id, "a field needs an id, which keeps what is typed")
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

-- the axis of a container: true for a row
local function horizontal(node)
  return node.kind == "row"
end

local function padding(node)
  local value = node.padding or 0
  return value, value
end

-- whether an element takes the whole cross size of its parent when nothing says otherwise:
-- containers always, and fields and key elements across a column, so they are as wide as it
local function stretches(node, row)
  if node.stretch ~= nil then
    return node.stretch
  end

  return CONTAINERS[node.kind] or (not row and (node.kind == "field" or node.kind == "key" or node.kind == "choice"))
end

-- the size an element needs, without growing; containers measure their children first
local function measure(node, state)
  local size = node.size or fontSize()
  local line = lineHeight(size)
  local width, height

  if node.kind == "text" then
    width, height = draw.measure(node.text, size), line
  elseif node.kind == "button" then
    width, height = draw.measure(node.label, size) + 2 * PADDING_X, line + 2 * PADDING_Y
  elseif node.kind == "choice" then
    local widest = 0

    for _, choice in ipairs(node.choices or {}) do
      widest = math.max(widest, draw.measure(tostring(choice), size))
    end

    width, height = widest + draw.measure("‹  ›", size) + 2 * PADDING_X, line + 2 * PADDING_Y
  elseif node.kind == "check" then
    width, height = CHECK_SIZE, math.max(CHECK_SIZE, line)

    if node.label then
      width = width + PADDING_X + draw.measure(node.label, size)
    end
  elseif node.kind == "field" or node.kind == "key" then
    width, height = FIELD_WIDTH, line + 2 * PADDING_Y
  elseif node.kind == "image" then
    local ok, imageWidth, imageHeight = draw.imageSize(node.path)
    width, height = ok and imageWidth or 0, ok and imageHeight or 0
  elseif node.kind == "space" then
    width, height = 0, 0
  else
    local across, along = 0, 0
    local count = 0

    for _, child in ipairs(node.children) do
      local childWidth, childHeight = measure(child, state)
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
      innerWidth = innerWidth - SCROLLBAR_WIDTH - 4
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
    local size = row and child.naturalWidth or child.naturalHeight

    if grows > 0 and grow > 0 and node.kind ~= "scroll" then
      size = math.max(0, size + room * grow / grows)
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

    if row then
      arrange(child, along, crossAt, size, cross, state)
    else
      arrange(child, crossAt, along, cross, size, state)
    end

    along = along + size + gap
  end
end

-- drawing --------------------------------------------------------------------

local function colorOf(name, fallback)
  return draw.color(name or fallback)
end

local function contains(node, x, y)
  return x >= node.x and x < node.x + node.w and y >= node.y and y < node.y + node.h
end

-- the rectangle of a scroll element's thumb, along its height
local function thumb(node)
  local length = math.max(math.min(THUMB_MIN, node.h), node.h * node.shown / node.total)
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

local function drawText(surface, node, text, x, y, width, color)
  local size = node.size or fontSize()
  draw.text(surface, fit(text, size, width), x, y, size, color)
end

local function paint(node, surface, state, clip)
  local function inside(x, y)
    return not clip or (x >= clip.x and x < clip.x + clip.w and y >= clip.y and y < clip.y + clip.h)
  end

  local line = lineHeight(node.size or fontSize())
  local textY = node.y + (node.h - line) / 2
  local hover = state.hover == node and inside(state.pointerX or 0, state.pointerY or 0)
  local pressed = state.pressed == node

  local background = hover and node.hoverBackground or node.background

  if background then
    draw.fill(surface, node.x, node.y, node.w, node.h, colorOf(background))
  end

  if node.border then
    draw.outline(surface, node.x, node.y, node.w, node.h, 1, colorOf(node.border))
  end

  if node.kind == "text" then
    drawText(surface, node, node.text, node.x, textY, node.w, colorOf(node.color, "text"))
  elseif node.kind == "button" or node.kind == "choice" then
    local fill = pressed and "accent" or (hover or node.chosen) and "tabShown" or "tab"
    draw.fill(surface, node.x, node.y, node.w, node.h, colorOf(fill))
    local label = node.kind == "button" and node.label or ("‹ %s ›"):format(tostring(node.value))
    local size = node.size or fontSize()
    local shown = fit(label, size, node.w - 2 * PADDING_X)
    local width = draw.measure(shown, size)
    draw.text(surface, shown, node.x + (node.w - width) / 2, textY, size, colorOf(node.color, "text"))
  elseif node.kind == "check" then
    local boxY = node.y + (node.h - CHECK_SIZE) / 2
    draw.fill(surface, node.x, boxY, CHECK_SIZE, CHECK_SIZE, colorOf("tabRow"))
    draw.outline(surface, node.x, boxY, CHECK_SIZE, CHECK_SIZE, 1, colorOf((node.on or hover) and "accent" or "textDim"))

    if node.on then
      draw.fill(surface, node.x + 4, boxY + 4, CHECK_SIZE - 8, CHECK_SIZE - 8, colorOf("accent"))
    end

    if node.label then
      drawText(surface, node, node.label, node.x + CHECK_SIZE + PADDING_X, textY, node.w - CHECK_SIZE - PADDING_X, colorOf(node.color, "text"))
    end
  elseif node.kind == "field" or node.kind == "key" then
    local focused = state.focus == node.id
    local text = node.text

    if focused and node.kind == "field" then
      text = state.typed
    elseif focused then
      text = "Press a key"
    end

    draw.fill(surface, node.x, node.y, node.w, node.h, colorOf("tabRow"))
    local size = node.size or fontSize()
    local width = draw.measure(text, size)
    local room = node.w - 2 * PADDING_X

    -- the text moves left when it is longer than the field, so its end and the cursor stay in view
    local textX = node.x + PADDING_X - ((focused and width > room) and width - room or 0)
    local inner = intersect({ x = node.x + 1, y = node.y + 1, w = node.w - 2, h = node.h - 2 }, clip)
    draw.clip(surface, inner.x, inner.y, inner.w, inner.h)
    draw.text(surface, (focused or width <= room) and text or fit(text, size, room), textX, textY, size, colorOf((focused and node.kind == "key") and "textDim" or node.color, "text"))

    if focused and node.kind == "field" then
      draw.fill(surface, textX + width + 1, textY, 2, line, colorOf("accent"))
      state.cursor = { x = textX + width, y = textY, h = line }
    end

    if clip then
      draw.clip(surface, clip.x, clip.y, clip.w, clip.h)
    else
      draw.unclip(surface)
    end

    local edge = node.valid == false and INVALID or focused and "accent" or "tabShown"
    draw.outline(surface, node.x, node.y, node.w, node.h, 1, colorOf(edge))
  elseif node.kind == "image" then
    draw.image(surface, node.path, node.x, node.y, node.w, node.h)
  elseif node.kind == "scroll" then
    -- the children draw inside the scroll element only
    local inner = intersect({ x = node.x, y = node.y, w = node.w, h = node.h }, clip)

    draw.clip(surface, inner.x, inner.y, inner.w, inner.h)

    for _, child in ipairs(node.children) do
      if child.y + child.h > node.y and child.y < node.y + node.h then
        paint(child, surface, state, inner)
      end
    end

    if clip then
      draw.clip(surface, clip.x, clip.y, clip.w, clip.h)
    else
      draw.unclip(surface)
    end

    if node.total > node.shown then
      local start, length = thumb(node)
      local x = node.x + node.w - SCROLLBAR_WIDTH
      draw.fill(surface, x, node.y, SCROLLBAR_WIDTH, node.h, colorOf("tabRow"))
      draw.fill(surface, x, start, SCROLLBAR_WIDTH, length, colorOf(state.dragging == node.id and "accent" or "tabShown"))
    end

    return
  end

  for _, child in ipairs(node.children or {}) do
    paint(child, surface, state, clip)
  end
end

-- the innermost element at a point that takes the pointer, or a scroll element; scrolled content counts only where it shows
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
    state.focus, state.typed, state.cursor = nil, nil, nil
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

local function activate(panel, state, node)
  if node.kind == "button" or CONTAINERS[node.kind] then
    call(node.onClick)
  elseif node.kind == "check" then
    call(node.onChange, not node.on)
  elseif node.kind == "choice" then
    local choices = node.choices or {}

    for i, choice in ipairs(choices) do
      if choice == node.value then
        call(node.onChange, choices[i % #choices + 1])
        return
      end
    end

    call(node.onChange, choices[1])
  elseif node.kind == "field" or node.kind == "key" then
    if state.focus ~= node.id then
      blur(panel, state)
      state.focus, state.typed = node.id, node.text
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

  if event.type == "text" then
    state.typed = state.typed .. event.text
    call(node.onChange, state.typed)
  elseif event.type == "keyDown" and event.key == "Backspace" then
    -- the last character, which may take several bytes
    local last = utf8.offset(state.typed, -1)
    state.typed = last and state.typed:sub(1, last - 1) or ""
    call(node.onChange, state.typed)
  elseif event.type == "keyDown" and (event.key == "Return" or event.key == "Keypad Enter") then
    -- onSubmit gives false to keep typing, such as for a text it cannot read
    if not (node.onSubmit and node.onSubmit(state.typed) == false) then
      blur(panel, state)
    end
  elseif event.type == "keyDown" and event.key == "Escape" then
    blur(panel, state)
  elseif event.type ~= "keyUp" and event.type ~= "keyDown" then
    return false
  end

  panel:redraw()
  return true
end

local function handle(panel, event)
  local state = panelState(panel)
  local root = state.root

  if not root then
    return false
  end

  if state.focus and (event.type == "text" or event.type == "keyDown" or event.type == "keyUp") then
    return typing(panel, state, event)
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

    local hover = find(root, x, y, interactive)

    if hover ~= state.hover then
      state.hover = hover
      panel:redraw()
    end

    return hover ~= nil
  elseif event.type == "pointerDown" and event.button == 1 then
    local scroll = find(root, x, y, scrollable)

    -- a press on a scrollbar grabs its thumb, or moves the thumb there first
    if scroll and x >= scroll.x + scroll.w - SCROLLBAR_WIDTH then
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

    local node = find(root, x, y, interactive)

    if not node or (node.kind ~= "field" and node.kind ~= "key") then
      blur(panel, state)
    end

    state.pressed = node

    if node then
      if node.kind == "field" or node.kind == "key" then
        activate(panel, state, node)
      end

      panel:redraw()
    end

    return node ~= nil
  elseif event.type == "pointerUp" and event.button == 1 then
    local pressed = state.pressed
    state.pressed, state.dragging = nil, nil

    -- a click is a press and a release on the same element
    local node = find(root, x, y, interactive)

    if pressed and node and node.id == pressed.id and node.kind == pressed.kind and node.kind ~= "field" and node.kind ~= "key" then
      activate(panel, state, node)
    end

    panel:redraw()
    return pressed ~= nil
  elseif event.type == "wheel" then
    local scroll = find(root, x, y, scrollable)

    if scroll then
      state.scroll[scroll.id] = (state.scroll[scroll.id] or 0) - event.wheelY * WHEEL_STEP
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

  -- ids that elements leave out come from their places, so they stay the same from one draw to the next
  local function name(node, path)
    node.id = node.id or path

    for i, child in ipairs(node.children or {}) do
      name(child, path .. "." .. i)
    end
  end

  name(root, "root")
  measure(root, state)
  arrange(root, 0, 0, width, height, state)

  -- the element under the pointer and the pressed element are found again in the new layout
  state.hover = state.hover and state.pointerX and find(root, state.pointerX, state.pointerY, interactive) or nil
  state.pressed = state.pressed and byId(root, state.pressed.id) or nil
  state.root = root

  draw.unclip(surface)
  draw.fill(surface, 0, 0, width, height, draw.color("background"))
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
    activate(panel, state, node)
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

local functions = {
  show = { sig = "void(handle<ecs.panel> panel, handle<ecs.surface> surface, value root)", doc = "Lay out elements in a panel and draw them", fn = services.show },
  event = { sig = "bool(handle<ecs.panel> panel, value event)", doc = "Handle a panel's event, and call the callbacks of the elements it reaches", fn = services.event },
  forget = { sig = "void(handle<ecs.panel> panel)", doc = "Forget a panel's state", fn = services.forget },
  rect = { sig = "bool(handle<ecs.panel> panel, string id, out float x, out float y, out float width, out float height)", doc = "Give an element's rectangle in the last layout", fn = services.rect },
  focus = { sig = "string(handle<ecs.panel> panel)", doc = "Give the id of the focused field", fn = services.focus },
  edit = { sig = "void(handle<ecs.panel> panel, string id)", doc = "Give a field the focus", fn = services.edit },
  reveal = { sig = "void(handle<ecs.panel> panel, string scroll, string child)", doc = "Scroll so a child shows", fn = services.reveal },
  column = { sig = "value(value props, value children)", doc = "Elements below each other", fn = elements.column },
  row = { sig = "value(value props, value children)", doc = "Elements beside each other", fn = elements.row },
  scroll = { sig = "value(value props, value children)", doc = "Elements below each other, which scroll", fn = elements.scroll },
  text = { sig = "value(string text, value props)", doc = "A line of text", fn = elements.text },
  button = { sig = "value(string label, value props)", doc = "A button; onClick runs when it is clicked", fn = elements.button },
  check = { sig = "value(bool on, value props)", doc = "A check box; onChange gets the new state", fn = elements.check },
  choice = { sig = "value(string value, value choices, value props)", doc = "One of a list, which a click moves to the next; onChange gets it", fn = elements.choice },
  field = { sig = "value(string text, value props)", doc = "A text field; onChange gets each edit and onSubmit the text when Return is pressed", fn = elements.field },
  key = { sig = "value(string combination, value props)", doc = "A key combination, which a click and a key press change; onChange gets it", fn = elements.key },
  image = { sig = "value(string path, value props)", doc = "An image file", fn = elements.image },
  space = { sig = "value(value props)", doc = "Empty room, which grows by default", fn = elements.space },
}

assert(ecs.service.register("ui", functions))
