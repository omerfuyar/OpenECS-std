-- a form built with the ui plugin; what its callbacks got, and where its elements are, are kept in the plugin state

local ecs = require("ecs")
local ui = require("ui")

local results = { clicks = 0, on = false, choice = "one", typed = "", submitted = "", key = "Ctrl+K", rowClicks = 0, rects = {} }
local IDS = { "button", "check", "choice", "field", "key", "list", "row:1", "row:30", "wide" }

ecs.panel.registerType({
  name = "former.form",
  title = "Form",
  create = function(panel)
    return { panel = panel }
  end,
  destroy = function(form)
    ui.forget(form.panel)
  end,
  draw = function(form, surface)
    local rows = {}

    for i = 1, 30 do
      rows[i] = ui.row({ id = "row:" .. i, padding = 4, onClick = function() results.rowClicks = results.rowClicks + 1 end }, { ui.text("Row " .. i) })
    end

    ui.show(form.panel, surface, ui.column({ padding = 10, gap = 6 }, {
      ui.row({ gap = 6 }, {
        ui.button("Press", { id = "button", onClick = function() results.clicks = results.clicks + 1 end }),
        ui.check(results.on, { id = "check", label = "On", onChange = function(on) results.on = on end }),
        ui.choice(results.choice, { "one", "two", "three" }, { id = "choice", onChange = function(value) results.choice = value end }),
      }),
      ui.row({ id = "wide", gap = 6 }, {
        ui.field(results.submitted, {
          id = "field",
          valid = #results.typed < 6,
          onChange = function(text) results.typed = text end,
          onSubmit = function(text) results.submitted = text end,
        }),
        ui.key(results.key, { id = "key", onChange = function(key) results.key = key end }),
      }),
      ui.scroll({ id = "list" }, rows),
    }))

    -- the elements' places, so the test can click them
    for _, id in ipairs(IDS) do
      local ok, x, y, width, height = ui.rect(form.panel, id)
      results.rects[id] = ok and { x = x, y = y, width = width, height = height } or nil
    end
  end,
  event = function(form, event)
    ui.event(form.panel, event)
  end,
})

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
