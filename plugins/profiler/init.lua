-- The profiler: a panel of where the main thread's time goes, by step of the main loop, by plugin and by panel (DESIGN 32).
-- The core measures while the setting ecs.profile is on (OpenECS DESIGN 3.5); the panel shows what it measured, built with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")

local STYLES = ui.styles()

local REFRESH = 0.5 -- seconds between the panel's readings of the profile
local STATE_VERSION = 1 -- version of a panel's saved state: the chosen tab, and whether it is paused
local TABS = { "Plugins", "Panels", "Steps" }
local STEPS = { "wait", "input", "timers", "events", "layout", "panels", "upload", "interface", "present" }

local PLUGIN_COLUMNS = {
  { id = "name", title = "Plugin" },
  { id = "share", title = "Of a second" },
  { id = "seconds", title = "ms each second" },
  { id = "longest", title = "Longest ms" },
  { id = "calls", title = "Calls" },
}

local PANEL_COLUMNS = {
  { id = "name", title = "Panel" },
  { id = "share", title = "Of a second" },
  { id = "seconds", title = "ms each second" },
  { id = "longest", title = "Longest ms" },
  { id = "calls", title = "Draws" },
}

local STEP_COLUMNS = {
  { id = "name", title = "Step" },
  { id = "average", title = "Average ms" },
  { id = "longest", title = "Longest ms" },
}

local function milliseconds(seconds)
  return ("%.2f"):format(seconds * 1000)
end

local function percent(seconds)
  return ("%.1f%%"):format(seconds * 100)
end

-- a pass's time without its wait: what the main thread did in it
local function busy(pass)
  local total = 0

  for _, step in ipairs(STEPS) do
    if step ~= "wait" then
      total = total + pass[step]
    end
  end

  return total
end

-- the passes that drew a frame, and their average and longest time without the wait
local function summarize(profile)
  local frames, total, longest = 0, 0, 0

  for _, pass in ipairs(profile.passes) do
    if pass.panels + pass.interface + pass.present > 0 then
      local time = busy(pass)
      frames = frames + 1
      total = total + time
      longest = math.max(longest, time)
    end
  end

  return frames, frames > 0 and total / frames or 0, longest
end

local function stepRows(profile)
  local rows = {}

  for _, step in ipairs(STEPS) do
    local total, longest = 0, 0

    for _, pass in ipairs(profile.passes) do
      total = total + pass[step]
      longest = math.max(longest, pass[step])
    end

    rows[#rows + 1] = { name = step, average = milliseconds(#profile.passes > 0 and total / #profile.passes or 0), longest = milliseconds(longest) }
  end

  return rows
end

local function timeRow(name, time)
  return { name = name, share = percent(time.seconds), seconds = milliseconds(time.seconds), longest = milliseconds(time.longest), calls = time.calls }
end

local function pluginRows(profile)
  local rows = {}

  for _, time in ipairs(profile.plugins) do
    rows[#rows + 1] = timeRow(time.name, time)
  end

  return rows
end

-- a panel is named by its title and type, while it is open
local function panelName(time)
  local panel = ecs.layout.findPanel(time.id)
  return panel and ("%s (%s)"):format(panel.title, time.type) or ("%s #%d"):format(time.type, time.id)
end

local function panelRows(profile)
  local rows = {}

  for _, time in ipairs(profile.panels) do
    rows[#rows + 1] = timeRow(panelName(time), time)
  end

  return rows
end

local function summaryText(profile)
  local frames, average, longest = summarize(profile)
  return ("%d frames in the last %d passes: %s ms on average, %s ms the longest, without waiting."):format(frames, #profile.passes, milliseconds(average),
    milliseconds(longest))
end

-- a plain text of the whole profile, for a report
local function report(profile)
  local lines = { "OpenECS profile", summaryText(profile), "", "Plugins, each one's own time in the last second:" }

  local function addRows(rows, columns)
    for _, row in ipairs(rows) do
      local cells = {}

      for _, column in ipairs(columns) do
        cells[#cells + 1] = ("%s %s"):format(column.title, tostring(row[column.id]))
      end

      lines[#lines + 1] = "  " .. table.concat(cells, ", ")
    end
  end

  addRows(pluginRows(profile), PLUGIN_COLUMNS)
  lines[#lines + 1] = ""
  lines[#lines + 1] = "Panels, each one's draw in the last second:"
  addRows(panelRows(profile), PANEL_COLUMNS)
  lines[#lines + 1] = ""
  lines[#lines + 1] = "Steps of the main loop:"
  addRows(stepRows(profile), STEP_COLUMNS)
  return table.concat(lines, "\n")
end

local function chosenTable(view)
  local tab = TABS[view.tab]
  local columns, rows

  if tab == "Plugins" then
    columns, rows = PLUGIN_COLUMNS, pluginRows(view.profile)
  elseif tab == "Panels" then
    columns, rows = PANEL_COLUMNS, panelRows(view.profile)
  else
    columns, rows = STEP_COLUMNS, stepRows(view.profile)
  end

  return ui.table(columns, rows, { id = "table:" .. tab, grow = 1, stretch = true })
end

local function describe(view)
  local measuring = ecs.setting.get("ecs.profile") == true
  local children = {
    ui.text("Profiler", { style = STYLES.title }),
    ui.row({ gap = 12, align = "center" }, {
      ui.toggle(measuring, {
        id = "measure",
        look = "switch",
        label = "Measure",
        tooltip = "Turns the setting ecs.profile on or off; measuring costs a little time",
        onChange = function(on)
          local ok, message = ecs.setting.set("ecs.profile", on)

          if not ok then
            ecs.log.warn(message or "The setting ecs.profile cannot be set.")
          end
        end,
      }),
      ui.toggle(view.paused, {
        id = "pause",
        label = "Pause",
        tooltip = "Keeps the numbers that show, to read or copy them",
        onChange = function(on)
          view.paused = on
        end,
      }),
      ui.button("Copy report", {
        id = "copy",
        tooltip = "Copies the whole profile as text",
        onClick = function()
          if view.profile then
            ecs.clipboard.setText(report(view.profile))
          end
        end,
      }),
    }),
  }

  if not view.profile then
    local hint = measuring and "Measuring; the first numbers show within a second." or "Turn Measure on to see where the time goes."
    children[#children + 1] = ui.text(hint, { color = "textDim", wrap = true, selectable = false })
  else
    children[#children + 1] = ui.text(summaryText(view.profile), { style = STYLES.note, wrap = true })
    children[#children + 1] = ui.tabs(view.tab, TABS, {
      id = "tab",
      onSelect = function(selected)
        view.tab = selected
      end,
    })
    children[#children + 1] = chosenTable(view)
  end

  return ui.column({ style = STYLES.view }, children)
end

-- the panel reads the profile on a timer, not in each draw, so its own drawing stays small
local function read(view)
  if view.paused then
    return
  end

  local shown = view.profile
  view.profile = ecs.profile.get()

  if view.profile or shown then
    view.panel:redraw()
  end
end

local views = {} -- every open profiler's state, by its panel handle

ecs.panel.registerType({
  name = "view",
  title = "Profiler",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local view = { panel = panel, tab = 1, paused = false }

    if version == STATE_VERSION and saved then
      view.tab = math.min(math.max(saved.tab or 1, 1), #TABS)
      view.paused = saved.paused == true
    end

    view.timer = ecs.timer.start(REFRESH, true, function()
      read(view)
    end)
    read(view)
    views[panel] = view
    return view
  end,
  destroy = function(state)
    state.timer:stop()
    ui.forget(state.panel)
    views[state.panel] = nil
  end,
  saveState = function(state)
    return { tab = state.tab, paused = state.paused }
  end,
  draw = function(state, surface)
    ui.show(state.panel, surface, describe(state))
  end,
  event = function(state, event)
    ui.event(state.panel, event)
  end,
})

-- opens the profiler, or shows the one that is open, so a key can bring it up
local function open()
  local shown = next(views)

  if shown then
    ecs.layout.focus(shown)
    return
  end

  local panel, message = ecs.layout.open("profiler.view")

  if not panel then
    ecs.log.warn(message or "Cannot open the profiler.")
  end
end

ecs.service.register({
  open = { sig = "void()", doc = "Open the profiler", fn = open },
})
