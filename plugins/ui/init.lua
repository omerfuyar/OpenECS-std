-- The ui standard plugin: user-interface elements with layout (DESIGN 5).
-- A panel describes its elements in draw, each time, and ui lays them out, draws them with the draw plugin, and calls their callbacks on events.
-- Its elements are generic, so that plugins build their own views from them.

local ecs = require("ecs")
local ffi = require("ffi")
local newTable = require("table.new")
local clearTable = require("table.clear")
local draw = require("draw")
local image = require("image")

-- a triangle's points, as draw takes them; one array is filled for each triangle, as draw copies the points or draws them at once
local triangle

do
  local points = ffi.new("float[6]")

  function triangle(surface, x1, y1, x2, y2, x3, y3, color)
    points[0], points[1], points[2], points[3], points[4], points[5] = x1, y1, x2, y2, x3, y3
    draw.triangles(surface, points, "", color)
  end
end

-- sizes in layout units, which the user can change as settings of the plugin
local SIZES = {
  { "paddingX",       8,  "Room left and right of the text of buttons, fields and lists" },
  { "paddingY",       3,  "Room above and below the text of buttons, fields and lists" },
  { "checkSize",      14, "Size of a toggle's box, and of a slider's knob" },
  { "fieldWidth",     120, "Width of a field or slider that is not stretched" },
  { "scrollbarWidth", 8,  "Width of a scroll bar" },
  { "radius",         3,  "Radius of the corners of buttons, fields, check boxes and popups; 0 makes them square" },
  { "thumbMin",       24, "Shortest length of a scroll bar's thumb" },
  { "wheelStep",      48, "Distance a wheel step scrolls" },
}

local size = {}

-- how far, in layout units, a press on a number moves before it drags the number, so a click that shakes is still a click
local NUMBER_DRAG_START = 3

-- a graph's height, in layout units, when it gives none
local GRAPH_HEIGHT = 80

local function readSizes()
  for _, entry in ipairs(SIZES) do
    size[entry[1]] = ecs.setting.get("ui." .. entry[1]) or entry[2]
  end
end

for _, entry in ipairs(SIZES) do
  ecs.setting.declare({ name = entry[1], type = "number", default = entry[2], description = entry[3] })
end

readSizes()

-- the props of an element's look, which a style gives (DESIGN 5)
local LOOK = {}

for name in ("width height grow stretch padding gap align background hoverBackground border color size font"):gmatch("%S+") do
  LOOK[name] = true
end

-- ui's own styles, which its views and the first-party views use so they look alike; ui.styles gives copies of them
local styles = {
  view = { padding = 16, gap = 10, background = "panel" },
  item = { padding = 8, gap = 12 },
  title = { size = 18 },
  heading = { color = "accent" },
  note = { size = 12, color = "textDim" },
  bar = { padding = 4, gap = 4, background = "tabRow", stretch = true },
}

-- how long, in seconds, the pointer rests on an element before its tooltip shows, and how long the tooltip stays;
-- a panel hears nothing when the pointer leaves it, so a tooltip goes by itself too
local TOOLTIP_DELAY, TOOLTIP_TIME = 0.5, 5

-- the elements that take the pointer
local INTERACTIVE = { button = true, toggle = true, selector = true, dropdown = true, slider = true, field = true, key = true, color = true }

-- the elements that take typing while they have the keys
local TAKES_TYPING = { field = true, key = true }

-- every panel's state, by its panel handle: the last layout, the element under the pointer, the pressed element, the edited field and the scroll offsets
local panels = {}

local function panelState(panel)
  local state = panels[panel]

  if not state then
    state = { scroll = {}, scale = 1, taken = {}, lists = {}, revealed = {} }
    panels[panel] = state
  end

  return state
end

-- what ui keeps until a setting changes: the font size, the fonts by name, what texts measure, and the lines of wrapped texts; draw keeps
-- the fonts themselves
local textSizeSetting
local fonts = {}
local measured, measuredCount = {}, 0
local wrapped, wrappedCount = {}, 0
local MEASURED_MOST = 8192
local settingsChanged = true

local function fontSize()
  textSizeSetting = textSizeSetting or ecs.setting.get("ecs.fontSize") or 14
  return textSizeSetting
end

local function fontOf(node)
  local name = node.font or "ui"
  local font = fonts[name]

  if font == nil then
    font = draw.font(name)

    -- a name that no font has yet is asked again, since a plugin may add the font later
    if font == nil then
      return fontOf({ font = name ~= "ui" and "ui" or nil })
    end

    fonts[name] = font
  end

  return font
end

-- the scale of the surface that the panel being shown or handled draws into: text is measured at the size it is drawn at
local textScale = 1

-- the width of a text alone; draw.measure gives the line height too, which a call such as math.max(...) would take as well
-- a text's width and line height at a font, size and scale, kept, since each show measures the same texts again
local function measureKept(font, text, textSize)
  local bySize = measured[font]

  if bySize == nil then
    bySize = {}
    measured[font] = bySize
  end

  local key = textSize * 1024 + textScale
  local byText = bySize[key]

  if byText == nil then
    byText = {}
    bySize[key] = byText
  end

  local kept = byText[text]

  if kept == nil then
    -- texts that change all the time, such as a clock's, would fill the memory
    if measuredCount >= MEASURED_MOST then
      measured, measuredCount = {}, 0
      return measureKept(font, text, textSize)
    end

    kept = { draw.measure(font, text, textSize, textScale) }
    byText[text] = kept
    measuredCount += 1
  end

  return kept[1], kept[2]
end

local function measureText(font, text, textSize)
  local width = measureKept(font, text, textSize)
  return width
end

-- whether a text matches a search: each word of the query, whatever its case, is somewhere in it; an empty query matches every text.
-- A view that searches several texts of an item joins them with new lines, so that no word matches across two of them
local function matchesQuery(query, text)
  text = tostring(text or ""):lower()

  for word in tostring(query or ""):lower():gmatch("%S+") do
    if not text:find(word, 1, true) then
      return false
    end
  end

  return true
end

-- partial drawing ---------------------------------------------------------------------------------------------------------------------

-- A show records its draw calls, each with the rectangle it may change, and compares them with the last show's. While the surface keeps
-- its pixels, only the rectangle around the calls that changed is drawn again, and the core shows only it: a hover changes a few elements,
-- and drawing every pixel of a large panel for it would be slow.

local drawn = draw -- the draw plugin's functions; draw below records calls while a show records them
local recording -- the calls of the show that records them, or nil; its tables are reused from the show before the last
local recorded = 0 -- how many calls the show has recorded
local recordClip -- the clip of the calls now, { left, top, right, bottom, x, y, width, height, outer, scroll }, or nil
local recordScroll -- the scroll element whose children the calls now draw, { id, outer }, or nil

-- room around a call's rectangle, in layout units, for smooth edges and slanted letters
local CALL_EDGE = 2

-- the show's next call, with its rectangle and clip; the caller puts its arguments after the surface in it, from 1 to n
local function record(name, left, top, right, bottom)
  local clip = recordClip

  if clip then
    left, top, right, bottom = math.max(left, clip[1]), math.max(top, clip[2]), math.min(right, clip[3]), math.min(bottom, clip[4])
  end

  recorded += 1
  local call = recording[recorded]

  if call == nil then
    call = newTable(6, 8)
    recording[recorded] = call
  end

  call.name, call.clip, call.key, call.scroll = name, clip, nil, recordScroll
  call.left, call.top, call.right, call.bottom = left - CALL_EDGE, top - CALL_EDGE, right + CALL_EDGE, bottom + CALL_EDGE
  return call
end

draw = setmetatable({}, { __index = drawn })

function draw.fill(surface, x, y, width, height, color)
  if not recording then
    return drawn.fill(surface, x, y, width, height, color)
  end

  local call = record("fill", x, y, x + width, y + height)
  call[1], call[2], call[3], call[4], call[5], call.n = x, y, width, height, color, 5
end

function draw.outline(surface, x, y, width, height, thickness, color)
  if not recording then
    return drawn.outline(surface, x, y, width, height, thickness, color)
  end

  local call = record("outline", x, y, x + width, y + height)
  call[1], call[2], call[3], call[4], call[5], call[6], call.n = x, y, width, height, thickness, color, 6
end

function draw.rounded(surface, x, y, width, height, radius, color)
  if not recording then
    return drawn.rounded(surface, x, y, width, height, radius, color)
  end

  local call = record("rounded", x, y, x + width, y + height)
  call[1], call[2], call[3], call[4], call[5], call[6], call.n = x, y, width, height, radius, color, 6
end

function draw.roundedOutline(surface, x, y, width, height, radius, thickness, color)
  if not recording then
    return drawn.roundedOutline(surface, x, y, width, height, radius, thickness, color)
  end

  local call = record("roundedOutline", x, y, x + width, y + height)
  call[1], call[2], call[3], call[4], call[5], call[6], call[7], call.n = x, y, width, height, radius, thickness, color, 7
end

function draw.circle(surface, x, y, radius, thickness, color)
  if not recording then
    return drawn.circle(surface, x, y, radius, thickness, color)
  end

  local call = record("circle", x - radius, y - radius, x + radius, y + radius)
  call[1], call[2], call[3], call[4], call[5], call.n = x, y, radius, thickness, color, 5
end

function draw.image(surface, picture, x, y, width, height)
  if not recording then
    return drawn.image(surface, picture, x, y, width, height)
  end

  local call = record("image", x, y, x + width, y + height)
  call[1], call[2], call[3], call[4], call[5], call.n = picture, x, y, width, height, 5
end

function draw.text(surface, font, text, x, y, textSize, color)
  if not recording then
    return drawn.text(surface, font, text, x, y, textSize, color)
  end

  local width, line = measureKept(font, text, textSize)
  local call = record("text", x, y, x + width, y + line)
  call[1], call[2], call[3], call[4], call[5], call[6], call.n = font, text, x, y, textSize, color, 6
  return width
end

-- a buffer's bytes as a string: a buffer is a string, a string.buffer or an FFI array
local function bytesOf(buffer)
  return type(buffer) == "cdata" and ffi.string(buffer, ffi.sizeof(buffer)) or tostring(buffer)
end

-- triangles compare by the bytes of their buffers, which a plugin may fill again in place, so their call keeps the bytes as its key
function draw.triangles(surface, points, indices, color)
  if not recording then
    return drawn.triangles(surface, points, indices, color)
  end

  local left, top, right, bottom = math.huge, math.huge, -math.huge, -math.huge
  local pointBytes = bytesOf(points)
  local xy = ffi.cast("const float *", pointBytes)

  for i = 0, #pointBytes / 8 - 1 do
    local x, y = xy[i * 2], xy[i * 2 + 1]
    left, top, right, bottom = math.min(left, x), math.min(top, y), math.max(right, x), math.max(bottom, y)
  end

  -- the call keeps the bytes, as the plugin may fill its buffers again before the show is drawn
  local indexBytes = bytesOf(indices)
  local call = record("triangles", left, top, right, bottom)
  call[1], call[2], call[3], call.n = pointBytes, indexBytes, color, 3
  call.key = pointBytes .. indexBytes .. tostring(color)
end

-- What ui's own clips paint: ui pushes the element, the surface and the interaction state on inside.stack, then clips a body, a function of
-- inside, that reads them from its top, so a show makes no closure for its clips; clips nest as the elements do. A show also records its
-- clips as it does its calls, in inside.clips, the tables of the show before the last, and counts them in inside.clipped.
local inside = { stack = { top = 0 }, clips = nil, clipped = 0 }

-- clips a body of inside, which paints an element on a surface with the interaction state
function inside.clip(surface, x, y, width, height, body, node, state)
  local stack = inside.stack
  local top = stack.top
  stack[top + 1], stack[top + 2], stack[top + 3], stack.top = node, surface, state, top + 3
  draw.clip(surface, x, y, width, height, body)
  stack.top = top
end

-- the element, surface and state of the clip that runs now
function inside.arguments()
  local stack = inside.stack
  local top = stack.top
  return stack[top - 2], stack[top - 1], stack[top]
end

function draw.clip(surface, x, y, width, height, body)
  if not recording then
    return drawn.clip(surface, x, y, width, height, body)
  end

  local outer = recordClip
  local clipped = inside.clipped + 1
  local clip = inside.clips[clipped]
  inside.clipped = clipped

  if clip == nil then
    clip = newTable(4, 6)
    inside.clips[clipped] = clip
  end

  clip[1], clip[2], clip[3], clip[4] = x, y, x + width, y + height
  clip.x, clip.y, clip.width, clip.height, clip.outer, clip.scroll = x, y, width, height, outer, recordScroll

  if outer then
    clip[1], clip[2], clip[3], clip[4] = math.max(clip[1], outer[1]), math.max(clip[2], outer[2]), math.min(clip[3], outer[3]), math.min(clip[4], outer[4])
  end

  recordClip = clip
  local ok, message = pcall(body)
  recordClip = outer

  if not ok then
    error(message, 0)
  end
end

-- whether two clips are the same: the same rectangles, inside the same clips
local function sameClip(a, b)
  while a ~= b do
    if a == nil or b == nil or a.x ~= b.x or a.y ~= b.y or a.width ~= b.width or a.height ~= b.height then
      return false
    end

    a, b = a.outer, b.outer
  end

  return true
end

-- whether two calls draw the same: the same function, clip and arguments. A handle is the same object each time, so rawequal compares all
local function sameCall(a, b)
  local n = a.n

  if a.name ~= b.name or n ~= b.n or a.key ~= b.key then
    return false
  end

  local clipA, clipB = a.clip, b.clip

  if clipA ~= clipB and not sameClip(clipA, clipB) then
    return false
  end

  if a.key then
    return true
  end

  for i = 1, n do
    if not rawequal(a[i], b[i]) then
      return false
    end
  end

  return true
end

-- a rectangle grown to hold a call's, when the call's has an area
local function grow(call, left, top, right, bottom)
  if call.right > call.left and call.bottom > call.top then
    return math.min(left, call.left), math.min(top, call.top), math.max(right, call.right), math.max(bottom, call.bottom)
  end

  return left, top, right, bottom
end

local changedAreas

do
  -- room between two changed rectangles, in layout units, under which they are drawn again as one; and the most rectangles a show draws
  local AREA_NEAR, AREA_MOST = 16, 4

  -- adds a rectangle to a list of rectangles to draw again: it joins one it is near, and the joined one is checked again; past
  -- AREA_MOST rectangles, it joins the one whose rectangle around both grows the least
  local function addArea(areas, left, top, right, bottom)
    if right <= left or bottom <= top then
      return
    end

    local i = 1

    while i <= #areas do
      local area = areas[i]

      if left - AREA_NEAR < area[3] and right + AREA_NEAR > area[1] and top - AREA_NEAR < area[4] and bottom + AREA_NEAR > area[2] then
        left, top, right, bottom = math.min(left, area[1]), math.min(top, area[2]), math.max(right, area[3]), math.max(bottom, area[4])
        table.remove(areas, i)
        i = 1
      else
        i += 1
      end
    end

    if #areas < AREA_MOST then
      areas[#areas + 1] = { left, top, right, bottom }
      return
    end

    local best, growth = 1, math.huge

    for k, area in ipairs(areas) do
      local width = math.max(right, area[3]) - math.min(left, area[1])
      local height = math.max(bottom, area[4]) - math.min(top, area[2])
      local grown = width * height - (area[3] - area[1]) * (area[4] - area[2])

      if grown < growth then
        best, growth = k, grown
      end
    end

    local area = table.remove(areas, best)
    addArea(areas, math.min(left, area[1]), math.min(top, area[2]), math.max(right, area[3]), math.max(bottom, area[4]))
  end

  -- the rectangles around the calls that differ between two shows, a list of { left, top, right, bottom }; nil when none differs, and
  -- false when some differ but change no pixel. The calls the shows start and end with alike are left out: a call between them that
  -- changed covers every pixel it changed, old or new, so the pixels outside the rectangles are drawn by the same calls in the same order.
  -- When as many calls are left in both, the calls are compared in pairs and only the pairs that differ are bounded; otherwise one
  -- rectangle bounds all that are left
  function changedAreas(old, oldCount, new, newCount)
    local first, most = 1, math.min(oldCount, newCount)

    while first <= most and sameCall(old[first], new[first]) do
      first += 1
    end

    if first > oldCount and first > newCount then
      return nil
    end

    local lastOld, lastNew = oldCount, newCount

    while lastOld >= first and lastNew >= first and sameCall(old[lastOld], new[lastNew]) do
      lastOld, lastNew = lastOld - 1, lastNew - 1
    end

    local areas = {}

    if lastOld == lastNew then
      for i = first, lastOld do
        if not sameCall(old[i], new[i]) then
          local left, top, right, bottom = grow(old[i], math.huge, math.huge, -math.huge, -math.huge)
          addArea(areas, grow(new[i], left, top, right, bottom))
        end
      end

      return #areas > 0 and areas
    end

    local left, top, right, bottom = math.huge, math.huge, -math.huge, -math.huge

    for i = first, lastOld do
      left, top, right, bottom = grow(old[i], left, top, right, bottom)
    end

    for i = first, lastNew do
      left, top, right, bottom = grow(new[i], left, top, right, bottom)
    end

    addArea(areas, left, top, right, bottom)
    return #areas > 0 and areas
  end
end

-- a scroll whose offset alone changed moves its pixels, and only what they do not show is drawn again: the strip that comes into view,
-- what changed inside it, and what is drawn over it. Its calls are compared with the offset taken off their position.
local movedAreas, movedScroll, reportChanged

do
  -- the argument of each function that is its top; triangles are compared by their bytes, so a moved one differs
  local TOP_ARGUMENT = { fill = 2, outline = 2, circle = 2, image = 3, text = 4 }

  -- whether a call or clip is drawn inside a scroll element's children, at any depth
  local function insideScroll(scroll, id)
    while scroll do
      if scroll.id == id then
        return true
      end

      scroll = scroll.outer
    end

    return false
  end

  local function near(a, b)
    return math.abs(a - b) < 1e-4
  end

  -- whether two clips are the same, those inside the scroll element moved up by shift
  local function sameMovedClip(a, b, id, shift)
    if a == b then
      return true
    elseif a == nil or b == nil then
      return false
    elseif not insideScroll(b.scroll, id) then
      return sameClip(a, b)
    end

    return a.x == b.x and a.width == b.width and a.height == b.height and near(a.y - shift, b.y) and sameMovedClip(a.outer, b.outer, id, shift)
  end

  -- whether a call inside the scroll element draws the same as an earlier one, moved up by shift
  local function sameMovedCall(a, b, id, shift)
    local top, n = TOP_ARGUMENT[a.name], a.n

    if top == nil or a.name ~= b.name or n ~= b.n or not sameMovedClip(a.clip, b.clip, id, shift) then
      return false
    end

    for i = 1, n do
      if i == top then
        if not near(a[i] - shift, b[i]) then
          return false
        end
      elseif not rawequal(a[i], b[i]) then
        return false
      end
    end

    return true
  end

  -- the first and last of the calls drawn inside the scroll element; they follow each other, as its children are drawn at once
  local function scrollCalls(calls, count, id)
    local first, last

    for i = 1, count do
      if insideScroll(calls[i].scroll, id) then
        first = first or i
        last = i
      elseif first then
        break
      end
    end

    return first, last
  end

  local function overlaps(call, box)
    return call.left < box[3] and call.right > box[1] and call.top < box[4] and call.bottom > box[2]
  end

  -- whether a call's clip leaves the whole box
  local function clipHolds(call, box)
    local clip = call.clip
    return clip == nil or (clip[1] <= box[1] and clip[2] <= box[2] and clip[3] >= box[3] and clip[4] >= box[4])
  end

  -- whether a call fills the whole box with one colour, so moving the box's pixels keeps it as it is
  local function fillsBox(call, box)
    return call.name == "fill" and call[1] <= box[1] and call[2] <= box[2] and call[1] + call[3] >= box[3] and call[2] + call[4] >= box[4]
      and clipHolds(call, box)
  end

  -- whether a call is an outline around the box, such as a list's border: its sides look the same at every height, so only its top
  -- and bottom lines move with the pixels
  local function outlinesBox(call, box)
    return call.name == "outline" and call[1] == box[1] and call[2] == box[2] and call[1] + call[3] == box[3] and call[2] + call[4] == box[4]
      and clipHolds(call, box)
  end

  -- grows a rectangle, { left, top, right, bottom } or nil, to hold another, or a call's moved down by shift, cut to a box if one is given
  local function boundArea(rect, left, top, right, bottom, box)
    if box then
      left, top, right, bottom = math.max(left, box[1]), math.max(top, box[2]), math.min(right, box[3]), math.min(bottom, box[4])
    end

    if right <= left or bottom <= top then
      return rect
    elseif rect == nil then
      return { left, top, right, bottom }
    end

    rect[1], rect[2], rect[3], rect[4] = math.min(rect[1], left), math.min(rect[2], top), math.max(rect[3], right), math.max(rect[4], bottom)
    return rect
  end

  local function bound(rect, call, shift, box)
    return boundArea(rect, call.left, call.top + shift, call.right, call.bottom + shift, box)
  end

  -- where the old and new calls inside the scroll element meet: the items that left the view start the old ones, and those that came
  -- into view start the new ones, when a long list builds only the items in view
  local function meet(old, oldFirst, oldLast, new, newFirst, newLast, id, shift)
    for skip = 0, math.max(oldLast - oldFirst, newLast - newFirst) do
      if oldFirst + skip <= oldLast and sameMovedCall(old[oldFirst + skip], new[newFirst], id, shift) then
        return oldFirst + skip, newFirst
      elseif newFirst + skip <= newLast and sameMovedCall(old[oldFirst], new[newFirst + skip], id, shift) then
        return oldFirst, newFirst + skip
      end
    end

    return oldLast + 1, newLast + 1
  end

  -- the rectangles to draw again after the pixels of a scroll element's box moved up by shift, or nil when moving them would not do:
  -- what is under the box must be one colour, and the calls before and after the scroll element's own must be found in both shows
  function movedAreas(old, oldCount, new, newCount, id, shift, box)
    local oldFirst, oldLast = scrollCalls(old, oldCount, id)
    local newFirst, newLast = scrollCalls(new, newCount, id)

    if not oldFirst or not newFirst or oldFirst ~= newFirst then
      return nil
    end

    -- an outline around the box is drawn again at its top and bottom lines, where they were drawn and where they moved to
    local topEdge, bottomEdge

    for i = 1, newFirst - 1 do
      local call = new[i]

      if not sameCall(old[i], call) then
        return nil
      elseif overlaps(call, box) and not fillsBox(call, box) then
        if not outlinesBox(call, box) then
          return nil
        end

        local edge = call[5] + CALL_EDGE
        topEdge = boundArea(topEdge, box[1], box[2], box[3], box[2] + edge, box)
        topEdge = boundArea(topEdge, box[1], box[2] - shift, box[3], box[2] + edge - shift, box)
        bottomEdge = boundArea(bottomEdge, box[1], box[4] - edge, box[3], box[4], box)
        bottomEdge = boundArea(bottomEdge, box[1], box[4] - edge - shift, box[3], box[4] - shift, box)
      end
    end

    -- the strip that comes into view, with room for the smooth edges of what it cuts
    local strip = shift > 0 and { box[1], box[4] - shift - CALL_EDGE, box[3], box[4] } or { box[1], box[2], box[3], box[2] - shift + CALL_EDGE }

    -- the calls inside that differ: those before the old and new meet, and those between the run that is the same and the calls the two
    -- end with alike; the old ones where their pixels moved to
    local oldAt, newAt = meet(old, oldFirst, oldLast, new, newFirst, newLast, id, shift)
    local leading, trailing

    for i = oldFirst, oldAt - 1 do
      leading = bound(leading, old[i], -shift, box)
    end

    for i = newFirst, newAt - 1 do
      leading = bound(leading, new[i], 0, box)
    end

    while oldAt <= oldLast and newAt <= newLast and sameMovedCall(old[oldAt], new[newAt], id, shift) do
      oldAt, newAt = oldAt + 1, newAt + 1
    end

    local lastOld, lastNew = oldLast, newLast

    while lastOld >= oldAt and lastNew >= newAt and sameMovedCall(old[lastOld], new[lastNew], id, shift) do
      lastOld, lastNew = lastOld - 1, lastNew - 1
    end

    for i = oldAt, lastOld do
      trailing = bound(trailing, old[i], -shift, box)
    end

    for i = newAt, lastNew do
      trailing = bound(trailing, new[i], 0, box)
    end

    -- the calls after it: those that differ, and those over the box, whose pixels moved with it
    local after, afterOld, afterNew = 1, oldCount - oldLast, newCount - newLast
    local later, over

    while after <= afterOld and after <= afterNew and sameCall(old[oldLast + after], new[newLast + after]) do
      after += 1
    end

    local tailOld, tailNew = oldCount, newCount

    while tailOld >= oldLast + after and tailNew >= newLast + after and sameCall(old[tailOld], new[tailNew]) do
      tailOld, tailNew = tailOld - 1, tailNew - 1
    end

    for i = oldLast + 1, oldCount do
      if i - oldLast >= after and i <= tailOld then
        later = bound(later, old[i], 0)
      end

      if overlaps(old[i], box) then
        over = bound(over, old[i], -shift, box)
      end
    end

    for i = newLast + 1, newCount do
      if i - newLast >= after and i <= tailNew then
        later = bound(later, new[i], 0)
      end

      if overlaps(new[i], box) then
        over = bound(over, new[i], 0, box)
      end
    end

    local rects = { strip }

    for _, rect in ipairs({ topEdge or false, bottomEdge or false, leading or false, trailing or false, later or false, over or false }) do
      if rect then
        rects[#rects + 1] = rect
      end
    end

    return rects
  end

  -- the scroll element whose offset alone changed since the last show, the only one that changed, by whole pixels, with its box as
  -- { left, top, right, bottom }; nil when none or more than one changed, or its box moved
  function movedScroll(before, now, scale)
    local found

    for id, layout in pairs(now) do
      local was = before and before[id]

      if was and was.offset ~= layout.offset then
        if found then
          return nil
        end

        found = id
      end
    end

    if not found then
      return nil
    end

    local layout, was = now[found], before[found]
    local shift = layout.offset - was.offset
    local pixels = shift * scale

    if layout.x ~= was.x or layout.y ~= was.y or layout.width ~= was.width or layout.height ~= was.height or math.abs(shift) >= layout.height
      or math.abs(pixels - math.floor(pixels + 0.5)) > 1e-3 then
      return nil
    end

    return found, shift, { layout.x, layout.y, layout.x + layout.width, layout.y + layout.height }
  end

  -- tells the core the rectangle of the surface that changed, in layout units
  function reportChanged(surface, left, top, right, bottom)
    local scale = surface.scale
    left, top = math.max(0, math.floor(left * scale)), math.max(0, math.floor(top * scale))
    right, bottom = math.min(surface.width, math.ceil(right * scale)), math.min(surface.height, math.ceil(bottom * scale))
    surface:changed(left, top, math.max(0, right - left), math.max(0, bottom - top))
  end
end

-- what a replay draws: drawn.clip takes a function, so the replay's functions read it here, made once, and not from closures made each show
local replaying = { surface = nil, calls = nil, count = 0, area = nil, from = 1, to = 0 }

-- makes the recorded calls from replaying.from to replaying.to that reach the area, or all of them
local function replayGroup()
  local surface, calls, area = replaying.surface, replaying.calls, replaying.area

  for k = replaying.from, replaying.to do
    local call = calls[k]

    if not area or (call.left < area[3] and call.right > area[1] and call.top < area[4] and call.bottom > area[2]) then
      local name = call.name

      if name == "fill" then
        drawn.fill(surface, call[1], call[2], call[3], call[4], call[5])
      elseif name == "text" then
        drawn.text(surface, call[1], call[2], call[3], call[4], call[5], call[6])
      elseif name == "outline" then
        drawn.outline(surface, call[1], call[2], call[3], call[4], call[5], call[6])
      elseif name == "circle" then
        drawn.circle(surface, call[1], call[2], call[3], call[4], call[5])
      elseif name == "rounded" then
        drawn.rounded(surface, call[1], call[2], call[3], call[4], call[5], call[6])
      elseif name == "roundedOutline" then
        drawn.roundedOutline(surface, call[1], call[2], call[3], call[4], call[5], call[6], call[7])
      elseif name == "image" then
        drawn.image(surface, call[1], call[2], call[3], call[4], call[5])
      else
        drawn.triangles(surface, call[1], call[2], call[3])
      end
    end
  end
end

-- makes the recorded calls, each group of calls with the same clip inside it; a group that does not reach the area is passed over
local function replayCalls()
  local surface, calls, count, area = replaying.surface, replaying.calls, replaying.count, replaying.area
  local i = 1

  while i <= count do
    local clip, last = calls[i].clip, i
    local reaches = not area

    while true do
      local call = calls[last]
      reaches = reaches or (call.left < area[3] and call.right > area[1] and call.top < area[4] and call.bottom > area[2])

      if last >= count or calls[last + 1].clip ~= clip then
        break
      end

      last += 1
    end

    replaying.from, replaying.to = i, last

    if reaches and clip then
      drawn.clip(surface, clip[1], clip[2], clip[3] - clip[1], clip[4] - clip[2], replayGroup)
    elseif reaches then
      replayGroup()
    end

    i = last + 1
  end
end

-- makes the recorded calls that reach an area, or all of them, each inside its clip
local function replay(surface, calls, count, area)
  replaying.surface, replaying.calls, replaying.count, replaying.area = surface, calls, count, area

  if area then
    drawn.clip(surface, area[1], area[2], area[3] - area[1], area[4] - area[2], replayCalls)
  else
    replayCalls()
  end

  replaying.surface, replaying.calls, replaying.area = nil, nil, nil
end

local function lineHeight(font, textSize)
  local _, line = measureKept(font, "Ag", textSize)
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
  at += 1

  while continues(text, at) do
    at += 1
  end

  return at
end

-- the byte position of the character before a position
local function previousCharacter(text, at)
  at -= 1

  while at > 1 and continues(text, at) do
    at -= 1
  end

  return at
end

local function isSpace(text, at)
  return text:sub(at, at):match("%s") ~= nil
end

-- the byte position of the start of the word before a position; words are what lies between spaces
local function wordStartBefore(text, at)
  while at > 1 and isSpace(text, at - 1) do
    at -= 1
  end

  while at > 1 and not isSpace(text, at - 1) do
    at -= 1
  end

  return at
end

-- the byte position after the end of the word after a position
local function wordEndAfter(text, at)
  while at <= #text and isSpace(text, at) do
    at += 1
  end

  while at <= #text and not isSpace(text, at) do
    at += 1
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
    local middle = math.floor((low + high + 1) / 2)
    local cut = prefix(text, middle) .. "…"

    if measureText(font, cut, textSize) <= width then
      low = middle
    else
      high = middle - 1
    end
  end

  return prefix(text, low) .. "…"
end

-- the parts of a word that may each start a line: a word wider than the width breaks after its slashes, so a path breaks between its folders,
-- and a part still wider breaks between characters
local function wordParts(font, word, textSize, width)
  if measureText(font, word, textSize) <= width then
    return { word }
  end

  local parts = {}

  for part in word:gmatch("[^/\\]*[/\\]?") do
    local rest = part

    while rest ~= "" do
      local low, high = 1, utf8.len(rest) or #rest

      while low < high do
        local middle = math.floor((low + high + 1) / 2)

        if measureText(font, prefix(rest, middle), textSize) <= width then
          low = middle
        else
          high = middle - 1
        end
      end

      local fits = prefix(rest, low)
      parts[#parts + 1] = fits
      rest = rest:sub(#fits + 1)
    end
  end

  return parts
end

-- the lines of a text that wraps at a width: it breaks at spaces, and at its own new lines; a word wider than the width breaks into parts (wordParts),
-- and a line keeps the blanks it starts with, so indented text stays indented
local function wrap(font, text, textSize, width)
  local lines = {}

  for paragraph in (text .. "\n"):gmatch("(.-)\n") do
    local current = paragraph:match("^%s+")

    for word in paragraph:gmatch("%S+") do
      for i, part in ipairs(wordParts(font, word, textSize, width)) do
        local words = current and current:find("%S")
        local joined = current and (words and i == 1 and current .. " " or current) .. part or part

        if words and measureText(font, joined, textSize) > width then
          lines[#lines + 1] = current
          current = part
        else
          current = joined
        end
      end
    end

    lines[#lines + 1] = current or ""
  end

  return lines
end

-- the lines of a text that wraps at a width, and the width of the widest, kept as measured texts are: each show wraps the same texts again,
-- and wrapping measures many texts, each line so far with the next word, which would fill the kept measures
local function wrapKept(font, text, textSize, width)
  local bySize = wrapped[font]

  if bySize == nil then
    bySize = {}
    wrapped[font] = bySize
  end

  local key = (textSize * 1024 + textScale) .. ":" .. width
  local byText = bySize[key]

  if byText == nil then
    byText = {}
    bySize[key] = byText
  end

  local kept = byText[text]

  if kept == nil then
    if wrappedCount >= MEASURED_MOST then
      wrapped, wrappedCount = {}, 0
      return wrapKept(font, text, textSize, width)
    end

    kept = { lines = wrap(font, text, textSize, width), width = 0 }

    for _, line in ipairs(kept.lines) do
      kept.width = math.max(kept.width, measureText(font, line, textSize))
    end

    byText[text] = kept
    wrappedCount += 1
  end

  return kept.lines, kept.width
end

local spanFont, layoutSpans

do
  -- the styled fonts by name, then by the bits of their style; they are made again when ui's fonts are, after a setting changes
  local STYLE_WORDS = { "bold", "italic", "underline", "strike" }
  local styledFor, styled = nil, {}

  -- a span's style as bits, from STYLE_WORDS: 1 bold, 2 italic, 4 underline, 8 strike; a link is underlined
  local function styleBits(span)
    return (span.bold and 1 or 0) + (span.italic and 2 or 0) + ((span.underline or span.link) and 4 or 0) + (span.strike and 8 or 0)
  end

  -- a span's font: mono for code, else the text's; draw makes bold, italic, underline and struck text from the font itself
  function spanFont(node, span)
    if styledFor ~= fonts then
      styledFor, styled = fonts, {}
    end

    local name = span.mono and "mono" or node.font or "ui"
    local bits = styleBits(span)

    if bits == 0 then
      return fontOf({ font = name })
    end

    local byBits = styled[name]

    if byBits == nil then
      byBits = {}
      styled[name] = byBits
    end

    local font = byBits[bits]

    if font == nil then
      local words = {}

      for i, word in ipairs(STYLE_WORDS) do
        if bits & (1 << (i - 1)) ~= 0 then
          words[#words + 1] = word
        end
      end

      font = draw.styledFont(name, table.concat(words, " ")) or fontOf(node)
      byBits[bits] = font
    end

    return font
  end

  -- the lines of a text of spans: each line is runs of one span, by its index in the spans, at x from the line's start; with a width, it
  -- breaks between words as wrap does, and a line after a break leaves out the blanks it would start with
  local function lay(node, textSize, width)
    local lines, x, broke = { { runs = {}, width = 0 } }, 0, false

    local function add(text, span, index, blank)
      local font = spanFont(node, span)
      local runWidth = measureText(font, text, textSize)
      local line = lines[#lines]

      if not blank and width and x > 0 and x + runWidth > width then
        lines[#lines + 1] = { runs = {}, width = 0 }
        line, x, broke = lines[#lines], 0, true
      end

      if blank and x == 0 and broke then
        return
      end

      local last = line.runs[#line.runs]

      -- runs of one span in a row are one run, so it is drawn with one call
      if last and last.index == index then
        last.text ..= text
        last.width += runWidth
      else
        line.runs[#line.runs + 1] = { x = x, text = text, index = index, width = runWidth }
      end

      x += runWidth

      if not blank then
        line.width = x
      end
    end

    for index, span in ipairs(node.spans) do
      local first = true

      -- an icon or an image is a square a line high, which wraps as a word does
      if span.icon or span.image then
        local side = lineHeight(spanFont(node, span), textSize)
        local line = lines[#lines]

        if width and x > 0 and x + side > width then
          lines[#lines + 1] = { runs = {}, width = 0 }
          line, x, broke = lines[#lines], 0, true
        end

        line.runs[#line.runs + 1] = { x = x, text = "", index = index, width = side, picture = true }
        x += side
        line.width = x
      end

      for part in ((span.icon or span.image) and "" or span.text .. "\n"):gmatch("(.-)\n") do
        if not first then
          lines[#lines + 1] = { runs = {}, width = 0 }
          x, broke = 0, false
        end

        first = false

        for blanks, word in part:gmatch("(%s*)(%S*)") do
          if blanks ~= "" then
            add(blanks, span, index, true)
          end

          -- a word wider than the width breaks into parts, as in wrap
          if word ~= "" then
            for _, wordPart in ipairs(width and wordParts(spanFont(node, span), word, textSize, width) or { word }) do
              add(wordPart, span, index, false)
            end
          end
        end
      end
    end

    return lines
  end

  -- the lines of texts of spans, kept by their font, size, scale, width and the spans' texts and styles, as wrapped texts are: each show
  -- lays out the same texts again. The lines are shared, so no one changes them
  local spannedFor, spanned, spannedCount = nil, {}, 0
  local keyParts = {}

  function layoutSpans(node, textSize, width)
    if spannedFor ~= fonts then
      spannedFor, spanned, spannedCount = fonts, {}, 0
    end

    keyParts[1], keyParts[2], keyParts[3] = textSize * 1024 + textScale, width or "", node.font or ""
    local parts = 3

    for _, span in ipairs(node.spans) do
      keyParts[parts + 1] = (span.mono and 16 or 0) + styleBits(span)
      keyParts[parts + 2] = span.icon and "\1icon:" .. span.icon or span.image and "\1image:" .. span.image or span.text
      parts += 2
    end

    local key = table.concat(keyParts, "\0", 1, parts)
    local kept = spanned[key]

    if kept == nil then
      if spannedCount >= MEASURED_MOST then
        spanned, spannedCount = {}, 0
      end

      kept = lay(node, textSize, width)
      spanned[key] = kept
      spannedCount += 1
    end

    return kept
  end
end

-- images read once for each path and scale, so that they are drawn sharp.
-- When the cache is full, it drops the images that no show used since the show before the last, so a panel that shows many keeps them all.
-- A file that could not be read is tried again after a while, so an image that is saved later shows.
local IMAGES_KEPT = 64
local IMAGE_RETRY_SECONDS = 2
local images = {}
local imageCount = 0
local showCount = 0 -- grows with each show, of any panel

local function loadImageCached(path, scale)
  local key = path .. "@" .. scale
  local found = images[key]

  if found and not found.picture and os.time() - found.triedAt >= IMAGE_RETRY_SECONDS then
    images[key], found, imageCount = nil, nil, imageCount - 1
  end

  if not found then
    if imageCount >= IMAGES_KEPT then
      for kept, entry in pairs(images) do
        if entry.usedAt < showCount - 1 then
          images[kept], imageCount = nil, imageCount - 1
        end
      end
    end

    found = { picture = image.load(path, scale), triedAt = os.time() }
    images[key] = found
    imageCount += 1
  end

  found.usedAt = showCount
  return found.picture
end

-- numbers ----------------------------------------------------------------------

-- a number field's or slider's number, kept from min to max and rounded to step from min, or from 0
local function snapNumber(node, number)
  return snap(number, node.min or -math.huge, node.max or math.huge, node.step, node.min or 0)
end

-- a number as a field shows it: 14 digits keep what a user types, such as 0.1, and leave out the float's last digits
local function formatNumber(number)
  return ("%.14g"):format(number)
end

-- element constructors -------------------------------------------------------

-- every prop an element may have; one that is not here is a mistake, such as onchange for onChange
local PROPS = {}

for name in ("id style width height grow stretch padding gap align background hoverBackground border color size font tooltip keys selectable title selected "
  .. "sort ascending vertical wrap label look min max step valid clickable open up alpha placeholder dialog pattern results colors lines folders count first "
  .. "onClick onChange onSubmit onSelect onActivate onSort onToggle onLink onMenu onDrop onDrag"):gmatch("%S+") do
  PROPS[name] = true
end

-- the tables of elements that a panel's next show left behind, cleared, which new elements are made from, as an arena is reused each
-- frame: so each draw makes little garbage. An element lives until its panel's next show, which recycles it
-- spare.trees holds the trees made since the last show, whose rows the next show builds once it knows which are in view; a main chunk of
-- LuaJIT has at most 200 locals, which this file reaches
local spare = { nodes = {}, layouts = {}, lists = {}, trees = {} }

-- the last of a list of cleared tables, taken from it, or nil
function spare.take(list)
  local count = #list
  local taken = list[count]

  if taken then
    list[count] = nil
  end

  return taken
end

-- clears an element of a show before the last, with its children and its layout, and keeps their tables for new elements
function spare.recycle(node)
  local children, layout = node.children, node.layout

  if children then
    for i = 1, #children do
      spare.recycle(children[i])
    end

    clearTable(children)
    spare.lists[#spare.lists + 1] = children
  end

  if layout then
    clearTable(layout)
    spare.layouts[#spare.layouts + 1] = layout
  end

  clearTable(node)
  spare.nodes[#spare.nodes + 1] = node
end

-- an element is a copy of its props, so the plugin's table stays as it gave it; room for the fields that ui adds is made at once
local function element(elementType, props, children)
  local node = spare.take(spare.nodes) or newTable(0, 12)

  for name, value in pairs(props or node) do
    if not PROPS[name] then
      ecs.check(false, ("ui does not know the prop '%s' of a %s."):format(tostring(name), elementType))
    end

    node[name] = value
  end

  -- the element takes each prop of its style that it does not give itself; a style's own style comes after it, so the nearer one wins
  local style, depth = node.style, 0
  node.style = nil

  while style ~= nil do
    ecs.check(type(style) == "table" and depth < 16, "A style is a table of look props, and styles build on fewer than 16 others.")

    for name, value in pairs(style) do
      if name ~= "style" then
        if not LOOK[name] then
          ecs.check(false, ("A style gives only an element's look, not '%s'."):format(tostring(name)))
        end

        if node[name] == nil then
          node[name] = value
        end
      end
    end

    style, depth = style.style, depth + 1
  end

  node.type = elementType

  -- a child that is false is left out, so a list of children may hold `shown and ui.text(...)`; each child keeps its place in the list for its id,
  -- so the children after one that is left out keep theirs
  if children then
    node.children = spare.take(spare.lists) or {}

    for place = 1, #children do
      local child = children[place]

      if child then
        child.place = place
        node.children[#node.children + 1] = child
      end
    end
  end

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
  ecs.check(node.id, "A scroll element needs an id, which keeps its offset.")

  if node.grow == nil then
    node.grow = 1
  end

  return node
end

-- a scroll element whose children are items to choose; children with selectable = false, such as headings, are not items;
-- a section's heading is not an item, and the children of an open section are items of the list, so Up and Down go through every section.
-- A long list gives count, its number of items, and only the items that ui.range names, from first: the others take their room without being built
function elements.list(props, children)
  local flat = {}

  for place = 1, #(children or {}) do
    local child = children[place]

    if child and child.section then
      for _, sectionChild in ipairs(child.children) do
        flat[#flat + 1] = sectionChild
      end
    else
      flat[#flat + 1] = child
    end
  end

  local node = elements.scroll(props, flat)
  node.items = {}
  node.first = node.first or 1

  -- selected is the chosen item's number, or a list of the chosen ones' numbers, each highlighted; Up and Down move from the first
  local chosen = {}

  if type(node.selected) == "table" then
    for _, index in ipairs(node.selected) do
      chosen[index] = true
    end

    node.selected = node.selected[1]
  elseif node.selected then
    chosen[node.selected] = true
  end

  local built = 0

  for _, child in ipairs(node.children) do
    if child.selectable ~= false then
      local index = node.first + built
      built += 1
      node.items[index] = child
      child.list, child.itemIndex = node, index

      -- an item of a long list keeps its id from its number, not from its place among the items built
      if node.count and child.place == nil then
        child.place = index
      end

      -- an item takes the list's whole width, so its highlight does
      if child.stretch == nil then
        child.stretch = true
      end

      child.chosen = chosen[index]

      if chosen[index] and child.background == nil then
        child.background = "selected"
      end

      if node.onActivate then
        child.hoverBackground = child.hoverBackground or "hover"
        child.onClick = child.onClick or function()
          node.onActivate(index)
        end
      end
    end
  end

  -- the items not built take their room in two spaces, before and after the built ones, whose heights measure gives
  if node.count then
    node.last = node.first + built - 1

    if node.first > 1 then
      table.insert(node.children, 1, elements.space({ grow = 0 }))
      node.children[1].place, node.spaceBefore = "before", node.children[1]
    end

    if node.last < node.count then
      node.children[#node.children + 1] = elements.space({ grow = 0 })
      node.children[#node.children].place, node.spaceAfter = "after", node.children[#node.children]
    end
  else
    node.count = built
  end

  return node
end

-- a tree: a list whose items are the shown nodes, each below its parent and indented by its depth;
-- a node is { id, label, children }, and one with children, even none, has an arrow that a click opens and closes;
-- open names the open nodes by id, selected is the chosen node's id or a list of the chosen ones' ids, onSelect and onActivate get a node's id,
-- and onToggle(id, open) its new state; a node's label is a text or spans, and its marks are short labels at the end of its row, such as an eye and a lock, each on or off,
-- and a click on one gives onMark(id, mark, on) its new state without choosing the node;
-- every tree builds only the rows in view and near it, as ui.range names them when its panel shows, in Lua and in C alike
function elements.tree(nodes, props)
  props = props or {}
  ecs.check(props.id, "A tree needs an id, which keeps its offset.")
  local open = props.open or {}
  local ids, parents, folders, items, depths = {}, {}, {}, {}, {}

  -- the shown nodes, in order: each node, then its children when it is open
  local function add(list, depth, parent)
    for _, item in ipairs(list) do
      local folder = item.children ~= nil
      local shown = folder and open[item.id] == true
      local index = #ids + 1
      ids[index], parents[index], folders[index] = item.id, parent, folder and (shown and "open" or "closed") or nil
      items[index], depths[index] = item, depth

      if shown then
        add(item.children, depth + 1, index)
      end
    end
  end

  add(nodes or {}, 0, nil)

  -- the row of a shown node
  local function buildRow(index)
    local item, depth = items[index], depths[index]
    local folder = item.children ~= nil
    local shown = folders[index] == "open"

    -- the arrow is a row with onClick, so a press on it toggles, and a press beside it only chooses
    local arrow = elements.space({ width = size.checkSize, grow = 0 })

    if folder then
      arrow = elements.row({ onClick = function()
        if props.onToggle then
          props.onToggle(item.id, not shown)
        end
      end }, { element("disclosure", { open = shown }) })
    end

    local cells = {
      elements.space({ width = depth * size.checkSize, grow = 0 }),
      arrow,
      elements.text(item.label or tostring(item.id), { color = item.color }),
    }

    -- the marks are rows with onClick, as the arrow is, so a press on one turns it and does not choose the node
    if item.marks and #item.marks > 0 then
      cells[#cells + 1] = elements.space({ grow = 1 })

      for _, mark in ipairs(item.marks) do
        local cell = elements.row({
          id = props.id .. ":" .. tostring(item.id) .. ":" .. tostring(mark.id),
          tooltip = mark.tooltip,
          keys = mark.keys,
          onClick = function()
            if props.onMark then
              props.onMark(item.id, mark.id, not mark.on)
            end
          end,
        }, { elements.text(mark.label, { color = mark.on and "accent" or "textDim" }) })
        -- a press on a mark keeps the list's choice, and drags nothing
        cell.keepsChoice = true
        cells[#cells + 1] = cell
      end
    end

    return elements.row({ id = props.id .. ":" .. tostring(item.id), gap = 4, padding = 2 }, cells)
  end

  -- the list works with indexes, and the tree with ids
  local listProps = shallowCopy(props)
  listProps.open, listProps.onToggle, listProps.selected, listProps.onMark = nil, nil, nil, nil
  listProps.count = #ids

  local indexOf, selected = {}, {}

  for index, id in ipairs(ids) do
    indexOf[id] = index
  end

  for _, id in ipairs(type(props.selected) == "table" and props.selected or { props.selected }) do
    selected[#selected + 1] = indexOf[id]
  end

  listProps.selected = #selected > 0 and selected or nil

  listProps.onSelect = props.onSelect and function(index, how, between)
    local betweenIds = nil

    if between then
      betweenIds = {}

      for i, other in ipairs(between) do
        betweenIds[i] = ids[other]
      end
    end

    props.onSelect(ids[index], how, betweenIds)
  end

  listProps.onActivate = props.onActivate and function(index)
    props.onActivate(ids[index])
  end

  listProps.onMenu = props.onMenu and function(index, x, y)
    props.onMenu(index and ids[index], x, y)
  end

  listProps.onDrag = props.onDrag and function(index)
    return props.onDrag(ids[index])
  end

  listProps.onDrop = props.onDrop and function(index, where, dataType, value)
    props.onDrop(index and ids[index], where, dataType, value)
  end

  -- the rows are built when the panel shows, for the shown nodes that ui.range names then: the list stands in for them until then
  local node = elements.list(listProps, {})
  local treeInfo = { ids = ids, parents = parents, folders = folders, onToggle = props.onToggle }
  node.tree, node.dropInto = treeInfo, true
  spare.trees[#spare.trees + 1] = {
    node = node,
    id = props.id,
    count = #ids,
    fill = function(first, last)
      local rows = {}

      for index = first, last do
        rows[#rows + 1] = buildRow(index)
      end

      listProps.first = first
      local built = elements.list(listProps, rows)
      clearTable(node)

      for key, value in pairs(built) do
        node[key] = value
      end

      -- the rows are items of the list that stands in the tree
      for _, item in pairs(node.items) do
        item.list = node
      end

      -- data dropped on the middle of a node goes into it
      node.tree, node.dropInto = treeInfo, true
    end,
  }
  return node
end

-- a table: a header of its columns' titles over a list of its rows, whose cells line up below the titles;
-- a column is { id, title, width }, and a row is a table of the cells' values by column id; a column is as wide as its widest cell unless it gives a width;
-- selected is the chosen row's number, onSelect and onActivate get a row's number, and a click on a title gives the column's id to onSort;
-- sort names the column the rows are sorted by, which shows an arrow, up with ascending = true; a title's id is the table's id, ":title:" and the column's id
function elements.table(columns, rows, props)
  props = props or {}
  ecs.check(props.id, "A table needs an id, which keeps its offset.")
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
  local list = elements.list({ id = props.id, grow = 1, selected = props.selected, onSelect = props.onSelect, onActivate = props.onActivate, onMenu = props.onMenu, onDrag = props.onDrag, onDrop = props.onDrop }, items)
  local header = elements.row({ id = props.id .. ":header", padding = 2, background = "tabRow", stretch = true }, cells(nil))
  return elements.column({ width = props.width, height = props.height, grow = props.grow, stretch = props.stretch, border = props.border }, { header, list })
end

-- a heading with an arrow, which a click opens and closes, over its children while it is open; the plugin keeps open, true if missing,
-- and onToggle(open) gets the new state, as a tree's nodes do; in a list, the heading is not an item, and the children are the list's items
function elements.section(title, props, children)
  props = props or {}
  local open = props.open ~= false
  local onToggle = props.onToggle
  local columnProps = shallowCopy(props)
  columnProps.open, columnProps.onToggle = nil, nil
  local node = element("column", columnProps, {})
  node.section = true

  node.children[1] = elements.row({ id = node.id and node.id .. ":heading" or nil, gap = 4, selectable = false, stretch = true, onClick = function()
    if onToggle then
      onToggle(not open)
    end
  end }, {
    element("disclosure", { open = open }),
    elements.text(type(title) == "table" and title or tostring(title or ""), { style = styles.heading }),
  })
  node.children[1].place = 1

  for place = 1, open and #(children or {}) or 0 do
    local child = children[place]

    if child then
      child.place = place + 1
      node.children[#node.children + 1] = child
    end
  end

  return node
end

-- a colour, "#RRGGBB" or "#RRGGBBAA": a swatch, which a click opens in a picker; onChange(color) gets the colour that a press
-- in the picker ends on, with its alpha only when alpha = true
function elements.color(value, props)
  local node = element("color", props)
  node.value = tostring(value or "#000000")
  return node
end

-- a column with a frame and a title
function elements.group(props, children)
  local node = element("column", props, {})
  node.padding = node.padding or styles.item.padding
  node.gap = node.gap or styles.item.padding / 2
  node.border = node.border or "frame"

  if node.title then
    node.children[1] = elements.text(node.title, { style = styles.note })
  end

  for _, child in ipairs(children or {}) do
    node.children[#node.children + 1] = child
  end

  return node
end

function elements.separator(props)
  return element("separator", props)
end

-- bars of numbers from the left; a value that is a list stacks its parts, each in its colour
function elements.graph(values, props)
  ecs.check(type(values) == "table", "A graph takes a list of values: numbers, or lists of numbers that stack.")
  local node = element("graph", props)
  node.values = values
  node.colors = node.colors or { "accent" }
  node.lines = node.lines or {}
  ecs.check(type(node.colors) == "table" and #node.colors > 0, "A graph's colors is a list of colours, one for each part of a bar.")
  return node
end

-- a span of a text: a text, a table of its text and its style, or a picture a line high, an icon or an image, given as its element or
-- as a table with its icon's name or its image's path; a picture's text is empty
function inside.span(span)
  if type(span) ~= "table" then
    return { text = tostring(span) }
  elseif span.type == "icon" then
    return { text = "", icon = span.name, color = span.color }
  elseif span.type == "image" then
    return { text = "", image = span.path }
  end

  ecs.check(span.icon == nil or inside.ICONS[span.icon] == true, ("'%s' is not an icon: eye, eyeClosed, lock or lockOpen."):format(tostring(span.icon)))
  return setmetatable({ text = (span.icon or span.image) and "" or tostring(span.text or "") }, { __index = span })
end

-- a label: a text, or a list of spans, as a text takes, so it can hold icons and images beside its words
-- a text, or a list of spans; text keeps the plain text either way
function elements.text(text, props)
  local node = element("text", props)

  if type(text) == "table" then
    local plain = {}
    node.spans = {}

    for i, span in ipairs(text) do
      node.spans[i] = inside.span(span)
      plain[i] = node.spans[i].text
    end

    node.text = table.concat(plain)
  else
    node.text = tostring(text)
  end

  return node
end

-- a button whose label is a text, or a list of spans, as a text takes
function elements.button(label, props)
  local node = element("button", props)

  if type(label) == "table" then
    node.spans = elements.text(label).spans
    local plain = {}

    for i, span in ipairs(node.spans) do
      plain[i] = span.text
    end

    node.label = table.concat(plain)
  else
    node.label = tostring(label)
  end

  return node
end

-- on or off: a check box, or a switch with look = "switch"
function elements.toggle(on, props)
  local node = element("toggle", props)
  node.on = on == true
  return node
end

-- a number between min and max, 0 and 1 if missing, which a drag changes; step rounds it
-- onChange gets each value of a drag, and onSubmit the one that its release ends on; Left and Right give theirs to both
function elements.slider(value, props)
  local node = element("slider", props)
  node.min, node.max = node.min or 0, node.max or 1
  node.value = clamp(tonumber(value) or node.min, node.min, node.max)
  return node
end

-- one of a few choices, side by side, or of a list of them in a popup
-- selected is the chosen one's number, from 1, and onSelect gets the number of the one the user chooses
local function choiceElement(elementType, selected, choices, props)
  local node = element(elementType, props)
  local onSelect = node.onSelect
  node.choices = choices or {}
  node.value, node.selected, node.onSelect = node.choices[selected], nil, nil

  -- a dropdown's popup calls it after the panel's next show recycled the element, so it keeps the choices, not the element
  local kept = node.choices

  node.onChange = onSelect and function(choice)
    for i, each in ipairs(kept) do
      if each == choice then
        onSelect(i)
        return
      end
    end
  end

  return node
end

function elements.selector(selected, choices, props)
  return choiceElement("selector", selected, choices, props)
end

-- tabs: a selector whose choices are headers of their natural widths from the left, with a line under the chosen one;
-- the panel shows below them what the chosen tab holds
function elements.tabs(selected, choices, props)
  local node = elements.selector(selected, choices, props)
  node.look = "tabs"
  return node
end

-- one of a list of choices, which a click opens in a popup
function elements.dropdown(selected, choices, props)
  return choiceElement("dropdown", selected, choices, props)
end

function elements.field(text, props)
  local node = element("field", props)
  node.text = tostring(text or "")
  ecs.check(node.id, "A field needs an id, which keeps what is typed.")
  return node
end

-- the number that a number field's text gives: a number, or a sum of numbers such as 32*4 or (10+2)/3, which Lua works out with
-- nothing else in reach; nil for any other text
local function readNumber(text)
  local number = tonumber(text)

  if number == nil and text:find("^[%d%.%s%+%-%*/%%%^%(%)eE]+$") then
    local chunk = load("return " .. text, "=number", "t", {})
    local ok, result = false, nil

    if chunk then
      ok, result = pcall(chunk)
    end

    if ok and type(result) == "number" and result == result and result ~= math.huge and result ~= -math.huge then
      number = result
    end
  end

  return number
end

-- a field for a number, kept from min to max if they are given and rounded to step if it is given; onChange gets the number at each edit
-- that is a number or a sum, and onSubmit at Return; Up and Down change it by step, or by 1, and give it to both
function elements.number(value, props)
  local node = elements.field("", props)
  local onChange, onSubmit = node.onChange, node.onSubmit
  node.isNumber = true
  node.value = snapNumber(node, tonumber(value) or 0)
  node.text = formatNumber(node.value)

  node.onChange = function(text)
    local number = readNumber(text)

    if number ~= nil and onChange then
      onChange(snapNumber(node, number))
    end
  end

  -- a text that is not a number keeps the field typing
  node.onSubmit = function(text)
    local number = readNumber(text)

    if number == nil then
      return false
    end

    if onSubmit then
      onSubmit(snapNumber(node, number))
    end
  end

  return node
end

-- a field for the path of a file, with a button that opens the system's file dialog; dialog is "openFile", "saveFile" or "openFolder", and pattern
-- the extensions the dialog shows, such as "ttf;otf"; the chosen path goes to onChange and onSubmit, as if it were typed and Return pressed
function elements.path(text, props)
  props = props or {}
  ecs.check(props.id, "A path needs an id, which keeps what is typed.")
  local rowProps, fieldProps = { id = props.id .. ":path", gap = 4 }, { grow = 1 }

  -- the row takes the place the path has in its parent, and the field the rest of its props
  for name, value in pairs(props) do
    if name == "width" or name == "grow" or name == "stretch" then
      rowProps[name] = value
    elseif name ~= "dialog" and name ~= "pattern" then
      fieldProps[name] = value
    end
  end

  local browse = elements.button("…", { id = props.id .. ":browse", tooltip = "Choose with the file dialog", onClick = function()
    local filters = props.pattern and { { name = props.pattern, pattern = props.pattern } } or nil
    local location = text ~= nil and text ~= "" and text or nil
    local ok, message = ecs.dialog.show({ type = props.dialog or "openFile", filters = filters, location = location }, function(files)
      if files and files[1] and props.onChange then
        props.onChange(files[1])
      end

      if files and files[1] and props.onSubmit then
        props.onSubmit(files[1])
      end
    end)

    if not ok then
      ecs.log.warn(message or "Cannot show the file dialog.")
    end
  end })

  return elements.row(rowProps, { elements.field(text, fieldProps), browse })
end

-- a field that narrows what a view shows, with a button that clears it while it holds a text; onChange gets the query at each edit, and
-- ui.matches tells the view which texts match it. Ctrl+F in the panel gives it the keys, and Escape clears it before taking them away.
-- results names the list that shows what matches, which Up and Down choose in while the search has the keys
function elements.search(query, props)
  props = props or {}
  local id = props.id or "search"
  query = tostring(query or "")
  local rowProps = { id = id .. ":search", gap = 4 }
  local fieldProps = { id = id, grow = 1, placeholder = "Search", tooltip = "Shows only what holds each word typed", keys = "Ctrl+F" }

  -- the row takes the place the search has in its parent, and the field the rest of its props
  for name, value in pairs(props) do
    if name == "width" or name == "grow" or name == "stretch" then
      rowProps[name] = value
    elseif name ~= "id" then
      fieldProps[name] = value
    end
  end

  fieldProps.placeholder ..= " (Ctrl+F)"
  local field = elements.field(query, fieldProps)
  field.isSearch = true
  local children = { field }

  if query ~= "" then
    children[2] = elements.button("\u{00D7}", { id = id .. ":clear", tooltip = "Clear the search", keys = "Escape", onClick = function()
      if props.onChange then
        props.onChange("")
      end

      if props.onSubmit then
        props.onSubmit("")
      end
    end })
  end

  return elements.row(rowProps, children)
end

function elements.key(combination, props)
  local node = element("key", props)
  node.text = tostring(combination or "")
  ecs.check(node.id, "A key element needs an id.")
  return node
end

function elements.image(path, props)
  local node = element("image", props)
  node.path = path
  return node
end

-- a small picture, a line of text high, in its colour: an eye, open or closed, or a padlock, locked or open; the main chunk is near Lua's
-- 200 locals, so the names stay inside this block
do
  inside.ICONS = { eye = true, eyeClosed = true, lock = true, lockOpen = true }

  function elements.icon(name, props)
    ecs.check(inside.ICONS[name] == true, ("'%s' is not an icon: eye, eyeClosed, lock or lockOpen."):format(tostring(name)))
    local node = element("icon", props)
    node.name = name
    return node
  end
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
local WIDE = { field = true, key = true, dropdown = true, selector = true, slider = true, separator = true, graph = true }

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

local measure, measureRowWidths

-- what ui keeps of a list between shows: the height of each item when it was last shown, by its number, with their sum and count, and the
-- height and gap of the list's view
local function listKept(state, id)
  local kept = state.lists[id]

  if not kept then
    kept = { heights = {}, known = 0, total = 0, view = 0, gap = 0 }
    state.lists[id] = kept
  end

  return kept
end

-- an item's height as it was last shown, or else the average of those shown, or else a line of text with room around it
local function itemHeight(kept, index)
  return kept.heights[index] or (kept.known > 0 and kept.total / kept.known) or (lineHeight(fontOf({}), fontSize()) + 2 * size.paddingY)
end

local function keepItemHeight(kept, index, height)
  local old = kept.heights[index]
  kept.known, kept.total = kept.known + (old and 0 or 1), kept.total - (old or 0) + height
  kept.heights[index] = height
end

-- the top of an item in its list's content, and its height, from the heights the list keeps
local function itemSpan(kept, index, padding)
  local top = padding

  for i = 1, index - 1 do
    top = top + itemHeight(kept, i) + kept.gap
  end

  return top, itemHeight(kept, index)
end

-- the lines of a row with wrap = true: each line takes the children that fit its width after the ones before, and a child wider than the width
-- has a line of its own; each line is { first, last, height }, and the row is as wide as its widest line
local function measureWrapped(node, state, rowRoom)
  local gap = node.gap or 0
  local lines, widest, height = {}, 0, 0
  local line = nil

  for i, child in ipairs(node.children) do
    local childWidth, childHeight = measure(child, state, nil)

    if line and line.width + gap + childWidth > rowRoom then
      line = nil
    end

    if not line then
      line = { first = i, width = -gap, height = 0 }
      lines[#lines + 1] = line
    end

    line.last, line.width, line.height = i, line.width + gap + childWidth, math.max(line.height, childHeight)
  end

  for i, each in ipairs(lines) do
    widest, height = math.max(widest, each.width), height + each.height + (i > 1 and gap or 0)
  end

  node.layout.lines = lines
  return widest, height
end

-- the width that an element's children, or its wrapped text, take: its own; but an element of width 0 that grows starts from no width, and its
-- share of a row is the room it is measured in again (measureRowWidths)
local function innerWidthOf(node, room)
  local grows = node.grow == true or (node.grow or 0) > 0
  return (node.width == 0 and grows and room) or node.width or room
end

-- the size of a container's children, along and across its direction
local function measureChildren(node, state, room)
  local along, across = 0, 0
  local width = innerWidthOf(node, room)

  -- a row with wrap = true in a column breaks its children into lines at the column's width
  if horizontal(node) and node.wrap and width then
    return measureWrapped(node, state, width - 2 * paddingOf(node))
  end

  -- a column's children may take its width less its padding, and a scroll element's less its scroll bar too; a row's take what they need
  local inner = nil

  if not horizontal(node) and width then
    inner = width - 2 * paddingOf(node) - (node.type == "scroll" and size.scrollbarWidth + 4 or 0)
  end

  for _, child in ipairs(node.children) do
    local childWidth, childHeight = measure(child, state, inner)

    if horizontal(node) then
      along, across = along + childWidth, math.max(across, childHeight)
    else
      along, across = along + childHeight, math.max(across, childWidth)
    end
  end

  local gaps = math.max(0, #node.children - 1) * (node.gap or 0)
  local rowRoom = horizontal(node) and width and width - 2 * paddingOf(node) - gaps

  if rowRoom then
    along, across = measureRowWidths(node, state, rowRoom, along)
  end

  return along + gaps, across
end

-- the children of a row whose width is known get the widths that arrange gives them, and their wrapped text breaks at those widths:
-- the children that can be narrower, wrapped text and columns, share what the others leave when the row's children need more than its width,
-- each taking no more than it needs; then the growing ones among them get their part of what is left, as arrange shares it
function measureRowWidths(node, state, rowRoom, along)
  local fixed, open, widths = 0, {}, {}

  for _, child in ipairs(node.children) do
    if child.width == 0 and (child.grow == true or (child.grow or 0) > 0) then
      -- it starts from no width, and is measured again at its share
      widths[child] = 0
    elseif not child.width and ((child.type == "text" and child.wrap) or (CONTAINERS[child.type] and not horizontal(child))) then
      open[#open + 1] = child
      widths[child] = child.layout.naturalWidth
    else
      fixed += child.layout.naturalWidth
    end
  end

  if along > rowRoom then
    local left = rowRoom - fixed
    local narrowed = true

    -- a child that needs less than an even share keeps what it needs, and the others share the rest
    while narrowed and #open > 0 do
      narrowed = false

      for i = #open, 1, -1 do
        if open[i].layout.naturalWidth <= left / #open then
          left -= open[i].layout.naturalWidth
          table.remove(open, i)
          narrowed = true
        end
      end
    end

    for _, child in ipairs(open) do
      widths[child] = math.max(left / #open, 1)
    end
  end

  local used, grows = 0, 0

  for _, child in ipairs(node.children) do
    used += (widths[child] or child.layout.naturalWidth)
    grows += (child.grow == true and 1 or child.grow or 0)
  end

  local across = 0
  along = 0

  for _, child in ipairs(node.children) do
    local width = widths[child]
    local grow = child.grow == true and 1 or child.grow or 0

    if width and grows > 0 and grow > 0 and rowRoom > used then
      width += (rowRoom - used) * grow / grows
    end

    if width and width ~= child.layout.naturalWidth then
      measure(child, state, width)
    end

    along, across = along + child.layout.naturalWidth, math.max(across, child.layout.naturalHeight)
  end

  return along, across
end

-- the size an element needs, without growing, which it keeps in its layout; containers measure their children first
-- room is the width a column gives its children, which text that wraps breaks at; nil in a row
function measure(node, state, room)
  local layout = node.layout

  -- a row measures a child again at its width; measured at the same room, the child keeps what it measured, and its subtree is not measured again
  if layout and layout.measured and layout.room == room then
    return layout.naturalWidth, layout.naturalHeight
  elseif layout then
    clearTable(layout)
  else
    layout = spare.take(spare.layouts) or {}
    node.layout = layout
  end

  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local line = lineHeight(font, textSize)
  local boxed = line + 2 * size.paddingY
  local width, height

  if node.type == "text" and node.spans then
    layout.spanLines = layoutSpans(node, textSize, node.wrap and (node.width or room) or nil)
    width, height = 0, #layout.spanLines * line

    for _, spanLine in ipairs(layout.spanLines) do
      width = math.max(width, spanLine.width)
    end
  elseif node.type == "text" and node.wrap and innerWidthOf(node, room) then
    layout.lines, width = wrapKept(font, node.text, textSize, innerWidthOf(node, room))
    height = #layout.lines * line
  elseif node.type == "text" and node.text:find("\n") then
    -- a text of many lines without wrap keeps its own lines, each cut with an ellipsis when it is drawn
    layout.lines = {}
    width = 0

    for text in (node.text .. "\n"):gmatch("(.-)\n") do
      layout.lines[#layout.lines + 1] = text
      width = math.max(width, measureText(font, text, textSize))
    end

    height = #layout.lines * line
  elseif node.type == "text" then
    width, height = measureText(font, node.text, textSize), line
  elseif node.type == "button" and node.spans then
    layout.spanLines = layoutSpans(node, textSize, nil)
    width, height = layout.spanLines[1].width + 2 * size.paddingX, boxed
  elseif node.type == "button" then
    width, height = measureText(font, node.label, textSize) + 2 * size.paddingX, boxed
  elseif node.type == "color" then
    width, height = 2 * boxed, boxed
  elseif node.type == "dropdown" then
    local widest = 0

    for _, choice in ipairs(node.choices) do
      widest = math.max(widest, measureText(font, tostring(choice), textSize))
    end

    width, height = widest + 3 * size.paddingX + line / 2, boxed
  elseif node.type == "selector" then
    width, height = 0, boxed

    for _, choiceWidth in ipairs(choiceWidths(node, font, textSize)) do
      width += choiceWidth
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
  elseif node.type == "graph" then
    width, height = 0, GRAPH_HEIGHT
  elseif node.type == "space" then
    width, height = 0, 0
  elseif node.type == "icon" then
    width, height = line, line
  elseif node.type == "disclosure" then
    width, height = size.checkSize, line
  else
    -- the spaces of a long list's items that are not built are as tall as those items were when they were last shown
    local kept = node.items and listKept(state, node.id)

    if kept then
      kept.gap = node.gap or 0

      if node.spaceBefore or node.spaceAfter then
        state.longLists = state.longLists or {}
        state.longLists[#state.longLists + 1] = node
      end

      if node.spaceBefore then
        node.spaceBefore.height = itemSpan(kept, node.first, 0) - kept.gap
      end

      if node.spaceAfter then
        local top, height = itemSpan(kept, node.count, 0)
        node.spaceAfter.height = top + height - itemSpan(kept, node.last + 1, 0)
      end
    end

    local along, across = measureChildren(node, state, room)

    for index, item in pairs(kept and node.items or {}) do
      keepItemHeight(kept, index, item.layout.naturalHeight)
    end
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
  layout.measured, layout.room = true, room
  return layout.naturalWidth, layout.naturalHeight
end

local arrangeLine

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

    innerY -= layout.offset
    innerHeight = math.max(layout.contentHeight - 2 * padding, innerHeight)
  end

  -- a wrapped row places each of its lines as a row of its own, below the line before
  if node.layout.lines then
    local lineY = innerY

    for _, line in ipairs(node.layout.lines) do
      arrangeLine(node, line, innerX, lineY, innerWidth, state)
      lineY = lineY + line.height + gap
    end

    return
  end

  -- the main axis: the natural sizes, then the room that is left, shared by the growing children
  local used, grows = 0, 0

  for i, child in ipairs(node.children) do
    used = used + (row and child.layout.naturalWidth or child.layout.naturalHeight) + (i > 1 and gap or 0)
    grows += (child.grow == true and 1 or child.grow or 0)
  end

  local room = (row and innerWidth or innerHeight) - used
  local along = row and innerX or innerY

  for _, child in ipairs(node.children) do
    local grow = child.grow == true and 1 or child.grow or 0
    local childSize = row and child.layout.naturalWidth or child.layout.naturalHeight

    if grows > 0 and grow > 0 and node.type ~= "scroll" then
      childSize = math.max(0, childSize + room * grow / grows)
    end

    -- the cross axis: the whole room, or the natural size aligned in it; an element without a fixed size across is no larger than the room,
    -- so a text that does not fit is cut with an ellipsis rather than drawn over its neighbours
    local crossRoom = row and innerHeight or innerWidth
    local cross = row and child.layout.naturalHeight or child.layout.naturalWidth
    local crossAt = row and innerY or innerX
    local fixed = row and child.height or not row and child.width

    if stretches(child, row) and not fixed then
      cross = crossRoom
    else
      if not fixed then
        cross = math.min(cross, math.max(0, crossRoom))
      end

      local align = node.align or (row and "center" or "start")
      crossAt += (align == "center" and (crossRoom - cross) / 2 or align == "end" and crossRoom - cross or 0)
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

-- places the children of one line of a wrapped row, from its left: the room the line leaves is shared by its growing children, and each child
-- is aligned in the line's height by the row's align
function arrangeLine(node, line, x, y, width, state)
  local gap, grows = node.gap or 0, 0

  for i = line.first, line.last do
    local grow = node.children[i].grow
    grows += (grow == true and 1 or grow or 0)
  end

  local room, along = width - line.width, x

  for i = line.first, line.last do
    local child = node.children[i]
    local grow = child.grow == true and 1 or child.grow or 0
    local childWidth = child.layout.naturalWidth + (grows > 0 and room > 0 and room * grow / grows or 0)
    local cross, crossAt = child.layout.naturalHeight, y

    if stretches(child, true) and not child.height then
      cross = line.height
    else
      local align = node.align or "center"
      crossAt = y + (align == "center" and (line.height - cross) / 2 or align == "end" and line.height - cross or 0)
    end

    child.layout.vertical = child.vertical == nil or child.vertical
    arrange(child, along, crossAt, childWidth, cross, state)
    along = along + childWidth + gap
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
    total += width
  end

  local rects, x = {}, node.layout.x

  for i, width in ipairs(widths) do
    local shared = node.look == "tabs" and width or total > 0 and width * node.layout.width / total or 0
    rects[i] = { x = x, width = shared }
    x += shared
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
  triangle(surface, x - half, y - half / 2, x + half, y - half / 2, x, y + half / 2, color)
end

local function paintSelector(node, surface, state, hover)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local textY = box.y + (box.height - lineHeight(font, textSize)) / 2

  draw.rounded(surface, box.x, box.y, box.width, box.height, size.radius, "button")

  for i, rect in ipairs(choiceRects(node)) do
    local choice = node.choices[i]
    local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.width

    if choice == node.value or over then
      draw.rounded(surface, rect.x, box.y, rect.width, box.height, size.radius, choice == node.value and "selected" or "buttonHover")
    end

    local shown = ellipsize(font, tostring(choice), textSize, rect.width - 2 * size.paddingX)
    local width = measureText(font, shown, textSize)
    draw.text(surface, font, shown, rect.x + (rect.width - width) / 2, textY, textSize, node.color or "text")
  end
end

-- what paintTabs clips: a line under the whole row, and a thicker one in the accent colour under the chosen tab
function inside.tabs()
  local node, surface, state = inside.arguments()
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local textY = box.y + (box.height - lineHeight(font, textSize)) / 2
  local hover = state.hover == node
  draw.fill(surface, box.x, box.y + box.height - 1, box.width, 1, "frame")

  for i, rect in ipairs(choiceRects(node)) do
    local choice = node.choices[i]
    local over = hover and (state.pointerX or 0) >= rect.x and (state.pointerX or 0) < rect.x + rect.width

    if over and choice ~= node.value then
      draw.rounded(surface, rect.x, box.y, rect.width, box.height - 1, size.radius, "hover")
    end

    if choice == node.value then
      draw.fill(surface, rect.x, box.y + box.height - 3, rect.width, 3, "accent")
    end

    local shown = ellipsize(font, tostring(choice), textSize, rect.width - 2 * size.paddingX)
    draw.text(surface, font, shown, rect.x + size.paddingX, textY, textSize, choice == node.value and (node.color or "text") or "textDim")
  end
end

-- tabs past the element's width are cut at its edge
local function paintTabs(node, surface, state)
  local box = node.layout
  inside.clip(surface, box.x, box.y, box.width, box.height, inside.tabs, node, state)
end

local function paintToggle(node, surface, hover)
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local check = size.checkSize
  local checkY = box.y + (box.height - check) / 2
  local labelX = box.x + check

  if node.look == "switch" then
    draw.rounded(surface, box.x, checkY, 2 * check, check, check / 2, node.on and "accent" or "field")
    draw.roundedOutline(surface, box.x, checkY, 2 * check, check, check / 2, 1, hover and "accent" or "frame")
    draw.circle(surface, box.x + (node.on and check or 0) + check / 2, checkY + check / 2, check / 2 - 3, 0, "text")
    labelX = box.x + 2 * check
  else
    draw.rounded(surface, box.x, checkY, check, check, size.radius, node.on and "accent" or "field")
    draw.roundedOutline(surface, box.x, checkY, check, check, size.radius, 1, hover and "accent" or "frame")

    -- a check mark, in the colour of the box when off, so it reads on the accent in both themes; it is triangles, which a show records
    if node.on then
      local half = math.max(0.75, check / 16)
      local x1, y1 = box.x + check * 0.22, checkY + check * 0.5
      local x2, y2 = box.x + check * 0.42, checkY + check * 0.72
      local x3, y3 = box.x + check * 0.8, checkY + check * 0.28

      -- each stroke is two triangles, offset by half its thickness across it
      local length = math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2)
      local nx, ny = -(y2 - y1) / length * half, (x2 - x1) / length * half
      triangle(surface, x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, "field")
      triangle(surface, x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, "field")
      length = math.sqrt((x3 - x2) ^ 2 + (y3 - y2) ^ 2)
      nx, ny = -(y3 - y2) / length * half, (x3 - x2) / length * half
      triangle(surface, x2 + nx, y2 + ny, x3 + nx, y3 + ny, x3 - nx, y3 - ny, "field")
      triangle(surface, x2 + nx, y2 + ny, x3 - nx, y3 - ny, x2 - nx, y2 - ny, "field")
    end
  end

  if node.label then
    local textY = box.y + (box.height - lineHeight(font, textSize)) / 2
    draw.text(surface, font, ellipsize(font, node.label, textSize, box.x + box.width - labelX - size.paddingX), labelX + size.paddingX, textY, textSize, node.color or "text")
  end
end

-- what paintTyped clips: the edited one's text, selection and cursor, a key element's request for a key, or the text or placeholder
function inside.typed()
  local node, surface, state = inside.arguments()
  local box = node.layout
  local font, textSize = fontOf(node), node.size or fontSize()
  local line = lineHeight(font, textSize)
  local textY = box.y + (box.height - line) / 2
  local editing = state.editing == node.id
  local room = box.width - 2 * size.paddingX

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
    -- a dragged number shows the number the drag gives, before the panel has taken it
    local drag = state.numberDrag
    local shown = drag and drag.id == node.id and drag.shown and formatNumber(drag.shown) or node.text
    -- an empty field shows its placeholder, dimmed, until it is typed in
    if shown == "" and node.placeholder then
      draw.text(surface, font, ellipsize(font, tostring(node.placeholder), textSize, room), box.x + size.paddingX, textY, textSize, "textDim")
    else
      draw.text(surface, font, ellipsize(font, shown, textSize, room), box.x + size.paddingX, textY, textSize, node.color or "text")
    end
  end
end

-- a field or a key element; the edited one shows what is typed, its selection and its cursor, and a key element asks for a key
local function paintTyped(node, surface, state)
  local box = node.layout
  local editing = state.editing == node.id
  draw.rounded(surface, box.x, box.y, box.width, box.height, size.radius, "field")

  inside.clip(surface, box.x + 1, box.y + 1, box.width - 2, box.height - 2, inside.typed, node, state)

  -- a number field marks a typed text that is not a number
  local valid = node.valid ~= false and not (editing and node.isNumber and readNumber(state.typed) == nil)
  draw.roundedOutline(surface, box.x, box.y, box.width, box.height, size.radius, 1, not valid and "error" or editing and "accent" or "frame")
end

-- down when open, right when closed, and up with up
-- the sum of a graph's value: a number, or the parts of a stacked bar
local function graphTotal(value)
  if type(value) == "number" then
    return value
  end

  local total = 0

  for _, part in ipairs(value) do
    total += part
  end

  return total
end

-- a graph's bars fill its width, the oldest value on the left, each part stacked on the one before it in its colour; a line across it marks each
-- of its lines' values; what goes above the top of its scale is cut there
local function paintGraph(node, surface)
  local box, values = node.layout, node.values
  local top = node.max

  if not top then
    top = 0

    for _, value in ipairs(values) do
      top = math.max(top, graphTotal(value))
    end
  end

  if top <= 0 or #values == 0 then
    return
  end

  local bar, bottom = box.width / #values, box.y + box.height

  for i, value in ipairs(values) do
    local x, height = box.x + (i - 1) * bar, 0

    for j, part in ipairs(type(value) == "number" and { value } or value) do
      local partHeight = math.min(part / top * box.height, box.height - height)

      if partHeight > 0 then
        draw.fill(surface, x, bottom - height - partHeight, bar, partHeight, node.colors[(j - 1) % #node.colors + 1])
        height += partHeight
      end
    end
  end

  for _, line in ipairs(node.lines) do
    if line.value >= 0 and line.value <= top then
      draw.fill(surface, box.x, bottom - line.value / top * box.height, box.width, 1, line.color or "textDim")
    end
  end
end

-- an icon, drawn from strokes, curves, a circle and a rounded box, which a show records; the main chunk is at Lua's 200 locals, so it is a
-- function of ui's draw, and its helpers stay inside the block
do
  -- a stroke between two points, as two triangles offset by half its thickness across it
  local function stroke(surface, x1, y1, x2, y2, half, color)
    local length = math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2)

    if length == 0 then
      return
    end

    local nx, ny = -(y2 - y1) / length * half, (x2 - x1) / length * half
    triangle(surface, x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, color)
    triangle(surface, x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, color)
  end

  -- strokes through the points of a curve, given as a function from 0 to 1
  local function curve(surface, at, half, color)
    local steps = 10
    local x1, y1 = at(0)

    for i = 1, steps do
      local x2, y2 = at(i / steps)
      stroke(surface, x1, y1, x2, y2, half, color)
      x1, y1 = x2, y2
    end
  end

  -- an icon by its name, in a square of side s whose top left is at x, y
  function draw.icon(surface, name, x, y, s, color)
    local cx, top = x + s / 2, y
    local half = math.max(0.6, s / 16)

    if name == "eye" or name == "eyeClosed" then
      local cy, across = top + s * 0.5, s * 0.42

      if name == "eye" then
        -- the lids meet at the corners, and the pupil is between them
        curve(surface, function(t)
          local u = 2 * t - 1
          return cx + u * across, cy - s * 0.24 * (1 - u * u)
        end, half, color)
        curve(surface, function(t)
          local u = 2 * t - 1
          return cx + u * across, cy + s * 0.24 * (1 - u * u)
        end, half, color)
        draw.circle(surface, cx, cy, s * 0.12, 0, color)
      else
        -- a shut lid, with its lashes below it
        local function lid(u)
          return cx + u * across, cy + s * 0.14 * (1 - u * u)
        end

        curve(surface, function(t)
          return lid(2 * t - 1)
        end, half, color)

        for _, u in ipairs({ -0.55, 0, 0.55 }) do
          local x, y = lid(u)
          stroke(surface, x, y, x + u * s * 0.12, y + s * 0.16, half, color)
        end
      end
    else
      -- the body, and the shackle's arch over it: an open padlock's shackle is lifted, and only its left leg reaches the body
      local bodyTop, radius = top + s * 0.48, s * 0.2
      draw.rounded(surface, cx - s * 0.32, bodyTop, s * 0.64, s * 0.42, s * 0.08, color)
      local archY = name == "lock" and top + s * 0.3 or top + s * 0.2
      curve(surface, function(t)
        local angle = math.pi + t * math.pi
        return cx + math.cos(angle) * radius, archY + math.sin(angle) * radius
      end, half, color)
      stroke(surface, cx - radius, archY, cx - radius, bodyTop, half, color)
      stroke(surface, cx + radius, archY, cx + radius, name == "lock" and bodyTop or archY + s * 0.08, half, color)
    end
  end
end

local function paintDisclosure(node, surface)
  local box = node.layout
  local half, x, y = size.checkSize / 4, box.x + box.width / 2, box.y + box.height / 2

  if node.up then
    triangle(surface, x - half, y + half / 2, x + half, y + half / 2, x, y - half / 2, "textDim")
  elseif node.open then
    arrow(surface, x, y, half, "textDim")
  else
    triangle(surface, x - half / 2, y - half, x - half / 2, y + half, x + half / 2, y, "textDim")
  end
end

local paint

-- what paintScroll clips: the children in view
function inside.scroll()
  local node, surface, state = inside.arguments()
  local box = node.layout
  local outer = recordScroll
  recordScroll = recording and { id = node.id, outer = outer }

  for _, child in ipairs(node.children) do
    if child.layout.y + child.layout.height > box.y and child.layout.y < box.y + box.height then
      paint(child, surface, state)
    end
  end

  recordScroll = outer
end

-- the children of a scroll element draw inside it only, and its scroll bar over them
local function paintScroll(node, surface, state)
  local box = node.layout
  inside.clip(surface, box.x, box.y, box.width, box.height, inside.scroll, node, state)

  if overflows(node) then
    -- no track: a pill in the bar's width, as editors draw it
    local start, length = thumb(node)
    local x = box.x + box.width - size.scrollbarWidth
    local width = size.scrollbarWidth - 2
    local over = state.hover == node and (state.pointerX or 0) >= x
    local color = state.dragging == node.id and "accent" or over and "buttonHover" or "button"
    draw.rounded(surface, x + 1, start + 1, width, length - 2, width / 2, color)
  end
end

-- the spans of a text that have a link, in their order; Tab stops at each
local function linkSpans(node)
  local spans, seen = {}, {}

  for _, spanLine in ipairs(node.layout.spanLines or {}) do
    for _, run in ipairs(spanLine.runs) do
      local span = node.spans[run.index]

      if span.link and not seen[span] then
        seen[span] = true
        spans[#spans + 1] = span
      end
    end
  end

  return spans
end

-- what a text of spans clips: its runs, line after line
function inside.spans()
  local node, surface = inside.arguments()
  inside.runs(node, surface, node.layout.x, node.layout.y)
end

-- what a button whose label is spans clips: its label in the middle, or from its padding when it is wider
function inside.label()
  local node, surface = inside.arguments()
  local box = node.layout
  local line = lineHeight(fontOf(node), node.size or fontSize())
  local x = box.x + math.max(size.paddingX, (box.width - box.spanLines[1].width) / 2)
  inside.runs(node, surface, x, box.y + (box.height - line) / 2)
end

-- the runs of a text of spans, or of a button's label, from x, y: words, and icons and images a line high
function inside.runs(node, surface, x, y)
  local textSize = node.size or fontSize()
  local line = lineHeight(fontOf(node), textSize)

  for i, spanLine in ipairs(node.layout.spanLines) do
    for _, run in ipairs(spanLine.runs) do
      local span = node.spans[run.index]
      local color = span.color or (span.link and "accent") or node.color or "text"
      local runX, runY = x + run.x, y + (i - 1) * line

      if span.icon then
        draw.icon(surface, span.icon, runX, runY, run.width, color)
      elseif span.image then
        local picture = loadImageCached(span.image, textScale)

        if picture then
          local imageWidth, imageHeight = draw.imageSize(picture)
          local fit = math.min(run.width / imageWidth, line / imageHeight)
          draw.image(surface, picture, runX + (run.width - imageWidth * fit) / 2, runY + (line - imageHeight * fit) / 2, imageWidth * fit, imageHeight * fit)
        end
      else
        draw.text(surface, spanFont(node, span), run.text, runX, runY, textSize, color)
      end
    end
  end
end

-- the corners of an element's background, border and ring: those of controls, but an element as wide as its surface, such as a bar or a view's
-- root, meets the panel's edges and stays square
local function cornerOf(node, state)
  return (state.root and node.layout.width >= state.root.layout.width) and 0 or size.radius
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

  local corner = cornerOf(node, state)

  if background then
    draw.rounded(surface, box.x, box.y, box.width, box.height, corner, background)
  end

  if node.border then
    draw.roundedOutline(surface, box.x, box.y, box.width, box.height, corner, 1, node.border)
  end

  -- the element that Tab gave the keys shows a ring, which fields and key elements show as their outline; a list shows it on its chosen item
  if state.showRing and state.keyTarget == node.id and node.type == "text" and box.spanLines then
    -- a text shows it on the link that Tab stopped at
    local span = linkSpans(node)[state.keyLink or 1]

    for i, spanLine in ipairs(box.spanLines) do
      for _, run in ipairs(spanLine.runs) do
        if node.spans[run.index] == span then
          draw.roundedOutline(surface, box.x + run.x - 1, box.y + (i - 1) * line - 1, run.width + 2, line + 2, size.radius, 1, "accent")
        end
      end
    end
  elseif state.showRing and state.keyTarget == node.id and not TAKES_TYPING[node.type] and not (node.items and node.items[node.selected]) then
    draw.roundedOutline(surface, box.x - 2, box.y - 2, box.width + 4, box.height + 4, corner > 0 and corner + 2 or 0, 2, "accent")
  elseif state.showRing and state.keyPart == node.id then
    -- inside the part, as the list's edge would cut a ring around one at the end of a row
    draw.roundedOutline(surface, box.x, box.y, box.width, box.height, corner, 2, "accent")
  elseif state.showRing and not state.keyPart and node.list and state.keyTarget == node.list.id and node.itemIndex == node.list.selected then
    draw.roundedOutline(surface, box.x, box.y, box.width, box.height, corner, 2, "accent")
  end

  if node.type == "text" and box.spanLines then
    -- runs past the box are cut at its edge
    inside.clip(surface, box.x, box.y, box.width, box.height, inside.spans, node, state)
  elseif node.type == "text" and box.lines then
    for i, text in ipairs(box.lines) do
      draw.text(surface, font, ellipsize(font, text, textSize, box.width), box.x, box.y + (i - 1) * line, textSize, node.color or "text")
    end
  elseif node.type == "text" then
    draw.text(surface, font, ellipsize(font, node.text, textSize, box.width), box.x, textY, textSize, node.color or "text")
  elseif node.type == "button" then
    draw.rounded(surface, box.x, box.y, box.width, box.height, size.radius, pressed and "accent" or hover and "buttonHover" or "button")

    if box.spanLines then
      -- a label of spans, in the middle, cut at the button's padding
      inside.clip(surface, box.x + size.paddingX, box.y, box.width - 2 * size.paddingX, box.height, inside.label, node, state)
    else
      local shown = ellipsize(font, node.label, textSize, box.width - 2 * size.paddingX)
      local width = measureText(font, shown, textSize)
      draw.text(surface, font, shown, box.x + (box.width - width) / 2, textY, textSize, node.color or "text")
    end
  elseif node.type == "color" then
    -- the swatch shows a colour with alpha over a light and a dark half, so its alpha shows
    draw.rounded(surface, box.x, box.y, box.width, box.height, size.radius, (pressed or hover) and "buttonHover" or "button")
    local inset = size.paddingY
    local width, height = box.width - 2 * inset, box.height - 2 * inset
    draw.fill(surface, box.x + inset, box.y + inset, width / 2, height, "#FFFFFF")
    draw.fill(surface, box.x + inset + width / 2, box.y + inset, width / 2, height, "#000000")
    draw.fill(surface, box.x + inset, box.y + inset, width, height, node.value)
    draw.outline(surface, box.x + inset, box.y + inset, width, height, 1, "textDim")
  elseif node.type == "dropdown" then
    draw.rounded(surface, box.x, box.y, box.width, box.height, size.radius, (pressed or hover) and "buttonHover" or "button")
    local room = box.width - 3 * size.paddingX - line / 2
    draw.text(surface, font, ellipsize(font, tostring(node.value or ""), textSize, room), box.x + size.paddingX, textY, textSize, node.color or "text")
    arrow(surface, box.x + box.width - size.paddingX - line / 4, box.y + box.height / 2, line / 4, "textDim")
  elseif node.type == "selector" and node.look == "tabs" then
    paintTabs(node, surface, state)
  elseif node.type == "selector" then
    paintSelector(node, surface, state, hover)
  elseif node.type == "toggle" then
    paintToggle(node, surface, hover)
  elseif node.type == "slider" then
    local radius = size.checkSize / 2
    local centerY = box.y + box.height / 2
    local span = node.max - node.min
    local value = state.sliding == node.id and state.slid or node.value
    local knobX = box.x + radius + (span > 0 and (value - node.min) / span or 0) * (box.width - 2 * radius)
    draw.rounded(surface, box.x + radius, centerY - 2, box.width - 2 * radius, 4, 2, "field")
    draw.rounded(surface, box.x + radius, centerY - 2, knobX - box.x - radius, 4, 2, "accent")
    draw.circle(surface, knobX, centerY, radius, 0, (hover or state.sliding == node.id) and "accent" or "text")
  elseif TAKES_TYPING[node.type] then
    paintTyped(node, surface, state)
  elseif node.type == "image" and box.picture then
    -- an image with a width or a height fits inside its box, keeping its shape, in the middle of it
    local width, height = box.width, box.height

    if node.width or node.height then
      local imageWidth, imageHeight = draw.imageSize(box.picture)
      local fit = math.min(box.width / imageWidth, box.height / imageHeight)
      width, height = imageWidth * fit, imageHeight * fit
    end

    draw.image(surface, box.picture, box.x + (box.width - width) / 2, box.y + (box.height - height) / 2, width, height)
  elseif node.type == "icon" then
    local side = math.min(box.width, box.height)
    draw.icon(surface, node.name, box.x + (box.width - side) / 2, box.y + (box.height - side) / 2, side, node.color or "text")
  elseif node.type == "disclosure" then
    paintDisclosure(node, surface)
  elseif node.type == "graph" then
    paintGraph(node, surface)
  elseif node.type == "separator" and box.vertical then
    draw.fill(surface, box.x + box.width / 2 - 0.5, box.y, 1, box.height, node.color or "frame")
  elseif node.type == "separator" then
    draw.fill(surface, box.x, box.y + box.height / 2 - 0.5, box.width, 1, node.color or "frame")
  elseif node.type == "scroll" then
    paintScroll(node, surface, state)
    return
  end

  for _, child in ipairs(node.children or {}) do
    paint(child, surface, state)
  end
end

-- the first element, in the order they are shown, that passes a test
local function firstWhere(node, test)
  if test(node) then
    return node
  end

  for _, child in ipairs(node.children or {}) do
    local found = firstWhere(child, test)

    if found then
      return found
    end
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
  return INTERACTIVE[node.type] == true or (CONTAINERS[node.type] == true and node.onClick ~= nil) or (node.type == "text" and node.onLink ~= nil)
end

-- the link of a text's span at a place, or of its first link without one
local function linkAt(node, x, y)
  local box = node.layout
  local lines = box.spanLines or {}
  local line = #lines > 0 and box.height / #lines or 0

  for i, spanLine in ipairs(lines) do
    for _, run in ipairs(spanLine.runs) do
      local inside = x and y and x >= box.x + run.x and x < box.x + run.x + run.width and y >= box.y + (i - 1) * line and y < box.y + i * line

      local link = node.spans[run.index].link

      if link and (inside or not x) then
        return link
      end
    end
  end
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

-- the text that a key adds to a dropdown's search: a key of one character, or Space; Shift gives - as _
local function searchCharacter(event)
  if event.key == "Space" then
    return " "
  elseif #event.key == 1 and not event.ctrl and not event.alt and not event.super then
    return event.shift and event.key == "-" and "_" or event.key:lower()
  end
end

-- a small triangle that points right, or left, centred at x, y: a folder's mark, and the row that goes back
local function sideArrow(surface, x, y, half, color, left)
  local tip, back = left and x - half / 2 or x + half / 2, left and x + half / 2 or x - half / 2
  triangle(surface, back, y - half, back, y + half, tip, y, color)
end

-- opens a dropdown's choices in a popup below it; a click or Return chooses one. Typing searches: the popup shows what is typed
-- in its first row, and below it only the choices that hold it, whatever their case; Backspace takes a character off. With
-- `folders`, a separator such as ".", the choices are paths: the popup shows one folder at a time, its folders first, which a
-- click, Return or Right opens in place, and a first row that goes back to the folder above, as Left and Backspace do; a search
-- looks in every folder, and shows the whole paths
local function openDropdown(panel, state, node)
  -- the popup outlives the element, which the panel's next show recycles, so it keeps a copy
  node = shallowCopy(node)
  node.layout = shallowCopy(node.layout)

  local all = node.choices

  if #all == 0 then
    return
  end

  local box = node.layout
  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local rowHeight = lineHeight(font, textSize) + 2 * size.paddingY
  local width = box.width
  local scale = state.scale
  local separator = type(node.folders) == "string" and node.folders ~= "" and node.folders or nil
  local hover = 1
  local typed = ""
  local folder = ""
  -- the rows below the search's: { label, choice } for a choice, { label, folder } for a folder, and { label, back = true }
  local rows = {}
  local popup

  -- the folder that holds a path, "" for the top, and the path's last part
  local function split(path)
    local at, last = path:find(separator, 1, true), nil

    while at do
      last = at
      at = path:find(separator, at + 1, true)
    end

    if not last then
      return "", path
    end

    return path:sub(1, last - 1), path:sub(last + #separator)
  end

  -- the parts of a choice's path below the open folder, or nil for a choice outside it
  local function below(choice)
    local text = tostring(choice)

    if folder == "" then
      return text
    end

    local start = folder .. separator
    return text:sub(1, #start) == start and text:sub(#start + 1) or nil
  end

  local function fill()
    rows = {}

    if typed ~= "" or not separator then
      for _, choice in ipairs(all) do
        if matchesQuery(typed, choice) then
          rows[#rows + 1] = { label = tostring(choice), choice = choice }
        end
      end

      return
    end

    if folder ~= "" then
      local above, name = split(folder)
      rows[#rows + 1] = { label = name, back = true, folder = above }
    end

    local seen, leaves = {}, {}

    for _, choice in ipairs(all) do
      local rest = below(choice)

      if rest then
        local at = rest:find(separator, 1, true)

        if at then
          local name = rest:sub(1, at - 1)

          if not seen[name] then
            seen[name] = true
            rows[#rows + 1] = { label = name, folder = folder == "" and name or folder .. separator .. name }
          end
        else
          leaves[#leaves + 1] = { label = rest, choice = choice }
        end
      end
    end

    for _, leaf in ipairs(leaves) do
      rows[#rows + 1] = leaf
    end
  end

  -- the rows above the choices: one for what is typed, while something is
  local function top()
    return typed ~= "" and 1 or 0
  end

  local function height()
    return rowHeight * math.max(1, #rows + top())
  end

  -- shows the rows again, hovering the first that is not the way back, or the given one
  local function refill(hovered)
    fill()
    hover = hovered or ((rows[1] and rows[1].back and rows[2]) and 2 or 1)
    popup:setSize(width, height())
    popup:redraw()
  end

  local function open(to)
    local from = folder
    folder = to
    fill()
    local hovered

    -- back in the folder above, the folder that was open is hovered
    for i, row in ipairs(rows) do
      if not row.back and row.folder == from then
        hovered = i
      end
    end

    refill(hovered)
  end

  local function choose(index)
    local row = rows[index]

    if row.choice == nil then
      open(row.folder)
      return
    end

    popup:close()
    call(node.onChange, row.choice)
    panel:redraw()
  end

  local function rowAt(y)
    local index = math.floor(y / scale / rowHeight) + 1 - top()
    return rows[index] ~= nil and index or nil
  end

  -- the chosen one's folder is open first, with it hovered
  if separator and node.value ~= nil then
    folder = split(tostring(node.value))
  end

  fill()

  for i, row in ipairs(rows) do
    if row.choice ~= nil and row.choice == node.value then
      hover = i
    end
  end

  local message
  popup, message = panel:openPopup({
    anchor = { x = box.x * state.scale, y = box.y * state.scale, width = box.width * state.scale, height = box.height * state.scale },
    width = width,
    height = height(),
    draw = function(surface)
      scale = surface.scale
      local h = height()
      local half = lineHeight(font, textSize) / 4
      draw.rounded(surface, 0, 0, width, h, size.radius + 1, "overlay")

      if typed ~= "" then
        draw.text(surface, font, ellipsize(font, typed, textSize, width - 2 * size.paddingX), size.paddingX, size.paddingY, textSize, "textDim")
      end

      for i, row in ipairs(rows) do
        local y = (i - 1 + top()) * rowHeight
        local x, room = size.paddingX, width - 2 * size.paddingX

        if i == hover then
          draw.rounded(surface, 2, y, width - 4, rowHeight, size.radius, "selected")
        end

        if row.back then
          sideArrow(surface, x + half, y + rowHeight / 2, half, "textDim", true)
          x, room = x + 2 * half + size.paddingX, room - 2 * half - size.paddingX
        elseif row.choice == nil then
          sideArrow(surface, width - size.paddingX - half, y + rowHeight / 2, half, "textDim")
          room = room - 2 * half - size.paddingX
        end

        local color = row.back and "textDim" or row.choice ~= nil and row.choice == node.value and "accent" or "text"
        draw.text(surface, font, ellipsize(font, row.label, textSize, room), x, y + size.paddingY, textSize, color)
      end

      draw.roundedOutline(surface, 0, 0, width, h, size.radius + 1, 1, "frame")
    end,
    event = function(event)
      if event.type == "pointerMove" then
        local index = rowAt(event.y)

        if index and index ~= hover then
          hover = index
          popup:redraw()
        end
      elseif event.type == "pointerUp" and event.button == 1 and rowAt(event.y) then
        choose(rowAt(event.y))
      elseif event.type == "keyDown" and (event.key == "Up" or event.key == "Down") then
        if #rows > 0 then
          hover = clamp(hover + (event.key == "Up" and -1 or 1), 1, #rows)
          popup:redraw()
        end
      elseif event.type == "keyDown" and isEnter(event) then
        if rows[hover] ~= nil then
          choose(hover)
        end
      elseif event.type == "keyDown" and event.key == "Right" and rows[hover] and rows[hover].choice == nil and not rows[hover].back then
        choose(hover)
      elseif event.type == "keyDown" and (event.key == "Left" or event.key == "Backspace") and typed == "" and separator and folder ~= "" then
        open(rows[1].folder)
      elseif event.type == "keyDown" and event.key == "Backspace" and typed ~= "" then
        typed = typed:sub(1, -2)
        refill()
      elseif event.type == "keyDown" and searchCharacter(event) then
        typed ..= searchCharacter(event)
        refill()
      end
    end,
  })

  if not popup then
    ecs.log.warn(message or "Cannot open the choices.")
  end
end

-- opens a menu of entries in a popup at a point of a panel, in layout units, such as where a right click was; an entry is
-- { label, keys, enabled, onClick }, { label, entries } for a submenu, or { line = true } for a line between entries; a click or
-- Return runs an entry, and closes the menu. A submenu opens in place, by a click, Return or Right, with a first row that goes back
-- to the menu above it, as Left and Backspace do
local function openMenu(panel, x, y, entries)
  local state = panelState(panel)
  local font = fontOf({})
  local textSize = fontSize()
  local rowHeight = lineHeight(font, textSize) + 2 * size.paddingY
  local lineGap = size.paddingY * 2 + 1
  local half = lineHeight(font, textSize) / 4
  local mark = 2 * half + size.paddingX
  -- the open menus, the first at the bottom: each the entries that it shows, with the way back first in a submenu
  local stack = {}
  local shown, tops, width, height
  local popup

  local function layout()
    shown = stack[#stack]
    tops, width, height = {}, 0, 0

    for i, entry in ipairs(shown) do
      tops[i] = height

      if entry.line then
        height += lineGap
      else
        local keys = entry.keys and measureText(font, entry.keys, textSize) + 3 * size.paddingX or 0
        local marks = (entry.entries or entry.back) and mark or 0
        width = math.max(width, measureText(font, entry.label or "", textSize) + keys + marks + 2 * size.paddingX)
        height += rowHeight
      end
    end
  end

  stack[1] = entries
  layout()

  if height == 0 then
    return nil
  end

  local function usable(index)
    local entry = shown[index]
    return entry ~= nil and not entry.line and entry.enabled ~= false
  end

  local function entryAt(at)
    for i = #shown, 1, -1 do
      if at >= tops[i] then
        return usable(i) and i or nil
      end
    end
  end

  -- the next usable entry from one, a step at a time, round from the last to the first and back, or nil when none is usable
  local function step(from, by)
    local index = from

    for _ = 1, #shown do
      index = (index + by - 1) % #shown + 1

      if usable(index) then
        return index
      end
    end

    return nil
  end

  local hover = step(0, 1)
  local scale = state.scale

  local function show(hovered)
    layout()
    hover = hovered or step(shown[1] and shown[1].back and 1 or 0, 1)
    popup:setSize(width, height)
    popup:redraw()
  end

  local function back()
    if #stack > 1 then
      local from = shown.from
      stack[#stack] = nil
      layout()
      local hovered

      for i, entry in ipairs(shown) do
        if entry == from then
          hovered = i
        end
      end

      show(hovered)
    end
  end

  local function run(index)
    local entry = shown[index]

    if entry.back then
      back()
      return
    elseif entry.entries then
      local inner = { from = entry, { label = entry.label, back = true } }

      for _, each in ipairs(entry.entries) do
        inner[#inner + 1] = each
      end

      stack[#stack + 1] = inner
      show()
      return
    end

    popup:close()
    call(entry.onClick)
    panel:redraw()
  end

  local message
  popup, message = panel:openPopup({
    type = "menu",
    anchor = { x = x * state.scale, y = y * state.scale, width = 0, height = 0 },
    width = width,
    height = height,
    draw = function(surface)
      scale = surface.scale
      draw.rounded(surface, 0, 0, width, height, size.radius + 1, "overlay")

      for i, entry in ipairs(shown) do
        if entry.line then
          draw.fill(surface, size.paddingX, tops[i] + size.paddingY, width - 2 * size.paddingX, 1, "frame")
        else
          if i == hover then
            draw.rounded(surface, 2, tops[i], width - 4, rowHeight, size.radius, "selected")
          end

          local color = (entry.enabled == false or entry.back) and "textDim" or "text"
          local middle = tops[i] + rowHeight / 2

          if entry.back then
            sideArrow(surface, size.paddingX + half, middle, half, "textDim", true)
            draw.text(surface, font, entry.label or "", size.paddingX + mark, tops[i] + size.paddingY, textSize, color)
          else
            draw.text(surface, font, entry.label or "", size.paddingX, tops[i] + size.paddingY, textSize, color)
          end

          if entry.entries then
            sideArrow(surface, width - size.paddingX - half, middle, half, color)
          elseif entry.keys then
            local keysWidth = measureText(font, entry.keys, textSize)
            draw.text(surface, font, entry.keys, width - size.paddingX - keysWidth, tops[i] + size.paddingY, textSize, "textDim")
          end
        end
      end

      draw.roundedOutline(surface, 0, 0, width, height, size.radius + 1, 1, "frame")
    end,
    event = function(event)
      if event.type == "pointerMove" then
        local index = entryAt(event.y / scale)

        if index ~= hover then
          hover = index
          popup:redraw()
        end
      elseif event.type == "pointerUp" and event.button == 1 and entryAt(event.y / scale) then
        run(entryAt(event.y / scale))
      elseif event.type == "keyDown" and (event.key == "Up" or event.key == "Down") then
        hover = step(hover or 0, event.key == "Up" and -1 or 1) or hover
        popup:redraw()
      elseif event.type == "keyDown" and isEnter(event) and hover then
        run(hover)
      elseif event.type == "keyDown" and event.key == "Right" and hover and shown[hover].entries then
        run(hover)
      elseif event.type == "keyDown" and (event.key == "Left" or event.key == "Backspace") then
        back()
      end
    end,
  })

  if not popup then
    ecs.log.warn(message or "Cannot open the menu.")
  end

  return popup
end

-- a colour's text read as red, green, blue and alpha from 0 to 255, or nil for a text that is not "#RRGGBB" or "#RRGGBBAA"
local function readColor(text)
  local hex = type(text) == "string" and (text:match("^#(%x%x%x%x%x%x%x%x)$") or text:match("^#(%x%x%x%x%x%x)$"))

  if not hex then
    return nil
  end

  return tonumber(hex:sub(1, 2), 16), tonumber(hex:sub(3, 4), 16), tonumber(hex:sub(5, 6), 16), #hex == 8 and tonumber(hex:sub(7, 8), 16) or 255
end

-- red, green and blue from 0 to 1 of a hue, a saturation and a value, each from 0 to 1
local function hsvToRgb(hue, saturation, value)
  local sector = (hue % 1) * 6
  local whole = math.floor(sector)
  local low, falling, rising = value * (1 - saturation), value * (1 - saturation * (sector - whole)), value * (1 - saturation * (1 - sector + whole))
  local channels = ({
    { value, rising, low }, { falling, value, low }, { low, value, rising }, { low, falling, value }, { rising, low, value }, { value, low, falling },
  })[whole % 6 + 1]
  return channels[1], channels[2], channels[3]
end

-- a hue, a saturation and a value, each from 0 to 1, of red, green and blue from 0 to 1
local function rgbToHsv(red, green, blue)
  local high, low = math.max(red, green, blue), math.min(red, green, blue)
  local range = high - low
  local hue = 0

  if range > 0 and high == red then
    hue = ((green - blue) / range) % 6
  elseif range > 0 and high == green then
    hue = (blue - red) / range + 2
  elseif range > 0 then
    hue = (red - green) / range + 4
  end

  return hue / 6, high > 0 and range / high or 0, high
end

-- a picker's sizes, in layout units: the square of saturation and value, the bars of hue and alpha beside and below it, and the pixels of the
-- square's image, which draw stretches; how far an arrow key moves in the square or a bar, of its length; and the arrow keys' directions,
-- down being positive, as in the surface
local PICKER = { square = 160, bar = 16, gap = 8, pixels = 64, step = 1 / 32, arrows = { Left = { -1, 0 }, Right = { 1, 0 }, Up = { 0, -1 }, Down = { 0, 1 } } }

-- the image of a picker's square at a hue: saturation grows to the right and value upwards
local function squareImage(hue)
  local pixels = ffi.new("uint32_t[?]", PICKER.pixels * PICKER.pixels)

  for y = 0, PICKER.pixels - 1 do
    for x = 0, PICKER.pixels - 1 do
      local red, green, blue = hsvToRgb(hue, x / (PICKER.pixels - 1), 1 - y / (PICKER.pixels - 1))
      pixels[y * PICKER.pixels + x] = 0xFF000000 | math.floor(red * 255 + 0.5) << 16 | math.floor(green * 255 + 0.5) << 8 | math.floor(blue * 255 + 0.5)
    end
  end

  return draw.newImage(PICKER.pixels, PICKER.pixels, pixels)
end

-- the image of the hue bar, from red at the top round to red at the bottom
local function hueImage()
  local pixels = ffi.new("uint32_t[?]", PICKER.pixels)

  for y = 0, PICKER.pixels - 1 do
    local red, green, blue = hsvToRgb(y / PICKER.pixels, 1, 1)
    pixels[y] = 0xFF000000 | math.floor(red * 255 + 0.5) << 16 | math.floor(green * 255 + 0.5) << 8 | math.floor(blue * 255 + 0.5)
  end

  return draw.newImage(1, PICKER.pixels, pixels)
end

-- opens a colour element's picker in a popup below it: a square of saturation and value, a hue bar beside it, and an alpha bar below it with
-- alpha = true; a press and a drag change the colour, which the popup shows, and the colour a press ends on goes to onChange. The arrows move
-- saturation and value, and with Shift, Up and Down the hue and Left and Right the alpha; each goes to onChange. Return closes it
local function openColorPicker(panel, state, node)
  -- the popup outlives the element, which the panel's next show recycles, so it keeps a copy
  node = shallowCopy(node)
  node.layout = shallowCopy(node.layout)

  local red, green, blue, alpha = readColor(node.value)
  red, green, blue, alpha = red or 0, green or 0, blue or 0, alpha or 255
  local hue, saturation, value = rgbToHsv(red / 255, green / 255, blue / 255)
  alpha = node.alpha and alpha or 255
  local width = PICKER.square + PICKER.gap + PICKER.bar + 2 * PICKER.gap
  local textSize = node.size or fontSize()
  local font = fontOf(node)
  local line = lineHeight(font, textSize)
  local height = PICKER.square + 3 * PICKER.gap + line + (node.alpha and PICKER.bar + PICKER.gap or 0)
  local box, scale = node.layout, state.scale
  local square, hues, squareHue, dragging = nil, hueImage(), nil, nil
  local popup

  local function text()
    local r, g, b = hsvToRgb(hue, saturation, value)
    local color = ("#%02X%02X%02X"):format(math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5))
    return node.alpha and color .. ("%02X"):format(alpha) or color
  end

  -- a press chooses the part it is on: the square, the hue bar or the alpha bar; the press and a drag after it change that part
  local function pick(pixelX, pixelY)
    local x, y = pixelX / scale - PICKER.gap, pixelY / scale - PICKER.gap
    local barX = PICKER.square + PICKER.gap

    if not dragging and x >= 0 and x < PICKER.square and y >= 0 and y < PICKER.square then
      dragging = "square"
    elseif not dragging and x >= barX and y >= 0 and y < PICKER.square then
      dragging = "hue"
    elseif not dragging and node.alpha and y >= barX then
      dragging = "alpha"
    end

    if dragging == "square" then
      saturation, value = clamp(x / PICKER.square, 0, 1), clamp(1 - y / PICKER.square, 0, 1)
    elseif dragging == "hue" then
      hue = clamp(y / PICKER.square, 0, 0.9999)
    elseif dragging == "alpha" then
      alpha = math.floor(clamp(x / (barX + PICKER.bar), 0, 1) * 255 + 0.5)
    end
  end

  local message
  popup, message = panel:openPopup({
    anchor = { x = box.x * scale, y = box.y * scale, width = box.width * scale, height = box.height * scale },
    width = width,
    height = height,
    draw = function(surface)
      scale = surface.scale
      draw.rounded(surface, 0, 0, width, height, size.radius + 1, "overlay")

      -- the square's image is made again when the hue changes
      if squareHue ~= hue then
        if square then
          draw.freeImage(square)
        end

        square, squareHue = squareImage(hue), hue
      end

      local barX = PICKER.gap + PICKER.square + PICKER.gap

      if square then
        draw.image(surface, square, PICKER.gap, PICKER.gap, PICKER.square, PICKER.square)
      end

      if hues then
        draw.image(surface, hues, barX, PICKER.gap, PICKER.bar, PICKER.square)
      end

      -- rings mark the chosen saturation and value, and lines the hue and the alpha
      draw.circle(surface, PICKER.gap + saturation * PICKER.square, PICKER.gap + (1 - value) * PICKER.square, 5, 2, value > 0.5 and "#000000" or "#FFFFFF")
      draw.fill(surface, barX - 2, PICKER.gap + hue * PICKER.square - 1, PICKER.bar + 4, 2, "text")

      if node.alpha then
        local barY, barWidth = PICKER.gap + PICKER.square + PICKER.gap, PICKER.square + PICKER.gap + PICKER.bar
        draw.fill(surface, PICKER.gap, barY, barWidth / 2, PICKER.bar, "#FFFFFF")
        draw.fill(surface, PICKER.gap + barWidth / 2, barY, barWidth / 2, PICKER.bar, "#000000")

        -- the bar fades the colour from transparent on its left to opaque on its right, in steps
        local r, g, b = hsvToRgb(hue, saturation, value)

        for step = 0, 31 do
          local color = ("#%02X%02X%02X%02X"):format(math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5), math.floor(step / 31 * 255 + 0.5))
          draw.fill(surface, PICKER.gap + step * barWidth / 32, barY, barWidth / 32 + 0.5, PICKER.bar, color)
        end

        draw.fill(surface, PICKER.gap + alpha / 255 * barWidth - 1, barY - 2, 2, PICKER.bar + 4, "text")
      end

      -- the colour's text, as onChange will get it
      draw.text(surface, font, text(), PICKER.gap, height - PICKER.gap - line, textSize, "text")
      draw.roundedOutline(surface, 0, 0, width, height, size.radius + 1, 1, "frame")
    end,
    event = function(event)
      if event.type == "pointerDown" and event.button == 1 then
        dragging = nil
        pick(event.x, event.y)
        popup:redraw()
      elseif event.type == "pointerMove" and dragging then
        pick(event.x, event.y)
        popup:redraw()
      elseif event.type == "pointerUp" and event.button == 1 and dragging then
        dragging = nil
        call(node.onChange, text())
        panel:redraw()
      elseif event.type == "keyDown" and isEnter(event) then
        popup:close()
      elseif event.type == "keyDown" and PICKER.arrows[event.key] then
        local dx, dy = PICKER.arrows[event.key][1] * PICKER.step, PICKER.arrows[event.key][2] * PICKER.step

        if event.shift and dy ~= 0 then
          hue = clamp(hue + dy, 0, 0.9999)
        elseif event.shift and node.alpha then
          alpha = math.floor(clamp(alpha / 255 + dx, 0, 1) * 255 + 0.5)
        elseif not event.shift then
          saturation, value = clamp(saturation + dx, 0, 1), clamp(value - dy, 0, 1)
        end

        call(node.onChange, text())
        popup:redraw()
        panel:redraw()
      end
    end,
    closed = function()
      if square then
        draw.freeImage(square)
      end

      if hues then
        draw.freeImage(hues)
      end
    end,
  })

  if not popup then
    ecs.log.warn(message or "Cannot open the colour picker.")
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

-- does what a click on an element does; x and y are where, for a selector and a text's links, and nil for a key
local function activate(panel, state, node, x, y)
  if node.type == "text" then
    -- a key gives the link that Tab stopped at
    local span = not x and linkSpans(node)[state.keyLink or 1]
    local link = span and span.link or linkAt(node, x, y)

    if link then
      call(node.onLink, link)
    end
  elseif node.type == "button" or CONTAINERS[node.type] then
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
  elseif node.type == "color" then
    openColorPicker(panel, state, node)
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
  local number = snapNumber(node, (readNumber(state.typed) or node.value) + (up and 1 or -1) * ((node.step and node.step > 0) and node.step or 1))
  state.typed = formatNumber(number)
  state.cursorPosition, state.anchor = #state.typed + 1, nil
  node.onChange(state.typed)
  node.onSubmit(state.typed)
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
local moveSelection -- chooses another item of a list; defined with the list's keys
local revealItem -- scrolls a list so that an item shows; defined with scrollIntoView

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
  elseif node.isSearch and node.results and (key == "Up" or key == "Down") then
    local results = findById(state.root, node.results)

    if results then
      moveSelection(state, results, key == "Up" and -1 or 1)
    end
  elseif key == "Escape" and node.isSearch and state.typed ~= "" then
    state.typed, state.cursorPosition, state.anchor = "", 1, nil
    edited = true
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

-- the elements that Tab goes through, in the order of the tree: the interactive ones, each list once, in place of its items and what is in
-- them, which Left and Right reach, and each scroll element with more than it shows, which the keys scroll
local function tabOrder(node, list)
  if isList(node) or scrollable(node) or (interactive(node) and not isListItem(node)) then
    list[#list + 1] = node
  end

  if isListItem(node) then
    return list
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

-- scrolls a list so that an item shows: one that is built as it is, and another by the heights the list keeps, and again at the next show,
-- where the item is built
function revealItem(state, list, index)
  if list.items[index] then
    scrollIntoView(state, list, list.items[index])
    return
  end

  local kept = listKept(state, list.id)
  local top, height = itemSpan(kept, index, paddingOf(list))
  local offset = state.scroll[list.id] or 0
  state.scroll[list.id] = math.min(math.max(offset, top + height - kept.view), top)
  state.revealed[list.id] = index
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
  state.keyLink, state.keyColumn = 1, 0

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
    call(node.onSubmit, value)
  end
end

-- chooses the item of a list a number of items away from the chosen one, and scrolls it into view
function moveSelection(state, list, step)
  if list.count == 0 then
    return
  end

  local index = clamp(list.selected and list.selected + step or (step > 0 and 1 or list.count), 1, list.count)
  call(list.onSelect, index)
  revealItem(state, list, index)
end

-- the parts of a list's chosen item that Left and Right go through: the interactive elements in it, in the order of the tree, such as a tree
-- node's arrow and marks
local function rowParts(list, parts)
  local function collect(node)
    for _, child in ipairs(node.children or {}) do
      if interactive(child) then
        parts[#parts + 1] = child
      end

      collect(child)
    end
  end

  local item = list.selected and list.items[list.selected]

  if item then
    collect(item)
  end

  return parts
end

-- the list that Up, Down and Return act on: the one that has the keys or holds the element that has them
local function keyList(state)
  local target = state.keyTarget and findById(state.root, state.keyTarget)

  if target and isList(target) then
    return target
  end

  return target and findAround(state.root, target.id, isList, nil)
end

-- what Return does on a list's chosen item: the list's onActivate, or else the item's onClick
local function activateItem(list)
  local item = list.selected and list.items[list.selected]

  if list.selected and list.onActivate then
    list.onActivate(list.selected)
    return true
  elseif item and item.onClick then
    item.onClick()
    return true
  end

  return false
end

-- scrolls the scroll element that has the keys, or holds the element that has them: the arrows by a line, Page Up and Page Down by a page less a line,
-- and Home and End to its ends; a list's arrows choose its items instead
local function scrollWithKey(state, node, key)
  local function isScroll(candidate)
    return candidate.type == "scroll"
  end

  local scroll = node and (isScroll(node) and node or findAround(state.root, node.id, isScroll, nil))

  if not scroll or not overflows(scroll) then
    return false
  end

  local line = lineHeight(fontOf(scroll), fontSize())
  local page = math.max(scroll.layout.height - line, line)
  local steps = { Up = -line, Down = line, PageUp = -page, PageDown = page, Home = -math.huge, End = math.huge }

  if not steps[key] then
    return false
  end

  local farthest = scroll.layout.contentHeight - scroll.layout.height
  state.scroll[scroll.id] = clamp((state.scroll[scroll.id] or 0) + steps[key], 0, farthest)
  return true
end

-- handles a key press when no field is edited: Tab moves the keys, Return or Space presses the element that has them, Left and Right change a
-- selector or a slider, and go through a text's links, Escape takes the keys away. In a list, Up and Down choose an item, Left and Right go
-- through the parts of the chosen one, Space presses the part, and Return activates the item
local function handleKeyDown(panel, state, event)
  if event.key == "Tab" then
    return moveKeyTarget(panel, state, event.shift)
  end

  local node = state.keyTarget and findById(state.root, state.keyTarget)
  local list = keyList(state)
  local enter = isEnter(event) or event.key == "Space"
  local leftOrRight = event.key == "Left" or event.key == "Right"
  local links = node and node.type == "text" and #linkSpans(node) or 0
  local parts = list and node == list and rowParts(list, {})
  local column = parts and math.min(state.keyColumn or 0, #parts)

  if event.key == "Escape" and node then
    state.keyTarget, state.showRing = nil, false
  elseif parts and leftOrRight and #parts > 0 then
    state.keyColumn, state.showRing = clamp(column + (event.key == "Right" and 1 or -1), 0, #parts), true
  elseif parts and event.key == "Space" then
    if column == 0 then
      return false
    end

    activate(panel, state, parts[column], nil)
  elseif links > 1 and leftOrRight then
    state.keyLink = clamp((state.keyLink or 1) + (event.key == "Right" and 1 or -1), 1, links)
  elseif node and node.type == "selector" and leftOrRight then
    nudgeSelector(node, event.key == "Right")
  elseif node and node.type == "slider" and leftOrRight then
    nudgeSlider(node, event.key == "Right")
  elseif node and enter and node.type ~= "selector" and node.type ~= "slider" and node.type ~= "scroll" then
    activate(panel, state, node, node.type ~= "text" and node.layout.x or nil)
  elseif list and (event.key == "Up" or event.key == "Down") then
    moveSelection(state, list, event.key == "Up" and -1 or 1)
  elseif not (list and isEnter(event) and activateItem(list)) and not scrollWithKey(state, node, event.key) then
    return false
  end

  panel:redraw()
  return true
end

-- sets a slider from the pointer, and calls onChange if its value changes; the drag keeps the value, which the slider shows until the
-- release, also when the panel takes it only at onSubmit
local function slide(state, node, x)
  local value = sliderValue(node, x)

  if value ~= (state.slid or node.value) then
    state.slid, node.value = value, value
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
    return candidate.tooltip ~= nil or candidate.keys ~= nil
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

  local font, textSize = draw.font("ui"), styles.note.size
  local line = lineHeight(font, textSize)
  local lines = node.tooltip and wrap(font, tostring(node.tooltip), textSize, math.min(320, width - 4 * size.paddingX)) or {}

  -- the keys that do the same come last, dimmed
  local keysLine = node.keys and #lines + 1

  if keysLine then
    lines[keysLine] = tostring(node.keys)
  end

  local boxWidth, boxHeight = 0, #lines * line + 2 * size.paddingY

  for _, text in ipairs(lines) do
    boxWidth = math.max(boxWidth, measureText(font, text, textSize))
  end

  boxWidth += 2 * size.paddingX
  local x = clamp((state.pointerX or 0) + 12, 0, math.max(0, width - boxWidth))
  local y = (state.pointerY or 0) + 18

  -- a tooltip that does not fit below the pointer goes above it
  if y + boxHeight > height then
    y = math.max(0, (state.pointerY or 0) - 6 - boxHeight)
  end

  draw.rounded(surface, x, y, boxWidth, boxHeight, size.radius + 1, "overlay")
  draw.roundedOutline(surface, x, y, boxWidth, boxHeight, size.radius + 1, 1, "frame")

  for i, text in ipairs(lines) do
    draw.text(surface, font, text, x + size.paddingX, y + size.paddingY + (i - 1) * line, textSize, i == keysLine and "textDim" or "text")
  end
end

-- the inspector: whether ui outlines every element of every panel and names the one under the pointer, for the person who builds a panel
local inspecting = false

-- the element under the pointer, the innermost one; a scroll element's children outside it are not under the pointer
local function inspected(state)
  if not (state.root and state.pointerX) then
    return nil
  end

  return find(state.root, state.pointerX, state.pointerY, function()
    return true
  end)
end

local paintOutlines

-- what paintOutlines clips for a scroll element: its children in view
function inside.outlines()
  local node, surface = inside.arguments()
  local box = node.layout

  for _, child in ipairs(node.children) do
    if child.layout.y + child.layout.height > box.y and child.layout.y < box.y + box.height then
      paintOutlines(child, surface)
    end
  end
end

-- outlines an element and its children; those of a scroll element only inside it, as they are drawn
function paintOutlines(node, surface)
  local box = node.layout
  draw.outline(surface, box.x, box.y, box.width, box.height, 1, "frame")

  if node.type == "scroll" then
    inside.clip(surface, box.x, box.y, box.width, box.height, inside.outlines, node, nil)
  else
    for _, child in ipairs(node.children or {}) do
      paintOutlines(child, surface)
    end
  end
end

-- a number as the inspector shows it: whole, or with one decimal
local function inspectNumber(number)
  return number == math.floor(number) and ("%d"):format(number) or ("%.1f"):format(number)
end

-- draws the inspector over a panel's elements: every element's outline, and the type, id, place and size of the one under the pointer, in layout units
local function paintInspector(surface, state, width, height)
  paintOutlines(state.root, surface)
  local node = inspected(state)

  if not node then
    return
  end

  local box = node.layout
  draw.outline(surface, box.x, box.y, box.width, box.height, 2, "accent")

  local lines = {
    ("%s  id = %q"):format(node.type, node.id),
    ("at %s, %s  size %s x %s"):format(inspectNumber(box.x), inspectNumber(box.y), inspectNumber(box.width), inspectNumber(box.height)),
  }

  local set = {}

  for _, name in ipairs({ "width", "height", "grow", "stretch", "padding", "gap", "align" }) do
    if node[name] ~= nil then
      set[#set + 1] = name .. " = " .. tostring(node[name])
    end
  end

  if #set > 0 then
    lines[#lines + 1] = table.concat(set, ", ")
  end

  local font, textSize = draw.font("ui"), styles.note.size
  local line = lineHeight(font, textSize)
  local boxWidth, boxHeight = 0, #lines * line + 2 * size.paddingY

  for _, text in ipairs(lines) do
    boxWidth = math.max(boxWidth, measureText(font, text, textSize))
  end

  boxWidth += 2 * size.paddingX
  local x = clamp(state.pointerX + 12, 0, math.max(0, width - boxWidth))
  local y = state.pointerY + 18

  -- a label that does not fit below the pointer goes above it
  if y + boxHeight > height then
    y = math.max(0, state.pointerY - 6 - boxHeight)
  end

  draw.rounded(surface, x, y, boxWidth, boxHeight, size.radius + 1, "overlay")
  draw.roundedOutline(surface, x, y, boxWidth, boxHeight, size.radius + 1, 1, "accent")

  for i, text in ipairs(lines) do
    draw.text(surface, font, text, x + size.paddingX, y + size.paddingY + (i - 1) * line, textSize, "text")
  end
end

-- a move of the pointer: a drag of a scroll bar, of a field's selection or of a slider goes on; otherwise the element under it is hovered
local function handlePointerMove(panel, state, x, y, shift)
  local root = state.root
  state.pointerX, state.pointerY = x, y

  -- the inspector names the element under the pointer, which may be another now
  if inspecting then
    panel:redraw()
  end

  -- the dragged scroll element may be gone since the press, when the view changed; the drag ends then
  local dragged = state.dragging and findById(root, state.dragging)
  state.dragging = dragged and state.dragging or nil

  if dragged then
    local node = dragged
    local _, length = thumb(node)
    local travel = node.layout.height - length

    if travel > 0 then
      state.scroll[node.id] = (y - node.layout.y - state.grab) / travel * (node.layout.contentHeight - node.layout.height)
    end

    panel:redraw()
    return true
  end

  local drag = state.numberDrag

  if drag then
    local node = findById(root, drag.id)
    local moved = x - drag.x

    -- a drag of a few units, so a click that shakes is still a click; then each unit is a step, or a tenth of one with Shift
    if node and (drag.moved or math.abs(moved) > NUMBER_DRAG_START) then
      drag.moved = true
      local step = (node.step and node.step > 0) and node.step or 1
      local number = snapNumber(node, drag.value + moved * step * (shift and 0.1 or 1))

      if number ~= drag.shown then
        drag.shown = number
        node.onChange(formatNumber(number))
        panel:redraw()
      end
    end

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
      slide(state, node, x)
    end

    panel:redraw()
    return true
  end

  -- a press on an item of a list with onDrag, and a move of the drag threshold, drag what onDrag gives as the core's data
  local itemDrag = state.itemDrag

  if itemDrag and math.abs(x - itemDrag.x) + math.abs(y - itemDrag.y) > (tonumber(ecs.setting.get("ecs.dragThreshold")) or 8) then
    local list = findById(root, itemDrag.list)
    state.itemDrag = nil

    if list and list.onDrag then
      local dataType, value = list.onDrag(itemDrag.index)

      if dataType then
        state.pressed = nil
        panel:startDrag(dataType, value)
        return true
      end
    end
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

-- gives a list's onSelect an item that a press chose, and how: "toggle" with Ctrl, to add it to the chosen ones or take it out,
-- and "range" with Shift, with the items from the first chosen one to it, in the order shown
local function selectItem(list, index, how)
  local between = nil

  if how == "range" then
    local from = list.selected or index
    between = {}

    for other = from, index, from <= index and 1 or -1 do
      between[#between + 1] = other
    end
  end

  call(list.onSelect, index, how, between)
end

-- a press of the first button: a list's item under it is chosen and its list gets the keys, and the element under it is pressed and gets them
local function handlePointerDown(panel, state, event, x, y)
  local root = state.root
  local scroll = find(root, x, y, scrollable)

  if scroll and x >= scroll.layout.x + scroll.layout.width - size.scrollbarWidth then
    return grabScrollbar(panel, state, scroll, y)
  end

  -- a press on a list's item chooses it; the element under the pointer still gets the press. A tree's mark keeps the choice.
  local item = find(root, x, y, isListItem)
  local keeps = find(root, x, y, function(node)
    return node.keepsChoice == true
  end)

  local how = event.shift and "range" or event.ctrl and "toggle" or nil

  if item and not keeps and (how or item.itemIndex ~= item.list.selected) then
    selectItem(item.list, item.itemIndex, how)
  end

  state.itemDrag = item and not keeps and item.list.onDrag and { list = item.list.id, index = item.itemIndex, x = x, y = y } or nil

  local node = find(root, x, y, interactive)

  if not node or not TAKES_TYPING[node.type] then
    stopEditing(panel, state)
  end

  -- the pressed element gets the keys, without a ring, so Tab goes on from it; a press on an item gives the keys to its list
  local target = (node and not isListItem(node)) and node or item and item.list or node or scroll
  state.pressed = node
  state.keyTarget, state.showRing, state.keyColumn = target and target.id, false, 0

  if node and node.isNumber and state.editing ~= node.id then
    -- a press on a number that is not typed in waits: a drag sideways changes the number, and a release without one types in it
    stopEditing(panel, state)
    state.numberDrag = { id = node.id, x = x, value = node.value, moved = false }
  elseif node and node.type == "field" then
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
    state.sliding, state.slid = node.id, nil
    slide(state, node, x)
  end

  if node or item then
    panel:redraw()
  end

  return node ~= nil or item ~= nil
end

-- a release of the first button: a click is a press and a release on the same element
local function handlePointerUp(panel, state, x, y)
  local pressed = state.pressed
  local drag = state.numberDrag
  local slid = state.sliding and findById(state.root, state.sliding)
  state.pressed, state.dragging, state.sliding, state.selecting, state.numberDrag, state.itemDrag = nil, nil, nil, nil, nil, nil
  local node = find(state.root, x, y, interactive)

  -- a drag gives its last number to onSubmit; a press that did not drag types in the number, with the cursor where it was pressed
  local dragged = drag and findById(state.root, drag.id)

  -- a slider gives the value that its press ended on to onSubmit
  if slid and slid.type == "slider" then
    call(slid.onSubmit, state.slid or slid.value)
  end

  state.slid = nil

  if dragged and drag.moved then
    dragged.onSubmit(formatNumber(drag.shown or drag.value))
  elseif dragged then
    startEditing(panel, state, dragged)
    state.cursorPosition = positionAt(dragged, state.typed, textStart(dragged, state), drag.x)
  end

  if pressed and node and node.id == pressed.id and node.type == pressed.type and not TAKES_TYPING[node.type] and node.type ~= "slider" then
    activate(panel, state, node, x, y)
  end

  panel:redraw()
  return pressed ~= nil
end

-- a press of the right button on a list's item that is not chosen chooses it alone, so the menu that the release opens is about it
local function handleMenuDown(panel, state, x, y)
  local item = find(state.root, x, y, isListItem)

  if item and not item.chosen then
    selectItem(item.list, item.itemIndex, nil)
    panel:redraw()
  end

  return item ~= nil
end

-- a release of the right button gives the place to the innermost element under it with onMenu, such as to open a menu there; a list
-- gives its item under the place first, or nil beside its items
local function handleMenuUp(state, x, y)
  local node = find(state.root, x, y, function(node)
    return node.onMenu ~= nil or (isListItem(node) and node.list.onMenu ~= nil)
  end)

  if not node then
    return false
  end

  if node.onMenu == nil then
    node.list.onMenu(node.itemIndex, x, y)
  elseif isList(node) then
    node.onMenu(nil, x, y)
  else
    node.onMenu(x, y)
  end

  return true
end

-- data dropped on the panel goes to the innermost element under it with onDrop; a list gives its item under the place, or nil beside its
-- items, and where on the item it landed: before it in its upper half and after it in its lower half, or for a tree before it in its upper
-- quarter, after it in its lower quarter and into it between
local function handleDrop(state, x, y, dataType, value)
  local node = find(state.root, x, y, function(node)
    return node.onDrop ~= nil or (isListItem(node) and node.list.onDrop ~= nil)
  end)

  if not node then
    return false
  end

  if node.onDrop == nil then
    local box, list = node.layout, node.list
    local part = box.height > 0 and (y - box.y) / box.height or 0.5
    local where = part < 0.5 and "before" or "after"

    if list.dropInto then
      where = part < 0.25 and "before" or part >= 0.75 and "after" or "into"
    end

    list.onDrop(node.itemIndex, where, dataType, value)
  elseif isList(node) then
    node.onDrop(nil, nil, dataType, value)
  else
    node.onDrop(dataType, value, x, y)
  end

  return true
end

-- handles a panel's event: calls the callbacks of the elements it reaches
local function handleEvent(panel, event)
  local state = panelState(panel)

  if not state.root then
    return false
  end

  textScale = state.scale

  -- a tooltip goes when anything but a move happens, and follows the pointer onto other elements
  if event.type == "pointerMove" then
    followTooltip(panel, state, (event.x or 0) / state.scale, (event.y or 0) / state.scale)
  else
    hideTooltip(panel, state)
  end

  -- Ctrl+F gives the keys to the panel's search field, the first if it has several
  if event.type == "keyDown" and event.key == "F" and event.ctrl and not event.alt and not event.shift and not event.super then
    local search = firstWhere(state.root, function(node)
      return node.isSearch == true
    end)

    if search then
      stopEditing(panel, state)
      state.keyTarget, state.showRing = search.id, false
      startEditing(panel, state, search)
      panel:redraw()
      return true
    end
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
    return handlePointerMove(panel, state, x, y, event.shift)
  elseif event.type == "pointerDown" and event.button == 1 then
    return handlePointerDown(panel, state, event, x, y)
  elseif event.type == "pointerUp" and event.button == 1 then
    return handlePointerUp(panel, state, x, y)
  elseif event.type == "pointerDown" and event.button == 3 then
    return handleMenuDown(panel, state, x, y)
  elseif event.type == "pointerUp" and event.button == 3 then
    return handleMenuUp(state, x, y)
  elseif event.type == "drop" then
    return handleDrop(state, x, y, event.dataType, event.value)
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

-- gives each element without an id one from its parent's id and its place, so it stays the same from one draw to the next. An id that two
-- elements have is a misuse: clicks and keys would reach only the first. An element is shown once: one that a show had already, or that a
-- show recycled, is a misuse. It walks the tree with a stack, without recursion, which LuaJIT's traces would unroll
local assignIds

do
  local pending = {}

  function assignIds(root, seen)
    root.id = root.id or "root"
    pending[1] = root
    local count = 1

    while count > 0 do
      local node = pending[count]
      pending[count] = nil
      count -= 1

      if node.shown or node.type == nil then
        ecs.check(false, "An element is shown once, and lives until its panel's next show: a panel makes its elements again at each draw, and keeps none.")
      end

      node.shown = true
      local id = node.id

      if seen[id] then
        ecs.check(false, "Two elements of a panel have the id '" .. tostring(id) .. "'. Give each its own id.")
      end

      seen[id] = true
      local children = node.children

      -- the last child is pushed first, so the first is taken first, as a walk in order takes them
      if children then
        for i = #children, 1, -1 do
          local child = children[i]
          child.id = child.id or id .. "." .. (child.place or i)
          count += 1
          pending[count] = child
        end
      end
    end
  end
end

-- keeps each scroll element's offset as the layout kept it, from 0 to as far as its content goes, and the height of each list's view
local function keepScrollOffsets(node, state)
  if node.type == "scroll" then
    state.scroll[node.id] = node.layout.offset
    state.scrolls[node.id] = node.layout
  end

  if node.items then
    listKept(state, node.id).view = node.layout.height
  end

  for _, child in ipairs(node.children or {}) do
    keepScrollOffsets(child, state)
  end
end

-- lays out the elements in the panel's surface and draws them; call it from the panel's draw
-- what a show paints: the background, the elements, and the inspector or a tooltip over them
function inside.show(root, surface, state, width, height)
  draw.fill(surface, 0, 0, width, height, "panel")
  paint(root, surface, state)

  if inspecting then
    paintInspector(surface, state, width, height)
  else
    paintTooltip(surface, state, width, height)
  end
end

function services.show(panel, surface, root)
  local state = panelState(panel)
  local width, height = surface.width / surface.scale, surface.height / surface.scale
  state.scale = surface.scale
  textScale = surface.scale

  if settingsChanged then
    settingsChanged = false
    fonts, measured, measuredCount, wrapped, wrappedCount = {}, {}, 0, {}, 0
    textSizeSetting = nil
    readSizes()
  end

  -- each tree made for this show builds its rows in view, which ui.range gives for its panel
  for _, tree in ipairs(spare.trees) do
    local first, last = services.range(panel, tree.id, tree.count)
    tree.fill(first, last)
  end

  clearTable(spare.trees)
  showCount += 1
  state.ids = state.ids or {}
  clearTable(state.ids)
  assignIds(root, state.ids)
  measure(root, state, width)
  arrange(root, 0, 0, width, height, state)
  state.scrolls = {}
  keepScrollOffsets(root, state)
  state.height = height

  -- a long list whose built items do not reach the edges of its view, as items were shorter than the list guessed, is built again
  for _, list in ipairs(state.longLists or {}) do
    local top, bottom = list.items[list.first], list.items[list.last]
    local box = list.layout

    if top and bottom and ((list.first > 1 and top.layout.y > box.y) or (list.last < list.count and bottom.layout.y + bottom.layout.height < box.y + box.height)) then
      panel:redraw()
    end
  end

  state.longLists = nil

  -- an item that ui.revealItem scrolled to by its list's kept heights shows now, so it is scrolled to where it really is
  for id, index in pairs(state.revealed) do
    local list = findById(root, id)
    local item = list and list.items and list.items[index]

    if item or not list or index > (list.count or 0) then
      state.revealed[id] = nil
    end

    if item then
      local offset = state.scroll[id]
      scrollIntoView(state, list, item)

      if state.scroll[id] ~= offset then
        panel:redraw()
      end
    end
  end

  -- the element under the pointer and the pressed element are found again in the new layout
  state.hover = state.hover and state.pointerX and find(root, state.pointerX, state.pointerY, interactive) or nil
  state.pressed = state.pressed and findById(root, state.pressed.id) or nil
  local previous = state.root
  state.root = root

  -- the part of the chosen item that Left and Right reached in the list that has the keys, which shows the ring; the column is kept as the
  -- choice moves, and an item with fewer parts shows it on its last
  local keyed = (state.keyColumn or 0) > 0 and keyList(state)
  local parts = keyed and keyed.id == state.keyTarget and rowParts(keyed, {})
  local part = parts and parts[math.min(state.keyColumn, #parts)]
  state.keyPart = part and part.id

  -- an element that ui.edit named before it was shown gets the keys now, or never
  if state.editAtShow then
    local id = state.editAtShow
    services.edit(panel, id)
    state.editAtShow = nil
  end

  -- the show is recorded, then drawn: all of it on a new surface, else only the rectangles around what changed since the last show
  local last, lastCount, lastClips = state.calls, state.callCount, state.clips
  recording, recorded, recordClip = state.spare or {}, 0, nil
  inside.clips, inside.clipped, inside.stack.top = state.spareClips or {}, 0, 0
  state.calls, state.spare, state.clips, state.spareClips = nil, nil, nil, nil

  local ok, message = pcall(inside.show, root, surface, state, width, height)

  local calls, count, clips = recording, recorded, inside.clips
  recording, recorded, recordClip, recordScroll, inside.clips, inside.clipped = nil, 0, nil, nil, nil, 0
  clearTable(inside.stack)
  inside.stack.top = 0

  if not ok then
    error(message, 0)
  end

  -- a scroll element that only scrolled moves its pixels, and the rest of the surface is compared as usual
  local moved, shift, box = movedScroll(state.shownScrolls, state.scrolls, surface.scale)
  local rects = surface.kept and last and moved and movedAreas(last, lastCount, calls, count, moved, shift, box)
  local areas = surface.kept and last and not rects and changedAreas(last, lastCount, calls, count)

  if not surface.kept or not last then
    replay(surface, calls, count, nil)
  elseif rects and box then
    drawn.move(surface, box[1], box[2], box[3] - box[1], box[4] - box[2], 0, -shift)
    local left, top, right, bottom = box[1], box[2], box[3], box[4]

    for _, rect in ipairs(rects) do
      replay(surface, calls, count, rect)
      left, top, right, bottom = math.min(left, rect[1]), math.min(top, rect[2]), math.max(right, rect[3]), math.max(bottom, rect[4])
    end

    reportChanged(surface, left, top, right, bottom)
  elseif areas then
    for _, area in ipairs(areas) do
      replay(surface, calls, count, area)
      reportChanged(surface, area[1], area[2], area[3], area[4])
    end
  else
    surface:changed(0, 0, 0, 0)
  end

  -- the last show's calls are compared with the next's, and the tables of the one before are reused for the next
  state.calls, state.callCount, state.spare, state.shownScrolls = calls, count, last, state.scrolls
  state.clips, state.spareClips = clips, lastClips

  -- an input method shows its candidates at the edited field's cursor
  local edited = state.editing and findById(root, state.editing)

  if edited and edited.type == "field" then
    local rect, scale = cursorRect(edited, state), surface.scale
    panel:setTextInput(true, rect.x * scale, rect.y * scale, rect.width * scale, rect.height * scale)
  end

  -- the elements of the show before are no one's now, so new elements are made from their tables
  if previous then
    spare.recycle(previous)
  end
end

-- a setting may change a size or a font, so the next show reads them again
ecs.event.subscribe("ecs.settingChanged", function()
  settingsChanged = true
end)

-- turns the inspector on or off, for every panel that ui shows
-- gives copies of ui's own styles, so a plugin that changes one changes only its own
function services.styles()
  local copies = {}

  for name, style in pairs(styles) do
    copies[name] = shallowCopy(style)
  end

  return copies
end

function services.inspect()
  inspecting = not inspecting

  -- a panel whose plugin did not call ui.forget may be gone
  for panel in pairs(panels) do
    pcall(panel.redraw, panel)
  end
end

-- the element under the pointer in a panel, as the inspector names it, or nil
function services.inspected(panel)
  local node = inspected(panelState(panel))

  if not node then
    return nil
  end

  local box = node.layout
  return { type = node.type, id = node.id, x = box.x, y = box.y, width = box.width, height = box.height }
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

-- gives a field, a key element or a list the keys, as a click on it does; before the panel's first show, the element gets them at that show
function services.edit(panel, id)
  local state = panelState(panel)
  local node = state.root and findById(state.root, id)
  state.editAtShow = not node and id or nil

  -- the keyboard's target follows, without its ring, as after a click
  if node and TAKES_TYPING[node.type] then
    textScale = state.scale
    state.keyTarget, state.showRing, state.keyColumn = node.id, false, 0
    startEditing(panel, state, node)
    panel:redraw()
  elseif node and isList(node) then
    stopEditing(panel, state)
    state.keyTarget, state.showRing, state.keyColumn = node.id, false, 0
    panel:redraw()
  end
end

-- scrolls a scroll element so that one of its children shows, or a tree so that one of its nodes does
function services.reveal(panel, scrollId, childId)
  local state = panelState(panel)
  local scroll = state.root and findById(state.root, scrollId)
  local child = scroll and findById(scroll, childId)

  if not child and scroll and scroll.tree then
    for index, id in ipairs(scroll.tree.ids) do
      -- a node of a long tree that is not built is scrolled to by its number
      if id == childId and not scroll.items[index] then
        revealItem(state, scroll, index)
        panel:redraw()
        return
      end

      child = child or (id == childId and scroll.items[index]) or nil
    end
  end

  if child then
    scrollIntoView(state, scroll, child)
    panel:redraw()
  end
end

-- the items of a long list that are in view, or near it, so a panel builds only those: first and last are numbers from 1, and last is less than
-- first when none shows
function services.range(panel, id, count)
  local state = panelState(panel)
  local kept = listKept(state, id)
  local offset, view = state.scroll[id] or 0, kept.view > 0 and kept.view or state.height
  local first, last, top = 1, 0, 0

  -- a panel not shown yet builds every item once, so the list learns its heights
  if not view then
    return 1, count
  end

  for i = 1, count do
    local bottom = top + itemHeight(kept, i)

    if bottom < offset - view / 2 then
      first = i + 1
    elseif top > offset + view * 1.5 then
      break
    end

    last, top = i, bottom + kept.gap
  end

  return first, math.max(last, first - 1)
end

-- scrolls a list so that an item shows, by its number, whether it is built or not
function services.revealItem(panel, id, index)
  local state = panelState(panel)
  local list = state.root and findById(state.root, id)

  if list and list.items and index >= 1 and index <= list.count then
    revealItem(state, list, index)
    panel:redraw()
  end
end

-- descriptions, for plugins in C -------------------------------------------

-- the callbacks of each type of element; a described element with an id keeps what they get, for ui.take, since C gives ui no functions
-- those of ASKED are kept only by an element whose props have them as true, since they change how the element behaves
local ASKED = { onActivate = true, onSort = true }
local CALLBACKS = {
  button = { "onClick" },
  toggle = { "onChange" },
  slider = { "onChange", "onSubmit" },
  selector = { "onSelect" },
  tabs = { "onSelect" },
  dropdown = { "onSelect" },
  number = { "onChange", "onSubmit" },
  key = { "onChange" },
  field = { "onChange", "onSubmit" },
  path = { "onChange", "onSubmit" },
  search = { "onChange", "onSubmit" },
  list = { "onSelect", "onActivate" },
  table = { "onSelect", "onActivate", "onSort" },
  tree = { "onSelect", "onActivate", "onToggle", "onMark" },
  section = { "onToggle" },
  color = { "onChange" },
}

-- what each type's element function takes besides its props: its children, a value, choices, columns and rows, or nodes; the others take a value
local TAKES = {
  column = "children", row = "children", scroll = "children", list = "children", group = "children",
  selector = "choices", tabs = "choices", dropdown = "choices",
  table = "table", tree = "tree", section = "section", separator = "none", space = "none",
}

-- the props a description may have: those of ui.Props and of each element's, without the callbacks, and what asks for one
local DESCRIBED = {}

for name in ("id style width height grow stretch padding gap align background hoverBackground border color size font tooltip keys selectable title selected "
  .. "sort ascending vertical wrap label look min max step valid clickable alpha placeholder dialog pattern results folders count first onActivate onSort"):gmatch("%S+") do
  DESCRIBED[name] = true
end

-- an element from a description, built with the element functions, as Lua code builds it
local function build(panel, description)
  local props = shallowCopy(description.props)

  -- a prop ui does not know is a mistake of the code that describes, such as a name that ui.h and ui no longer share
  for name in pairs(props) do
    if not DESCRIBED[name] then
      ecs.check(false, ("A described '%s' has the prop '%s', which ui does not know."):format(tostring(description.type), tostring(name)))
    end
  end

  -- a column or row with clickable = true is clicked like a button
  local elementType = description.type
  local callbacks = CALLBACKS[elementType] or (props.clickable and { "onClick" }) or {}
  props.clickable = nil

  -- a field with valid = false refuses the submit, as a Lua onSubmit that returns false does
  local refuses = (elementType == "field" or elementType == "path") and props.valid == false

  for _, callback in ipairs(callbacks) do
    local id = props.id
    local asked = props[callback] == true
    props[callback] = nil

    if refuses and callback == "onSubmit" then
      props.onSubmit = function()
        return false
      end
    elseif id and (asked or not ASKED[callback]) then
      -- onToggle gets a node's new state second, and onMark a mark's id second and its new state third
      props[callback] = function(value, second, third)
        local taken = panelState(panel).taken

        if callback == "onMark" then
          taken[id .. "\0" .. callback] = { value = value, mark = second, on = third }
        else
          taken[id .. "\0" .. callback] = { value = value, open = second }
        end

        panel:redraw()
      end
    end
  end

  -- a described text with a link keeps the link a click gave, under its id, or under "" without one, so a panel takes the links of every text,
  -- such as markdown's; a text without a link takes no keys
  if elementType == "text" and type(description.value) == "table" then
    for _, span in ipairs(description.value) do
      if type(span) == "table" and span.link then
        local key = (props.id or "") .. "\0onLink"

        props.onLink = function(value)
          panelState(panel).taken[key] = { value = value }
          panel:redraw()
        end

        break
      end
    end
  end

  local takes = TAKES[elementType]
  local make = elements[elementType]
  if not make then
    ecs.check(false, ("There is no element of type '%s'."):format(tostring(elementType)))
  end

  if takes == "children" then
    local children = {}

    for i, child in ipairs(description.children or {}) do
      children[i] = build(panel, child)
    end

    return make(props, children)
  elseif takes == "choices" then
    return make(description.selected, description.choices, props)
  elseif takes == "table" then
    return make(description.columns, description.rows, props)
  elseif takes == "tree" then
    props.open, props.selected = description.open, description.selected
    return make(description.nodes, props)
  elseif takes == "section" then
    local children = {}

    for i, child in ipairs(description.children or {}) do
      children[i] = build(panel, child)
    end

    props.open = description.open
    return make(description.value, props, children)
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
  local key = (id or "") .. "\0" .. callback
  local got = taken[key]
  taken[key] = nil
  return got
end

ecs.service.register({
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
  menu = {
    sig = "void(handle<ecs.panel> panel, float x, float y, value<ui.MenuEntry[]> entries)",
    doc = "Open a menu of entries in a popup at a point of a panel, such as where a right click was; a click or Return runs an entry and closes the menu\n"
        .. "@param panel The panel\n"
        .. "@param x The point's x in the panel, in layout units, as a pointer event's position divided by the surface's scale\n"
        .. "@param y The point's y\n"
        .. "@param entries The entries, in order: each a table of its label, the keys that run it too, whether it is enabled, and its onClick, or a line between entries",
    fn = function(panel, x, y, entries)
      openMenu(panel, x, y, entries)
    end,
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
  styles = {
    sig = "value<ui.Styles>()",
    doc = "Give ui's own styles, which its views and the first-party views use so they look alike\n"
        .. "@return Copies of the styles view, item, title, heading and note, which an element's style prop takes",
    fn = services.styles,
  },
  inspect = {
    sig = "void()",
    doc = "Turn the inspector on or off: it outlines every element of every panel that ui shows, and names the one under the pointer "
        .. "with its type, id, place and size in layout units, and the layout props it has",
    fn = services.inspect,
  },
  inspected = {
    sig = "value<ui.Inspected?>(handle<ecs.panel> panel)",
    doc = "Give the element under the pointer in a panel, the innermost one, as the inspector names it\n"
        .. "@param panel The panel that shows the elements\n"
        .. "@return The element's type, id, place and size in layout units, or nil if the pointer is over none",
    fn = services.inspected,
  },
  edit = {
    sig = "void(handle<ecs.panel> panel, string id)",
    doc = "Give an element the keys\n"
        .. "@param panel The panel that shows the element, often the focused panel that a key passes\n"
        .. "@param id The id of a field, a key element or a list, tree or table in the last layout, or else in the next one, such as an element"
        .. " that shows from the next draw; another id does nothing",
    fn = services.edit,
  },
  range = {
    sig = "void(handle<ecs.panel> panel, string list, int count, out int first, out int last)",
    doc = "Give the items of a long list that are in view or near it, so the panel builds only those and gives the list count and first\n"
        .. "@param panel The panel that shows the list\n"
        .. "@param list The list's id\n"
        .. "@param count The number of the list's items\n"
        .. "@param first The number of the first item to build, from 1\n"
        .. "@param last The number of the last item to build; less than first when there is none",
    fn = services.range,
  },
  revealItem = {
    sig = "void(handle<ecs.panel> panel, string list, int index)",
    doc = "Scroll a list so an item shows, by its number, whether the last draw built it or not\n"
        .. "@param panel The panel that shows the list\n"
        .. "@param list The list's id, in the last layout; nothing happens if no list has it\n"
        .. "@param index The item's number, from 1",
    fn = services.revealItem,
  },
  reveal = {
    sig = "void(handle<ecs.panel> panel, string scroll, string child)",
    doc = "Scroll so a child shows\n"
        .. "@param panel The panel that shows the scroll element\n"
        .. "@param scroll The id of the scroll element, or of a list, tree or table\n"
        .. "@param child The id of an element inside it, or of a tree's node, in the last layout; nothing happens if it has none",
    fn = services.reveal,
  },
  column = {
    sig = "value<ui.Element>(value<ui.ContainerProps?> props, value<ui.Child[]?> children)",
    doc = "Elements below each other\n"
        .. "@param props The column's properties (ui.ContainerProps); with onClick, it is clicked like a button\n"
        .. "@param children The elements, from the top; one that is false is left out; none if nil\n"
        .. "@return The column",
    fn = elements.column,
  },
  row = {
    sig = "value<ui.Element>(value<ui.ContainerProps?> props, value<ui.Child[]?> children)",
    doc = "Elements beside each other\n"
        .. "@param props The row's properties (ui.ContainerProps); with onClick, it is clicked like a button\n"
        .. "@param children The elements, from the left; one that is false is left out; none if nil\n"
        .. "@return The row",
    fn = elements.row,
  },
  scroll = {
    sig = "value<ui.Element>(value<ui.ScrollProps?> props, value<ui.Child[]?> children)",
    doc = "Elements below each other, which scroll\n"
        .. "@param props The scroll element's properties (ui.ScrollProps), which must have an id; it grows unless grow says otherwise\n"
        .. "@param children The elements, from the top; one that is false is left out; none if nil\n"
        .. "@return The scroll element",
    fn = elements.scroll,
  },
  list = {
    sig = "value<ui.Element>(value<ui.ListProps?> props, value<ui.Child[]?> children)",
    doc = "Items below each other, which scroll; selected is the chosen one, a press chooses one for onSelect, and a click gives it to onActivate\n"
        .. "@param props The list's properties (ui.ListProps), which must have an id\n"
        .. "@param children The items, from the top; those with selectable = false, such as headings, are not items; none if nil\n"
        .. "@return The list",
    fn = elements.list,
  },
  group = {
    sig = "value<ui.Element>(value<ui.GroupProps?> props, value<ui.Child[]?> children)",
    doc = "Elements below each other in a frame, under a title\n"
        .. "@param props The group's properties (ui.GroupProps), with its title; padding, gap and border have defaults\n"
        .. "@param children The elements, from the top, below the title; none if nil\n"
        .. "@return The group",
    fn = elements.group,
  },
  section = {
    sig = "value<ui.Element>(value<ui.Text> title, value<ui.SectionProps?> props, value<ui.Child[]?> children)",
    doc = "A heading with an arrow over elements, which a click on the heading shows and hides; in a list, its elements are the list's items\n"
        .. "@param title The heading: a text, or spans, as a text takes\n"
        .. "@param props The section's properties (ui.SectionProps); the plugin keeps open, and onToggle gets the new state\n"
        .. "@param children The elements, from the top, below the heading while it is open; none if nil\n"
        .. "@return The section",
    fn = elements.section,
  },
  color = {
    sig = "value<ui.Element>(string color, value<ui.ColorProps?> props)",
    doc = "A colour, as a swatch, which a click opens in a picker\n"
        .. "@param color The colour, \"#RRGGBB\" or \"#RRGGBBAA\"\n"
        .. "@param props The element's properties (ui.ColorProps); onChange gets the colour a press in the picker ends on\n"
        .. "@return The colour element",
    fn = elements.color,
  },
  table = {
    sig = "value<ui.Element>(value<ui.TableColumn[]?> columns, value<ui.TableRow[]?> rows, value<ui.TableProps?> props)",
    doc = "Rows of cells below a header of the columns' titles, which scroll; selected is the chosen row, and a click on a title gives its column to onSort\n"
        .. "@param columns The columns, from the left (ui.TableColumn); none if nil\n"
        .. "@param rows The rows, from the top, in the order given, each a table of its cells' values by column id; none if nil\n"
        .. "@param props The table's properties (ui.TableProps), which must have an id\n"
        .. "@return The table",
    fn = elements.table,
  },
  tree = {
    sig = "value<ui.Element>(value<ui.TreeNode[]?> nodes, value<ui.TreeProps?> props)",
    doc = "A list of nodes, each below its parent and indented, which open and close; onSelect, onActivate and onToggle get a node's id\n"
        .. "@param nodes The top nodes, each with its children (ui.TreeNode); none if nil\n"
        .. "@param props The tree's properties (ui.TreeProps), which must have an id; open names the open nodes\n"
        .. "@return The tree",
    fn = elements.tree,
  },
  graph = {
    sig = "value<ui.Element>(value<ui.GraphValue[]> values, value<ui.GraphProps?> props)",
    doc = "Bars of numbers, such as times; Lua only\n"
        .. "@param values The bars, the oldest on the left: each a number, or a list of numbers that stack, each in its colour\n"
        .. "@param props The graph's properties (ui.GraphProps)\n"
        .. "@return The graph",
    fn = elements.graph,
  },
  separator = {
    sig = "value<ui.Element>(value<ui.SeparatorProps?> props)",
    doc = "A line across a column, or up a row\n"
        .. "@param props The separator's properties (ui.SeparatorProps)\n"
        .. "@return The separator",
    fn = elements.separator,
  },
  text = {
    sig = "value<ui.Element>(value<ui.Text> text, value<ui.TextProps?> props)",
    doc = "A line of text, or of spans in their own styles\n"
        .. "@param text The text, or a list of spans (ui.Span), each a text or a table of its text and style; cut with an ellipsis, or at the edge for spans, when it does not fit unless props has wrap = true\n"
        .. "@param props The text's properties (ui.TextProps)\n"
        .. "@return The text element",
    fn = elements.text,
  },
  button = {
    sig = "value<ui.Element>(value<ui.Text> label, value<ui.ButtonProps?> props)",
    doc = "A button; onClick runs when it is clicked\n"
        .. "@param label The button's label: a text, or spans with icons and images, as a text takes\n"
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
    doc = "A number from min to max, which a drag changes, rounded to step; onChange gets each value of a drag, and onSubmit the one its release ends on\n"
        .. "@param value The number it shows, kept from min to max\n"
        .. "@param props The slider's properties (ui.SliderProps); min and max are 0 and 1 if missing\n"
        .. "@return The slider",
    fn = elements.slider,
  },
  selector = {
    sig = "value<ui.Element>(int selected, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "One of a few choices, side by side; onSelect gets the number of the clicked one\n"
        .. "@param selected The chosen one's number, from 1; 0 for none\n"
        .. "@param choices The texts to choose from, from the left; none if nil\n"
        .. "@param props The selector's properties (ui.ChoiceProps)\n"
        .. "@return The selector",
    fn = elements.selector,
  },
  tabs = {
    sig = "value<ui.Element>(int selected, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "Tabs: headers of the choices, with a line under the chosen one; onSelect gets the number of the clicked one\n"
        .. "@param selected The chosen tab's number, from 1\n"
        .. "@param choices The tabs' texts, from the left; none if nil\n"
        .. "@param props The tabs' properties (ui.ChoiceProps)\n"
        .. "@return The tabs",
    fn = elements.tabs,
  },
  dropdown = {
    sig = "value<ui.Element>(int selected, value<string[]?> choices, value<ui.ChoiceProps?> props)",
    doc = "One of a list of choices, which a click opens; onSelect gets the number of the chosen one\n"
        .. "@param selected The chosen one's number, from 1, whose text the closed dropdown shows; 0 for none\n"
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
    doc = "A number that the user types, from min to max, rounded to step; onChange gets it at each edit, and onSubmit at Return; Up and Down give it to both\n"
        .. "@param value The number it shows, kept from min to max and rounded to step; 0 if it is not a number\n"
        .. "@param props The field's properties (ui.NumberProps), which must have an id\n"
        .. "@return The number field",
    fn = elements.number,
  },
  path = {
    sig = "value<ui.Element>(string text, value<ui.PathProps?> props)",
    doc = "A field for the path of a file, with a button that opens the system's file dialog; onChange and onSubmit get the chosen path as a typed one\n"
        .. "@param text The path, which typing starts from; empty if nil\n"
        .. "@param props The path's properties (ui.PathProps), which must have an id; dialog chooses the dialog, and pattern the extensions it shows\n"
        .. "@return The path element",
    fn = elements.path,
  },
  search = {
    sig = "value<ui.Element>(string query, value<ui.SearchProps?> props)",
    doc = "A field that narrows what a view shows, with a button that clears it; onChange gets the query at each edit, Ctrl+F types in it, Escape clears it\n"
        .. "@param query The query it shows; empty if nil\n"
        .. "@param props The search's properties (ui.SearchProps); its id is \"search\" if it has none\n"
        .. "@return The search element",
    fn = elements.search,
  },
  matches = {
    sig = "bool(string query, string text)",
    doc = "Tell whether a text matches a search: each word of the query, whatever its case, is somewhere in it; an empty query matches every text\n"
        .. "@param query The search's query, as ui.search gives it\n"
        .. "@param text The text to search; several texts of an item are joined with new lines, so that no word matches across two of them\n"
        .. "@return True if every word is in the text",
    fn = matchesQuery,
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
  icon = {
    sig = "value<ui.Element>(string name, value<ui.Props?> props)",
    doc = "A small picture, a line of text high and drawn from shapes in its color, the text's colour if missing\n"
        .. "@param name The picture: eye, eyeClosed, lock or lockOpen\n"
        .. "@param props The icon's properties (ui.Props), such as color and tooltip\n"
        .. "@return The icon element",
    fn = elements.icon,
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
        .. "@param id The described element's id; \"\" for the link that a click gave in a described text without an id\n"
        .. "@param callback The callback's name, such as \"onClick\", \"onChange\" or \"onToggle\"\n"
        .. "@return What the callback got, as value and, for onToggle, open; nil if it did not run or was taken already",
    fn = services.take,
  },
})
