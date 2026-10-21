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

-- how long, in seconds, the pointer rests on an element before its tooltip shows, and how long the tooltip stays;
-- a panel hears nothing when the pointer leaves it, so a tooltip goes by itself too
local TOOLTIP_DELAY, TOOLTIP_TIME = 0.5, 5

-- the elements that take the pointer
local INTERACTIVE = { button = true, toggle = true, selector = true, dropdown = true, slider = true, field = true, key = true }

-- every panel's state, by its panel handle: the last layout, the element under the pointer, the pressed element, the focused field and the scroll offsets
local panels = {}

local function panelState(panel)
  local state = panels[panel]

  if not state then
    state = { scroll = {}, scale = 1, taken = {} }
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

-- whether a byte continues a UTF-8 character
local function continues(text, at)
  local byte = text:byte(at)
  return byte ~= nil and byte >= 0x80 and byte < 0xC0
end

-- the byte position of the character after the one at a position; unlike utf8.offset, a text that is not valid UTF-8 raises no error
local function nextCharacter(text, at)
  at = at + 1

  while continues(text, at) do
    at = at + 1
  end

  return at
end

-- the byte position of the character before a position
local function previousCharacter(text, at)
  at = at - 1

  while at > 1 and continues(text, at) do
    at = at - 1
  end

  return at
end

-- the byte position of the start of the word before a position, or of the end of the word after it; words are what lies between spaces
local function wordEdge(text, at, forward)
  local function space(position)
    return text:sub(position, position):match("%s") ~= nil
  end

  if forward then
    while at <= #text and space(at) do
      at = at + 1
    end

    while at <= #text and not space(at) do
      at = at + 1
    end

    return at
  end

  while at > 1 and space(at - 1) do
    at = at - 1
  end

  while at > 1 and not space(at - 1) do
    at = at - 1
  end

  return at
end

-- the first characters of a text
local function prefix(text, count)
  local at = 1

  for _ = 1, count do
    if at > #text then
      break
    end

    at = nextCharacter(text, at)
  end

  return text:sub(1, at - 1)
end

-- the text that fits a width, ended with an ellipsis when it is cut
local function fit(font, text, textSize, width)
  if measureText(font, text, textSize) <= width then
    return text
  end

  local low, high = 0, utf8.len(text) or #text

  while low < high do
    local middle = (low + high + 1) // 2
    local cut = prefix(text, middle) .. "…"

    if measureText(font, cut, textSize) <= width then
      low = middle
    else
      high = middle - 1
    end
  end

  return prefix(text, low) .. "…"
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

-- images read once for each path and scale, so that they are drawn sharp.
-- The cache starts again when it is full, so that images no longer shown are collected; a shown image stays in its node.
local IMAGES_KEPT = 64
local images = {}
local imageCount = 0

local function picture(path, scale)
  local key = path .. "@" .. scale
  local found = images[key]

  if found == nil then
    if imageCount >= IMAGES_KEPT then
      images = {}
      imageCount = 0
    end

    found = image.load(path, scale) or false
    images[key] = found
    imageCount = imageCount + 1
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

-- a tree: a list whose items are the shown nodes, each below its parent and indented by its depth;
-- a node is { id, label, children }, and one with children, even none, has an arrow that a click opens and closes;
-- open names the open nodes by id, selected is the chosen node's id, onSelect and onActivate get a node's id, and onToggle(id, open) its new state
function elements.tree(props, nodes)
  props = props or {}
  assert(props.id, "a tree needs an id, which keeps its offset")
  local open, rows = props.open or {}, {}
  local ids, parents, folders = {}, {}, {}

  local function add(list, depth, parent)
    for _, item in ipairs(list) do
      local folder = item.children ~= nil
      local shown = folder and open[item.id] == true
      local index = #rows + 1

      -- the arrow is a row with onClick, so a press on it toggles, and a press beside it only chooses
      local arrow = elements.space({ width = size.checkSize, grow = 0 })

      if folder then
        arrow = elements.row({ onClick = function()
          if props.onToggle then
            props.onToggle(item.id, not shown)
          end
        end }, { element("disclosure", { open = shown }) })
      end

      rows[index] = elements.row({ id = props.id .. ":" .. tostring(item.id), gap = 4, padding = 2 }, {
        elements.space({ width = depth * size.checkSize, grow = 0 }),
        arrow,
        elements.text(item.label or tostring(item.id), { color = item.color }),
      })
      ids[index], parents[index], folders[index] = item.id, parent, folder and (shown and "open" or "closed") or nil

      if shown then
        add(item.children, depth + 1, index)
      end
    end
  end

  add(nodes or {}, 0, nil)

  -- the list works with indexes, and the tree with ids
  local listProps = {}

  for key, value in pairs(props) do
    listProps[key] = value
  end

  listProps.open, listProps.onToggle = nil, nil
  listProps.selected = nil

  for index, id in ipairs(ids) do
    if id == props.selected then
      listProps.selected = index
    end
  end

  listProps.onSelect = props.onSelect and function(index)
    props.onSelect(ids[index])
  end

  listProps.onActivate = props.onActivate and function(index)
    props.onActivate(ids[index])
  end

  local node = elements.list(listProps, rows)
  node.tree = { ids = ids, parents = parents, folders = folders, onToggle = props.onToggle }
  return node
end

-- a table: a header of its columns' titles over a list of its rows, whose cells line up below the titles;
-- a column is { id, title, width }, and a row is a table of the cells' values by column id; a column is as wide as its widest cell unless it gives a width;
-- selected is the chosen row's number, onSelect and onActivate get a row's number, and a click on a title gives the column's id to onSort;
-- sort names the column the rows are sorted by, which shows an arrow, up with ascending = true; a title's id is the table's id, ":title:" and the column's id
function elements.table(props, columns, rows)
  props = props or {}
  assert(props.id, "a table needs an id, which keeps its offset")
  columns, rows = columns or {}, rows or {}
  local font, textSize = fontOf(props), props.size or fontSize()
  local widths = {}

  local function title(column)
    return tostring(column.title or column.id)
  end

  for i, column in ipairs(columns) do
    local widest = measureText(font, title(column), textSize) + (props.sort == column.id and size.checkSize or 0)

    for _, values in ipairs(rows) do
      widest = math.max(widest, measureText(font, tostring(values[column.id] or ""), textSize))
    end

    widths[i] = column.width or widest + 2 * size.paddingX
  end

  -- a cell is a row of a column's width, with room before its text; a title cell is clicked to sort
  local function cells(values)
    local list = {}

    for i, column in ipairs(columns) do
      local children = {
        elements.space({ width = size.paddingX, grow = 0 }),
        elements.text(values and tostring(values[column.id] or "") or title(column), { grow = 1, size = props.size, font = props.font, color = not values and "textDim" or nil }),
      }

      if not values and props.sort == column.id then
        children[3] = element("disclosure", { open = not props.ascending, up = props.ascending == true })
      end

      list[i] = elements.row({ id = not values and props.id .. ":title:" .. tostring(column.id) or nil, width = widths[i], onClick = not values and props.onSort and function()
        props.onSort(column.id)
      end or nil }, children)
    end

    return list
  end

  local items = {}

  for i, values in ipairs(rows) do
    items[i] = elements.row({ id = props.id .. ":" .. i, padding = 2 }, cells(values))
  end

  -- the list scrolls its rows below the header, and takes the room the table has
  local list = elements.list({ id = props.id, grow = 1, selected = props.selected, onSelect = props.onSelect, onActivate = props.onActivate }, items)
  local header = elements.row({ id = props.id .. ":header", padding = 2, background = "tabRow", stretch = true }, cells(nil))
  return elements.column({ width = props.width, height = props.height, grow = props.grow, stretch = props.stretch, border = props.border }, { header, list })
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

-- tabs: a selector whose choices are headers of their natural widths from the left, with a line under the chosen one;
-- the panel shows below them what the chosen tab holds
function elements.tabs(value, choices, props)
  local node = elements.selector(value, choices, props)
  node.look = "tabs"
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
    -- 14 digits keep what a user types, such as 0.1, and leave out the float's last digits
    return math.type(number) == "integer" and tostring(number) or ("%.14g"):format(number)
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
    state.at, state.anchor = #state.typed + 1, nil
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
  elseif node.kind == "disclosure" then
    width, height = size.checkSize, line
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

-- the first and the last byte positions of the selection of the focused field, or nil when nothing is selected; last is the position after it
local function selection(state)
  if state.anchor and state.anchor ~= state.at then
    return math.min(state.anchor, state.at), math.max(state.anchor, state.at)
  end
end

-- the part of a rectangle inside a clip rectangle, if there is one; draw.clip clips the drawing the same way, and this tells where the pointer can hit
local function intersect(rect, clip)
  if not clip then
    return rect
  end

  local right, bottom = math.min(rect.x + rect.w, clip.x + clip.w), math.min(rect.y + rect.h, clip.y + clip.h)
  local x, y = math.max(rect.x, clip.x), math.max(rect.y, clip.y)
  return { x = x, y = y, w = math.max(0, right - x), h = math.max(0, bottom - y) }
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
    local shared = node.look == "tabs" and width or total > 0 and width * node.w / total or 0
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
  elseif node.kind == "selector" and node.look == "tabs" then
    -- a line under the whole row, and a thicker one in the accent colour under the chosen tab; tabs past the element's width are cut at its edge
    draw.clip(surface, node.x, node.y, node.w, node.h, function()
      draw.fill(surface, node.x, node.y + node.h - 1, node.w, 1, "tabShown")

      for i, rect in ipairs(segmentRects(node)) do
        local choice = node.choices[i]
        local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.w

        if over and choice ~= node.value then
          draw.fill(surface, rect.x, node.y, rect.w, node.h - 1, "tab")
        end

        if choice == node.value then
          draw.fill(surface, rect.x, node.y + node.h - 3, rect.w, 3, "accent")
        end

        local shown = fit(font, tostring(choice), textSize, rect.w - 2 * size.paddingX)
        draw.text(surface, font, shown, rect.x + size.paddingX, textY, textSize, choice == node.value and (node.color or "text") or "textDim")
      end
    end)
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
    draw.clip(surface, node.x + 1, node.y + 1, node.w - 2, node.h - 2, function()
      local first, last = selection(state)

      if focused and node.kind == "field" and first then
        local left = textX + measureText(font, text:sub(1, first - 1), textSize)
        draw.fill(surface, left, textY, textX + measureText(font, text:sub(1, last - 1), textSize) - left, line, "selected")
      end

      draw.text(surface, font, (focused or width <= room) and text or fit(font, text, textSize, room), textX, textY, textSize,
        (focused and node.kind == "key") and "textDim" or node.color or "text")

      if focused and node.kind == "field" then
        draw.fill(surface, textX + before, textY, 2, line, "accent")
        state.cursor = { x = textX + before, y = textY, h = line }
        state.textX = textX
      end
    end)

    -- a number element marks a typed text that is not a number
    local valid = node.valid ~= false and not (focused and node.change and tonumber(state.typed) == nil)
    draw.outline(surface, node.x, node.y, node.w, node.h, 1, not valid and INVALID or focused and "accent" or "tabShown")
  elseif node.kind == "image" then
    if node.picture then
      draw.image(surface, node.picture, node.x, node.y, node.w, node.h)
    end
  elseif node.kind == "disclosure" then
    -- down when open, right when closed, and up with up
    local half, x, y = size.checkSize / 4, node.x + node.w / 2, node.y + node.h / 2

    if node.up then
      draw.triangles(surface, string.pack("ffffff", x - half, y + half / 2, x + half, y + half / 2, x, y - half / 2), "", "textDim")
    elseif node.open then
      arrow(surface, x, y, half, "textDim")
    else
      draw.triangles(surface, string.pack("ffffff", x - half / 2, y - half, x - half / 2, y + half, x + half / 2, y), "", "textDim")
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

    draw.clip(surface, node.x, node.y, node.w, node.h, function()
      for _, child in ipairs(node.children) do
        if child.y + child.h > node.y and child.y < node.y + node.h then
          paint(child, surface, state, inner)
        end
      end
    end)

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

local function byID(node, id)
  if node.id == id then
    return node
  end

  for _, child in ipairs(node.children or {}) do
    local found = byID(child, id)

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
    state.focus, state.typed, state.cursor, state.at, state.anchor, state.selecting = nil, nil, nil, nil, nil, nil
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

-- puts text in place of the selection, or at the cursor
local function replace(state, text)
  local first, last = selection(state)
  first, last = first or state.at, last or state.at
  state.typed = state.typed:sub(1, first - 1) .. text .. state.typed:sub(last)
  state.at, state.anchor = first + #text, nil
end

-- the byte position in a field's text nearest to a position across it, in layout units
local function positionAt(node, text, textX, x)
  local font, textSize = fontOf(node), node.size or fontSize()
  local at, left = 1, textX

  while at <= #text do
    local after = nextCharacter(text, at)
    local right = textX + measureText(font, text:sub(1, after - 1), textSize)

    if x < (left + right) / 2 then
      return at
    end

    at, left = after, right
  end

  return at
end

-- handles a key or text event while a field or key element has the focus
local function typing(panel, state, event)
  local node = state.root and byID(state.root, state.focus)

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
  -- the selection lies between the anchor and the cursor, and what is typed replaces it
  local typed, at = state.typed, state.at
  local first, last = selection(state)
  local key = event.type == "keyDown" and event.key or nil
  local moves = { Left = true, Right = true, Home = true, End = true }

  if event.type == "text" then
    replace(state, event.text)
    call(node.onChange, state.typed)
  elseif (key == "Backspace" or key == "Delete") and first then
    replace(state, "")
    call(node.onChange, state.typed)
  elseif key == "Backspace" and at > 1 then
    local previous = event.ctrl and wordEdge(typed, at, false) or previousCharacter(typed, at)
    state.typed, state.at = typed:sub(1, previous - 1) .. typed:sub(at), previous
    call(node.onChange, state.typed)
  elseif key == "Delete" and at <= #typed then
    state.typed = typed:sub(1, at - 1) .. typed:sub(event.ctrl and wordEdge(typed, at, true) or nextCharacter(typed, at))
    call(node.onChange, state.typed)
  elseif (key == "Left" or key == "Right") and first and not event.shift and not event.ctrl then
    -- an arrow ends a selection at its side
    state.at, state.anchor = key == "Left" and first or last, nil
  elseif moves[key] then
    -- Shift keeps the anchor, so the move selects; Ctrl moves by words
    local to = key == "Home" and 1 or key == "End" and #typed + 1
      or event.ctrl and wordEdge(typed, at, key == "Right")
      or key == "Left" and (at > 1 and previousCharacter(typed, at) or 1)
      or (at <= #typed and nextCharacter(typed, at) or at)
    state.anchor = event.shift and (state.anchor or at) or nil
    state.at = to
  elseif key == "A" and event.ctrl then
    state.anchor, state.at = 1, #typed + 1
  elseif (key == "C" or key == "X") and event.ctrl then
    if first then
      ecs.clipboard.setText(typed:sub(first, last - 1))

      if key == "X" then
        replace(state, "")
        call(node.onChange, state.typed)
      end
    end
  elseif key == "V" and event.ctrl then
    -- a field holds one line, so the lines of the pasted text are joined by spaces
    local pasted = (ecs.clipboard.getText() or ""):gsub("\r?\n", " ")

    if pasted ~= "" then
      replace(state, pasted)
      call(node.onChange, state.typed)
    end
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

  local node = state.target and byID(state.root, state.target)

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

-- hides a tooltip, shown or waiting
local function hideTip(panel, state)
  if state.tipTimer then
    state.tipTimer:stop()
  end

  if state.tipShown then
    panel:redraw()
  end

  state.tip, state.tipTimer, state.tipShown = nil, nil, false
end

-- starts waiting to show the tooltip of the element under the pointer, unless it shows already;
-- the pointer rests before a tooltip shows, so each move starts the wait again
local function followTip(panel, state, x, y)
  local node = find(state.root, x, y, function(candidate)
    return candidate.tooltip ~= nil
  end)

  if node and node.id == state.tip and state.tipShown then
    return
  end

  hideTip(panel, state)

  if node then
    state.tip = node.id
    state.tipTimer = ecs.timer.start(TOOLTIP_DELAY, false, function()
      state.tipShown = true
      panel:redraw()

      state.tipTimer = ecs.timer.start(TOOLTIP_TIME, false, function()
        state.tipTimer = nil
        hideTip(panel, state)
      end)
    end)
  end
end

-- draws the shown tooltip below and right of the pointer, inside the surface
local function paintTip(surface, state, width, height)
  local node = state.tipShown and byID(state.root, state.tip)

  if not node then
    return
  end

  local font, textSize = draw.font("ui"), size.noteSize
  local line = lineHeight(font, textSize)
  local lines = wrap(font, tostring(node.tooltip), textSize, math.min(320, width - 4 * size.paddingX))
  local boxWidth, boxHeight = 0, #lines * line + 2 * size.paddingY

  for _, text in ipairs(lines) do
    boxWidth = math.max(boxWidth, measureText(font, text, textSize))
  end

  boxWidth = boxWidth + 2 * size.paddingX
  local x = math.max(0, math.min((state.pointerX or 0) + 12, width - boxWidth))
  local y = (state.pointerY or 0) + 18

  -- a tooltip that does not fit below the pointer goes above it
  if y + boxHeight > height then
    y = math.max(0, (state.pointerY or 0) - 6 - boxHeight)
  end

  draw.fill(surface, x, y, boxWidth, boxHeight, "tabRow")
  draw.outline(surface, x, y, boxWidth, boxHeight, 1, "tabShown")

  for i, text in ipairs(lines) do
    draw.text(surface, font, text, x + size.paddingX, y + size.paddingY + (i - 1) * line, textSize, "text")
  end
end

local function handle(panel, event)
  local state = panelState(panel)
  local root = state.root

  if not root then
    return false
  end

  -- a tooltip goes when anything but a move happens, and follows the pointer onto other elements
  if event.type == "pointerMove" then
    followTip(panel, state, (event.x or 0) / state.scale, (event.y or 0) / state.scale)
  else
    hideTip(panel, state)
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
      local node = byID(root, state.dragging)
      local _, length = thumb(node)
      local travel = node.h - length

      if travel > 0 then
        state.scroll[node.id] = (y - node.y - state.grab) / travel * (node.total - node.shown)
      end

      panel:redraw()
      return true
    end

    if state.selecting then
      local node = byID(root, state.selecting)

      if node and state.focus == node.id then
        state.at = positionAt(node, state.typed, state.textX or node.x + size.paddingX, x)
      end

      panel:redraw()
      return true
    end

    if state.sliding then
      local node = byID(root, state.sliding)

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

    if node and node.kind == "field" then
      -- the press puts the cursor where it is, with Shift it selects up to there, and a drag selects
      local focused = state.focus == node.id
      local textX = focused and state.textX or node.x + size.paddingX
      activate(panel, state, node, x)
      local to = positionAt(node, state.typed, textX, x)
      state.anchor = (focused and event.shift) and (state.anchor or state.at) or to
      state.at, state.selecting = to, node.id
    elseif node and node.kind == "key" then
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
    state.pressed, state.dragging, state.sliding, state.selecting = nil, nil, nil, nil

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
  state.pressed = state.pressed and byID(root, state.pressed.id) or nil
  state.root = root

  draw.fill(surface, 0, 0, width, height, "background")
  paint(root, surface, state, nil)
  paintTip(surface, state, width, height)

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
  -- a closing panel is not drawn again, so its tooltip's timer only stops
  if panels[panel] and panels[panel].tipTimer then
    panels[panel].tipTimer:stop()
  end

  panels[panel] = nil
end

-- the rectangle of an element in the last layout, in layout units
function services.rect(panel, id)
  local root = panelState(panel).root
  local node = root and byID(root, id)

  if not node then
    return nil
  end

  return { x = node.x, y = node.y, width = node.w, height = node.h }
end

-- the id of the field or key element that has the focus, or nil
function services.focus(panel)
  return panelState(panel).focus
end

-- the id of the element whose tooltip shows, or nil
function services.tip(panel)
  local state = panelState(panel)
  return state.tipShown and state.tip or nil
end

-- gives a field or key element the focus, as a click on it does
function services.edit(panel, id)
  local state = panelState(panel)
  local node = state.root and byID(state.root, id)

  -- the keyboard's target follows, without its ring, as after a click
  if node and (node.kind == "field" or node.kind == "key") then
    state.target, state.ring = node.id, false
    activate(panel, state, node, 0)
    panel:redraw()
  end
end

-- scrolls a scroll element so that one of its children shows
function services.reveal(panel, scrollID, childID)
  local state = panelState(panel)
  local scroll = state.root and byID(state.root, scrollID)
  local child = scroll and byID(scroll, childID)

  if child then
    local top = child.y - scroll.y + scroll.offset
    local offset = state.scroll[scrollID] or 0
    state.scroll[scrollID] = math.min(math.max(offset, top + child.h - scroll.h), top)
    panel:redraw()
  end
end

-- chooses the item of a list a number of items away from the chosen one, and scrolls it into view
function services.move(panel, listID, step)
  local state = panelState(panel)
  local list = state.root and byID(state.root, listID)

  if not list or not list.items or #list.items == 0 then
    return
  end

  local index = list.selected and list.selected + step or (step > 0 and 1 or #list.items)
  index = math.min(math.max(index, 1), #list.items)
  call(list.onSelect, index)
  services.reveal(panel, listID, list.items[index].id)
end

-- opens or closes a tree's chosen node; opening an open node chooses its first child, and closing a node that is closed, or has no children, chooses its parent
function services.expand(panel, treeID, open)
  local state = panelState(panel)
  local node = state.root and byID(state.root, treeID)
  local tree = node and node.tree
  local index = node and node.selected

  if not tree or not index or not tree.ids[index] then
    return
  end

  local folder = tree.folders[index]

  if folder and (folder == "open") ~= open then
    call(tree.onToggle, tree.ids[index], open)
    panel:redraw()
  elseif open and folder == "open" and tree.parents[index + 1] == index then
    services.move(panel, treeID, 1)
  elseif not open and tree.parents[index] then
    services.move(panel, treeID, tree.parents[index] - index)
  end
end

-- activates a list's chosen item, as a click on it does
function services.activate(panel, listID)
  local state = panelState(panel)
  local list = state.root and byID(state.root, listID)

  if list and list.selected and list.items and list.items[list.selected] then
    call(list.onActivate, list.selected)
  end
end

-- descriptions, for plugins in C -------------------------------------------

-- the callbacks of each kind of element; a described element with an id keeps what they get, for ui.take, since C gives ui no functions
local CALLBACKS = {
  button = { "onClick" },
  toggle = { "onChange" },
  slider = { "onChange" },
  selector = { "onChange" },
  tabs = { "onChange" },
  dropdown = { "onChange" },
  number = { "onChange" },
  key = { "onChange" },
  field = { "onChange", "onSubmit" },
  list = { "onSelect", "onActivate" },
  table = { "onSelect", "onActivate", "onSort" },
  tree = { "onSelect", "onActivate", "onToggle" },
}

-- what each kind's element function takes besides its props: its children, a value, choices, columns and rows, or nodes; the others take a value
local TAKES = {
  column = "children", row = "children", scroll = "children", list = "children", group = "children",
  selector = "choices", tabs = "choices", dropdown = "choices",
  table = "table", tree = "tree", separator = "none", space = "none",
}

-- an element from a description, built with the element functions, as Lua code builds it
local function build(panel, description)
  local props = {}

  for key, value in pairs(description.props or {}) do
    props[key] = value
  end

  -- a column or row with clickable = true is clicked like a button
  local kind = description.kind
  local callbacks = CALLBACKS[kind] or (props.clickable and { "onClick" }) or {}
  props.clickable = nil

  for _, callback in ipairs(callbacks) do
    local id = props.id

    if id then
      props[callback] = function(value, open)
        local taken = panelState(panel).taken
        taken[id .. "\0" .. callback] = { value = value, open = open }
        panel:redraw()
      end
    end
  end

  local takes = TAKES[kind]
  local make = elements[kind]
  assert(make, ("no element of kind '%s'"):format(tostring(kind)))

  if takes == "children" then
    local children = {}

    for i, child in ipairs(description.children or {}) do
      children[i] = build(panel, child)
    end

    return make(props, children)
  elseif takes == "choices" then
    return make(description.value, description.choices, props)
  elseif takes == "table" then
    return make(props, description.columns, description.rows)
  elseif takes == "tree" then
    return make(props, description.nodes)
  elseif takes == "none" then
    return make(props)
  end

  return make(description.value, props)
end

-- builds the elements of a description and shows them; what the callbacks got and no one took is forgotten, so each draw takes only what came since the last
function services.showDescription(panel, surface, description)
  local root = build(panel, description)
  panelState(panel).taken = {}
  services.show(panel, surface, root)
end

-- takes what a callback of a described element got since the last draw, or nil if it did not run
function services.take(panel, id, callback)
  local taken = panelState(panel).taken
  local key = id .. "\0" .. callback
  local got = taken[key]
  taken[key] = nil
  return got
end

assert(ecs.service.register({
  show = {
    sig = "void(handle<ecs.panel> panel, handle<ecs.surface> surface, value<ui.Element> root)",
    doc = "Lay out elements in a panel and draw them\n"
        .. "@param panel The panel, as its draw gets it\n"
        .. "@param surface The panel's surface, as its draw gets it; ui fills it with the theme's background first\n"
        .. "@param root The element that takes the whole surface, with the others inside it; elements without an id get one from their place",
    fn = services.show,
  },
  event = {
    sig = "bool(handle<ecs.panel> panel, value<ecs.PanelEvent> event)",
    doc = "Handle a panel's event, and call the callbacks of the elements it reaches\n"
        .. "@param panel The panel, as its event gets it\n"
        .. "@param event The event, as the panel's event gets it\n"
        .. "@return True if ui handled the event, false if it reached no element or the panel was not shown yet",
    fn = services.event,
  },
  forget = {
    sig = "void(handle<ecs.panel> panel)",
    doc = "Forget a panel's state\n"
        .. "@param panel The panel, as its destroy gets it",
    fn = services.forget,
  },
  rect = {
    sig = "value<ui.Rect?>(handle<ecs.panel> panel, string id)",
    doc = "Give an element's rectangle in the last layout, a table of x, y, width and height, or nil if no element has the id\n"
        .. "@param panel The panel that showed the element\n"
        .. "@param id The element's id\n"
        .. "@return The rectangle, in layout units, or nil if no element has the id or the panel was not shown yet",
    fn = services.rect,
  },
  focus = {
    sig = "string(handle<ecs.panel> panel)",
    doc = "Give the id of the focused field\n"
        .. "@param panel The panel that shows the field\n"
        .. "@return The id of the field or key element that takes typing, or nil if none does",
    fn = services.focus,
  },
  tip = {
    sig = "string(handle<ecs.panel> panel)",
    doc = "Give the id of the element whose tooltip shows\n"
        .. "@param panel The panel that shows the element\n"
        .. "@return The element's id, or nil if no tooltip shows",
    fn = services.tip,
  },
  edit = {
    sig = "void(handle<ecs.panel> panel, string id)",
    doc = "Give a field the focus\n"
        .. "@param panel The panel that shows the field, often the focused panel that a key passes\n"
        .. "@param id The id of a field or key element in the last layout; another id does nothing",
    fn = services.edit,
  },
  reveal = {
    sig = "void(handle<ecs.panel> panel, string scroll, string child)",
    doc = "Scroll so a child shows\n"
        .. "@param panel The panel that shows the scroll element\n"
        .. "@param scroll The id of the scroll element, or of a list, tree or table\n"
        .. "@param child The id of an element inside it, in the last layout; nothing happens if it has none",
    fn = services.reveal,
  },
  move = {
    sig = "void(handle<ecs.panel> panel, string list, int step)",
    doc = "Choose the item of a list a number of items away, and scroll it into view; onSelect gets it\n"
        .. "@param panel The panel that shows the list, often the focused panel that a key passes\n"
        .. "@param list The id of the list, tree or table\n"
        .. "@param step How many items away, down if positive and up if negative; it stops at the first and last items; with no chosen item, a positive step chooses the first and another the last",
    fn = services.move,
  },
  activate = {
    sig = "void(handle<ecs.panel> panel, string list)",
    doc = "Activate a list's chosen item; onActivate gets it\n"
        .. "@param panel The panel that shows the list, often the focused panel that a key passes\n"
        .. "@param list The id of the list, tree or table; nothing happens if no item is chosen",
    fn = services.activate,
  },
  expand = {
    sig = "void(handle<ecs.panel> panel, string tree, bool open)",
    doc = "Open or close a tree's chosen node; onToggle gets it, opening an open node chooses its first child, and closing a closed one its parent\n"
        .. "@param panel The panel that shows the tree, often the focused panel that a key passes\n"
        .. "@param tree The id of the tree; nothing happens if no node is chosen\n"
        .. "@param open True to open the node, false to close it",
    fn = services.expand,
  },
  column = {
    sig = "value<ui.Element>(value<ui.ContainerProps?> props, value<ui.Element[]?> children)",
    doc = "Elements below each other\n"
        .. "@param props The column's properties (ui.ContainerProps); with onClick, it is clicked like a button\n"
        .. "@param children The elements, from the top; none if nil\n"
        .. "@return The column",
    fn = elements.column,
  },
  row = {
    sig = "value<ui.Element>(value<ui.ContainerProps?> props, value<ui.Element[]?> children)",
    doc = "Elements beside each other\n"
        .. "@param props The row's properties (ui.ContainerProps); with onClick, it is clicked like a button\n"
        .. "@param children The elements, from the left; none if nil\n"
        .. "@return The row",
    fn = elements.row,
  },
  scroll = {
    sig = "value<ui.Element>(value<ui.ScrollProps?> props, value<ui.Element[]?> children)",
    doc = "Elements below each other, which scroll\n"
        .. "@param props The scroll element's properties (ui.ScrollProps), which must have an id; it grows unless grow says otherwise\n"
        .. "@param children The elements, from the top; none if nil\n"
        .. "@return The scroll element",
    fn = elements.scroll,
  },
  list = {
    sig = "value<ui.Element>(value<ui.ListProps?> props, value<ui.Element[]?> children)",
    doc = "Items below each other, which scroll; selected is the chosen one, a press chooses one for onSelect, and a click gives it to onActivate\n"
        .. "@param props The list's properties (ui.ListProps), which must have an id\n"
        .. "@param children The items, from the top; those with selectable = false, such as headings, are not items; none if nil\n"
        .. "@return The list",
    fn = elements.list,
  },
  group = {
    sig = "value<ui.Element>(value<ui.GroupProps?> props, value<ui.Element[]?> children)",
    doc = "Elements below each other in a frame, under a title\n"
        .. "@param props The group's properties (ui.GroupProps), with its title; padding, gap and border have defaults\n"
        .. "@param children The elements, from the top, below the title; none if nil\n"
        .. "@return The group",
    fn = elements.group,
  },
  table = {
    sig = "value<ui.Element>(value<ui.TableProps?> props, value<ui.TableColumn[]?> columns, value<ui.TableRow[]?> rows)",
    doc = "Rows of cells below a header of the columns' titles, which scroll; selected is the chosen row, and a click on a title gives its column to onSort\n"
        .. "@param props The table's properties (ui.TableProps), which must have an id\n"
        .. "@param columns The columns, from the left (ui.TableColumn); none if nil\n"
        .. "@param rows The rows, from the top, in the order given, each a table of its cells' values by column id; none if nil\n"
        .. "@return The table",
    fn = elements.table,
  },
  tree = {
    sig = "value<ui.Element>(value<ui.TreeProps?> props, value<ui.TreeNode[]?> nodes)",
    doc = "A list of nodes, each below its parent and indented, which open and close; onSelect, onActivate and onToggle get a node's id\n"
        .. "@param props The tree's properties (ui.TreeProps), which must have an id; open names the open nodes\n"
        .. "@param nodes The top nodes, each with its children (ui.TreeNode); none if nil\n"
        .. "@return The tree",
    fn = elements.tree,
  },
  separator = {
    sig = "value<ui.Element>(value<ui.SeparatorProps?> props)",
    doc = "A line across a column, or up a row\n"
        .. "@param props The separator's properties (ui.SeparatorProps)\n"
        .. "@return The separator",
    fn = elements.separator,
  },
  text = {
    sig = "value<ui.Element>(string text, value<ui.TextProps?> props)",
    doc = "A line of text\n"
        .. "@param text The text, cut with an ellipsis when it does not fit unless props has wrap = true\n"
        .. "@param props The text's properties (ui.TextProps)\n"
        .. "@return The text element",
    fn = elements.text,
  },
  button = {
    sig = "value<ui.Element>(string label, value<ui.ButtonProps?> props)",
    doc = "A button; onClick runs when it is clicked\n"
        .. "@param label The text on the button\n"
        .. "@param props The button's properties (ui.ButtonProps)\n"
        .. "@return The button",
    fn = elements.button,
  },
  toggle = {
    sig = "value<ui.Element>(bool on, value<ui.ToggleProps?> props)",
    doc = "On or off: a check box, or a switch with look = \"switch\"; onChange gets the new state\n"
        .. "@param on Whether it shows as on\n"
        .. "@param props The toggle's properties (ui.ToggleProps), with its label and look\n"
        .. "@return The toggle",
    fn = elements.toggle,
  },
  slider = {
    sig = "value<ui.Element>(double value, value<ui.SliderProps?> props)",
    doc = "A number from min to max, which a drag changes, rounded to step; onChange gets it\n"
        .. "@param value The number it shows, kept from min to max\n"
        .. "@param props The slider's properties (ui.SliderProps); min and max are 0 and 1 if missing\n"
        .. "@return The slider",
    fn = elements.slider,
  },
  selector = {
    sig = "value<ui.Element>(string value, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "One of a few choices, side by side; onChange gets the clicked one\n"
        .. "@param value The chosen text\n"
        .. "@param choices The texts to choose from, from the left; none if nil\n"
        .. "@param props The selector's properties (ui.ChoiceProps)\n"
        .. "@return The selector",
    fn = elements.selector,
  },
  tabs = {
    sig = "value<ui.Element>(string value, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "Tabs: headers of the choices, with a line under the chosen one; onChange gets the clicked one\n"
        .. "@param value The chosen tab's text\n"
        .. "@param choices The tabs' texts, from the left; none if nil\n"
        .. "@param props The tabs' properties (ui.ChoiceProps)\n"
        .. "@return The tabs",
    fn = elements.tabs,
  },
  dropdown = {
    sig = "value<ui.Element>(string value, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "One of a list of choices, which a click opens; onChange gets the chosen one\n"
        .. "@param value The chosen text, which the closed dropdown shows\n"
        .. "@param choices The texts to choose from, from the top; none if nil\n"
        .. "@param props The dropdown's properties (ui.ChoiceProps)\n"
        .. "@return The dropdown",
    fn = elements.dropdown,
  },
  field = {
    sig = "value<ui.Element>(string text, value<ui.FieldProps?> props)",
    doc = "A text field; onChange gets each edit and onSubmit the text when Return is pressed\n"
        .. "@param text The field's text, which typing starts from; empty if nil\n"
        .. "@param props The field's properties (ui.FieldProps), which must have an id\n"
        .. "@return The field",
    fn = elements.field,
  },
  number = {
    sig = "value<ui.Element>(double value, value<ui.NumberProps?> props)",
    doc = "A number that the user types, from min to max, rounded to step; Return or Up and Down give it to onChange\n"
        .. "@param value The number it shows, kept from min to max and rounded to step; 0 if it is not a number\n"
        .. "@param props The field's properties (ui.NumberProps), which must have an id\n"
        .. "@return The number field",
    fn = elements.number,
  },
  key = {
    sig = "value<ui.Element>(string combination, value<ui.KeyProps?> props)",
    doc = "A key combination, which a click and a key press change; onChange gets it\n"
        .. "@param combination The key combination it shows, such as \"Ctrl+S\"; empty if nil\n"
        .. "@param props The key element's properties (ui.KeyProps), which must have an id\n"
        .. "@return The key element",
    fn = elements.key,
  },
  image = {
    sig = "value<ui.Element>(string path, value<ui.Props?> props)",
    doc = "An image file\n"
        .. "@param path The image file's path, read with image at the surface's scale and kept; one that cannot be read takes no room\n"
        .. "@param props The image's properties (ui.Props)\n"
        .. "@return The image element",
    fn = elements.image,
  },
  space = {
    sig = "value<ui.Element>(value<ui.Props?> props)",
    doc = "Empty room, which grows by default\n"
        .. "@param props The space's properties (ui.Props); it grows unless grow says otherwise\n"
        .. "@return The space",
    fn = elements.space,
  },
  showDescription = {
    sig = "void(handle<ecs.panel> panel, handle<ecs.surface> surface, value<ui.Description> root)",
    doc = "Build the elements of a description, as C plugins give them, and show them; a described element with an id keeps what its callbacks get, for take\n"
        .. "@param panel The panel, as its draw gets it\n"
        .. "@param surface The panel's surface, as its draw gets it\n"
        .. "@param root The description of the element that takes the whole surface, with the others as its children (ui.Description)",
    fn = services.showDescription,
  },
  take = {
    sig = "value<ui.Taken?>(handle<ecs.panel> panel, string id, string callback)",
    doc = "Take what a callback of a described element got since the last draw, such as onClick or onChange, or nil if it did not run\n"
        .. "@param panel The panel that showed the description\n"
        .. "@param id The described element's id\n"
        .. "@param callback The callback's name, such as \"onClick\", \"onChange\" or \"onToggle\"\n"
        .. "@return What the callback got, as value and, for onToggle, open; nil if it did not run or was taken already",
    fn = services.take,
  },
}))
