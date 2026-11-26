-- Undo and redo for any tool (DESIGN 34): histories of records that plugins push, each with the functions that take a change back and make it
-- again; the keys and functions that act on the focused panel's history; and the History panel, which lists the records.
-- It is built with the ui standard plugin.

local ecs = require("ecs")
local ui = require("ui")

local STYLES = ui.styles()

ecs.setting.declare({
  name = "mergeTime",
  type = "number",
  description = "Seconds within which a change with the same merge key joins the change before it, so one undo takes back a whole drag",
  default = 1,
})
ecs.setting.declare({
  name = "limit",
  type = "integer",
  description = "How many records a history keeps at most; a change past it drops the oldest",
  default = 1000,
})

ecs.event.declare("changed", "A history's records changed: a push, an undo, a redo, a drop or a clear; the value names the history")
ecs.handle.registerType("history")

---A history: its records, the oldest first, and how many are done.
---@class history.Kept
---@field name string
---@field handle any its handle, which open gives each time
---@field records table[] each { label, merge, tags, undo, redo, undoData, redoData }
---@field position integer
---@field mergeOpen boolean whether the last record still takes a record with its merge key
---@field mergeTimer any? the timer that closes the last record to merging

local histories = {} ---@type table<string, history.Kept> every open history, by name
local byPanel = {} ---@type table<integer, history.Kept> the history of each panel that uses one, by the panel's id
local current ---@type history.Kept? the history of the panel that had the focus last and has one
local lists = {} ---@type table<ecs.Panel, table> every open History panel's state, by its panel
local busy = false -- an undo or a redo runs, so a push is refused

local function changed(kept)
  ecs.event.emit("history.changed", { name = kept.name })
end

local function stopMerging(kept)
  if kept.mergeTimer then
    kept.mergeTimer:stop()
  end

  kept.mergeTimer, kept.mergeOpen = nil, false
end

-- the history of a handle that open gave
local function historyOf(handle)
  local kept = handle and ecs.handle.value(handle, "history")
  ecs.check(kept ~= nil and histories[kept.name] == kept, "A history's handle names a history that is open.")
  return kept
end

local services = {}

function services.open(name)
  ecs.check(type(name) == "string" and name ~= "", "history.open takes a history's name.")
  local kept = histories[name]

  if not kept then
    kept = { name = name, records = {}, position = 0, mergeOpen = false }
    kept.handle = ecs.handle.new("history", kept)
    histories[name] = kept
  end

  return kept.handle
end

function services.close(handle)
  local kept = historyOf(handle)
  stopMerging(kept)
  histories[kept.name] = nil

  for id, used in pairs(byPanel) do
    if used == kept then
      byPanel[id] = nil
    end
  end

  if current == kept then
    current = nil
  end

  kept.records, kept.position = {}, 0
  changed(kept)
end

function services.use(panel, handle)
  local kept = handle and historyOf(handle) or nil
  byPanel[panel.id] = kept
  local focus = ecs.layout.getFocus()

  -- the first history in use shows until a panel with one gets the focus
  if kept and (not current or (focus and focus.id == panel.id)) then
    current = kept
  end
end

-- what the panel with the focus undoes: its own history, or for a History panel the one it shows
local function focused()
  local focus = ecs.layout.getFocus()

  if not focus then
    return nil
  end

  local list = lists[focus]
  return byPanel[focus.id] or (list and list:shown()) or nil
end

-- calls a record's undo or redo; a failure clears the history, as its records may no longer match the data
local function run(kept, fn, data, what)
  busy = true
  local ok, problem = pcall(fn, data)
  busy = false

  if not ok then
    ecs.log.error(("The %s of a change in the history %s failed, so the history is cleared: %s"):format(what, kept.name, tostring(problem)))
    stopMerging(kept)
    kept.records, kept.position = {}, 0
  end

  return ok
end

-- undoes the last record done, or redoes the next one; false when there is none, or it failed
local function step(kept, back)
  stopMerging(kept)
  local record = kept.records[back and kept.position or kept.position + 1]

  if not record then
    return false
  end

  local ok

  if back then
    ok = run(kept, record.undo, record.undoData, "undo")
  else
    ok = run(kept, record.redo, record.redoData, "redo")
  end

  if ok then
    kept.position = kept.position + (back and -1 or 1)
  end

  return ok
end

-- records a change; one with the merge key of the last record, while it still merges, joins it and keeps its undo
local function record(kept, label, merge, tags, undo, redo, undoData, redoData)
  if busy then
    return
  end

  merge = merge ~= "" and merge or nil
  local last = kept.records[kept.position]

  if merge and last and kept.position == #kept.records and kept.mergeOpen and last.merge == merge then
    last.redo, last.redoData, last.label = redo, redoData, label or last.label
  else
    for i = #kept.records, kept.position + 1, -1 do
      kept.records[i] = nil
    end

    local copied = {}

    for i, tag in ipairs(type(tags) == "table" and tags or {}) do
      copied[i] = tostring(tag)
    end

    kept.records[#kept.records + 1] = {
      label = label or "", merge = merge, tags = copied, undo = undo, redo = redo, undoData = undoData, redoData = redoData,
    }
    kept.position = #kept.records
    local limit = math.max(1, tonumber(ecs.setting.get("history.limit")) or 1000)

    while #kept.records > limit do
      table.remove(kept.records, 1)
      kept.position = kept.position - 1
    end
  end

  stopMerging(kept)

  if merge then
    kept.mergeOpen = true
    kept.mergeTimer = ecs.timer.start(tonumber(ecs.setting.get("history.mergeTime")) or 1, false, function()
      kept.mergeTimer, kept.mergeOpen = nil, false
    end)
  end

  changed(kept)
end

function services.push(handle, label, merge, tags, undo, redo, data)
  local kept = historyOf(handle)
  ecs.check(type(undo) == "function" and type(redo) == "function", "history.push takes the record's undo and redo functions.")
  record(kept, label, merge, tags, undo, redo, data, data)
end

function services.change(handle, label, merge, old, new, apply, data)
  local kept = historyOf(handle)
  ecs.check(type(apply) == "function", "history.change takes the function that sets the value.")

  -- undo gives apply the old value, and redo the new one, each with data
  local function set(value)
    apply(value, data)
  end

  set(new)
  record(kept, label, merge, nil, set, set, old, new)
end

function services.drop(handle, tag)
  local kept = historyOf(handle)
  stopMerging(kept)
  local records, position = {}, kept.position

  for i, record in ipairs(kept.records) do
    local tagged = false

    for _, own in ipairs(record.tags) do
      tagged = tagged or own == tag
    end

    if not tagged then
      records[#records + 1] = record
    elseif i <= kept.position then
      position = position - 1
    end
  end

  kept.records, kept.position = records, position
  changed(kept)
end

function services.clear(handle)
  local kept = historyOf(handle)
  stopMerging(kept)
  kept.records, kept.position = {}, 0
  changed(kept)
end

function services.goTo(handle, position)
  local kept = historyOf(handle)
  position = math.max(0, math.min(#kept.records, math.floor(tonumber(position) or 0)))

  while kept.position > position and step(kept, true) do
  end

  while kept.position < position and step(kept, false) do
  end

  changed(kept)
end

function services.undoIn(handle)
  services.goTo(handle, historyOf(handle).position - 1)
end

function services.redoIn(handle)
  services.goTo(handle, historyOf(handle).position + 1)
end

function services.undo()
  local kept = focused()

  if kept then
    services.undoIn(kept.handle)
  end
end

function services.redo()
  local kept = focused()

  if kept then
    services.redoIn(kept.handle)
  end
end

function services.state(handle)
  local kept = historyOf(handle)
  local records = {}

  for i, record in ipairs(kept.records) do
    records[i] = { label = record.label, tags = record.tags }
  end

  return { name = kept.name, records = records, position = kept.position }
end

-- the History panel ----------------------------------------------------------------------------------------------------------------------

local List = {}
List.__index = List

function List.create(panel)
  local self = setmetatable({ panel = panel }, List)
  lists[panel] = self
  return self
end

function List:destroy()
  lists[self.panel] = nil
end

-- the history that the panel shows: the one its dropdown chose, or the focused panel's
function List:shown()
  local chosen = self.chosen and histories[self.chosen]
  return chosen or current
end

function List:view()
  local kept = self:shown()
  local names = {}

  for name in pairs(histories) do
    names[#names + 1] = name
  end

  table.sort(names)

  if not kept then
    return ui.column({ style = STYLES.view }, {
      ui.text("No history yet. A panel that keeps one shows its changes here once it has the focus.", { style = STYLES.note, wrap = true }),
    })
  end

  local selected = 1

  for i, name in ipairs(names) do
    if name == kept.name then
      selected = i
    end
  end

  local first, last = ui.range(self.panel, "records", #kept.records + 1)
  local rows = {}

  for row = first, last do
    local record = kept.records[row - 1]
    -- the records after the last one done are undone, and Redo or a click brings them back
    rows[#rows + 1] = ui.text(record and record.label or "Before every change", { color = row - 1 > kept.position and "textDim" or nil })
  end

  return ui.column({ style = STYLES.view, gap = 6 }, {
    ui.dropdown(selected, names, {
      id = "history",
      tooltip = "The history that shows; it follows the focused panel's until one is chosen here",
      onSelect = function(index)
        self.chosen = names[index]
      end,
    }),
    ui.list({
      id = "records", grow = 1, selected = kept.position + 1, count = #kept.records + 1, first = first,
      tooltip = "A click undoes or redoes until that record is the last one done",
      onSelect = function(item)
        services.goTo(kept.handle, item - 1)
      end,
    }, rows),
  })
end

function List:saveState()
  -- not read back; a test reads what shows, and where the list of records is, in layout units
  local kept = self:shown()
  return { shown = kept and kept.name or nil, records = ui.rect(self.panel, "records") }
end

ecs.panel.registerType(ui.panelType({
  name = "list",
  title = "History",
  redrawOn = { "history.changed", "ecs.focusChanged" },
  create = List.create,
  view = List.view,
  saveState = List.saveState,
  destroy = List.destroy,
}))

-- the history of the panel that gets the focus, if it has one, shows; a History panel that follows it forgets its own choice
ecs.event.subscribe("ecs.focusChanged", function(_, value)
  local kept = type(value) == "table" and value.panel and byPanel[value.panel]

  if kept then
    current = kept

    for _, list in pairs(lists) do
      list.chosen = nil
    end
  end
end)

ecs.event.subscribe("ecs.panelClosed", function(_, value)
  if type(value) == "table" and value.panel then
    byPanel[value.panel] = nil
  end
end)

function services.show()
  local shown = next(lists)

  if shown then
    ecs.layout.focus(shown)
    return
  end

  local panel, message = ecs.layout.open("history.list")

  if not panel then
    ecs.log.warn(message or "Cannot open the History panel.")
  end
end

local HISTORY = "handle<history.history> history"

ecs.service.register({
  open = {
    sig = "handle<history.history>(string name)",
    doc = "Give the history of a name, and make it the first time\n@param name The history's name, such as \"notes:\" .. path, or the tool's own\n@return The history; the same name gives the same one",
    fn = services.open,
  },
  close = { sig = "void(" .. HISTORY .. ")", doc = "Drop a history and its records\n@param history The history, as open gives it", fn = services.close },
  use = {
    sig = "void(handle<ecs.panel> panel, " .. HISTORY .. ")",
    doc = "Make a panel's undo and redo act on a history\n@param panel The panel\n@param history The history, or nothing to take it away",
    fn = services.use,
  },
  push = {
    sig = "void(" .. HISTORY .. ", string label, string merge, value<string[]> tags, keep fn<void(value data)> undo, keep fn<void(value data)> redo, value data)",
    doc = "Record a change that the plugin has made\n@param history The history\n@param label What the change was, as the History panel shows it\n"
        .. "@param merge A key, such as \"A.x\": a change with the same key as the last one, within history.mergeTime seconds, joins it; empty never merges\n"
        .. "@param tags Texts, such as the files the change touches, which drop names\n@param undo Takes the change back; it gets data\n"
        .. "@param redo Makes the change again; it gets data\n@param data What undo and redo get, such as the change's values",
    fn = services.push,
  },
  change = {
    sig = "void(" .. HISTORY .. ", string label, string merge, value old, value new, keep fn<void(value value, value data)> apply, value data)",
    doc = "Change one value and record it: apply gets the new value now, the old one at an undo and the new one at a redo\n@param history The history\n"
        .. "@param label What the change was\n@param merge A merge key, as push takes it\n@param old The value before\n@param new The value after\n"
        .. "@param apply Sets the value; it gets the value and data\n@param data What apply gets after the value, such as what the value belongs to",
    fn = services.change,
  },
  drop = {
    sig = "void(" .. HISTORY .. ", string tag)",
    doc = "Remove the records with a tag, such as a file that changed on disk\n@param history The history\n@param tag The tag",
    fn = services.drop,
  },
  clear = { sig = "void(" .. HISTORY .. ")", doc = "Remove every record of a history\n@param history The history", fn = services.clear },
  undo = { sig = "void()", doc = "Undo the last change of the focused panel's history", fn = services.undo },
  redo = { sig = "void()", doc = "Redo the last undone change of the focused panel's history", fn = services.redo },
  undoIn = { sig = "void(" .. HISTORY .. ")", doc = "Undo the last change of a history\n@param history The history", fn = services.undoIn },
  redoIn = { sig = "void(" .. HISTORY .. ")", doc = "Redo the last undone change of a history\n@param history The history", fn = services.redoIn },
  goTo = {
    sig = "void(" .. HISTORY .. ", int position)",
    doc = "Undo or redo until a number of records are done\n@param history The history\n@param position How many records are done after it; 0 for none",
    fn = services.goTo,
  },
  state = {
    sig = "value<history.State>(" .. HISTORY .. ")",
    doc = "Give a history's records and how many are done\n@param history The history\n@return Its name, its records, each a label and tags, the oldest first, and position",
    fn = services.state,
  },
  show = { sig = "void()", doc = "Open the History panel, or focus the open one", fn = services.show },
})

-- its key after the prefix, and its entry in the title bar's menu Tools
ecs.key.bind("prefix", "H", "history.show")
ecs.app.addMenuEntry("history.show")
