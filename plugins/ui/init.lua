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
    size[entry[1]] = ecs.setting.get("ui." .. entry[1]) or entry[2]
  end
end

for _, entry in ipairs(SIZES) do
  assert(ecs.setting.declare({ name = entry[1], type = "number", default = entry[2], description = entry[3] }))
end

readSizes()

-- the colour of a field whose text is not valid
local INVALID_COLOR = "#E0505A"

-- how long, in seconds, the pointer rests on an element before its tooltip shows, and how long the tooltip stays;
-- a panel hears nothing when the pointer leaves it, so a tooltip goes by itself too
local TOOLTIP_DELAY, TOOLTIP_TIME = 0.5, 5

-- the elements that take the pointer
local INTERACTIVE = { button = true, toggle = true, selector = true, dropdown = true, slider = true, field = true, key = true }

-- the elements that take typing while they have the keys
local TAKES_TYPING = { field = true, key = true }

-- every panel's state, by its panel handle: the last layout, the element under the pointer, the pressed element, the edited field and the scroll offsets
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
  return ecs.setting.get("ecs.fontSize") or 14
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

-- the width of a text alone; draw.measure gives the line height too, which a call such as math.max(...) would take as well
local function measureText(font, text, textSize)
  local width = draw.measure(font, text, textSize)
  return width
end

local function lineHeight(font, textSize)
  local _, line = draw.measure(font, "Ag", textSize)
  return line
end

local function clamp(value, low, high)
  return math.min(math.max(value, low), high)
end

-- a value rounded to a step from a start, when the step is above 0, and kept from low to high
local function snap(value, low, high, step, start)
  if step and step > 0 then
    value = start + math.floor((value - start) / step + 0.5) * step
  end

  return clamp(value, low, high)
end

-- a copy of a table's fields, so the copy changes without the table
local function shallowCopy(source)
  local copy = {}

  for key, value in pairs(source or {}) do
    copy[key] = value
  end

  return copy
end

local function isEnter(event)
  return event.key == "Return" or event.key == "Keypad Enter"
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

local function isSpace(text, at)
  return text:sub(at, at):match("%s") ~= nil
end

-- the byte position of the start of the word before a position; words are what lies between spaces
local function wordStartBefore(text, at)
  while at > 1 and isSpace(text, at - 1) do
    at = at - 1
  end

  while at > 1 and not isSpace(text, at - 1) do
    at = at - 1
  end

  return at
end

-- the byte position after the end of the word after a position
local function wordEndAfter(text, at)
  while at <= #text and isSpace(text, at) do
    at = at + 1
  end

  while at <= #text and not isSpace(text, at) do
    at = at + 1
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
local function ellipsize(font, text, textSize, width)
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
-- The cache starts again when it is full, so that images no longer shown are collected; a shown image stays in its node's layout.
local IMAGES_KEPT = 64
local images = {}
local imageCount = 0

local function loadImageCached(path, scale)
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

-- numbers ----------------------------------------------------------------------

-- a number field's or slider's number, kept from min to max and rounded to step from min, or from 0
local function snapNumber(node, number)
  return snap(number, node.min or -math.huge, node.max or math.huge, node.step, node.min or 0)
end

-- a number as a field shows it: 14 digits keep what a user types, such as 0.1, and leave out the float's last digits
local function formatNumber(number)
  return math.type(number) == "integer" and tostring(number) or ("%.14g"):format(number)
end

-- element constructors -------------------------------------------------------

local function element(elementType, props, children)
  local node = shallowCopy(props)
  node.type = elementType
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
      child.list, child.itemIndex = node, index

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
  local listProps = shallowCopy(props)
  listProps.open, listProps.onToggle, listProps.selected = nil, nil, nil

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
  node.value = clamp(tonumber(value) or node.min, node.min, node.max)
  return node
end

-- one of a few choices, side by side, or of a list of them in a popup
local function choiceElement(elementType, value, choices, props)
  local node = element(elementType, props)
  node.value, node.choices = value, choices or {}
  return node
end

function elements.selector(value, choices, props)
  return choiceElement("selector", value, choices, props)
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
  return choiceElement("dropdown", value, choices, props)
end

function elements.field(text, props)
  local node = element("field", props)
  node.text = tostring(text or "")
  assert(node.id, "a field needs an id, which keeps what is typed")
  return node
end

-- a field for a number, kept from min to max if they are given and rounded to step if it is given;
-- Return gives the typed number to onNumber, the props' onChange, and Up and Down change it by step, or by 1
function elements.number(value, props)
  local node = elements.field("", props)
  node.isNumber = true
  node.onNumber = node.onChange or function(_) end
  node.onChange = nil
  node.value = snapNumber(node, tonumber(value) or 0)
  node.text = formatNumber(node.value)

  -- a text that is not a number keeps the field typing
  node.onSubmit = function(text)
    local number = tonumber(text)

    if number == nil then
      return false
    end

    node.onNumber(snapNumber(node, number))
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
-- Layout keeps its results in each node's layout table: its natural size, its rectangle in the surface, and what was measured to draw it.
-- The node's own fields stay the props it was described with.

local CONTAINERS = { column = true, row = true, scroll = true }

-- elements that take the whole width of a column when nothing says otherwise
local WIDE = { field = true, key = true, dropdown = true, selector = true, slider = true, separator = true }

-- the axis of a container: true for a row
local function horizontal(node)
  return node.type == "row"
end

local function paddingOf(node)
  return node.padding or 0
end

-- whether an element takes the whole cross size of its parent when nothing says otherwise:
-- containers always, separators always, and wide elements across a column
local function stretches(node, row)
  if node.stretch ~= nil then
    return node.stretch
  end

  return CONTAINERS[node.type] or node.type == "separator" or (not row and WIDE[node.type])
end

-- the widths of a selector's choices
local function choiceWidths(node, font, textSize)
  local widths = {}

  for i, choice in ipairs(node.choices) do
    widths[i] = measureText(font, tostring(choice), textSize) + 2 * size.paddingX
  end

  return widths
end

local measure

-- the size of a container's children, along and across its direction
local function measureChildren(node, state, room)
  local along, across = 0, 0

  -- a column's children may take its width less its padding, and a scroll element's less its scroll bar too; a row's take what they need
  local inner = nil

  if not horizontal(node) and (node.width or room) then
    inner = (node.width or room) - 2 * paddingOf(node) - (node.type == "scroll" and size.scrollbarWidth + 4 or 0)
  end

  for _, child in ipairs(node.children) do
    local childWidth, childHeight = measure(child, state, inner)

    if horizontal(node) then
      along, across = along + childWidth, math.max(across, childHeight)
    else
      along, across = along + childHeight, math.max(across, childWidth)
    end
  end

  return along + math.max(0, #node.children - 1) * (node.gap or 0), across
end

-- the size an element needs, without growing, which it keeps in its layout; containers measure their children first
-- room is the width a column gives its children, which text that wraps breaks at; nil in a row
function measure(node, state, room)
  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local line = lineHeight(font, textSize)
  local boxed = line + 2 * size.paddingY
  local layout = {}
  local width, height
  node.layout = layout

  if node.type == "text" and node.wrap and (node.width or room) then
    layout.lines = wrap(font, node.text, textSize, node.width or room)
    width, height = 0, #layout.lines * line

    for _, text in ipairs(layout.lines) do
      width = math.max(width, measureText(font, text, textSize))
    end
  elseif node.type == "text" then
    width, height = measureText(font, node.text, textSize), line
  elseif node.type == "button" then
    width, height = measureText(font, node.label, textSize) + 2 * size.paddingX, boxed
  elseif node.type == "dropdown" then
    local widest = 0

    for _, choice in ipairs(node.choices) do
      widest = math.max(widest, measureText(font, tostring(choice), textSize))
    end

    width, height = widest + 3 * size.paddingX + line / 2, boxed
  elseif node.type == "selector" then
    width, height = 0, boxed

    for _, choiceWidth in ipairs(choiceWidths(node, font, textSize)) do
      width = width + choiceWidth
    end
  elseif node.type == "toggle" then
    local box = node.look == "switch" and 2 * size.checkSize or size.checkSize
    width, height = box, math.max(size.checkSize, line)

    if node.label then
      width = width + size.paddingX + measureText(font, node.label, textSize)
    end
  elseif node.type == "slider" then
    width, height = size.fieldWidth, math.max(size.checkSize, line)
  elseif TAKES_TYPING[node.type] then
    width, height = size.fieldWidth, boxed
  elseif node.type == "image" then
    layout.picture = loadImageCached(node.path, state.scale)
    width, height = 0, 0

    if layout.picture then
      width, height = draw.imageSize(layout.picture)
      width, height = width / state.scale, height / state.scale
    end
  elseif node.type == "separator" then
    width, height = 1, 1
  elseif node.type == "space" then
    width, height = 0, 0
  elseif node.type == "disclosure" then
    width, height = size.checkSize, line
  else
    local along, across = measureChildren(node, state, room)
    local padding = paddingOf(node)
    width, height = across + 2 * padding, along + 2 * padding

    if horizontal(node) then
      width, height = along + 2 * padding, across + 2 * padding
    end

    -- a scroll element shows a part of its content, so it needs little room
    if node.type == "scroll" then
      layout.contentHeight = height
      height = 0
    end
  end

  layout.naturalWidth = node.width or width
  layout.naturalHeight = node.height or height
  return layout.naturalWidth, layout.naturalHeight
end

-- places an element and its children in a rectangle; measure ran first
-- a scroll element's content moves up by its offset, which is kept from 0 to as far as the content goes
local function arrange(node, x, y, width, height, state)
  local layout = node.layout
  layout.x, layout.y, layout.width, layout.height = x, y, width, height

  if not CONTAINERS[node.type] then
    return
  end

  local padding = paddingOf(node)
  local innerX, innerY = x + padding, y + padding
  local innerWidth, innerHeight = width - 2 * padding, height - 2 * padding
  local gap = node.gap or 0
  local row = horizontal(node)

  -- the content of a scroll element is as tall as it needs, and moves up by the offset
  if node.type == "scroll" then
    layout.offset = clamp(state.scroll[node.id] or 0, 0, math.max(0, layout.contentHeight - height))

    if layout.contentHeight > height then
      innerWidth = innerWidth - size.scrollbarWidth - 4
    end

    innerY = innerY - layout.offset
    innerHeight = math.max(layout.contentHeight - 2 * padding, innerHeight)
  end

  -- the main axis: the natural sizes, then the room that is left, shared by the growing children
  local used, grows = 0, 0

  for i, child in ipairs(node.children) do
    used = used + (row and child.layout.naturalWidth or child.layout.naturalHeight) + (i > 1 and gap or 0)
    grows = grows + (child.grow == true and 1 or child.grow or 0)
  end

  local room = (row and innerWidth or innerHeight) - used
  local along = row and innerX or innerY

  for _, child in ipairs(node.children) do
    local grow = child.grow == true and 1 or child.grow or 0
    local childSize = row and child.layout.naturalWidth or child.layout.naturalHeight

    if grows > 0 and grow > 0 and node.type ~= "scroll" then
      childSize = math.max(0, childSize + room * grow / grows)
    end

    -- the cross axis: the whole room, or the natural size aligned in it
    local crossRoom = row and innerHeight or innerWidth
    local cross = row and child.layout.naturalHeight or child.layout.naturalWidth
    local crossAt = row and innerY or innerX

    if stretches(child, row) and not (row and child.height or not row and child.width) then
      cross = crossRoom
    else
      local align = node.align or (row and "center" or "start")
      crossAt = crossAt + (align == "center" and (crossRoom - cross) / 2 or align == "end" and crossRoom - cross or 0)
    end

    -- a separator stands up in a row, and lies across a column, unless its vertical prop says otherwise
    if child.vertical == nil then
      child.layout.vertical = row
    else
      child.layout.vertical = child.vertical
    end

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
  local box = node.layout
  return x >= box.x and x < box.x + box.width and y >= box.y and y < box.y + box.height
end

-- whether a scroll element's content is taller than it, so it scrolls
local function overflows(node)
  return node.layout.contentHeight > node.layout.height
end

-- the top and the length of a scroll element's thumb, along its height
local function thumb(node)
  local box = node.layout
  local length = math.max(math.min(size.thumbMin, box.height), box.height * box.height / box.contentHeight)
  local travel = box.contentHeight - box.height
  local start = travel > 0 and (box.height - length) * box.offset / travel or 0
  return box.y + start, length
end

-- the first and the last byte positions of the selection of the edited field, or nil when nothing is selected; last is the position after it
local function selection(state)
  if state.anchor and state.anchor ~= state.cursorPosition then
    return math.min(state.anchor, state.cursorPosition), math.max(state.anchor, state.cursorPosition)
  end
end

-- the left edges and widths of a selector's choices, shared out over its width
local function choiceRects(node)
  local widths = choiceWidths(node, fontOf(node), node.size or fontSize())
  local total = 0

  for _, width in ipairs(widths) do
    total = total + width
  end

  local rects, x = {}, node.layout.x

  for i, width in ipairs(widths) do
    local shared = node.look == "tabs" and width or total > 0 and width * node.layout.width / total or 0
    rects[i] = { x = x, width = shared }
    x = x + shared
  end

  return rects
end

-- the slider's value at a position across it
local function sliderValue(node, x)
  local box = node.layout
  local radius = size.checkSize / 2
  local travel = box.width - 2 * radius
  local progress = travel > 0 and clamp((x - box.x - radius) / travel, 0, 1) or 0
  return snapNumber(node, node.min + progress * (node.max - node.min))
end

-- the text an edited field shows, where it starts, and the width before the cursor: the text moves left when the cursor would leave the field,
-- so the cursor stays in view
local function fieldText(node, state)
  local font, textSize = fontOf(node), node.size or fontSize()
  local room = node.layout.width - 2 * size.paddingX
  local before = measureText(font, state.typed:sub(1, state.cursorPosition - 1), textSize)
  local textX = node.layout.x + size.paddingX - (before > room and before - room or 0)
  return textX, before
end

-- the rectangle of an edited field's cursor, in layout units
local function cursorRect(node, state)
  local line = lineHeight(fontOf(node), node.size or fontSize())
  local textX, before = fieldText(node, state)
  return { x = textX + before, y = node.layout.y + (node.layout.height - line) / 2, width = 2, height = line }
end

-- a small triangle that points down, centred on a point
local function arrow(surface, x, y, half, color)
  draw.triangles(surface, string.pack("ffffff", x - half, y - half / 2, x + half, y - half / 2, x, y + half / 2), "", color)
end

local function paintSelector(node, surface, state, hover)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local textY = box.y + (box.height - lineHeight(font, textSize)) / 2

  for i, rect in ipairs(choiceRects(node)) do
    local choice = node.choices[i]
    local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.width
    draw.fill(surface, rect.x, box.y, rect.width, box.height, choice == node.value and "accent" or over and "tabShown" or "tab")
    local shown = ellipsize(font, tostring(choice), textSize, rect.width - 2 * size.paddingX)
    local width = measureText(font, shown, textSize)
    draw.text(surface, font, shown, rect.x + (rect.width - width) / 2, textY, textSize, node.color or "text")
  end
end

-- a line under the whole row, and a thicker one in the accent colour under the chosen tab; tabs past the element's width are cut at its edge
local function paintTabs(node, surface, state, hover)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local textY = box.y + (box.height - lineHeight(font, textSize)) / 2

  draw.clip(surface, box.x, box.y, box.width, box.height, function()
    draw.fill(surface, box.x, box.y + box.height - 1, box.width, 1, "tabShown")

    for i, rect in ipairs(choiceRects(node)) do
      local choice = node.choices[i]
      local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.width

      if over and choice ~= node.value then
        draw.fill(surface, rect.x, box.y, rect.width, box.height - 1, "tab")
      end

      if choice == node.value then
        draw.fill(surface, rect.x, box.y + box.height - 3, rect.width, 3, "accent")
      end

      local shown = ellipsize(font, tostring(choice), textSize, rect.width - 2 * size.paddingX)
      draw.text(surface, font, shown, rect.x + size.paddingX, textY, textSize, choice == node.value and (node.color or "text") or "textDim")
    end
  end)
end

local function paintToggle(node, surface, hover)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local check = size.checkSize
  local checkY = box.y + (box.height - check) / 2
  local labelX = box.x + check

  if node.look == "switch" then
    draw.fill(surface, box.x, checkY, 2 * check, check, node.on and "accent" or "tabRow")
    draw.outline(surface, box.x, checkY, 2 * check, check, 1, hover and "accent" or "textDim")
    draw.fill(surface, box.x + (node.on and check or 0) + 3, checkY + 3, check - 6, check - 6, "text")
    labelX = box.x + 2 * check
  else
    draw.fill(surface, box.x, checkY, check, check, "tabRow")
    draw.outline(surface, box.x, checkY, check, check, 1, (node.on or hover) and "accent" or "textDim")

    if node.on then
      draw.fill(surface, box.x + 4, checkY + 4, check - 8, check - 8, "accent")
    end
  end

  if node.label then
    local textY = box.y + (box.height - lineHeight(font, textSize)) / 2
    draw.text(surface, font, ellipsize(font, node.label, textSize, box.x + box.width - labelX - size.paddingX), labelX + size.paddingX, textY, textSize, node.color or "text")
  end
end

-- a field or a key element; the edited one shows what is typed, its selection and its cursor, and a key element asks for a key
local function paintTyped(node, surface, state)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local line = lineHeight(font, textSize)
  local textY = box.y + (box.height - line) / 2
  local editing = state.editing == node.id
  local room = box.width - 2 * size.paddingX
  draw.fill(surface, box.x, box.y, box.width, box.height, "tabRow")

  draw.clip(surface, box.x + 1, box.y + 1, box.width - 2, box.height - 2, function()
    if editing and node.type == "field" then
      local textX, before = fieldText(node, state)
      local first, last = selection(state)

      if first then
        local left = textX + measureText(font, state.typed:sub(1, first - 1), textSize)
        draw.fill(surface, left, textY, textX + measureText(font, state.typed:sub(1, last - 1), textSize) - left, line, "selected")
      end

      draw.text(surface, font, state.typed, textX, textY, textSize, node.color or "text")
      draw.fill(surface, textX + before, textY, 2, line, "accent")
    elseif editing then
      draw.text(surface, font, "Press a key", box.x + size.paddingX, textY, textSize, "textDim")
    else
      draw.text(surface, font, ellipsize(font, node.text, textSize, room), box.x + size.paddingX, textY, textSize, node.color or "text")
    end
  end)

  -- a number field marks a typed text that is not a number
  local valid = node.valid ~= false and not (editing and node.isNumber and tonumber(state.typed) == nil)
  draw.outline(surface, box.x, box.y, box.width, box.height, 1, not valid and INVALID_COLOR or editing and "accent" or "tabShown")
end

-- down when open, right when closed, and up with up
local function paintDisclosure(node, surface)
  local box = node.layout
  local half, x, y = size.checkSize / 4, box.x + box.width / 2, box.y + box.height / 2

  if node.up then
    draw.triangles(surface, string.pack("ffffff", x - half, y + half / 2, x + half, y + half / 2, x, y - half / 2), "", "textDim")
  elseif node.open then
    arrow(surface, x, y, half, "textDim")
  else
    draw.triangles(surface, string.pack("ffffff", x - half / 2, y - half, x - half / 2, y + half, x + half / 2, y), "", "textDim")
  end
end

local paint

-- the children of a scroll element draw inside it only, and its scroll bar over them
local function paintScroll(node, surface, state)
  local box = node.layout

  draw.clip(surface, box.x, box.y, box.width, box.height, function()
    for _, child in ipairs(node.children) do
      if child.layout.y + child.layout.height > box.y and child.layout.y < box.y + box.height then
        paint(child, surface, state)
      end
    end
  end)

  if overflows(node) then
    local start, length = thumb(node)
    local x = box.x + box.width - size.scrollbarWidth
    draw.fill(surface, x, box.y, size.scrollbarWidth, box.height, "tabRow")
    draw.fill(surface, x, start, size.scrollbarWidth, length, state.dragging == node.id and "accent" or "tabShown")
  end
end

-- draws an element and its children; it reads the interaction state, and changes none of it
function paint(node, surface, state)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local line = lineHeight(font, textSize)
  local textY = box.y + (box.height - line) / 2
  local hover = state.hover == node
  local pressed = state.pressed == node
  local background = hover and node.hoverBackground or node.background

  if background then
    draw.fill(surface, box.x, box.y, box.width, box.height, background)
  end

  if node.border then
    draw.outline(surface, box.x, box.y, box.width, box.height, 1, node.border)
  end

  -- the element that Tab gave the keys shows a ring, which fields and key elements show as their outline
  if state.showRing and state.keyTarget == node.id and not TAKES_TYPING[node.type] then
    draw.outline(surface, box.x - 2, box.y - 2, box.width + 4, box.height + 4, 2, "accent")
  end

  if node.type == "text" and box.lines then
    for i, text in ipairs(box.lines) do
      draw.text(surface, font, ellipsize(font, text, textSize, box.width), box.x, box.y + (i - 1) * line, textSize, node.color or "text")
    end
  elseif node.type == "text" then
    draw.text(surface, font, ellipsize(font, node.text, textSize, box.width), box.x, textY, textSize, node.color or "text")
  elseif node.type == "button" then
    draw.fill(surface, box.x, box.y, box.width, box.height, pressed and "accent" or hover and "tabShown" or "tab")
    local shown = ellipsize(font, node.label, textSize, box.width - 2 * size.paddingX)
    local width = measureText(font, shown, textSize)
    draw.text(surface, font, shown, box.x + (box.width - width) / 2, textY, textSize, node.color or "text")
  elseif node.type == "dropdown" then
    draw.fill(surface, box.x, box.y, box.width, box.height, (pressed or hover) and "tabShown" or "tab")
    local room = box.width - 3 * size.paddingX - line / 2
    draw.text(surface, font, ellipsize(font, tostring(node.value or ""), textSize, room), box.x + size.paddingX, textY, textSize, node.color or "text")
    arrow(surface, box.x + box.width - size.paddingX - line / 4, box.y + box.height / 2, line / 4, "textDim")
  elseif node.type == "selector" and node.look == "tabs" then
    paintTabs(node, surface, state, hover)
  elseif node.type == "selector" then
    paintSelector(node, surface, state, hover)
  elseif node.type == "toggle" then
    paintToggle(node, surface, hover)
  elseif node.type == "slider" then
    local radius = size.checkSize / 2
    local centerY = box.y + box.height / 2
    local span = node.max - node.min
    local knobX = box.x + radius + (span > 0 and (node.value - node.min) / span or 0) * (box.width - 2 * radius)
    draw.fill(surface, box.x + radius, centerY - 2, box.width - 2 * radius, 4, "tabRow")
    draw.fill(surface, box.x + radius, centerY - 2, knobX - box.x - radius, 4, "accent")
    draw.circle(surface, knobX, centerY, radius, 0, (hover or state.sliding == node.id) and "accent" or "text")
  elseif TAKES_TYPING[node.type] then
    paintTyped(node, surface, state)
  elseif node.type == "image" and box.picture then
    draw.image(surface, box.picture, box.x, box.y, box.width, box.height)
  elseif node.type == "disclosure" then
    paintDisclosure(node, surface)
  elseif node.type == "separator" and box.vertical then
    draw.fill(surface, box.x + box.width / 2 - 0.5, box.y, 1, box.height, node.color or "tabShown")
  elseif node.type == "separator" then
    draw.fill(surface, box.x, box.y + box.height / 2 - 0.5, box.width, 1, node.color or "tabShown")
  elseif node.type == "scroll" then
    paintScroll(node, surface, state)
    return
  end

  for _, child in ipairs(node.children or {}) do
    paint(child, surface, state)
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
  return INTERACTIVE[node.type] == true or (CONTAINERS[node.type] == true and node.onClick ~= nil)
end

local function scrollable(node)
  return node.type == "scroll" and overflows(node)
end

local function isListItem(node)
  return node.itemIndex ~= nil
end

local function isList(node)
  return node.items ~= nil
end

local function findById(node, id)
  if node.id == id then
    return node
  end

  for _, child in ipairs(node.children or {}) do
    local found = findById(child, id)

    if found then
      return found
    end
  end
end

-- the first element of the tree that passes a test, or nil
local function findFirst(node, test)
  if test(node) then
    return node
  end

  for _, child in ipairs(node.children or {}) do
    local found = findFirst(child, test)

    if found then
      return found
    end
  end
end

-- the innermost element that holds an element of an id and passes a test, or nil
local function findAround(node, id, test, around)
  if node.id == id then
    return around
  end

  for _, child in ipairs(node.children or {}) do
    local found = findAround(child, id, test, test(node) and node or around)

    if found ~= nil then
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

-- ends the editing of a field or key element
local function stopEditing(panel, state)
  if state.editing then
    state.editing, state.typed, state.cursorPosition, state.anchor, state.selecting = nil, nil, nil, nil, nil
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

  local box = node.layout
  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local rowHeight = lineHeight(font, textSize) + 2 * size.paddingY
  local width, height = box.width, rowHeight * #choices
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

  local function choiceAt(y)
    local index = math.floor(y / scale / rowHeight) + 1
    return choices[index] ~= nil and index or nil
  end

  local message
  popup, message = panel:openPopup({
    anchor = { x = box.x * state.scale, y = box.y * state.scale, width = box.width * state.scale, height = box.height * state.scale },
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

        draw.text(surface, font, ellipsize(font, tostring(choice), textSize, width - 2 * size.paddingX), size.paddingX, y + size.paddingY, textSize,
          choice == node.value and "accent" or "text")
      end

      draw.outline(surface, 0, 0, width, height, 1, "tabShown")
    end,
    event = function(event)
      if event.type == "pointerMove" then
        local index = choiceAt(event.y)

        if index and index ~= hover then
          hover = index
          popup:redraw()
        end
      elseif event.type == "pointerUp" and event.button == 1 and choiceAt(event.y) then
        choose(choiceAt(event.y))
      elseif event.type == "keyDown" and (event.key == "Up" or event.key == "Down") then
        hover = clamp(hover + (event.key == "Up" and -1 or 1), 1, #choices)
        popup:redraw()
      elseif event.type == "keyDown" and isEnter(event) then
        choose(hover)
      end
    end,
  })

  if not popup then
    ecs.log.warn(message or "Cannot open the choices.")
  end
end

-- starts editing a field or key element, as a click on it does
local function startEditing(panel, state, node)
  if state.editing ~= node.id then
    stopEditing(panel, state)
    state.editing, state.typed, state.cursorPosition = node.id, node.text, #node.text + 1
    panel:setTextInput(node.type == "field")
  end
end

-- does what a click on an element does; x is where across it, for a selector
local function activate(panel, state, node, x)
  if node.type == "button" or CONTAINERS[node.type] then
    call(node.onClick)
  elseif node.type == "toggle" then
    call(node.onChange, not node.on)
  elseif node.type == "selector" then
    for i, rect in ipairs(choiceRects(node)) do
      if x >= rect.x and x < rect.x + rect.width and node.choices[i] ~= node.value then
        call(node.onChange, node.choices[i])
      end
    end
  elseif node.type == "dropdown" then
    openDropdown(panel, state, node)
  elseif TAKES_TYPING[node.type] then
    startEditing(panel, state, node)
  end
end

-- puts text in place of the selection, or at the cursor
local function replace(state, text)
  local first, last = selection(state)
  first, last = first or state.cursorPosition, last or state.cursorPosition
  state.typed = state.typed:sub(1, first - 1) .. text .. state.typed:sub(last)
  state.cursorPosition, state.anchor = first + #text, nil
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

-- where an edited field's text starts, or a field's that is not edited
local function textStart(node, state)
  if state.editing == node.id then
    return (fieldText(node, state))
  end

  return node.layout.x + size.paddingX
end

-- a key element that is edited takes the next key press as its combination; Escape keeps the one it had
local function captureKey(panel, state, node, event)
  if event.type ~= "keyDown" or MODIFIER_KEYS[event.key] then
    return event.type == "keyUp" or event.type == "text"
  end

  if event.key ~= "Escape" then
    call(node.onChange, combination(event))
  end

  stopEditing(panel, state)
  return true
end

-- Up and Down change an edited number field's number by its step, or by 1, from what is typed, or from its value if that is not a number, and give it at once
local function stepNumber(node, state, up)
  local number = snapNumber(node, (tonumber(state.typed) or node.value) + (up and 1 or -1) * ((node.step and node.step > 0) and node.step or 1))
  state.typed = formatNumber(number)
  state.cursorPosition, state.anchor = #state.typed + 1, nil
  node.onNumber(number)
end

-- the cursor's place after a move key in an edited field: Home and End go to the ends, Ctrl moves by words, and the arrows by characters
local function movedCursor(text, at, key, ctrl)
  if key == "Home" then
    return 1
  elseif key == "End" then
    return #text + 1
  elseif ctrl and key == "Right" then
    return wordEndAfter(text, at)
  elseif ctrl then
    return wordStartBefore(text, at)
  elseif key == "Left" then
    return at > 1 and previousCharacter(text, at) or 1
  end

  return at <= #text and nextCharacter(text, at) or at
end

local MOVE_KEYS = { Left = true, Right = true, Home = true, End = true }

-- handles a key or text event while a field is edited
local function editField(panel, state, node, event)
  -- the cursor is a byte position before which text goes; a character may take several bytes
  -- the selection lies between the anchor and the cursor, and what is typed replaces it
  local typed, at = state.typed, state.cursorPosition
  local first, last = selection(state)
  local key = event.type == "keyDown" and event.key or nil
  local edited = false

  if event.type == "text" then
    replace(state, event.text)
    edited = true
  elseif (key == "Backspace" or key == "Delete") and first then
    replace(state, "")
    edited = true
  elseif key == "Backspace" and at > 1 then
    local previous = event.ctrl and wordStartBefore(typed, at) or previousCharacter(typed, at)
    state.typed, state.cursorPosition = typed:sub(1, previous - 1) .. typed:sub(at), previous
    edited = true
  elseif key == "Delete" and at <= #typed then
    state.typed = typed:sub(1, at - 1) .. typed:sub(event.ctrl and wordEndAfter(typed, at) or nextCharacter(typed, at))
    edited = true
  elseif (key == "Left" or key == "Right") and first and not event.shift and not event.ctrl then
    -- an arrow ends a selection at its side
    state.cursorPosition, state.anchor = key == "Left" and first or last, nil
  elseif MOVE_KEYS[key] then
    -- Shift keeps the anchor, so the move selects
    state.anchor = event.shift and (state.anchor or at) or nil
    state.cursorPosition = movedCursor(typed, at, key, event.ctrl)
  elseif key == "A" and event.ctrl then
    state.anchor, state.cursorPosition = 1, #typed + 1
  elseif (key == "C" or key == "X") and event.ctrl and first then
    ecs.clipboard.setText(typed:sub(first, last - 1))

    if key == "X" then
      replace(state, "")
      edited = true
    end
  elseif key == "V" and event.ctrl then
    -- a field holds one line, so the lines of the pasted text are joined by spaces
    local pasted = (ecs.clipboard.getText() or ""):gsub("\r?\n", " ")

    if pasted ~= "" then
      replace(state, pasted)
      edited = true
    end
  elseif key and isEnter(event) then
    -- onSubmit gives false to keep typing, such as for a text it cannot read
    if not (node.onSubmit and node.onSubmit(state.typed) == false) then
      stopEditing(panel, state)
    end
  elseif key == "Escape" then
    stopEditing(panel, state)
  elseif node.isNumber and (key == "Up" or key == "Down") then
    stepNumber(node, state, key == "Up")
  elseif event.type ~= "keyUp" and event.type ~= "keyDown" then
    return false
  end

  if edited then
    call(node.onChange, state.typed)
  end

  panel:redraw()
  return true
end

-- handles a key or text event while a field or key element is edited
local function handleTyping(panel, state, event)
  local node = state.root and findById(state.root, state.editing)

  if not node then
    stopEditing(panel, state)
    return false
  end

  if node.type == "key" then
    return captureKey(panel, state, node, event)
  end

  return editField(panel, state, node, event)
end

-- the interactive elements in the order of the tree, which Tab goes through
local function tabOrder(node, list)
  if interactive(node) then
    list[#list + 1] = node
  end

  for _, child in ipairs(node.children or {}) do
    tabOrder(child, list)
  end

  return list
end

-- scrolls a scroll element so that an element in it shows whole, or its top when it is taller than the scroll element
local function scrollIntoView(state, scroll, node)
  local top = node.layout.y - scroll.layout.y + scroll.layout.offset
  local offset = state.scroll[scroll.id] or 0
  state.scroll[scroll.id] = math.min(math.max(offset, top + node.layout.height - scroll.layout.height), top)
end

-- gives the keys to the next interactive element, or to the one before; a field or key element takes typing then
local function moveKeyTarget(panel, state, back)
  local list = tabOrder(state.root, {})

  if #list == 0 then
    return false
  end

  local index = 0

  for i, node in ipairs(list) do
    index = node.id == state.keyTarget and i or index
  end

  if back then
    index = index <= 1 and #list or index - 1
  else
    index = index % #list + 1
  end

  local node = list[index]
  stopEditing(panel, state)
  state.keyTarget, state.showRing = node.id, true

  if TAKES_TYPING[node.type] then
    startEditing(panel, state, node)
  end

  local scroll = findAround(state.root, node.id, function(candidate)
    return candidate.type == "scroll"
  end, nil)

  if scroll then
    scrollIntoView(state, scroll, node)
  end

  panel:redraw()
  return true
end

-- the next choice of a selector, one way along it
local function nudgeSelector(node, forward)
  local index = 0

  for i, choice in ipairs(node.choices) do
    index = choice == node.value and i or index
  end

  local choice = node.choices[clamp(index + (forward and 1 or -1), 1, #node.choices)]

  if choice ~= nil and choice ~= node.value then
    call(node.onChange, choice)
  end
end

-- the next value of a slider, one way along it: a step, or a twentieth of its span
local function nudgeSlider(node, forward)
  local amount = (node.step and node.step > 0) and node.step or (node.max - node.min) / 20
  local value = clamp(node.value + (forward and amount or -amount), node.min, node.max)

  if value ~= node.value then
    call(node.onChange, value)
  end
end

-- chooses the item of a list a number of items away from the chosen one, and scrolls it into view
local function moveSelection(state, list, step)
  if #list.items == 0 then
    return
  end

  local index = clamp(list.selected and list.selected + step or (step > 0 and 1 or #list.items), 1, #list.items)
  call(list.onSelect, index)
  scrollIntoView(state, list, list.items[index])
end

-- opens or closes a tree's chosen node; opening an open node chooses its first child, and closing a node that is closed, or has no children, chooses its parent
local function expandTreeNode(state, list, open)
  local tree, index = list.tree, list.selected

  if not index or not tree.ids[index] then
    return
  end

  local folder = tree.folders[index]

  if folder and (folder == "open") ~= open then
    call(tree.onToggle, tree.ids[index], open)
  elseif open and folder == "open" and tree.parents[index + 1] == index then
    moveSelection(state, list, 1)
  elseif not open and tree.parents[index] then
    moveSelection(state, list, tree.parents[index] - index)
  end
end

-- the list that Up, Down and Return act on: the one that has the keys or holds the element that has them, or else the panel's first list
local function keyList(state)
  local target = state.keyTarget and findById(state.root, state.keyTarget)

  if target and isList(target) then
    return target
  end

  return target and findAround(state.root, target.id, isList, nil) or findFirst(state.root, isList)
end

-- handles a key press when no field is edited: Tab moves the keys, Return or Space activates the element that has them, Left and Right change a
-- selector or a slider, and open and close a tree's node, Up and Down choose a list's item, Return activates it if the list has onActivate,
-- and Escape takes the keys away
local function handleKeyDown(panel, state, event)
  if event.key == "Tab" then
    return moveKeyTarget(panel, state, event.shift)
  end

  local node = state.keyTarget and findById(state.root, state.keyTarget)
  local list = keyList(state)
  local enter = isEnter(event) or event.key == "Space"
  local leftOrRight = event.key == "Left" or event.key == "Right"

  if event.key == "Escape" and node then
    state.keyTarget, state.showRing = nil, false
  elseif node and node.type == "selector" and leftOrRight then
    nudgeSelector(node, event.key == "Right")
  elseif node and node.type == "slider" and leftOrRight then
    nudgeSlider(node, event.key == "Right")
  elseif node and enter and node.type ~= "selector" and node.type ~= "slider" and not isList(node) then
    activate(panel, state, node, node.layout.x)
  elseif list and (event.key == "Up" or event.key == "Down") then
    moveSelection(state, list, event.key == "Up" and -1 or 1)
  elseif list and list.tree and leftOrRight then
    expandTreeNode(state, list, event.key == "Right")
  elseif list and isEnter(event) and list.onActivate and list.selected and list.items[list.selected] then
    list.onActivate(list.selected)
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
local function hideTooltip(panel, state)
  if state.tooltipTimer then
    state.tooltipTimer:stop()
  end

  if state.tooltipShown then
    panel:redraw()
  end

  state.tooltip, state.tooltipTimer, state.tooltipShown = nil, nil, false
end

-- starts waiting to show the tooltip of the element under the pointer, unless it shows already;
-- the pointer rests before a tooltip shows, so each move starts the wait again
local function followTooltip(panel, state, x, y)
  local node = find(state.root, x, y, function(candidate)
    return candidate.tooltip ~= nil
  end)

  if node and node.id == state.tooltip and state.tooltipShown then
    return
  end

  hideTooltip(panel, state)

  if node then
    state.tooltip = node.id
    state.tooltipTimer = ecs.timer.start(TOOLTIP_DELAY, false, function()
      state.tooltipShown = true
      panel:redraw()

      state.tooltipTimer = ecs.timer.start(TOOLTIP_TIME, false, function()
        state.tooltipTimer = nil
        hideTooltip(panel, state)
      end)
    end)
  end
end

-- draws the shown tooltip below and right of the pointer, inside the surface
local function paintTooltip(surface, state, width, height)
  local node = state.tooltipShown and findById(state.root, state.tooltip)

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
  local x = clamp((state.pointerX or 0) + 12, 0, math.max(0, width - boxWidth))
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

-- a move of the pointer: a drag of a scroll bar, of a field's selection or of a slider goes on; otherwise the element under it is hovered
local function handlePointerMove(panel, state, x, y)
  local root = state.root
  state.pointerX, state.pointerY = x, y

  if state.dragging then
    local node = findById(root, state.dragging)
    local _, length = thumb(node)
    local travel = node.layout.height - length

    if travel > 0 then
      state.scroll[node.id] = (y - node.layout.y - state.grab) / travel * (node.layout.contentHeight - node.layout.height)
    end

    panel:redraw()
    return true
  end

  if state.selecting then
    local node = findById(root, state.selecting)

    if node and state.editing == node.id then
      state.cursorPosition = positionAt(node, state.typed, textStart(node, state), x)
    end

    panel:redraw()
    return true
  end

  if state.sliding then
    local node = findById(root, state.sliding)

    if node then
      slide(node, x)
    end

    panel:redraw()
    return true
  end

  local hover = find(root, x, y, interactive)

  if hover ~= state.hover or (hover and hover.type == "selector") then
    state.hover = hover
    panel:redraw()
  end

  return hover ~= nil
end

-- a press on a scroll bar grabs its thumb, or moves the thumb there first
local function grabScrollbar(panel, state, scroll, y)
  local box = scroll.layout
  local start, length = thumb(scroll)

  if y < start or y >= start + length then
    local travel = box.height - length
    state.scroll[scroll.id] = travel > 0 and (y - box.y - length / 2) / travel * (box.contentHeight - box.height) or 0
    start = box.y + (y - box.y - length / 2)
  end

  state.dragging, state.grab = scroll.id, y - start
  panel:redraw()
  return true
end

-- a press of the first button: a list's item under it is chosen and its list gets the keys, and the element under it is pressed and gets them
local function handlePointerDown(panel, state, event, x, y)
  local root = state.root
  local scroll = find(root, x, y, scrollable)

  if scroll and x >= scroll.layout.x + scroll.layout.width - size.scrollbarWidth then
    return grabScrollbar(panel, state, scroll, y)
  end

  -- a press on a list's item chooses it; the element under the pointer still gets the press
  local item = find(root, x, y, isListItem)

  if item and item.itemIndex ~= item.list.selected then
    call(item.list.onSelect, item.itemIndex)
  end

  local node = find(root, x, y, interactive)

  if not node or not TAKES_TYPING[node.type] then
    stopEditing(panel, state)
  end

  -- the pressed element gets the keys, without a ring, so Tab goes on from it; a press on an item that takes no press gives the keys to its list
  state.pressed = node
  state.keyTarget, state.showRing = node and node.id or item and item.list.id, false

  if node and node.type == "field" then
    -- the press puts the cursor where it is, with Shift it selects up to there, and a drag selects
    local edited = state.editing == node.id
    local textX = textStart(node, state)
    startEditing(panel, state, node)
    local to = positionAt(node, state.typed, textX, x)
    state.anchor = (edited and event.shift) and (state.anchor or state.cursorPosition) or to
    state.cursorPosition, state.selecting = to, node.id
  elseif node and node.type == "key" then
    startEditing(panel, state, node)
  elseif node and node.type == "slider" then
    state.sliding = node.id
    slide(node, x)
  end

  if node or item then
    panel:redraw()
  end

  return node ~= nil or item ~= nil
end

-- a release of the first button: a click is a press and a release on the same element
local function handlePointerUp(panel, state, x, y)
  local pressed = state.pressed
  state.pressed, state.dragging, state.sliding, state.selecting = nil, nil, nil, nil
  local node = find(state.root, x, y, interactive)

  if pressed and node and node.id == pressed.id and node.type == pressed.type and not TAKES_TYPING[node.type] and node.type ~= "slider" then
    activate(panel, state, node, x)
  end

  panel:redraw()
  return pressed ~= nil
end

-- handles a panel's event: calls the callbacks of the elements it reaches
local function handleEvent(panel, event)
  local state = panelState(panel)

  if not state.root then
    return false
  end

  -- a tooltip goes when anything but a move happens, and follows the pointer onto other elements
  if event.type == "pointerMove" then
    followTooltip(panel, state, (event.x or 0) / state.scale, (event.y or 0) / state.scale)
  else
    hideTooltip(panel, state)
  end

  -- Tab leaves a field too
  if state.editing and (event.type == "text" or event.type == "keyDown" or event.type == "keyUp") and not (event.type == "keyDown" and event.key == "Tab") then
    return handleTyping(panel, state, event)
  end

  if event.type == "keyDown" then
    return handleKeyDown(panel, state, event)
  end

  -- pointer positions come in surface pixels, and the layout is in layout units
  local x, y = (event.x or 0) / state.scale, (event.y or 0) / state.scale

  if event.type == "pointerMove" then
    return handlePointerMove(panel, state, x, y)
  elseif event.type == "pointerDown" and event.button == 1 then
    return handlePointerDown(panel, state, event, x, y)
  elseif event.type == "pointerUp" and event.button == 1 then
    return handlePointerUp(panel, state, x, y)
  elseif event.type == "wheel" then
    local scroll = find(state.root, x, y, scrollable)

    if scroll then
      state.scroll[scroll.id] = (state.scroll[scroll.id] or 0) - event.scrollY * size.wheelStep
      panel:redraw()
      return true
    end
  elseif event.type == "unfocused" or event.type == "hidden" then
    stopEditing(panel, state)
  end

  return false
end

-- services -------------------------------------------------------------------

local services = {}

-- ids that elements leave out come from their places, so they stay the same from one draw to the next
local function assignIds(node, path)
  node.id = node.id or path

  for i, child in ipairs(node.children or {}) do
    assignIds(child, path .. "." .. i)
  end
end

-- keeps each scroll element's offset as the layout kept it, from 0 to as far as its content goes
local function keepScrollOffsets(node, state)
  if node.type == "scroll" then
    state.scroll[node.id] = node.layout.offset
  end

  for _, child in ipairs(node.children or {}) do
    keepScrollOffsets(child, state)
  end
end

-- lays out the elements in the panel's surface and draws them; call it from the panel's draw
function services.show(panel, surface, root)
  local state = panelState(panel)
  local width, height = surface.width / surface.scale, surface.height / surface.scale
  state.scale = surface.scale
  fonts = {}
  readSizes()
  assignIds(root, "root")
  measure(root, state, width)
  arrange(root, 0, 0, width, height, state)
  keepScrollOffsets(root, state)

  -- the element under the pointer and the pressed element are found again in the new layout
  state.hover = state.hover and state.pointerX and find(root, state.pointerX, state.pointerY, interactive) or nil
  state.pressed = state.pressed and findById(root, state.pressed.id) or nil
  state.root = root

  draw.fill(surface, 0, 0, width, height, "background")
  paint(root, surface, state)
  paintTooltip(surface, state, width, height)

  -- an input method shows its candidates at the edited field's cursor
  local edited = state.editing and findById(root, state.editing)

  if edited and edited.type == "field" then
    local rect, scale = cursorRect(edited, state), surface.scale
    panel:setTextInput(true, rect.x * scale, rect.y * scale, rect.width * scale, rect.height * scale)
  end
end

-- forgets a panel's state; call it from the panel's destroy
function services.forget(panel)
  -- a closing panel is not drawn again, so its tooltip's timer only stops
  if panels[panel] and panels[panel].tooltipTimer then
    panels[panel].tooltipTimer:stop()
  end

  panels[panel] = nil
end

-- the rectangle of an element in the last layout, in layout units
function services.rect(panel, id)
  local root = panelState(panel).root
  local node = root and findById(root, id)

  if not node then
    return nil
  end

  return { x = node.layout.x, y = node.layout.y, width = node.layout.width, height = node.layout.height }
end

-- the id of the field or key element that is edited, or nil
function services.focus(panel)
  return panelState(panel).editing
end

-- the id of the element whose tooltip shows, or nil
function services.tooltip(panel)
  local state = panelState(panel)
  return state.tooltipShown and state.tooltip or nil
end

-- starts editing a field or key element, as a click on it does
function services.edit(panel, id)
  local state = panelState(panel)
  local node = state.root and findById(state.root, id)

  -- the keyboard's target follows, without its ring, as after a click
  if node and TAKES_TYPING[node.type] then
    state.keyTarget, state.showRing = node.id, false
    startEditing(panel, state, node)
    panel:redraw()
  end
end

-- scrolls a scroll element so that one of its children shows
function services.reveal(panel, scrollId, childId)
  local state = panelState(panel)
  local scroll = state.root and findById(state.root, scrollId)
  local child = scroll and findById(scroll, childId)

  if child then
    scrollIntoView(state, scroll, child)
    panel:redraw()
  end
end

-- descriptions, for plugins in C -------------------------------------------

-- the callbacks of each type of element; a described element with an id keeps what they get, for ui.take, since C gives ui no functions
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

-- what each type's element function takes besides its props: its children, a value, choices, columns and rows, or nodes; the others take a value
local TAKES = {
  column = "children", row = "children", scroll = "children", list = "children", group = "children",
  selector = "choices", tabs = "choices", dropdown = "choices",
  table = "table", tree = "tree", separator = "none", space = "none",
}

-- an element from a description, built with the element functions, as Lua code builds it
local function build(panel, description)
  local props = shallowCopy(description.props)

  -- a column or row with clickable = true is clicked like a button
  local elementType = description.type
  local callbacks = CALLBACKS[elementType] or (props.clickable and { "onClick" }) or {}
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

  local takes = TAKES[elementType]
  local make = elements[elementType]
  assert(make, ("no element of type '%s'"):format(tostring(elementType)))

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
    props.open, props.selected = description.open, description.selected
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
    fn = handleEvent,
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
    sig = "value<string?>(handle<ecs.panel> panel)",
    doc = "Give the id of the edited field\n"
        .. "@param panel The panel that shows the field\n"
        .. "@return The id of the field or key element that takes typing, or nil if none does",
    fn = services.focus,
  },
  tooltip = {
    sig = "value<string?>(handle<ecs.panel> panel)",
    doc = "Give the id of the element whose tooltip shows\n"
        .. "@param panel The panel that shows the element\n"
        .. "@return The element's id, or nil if no tooltip shows",
    fn = services.tooltip,
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
