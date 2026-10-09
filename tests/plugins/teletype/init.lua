-- a grid that prints, wraps and scrolls, and a panel that draws it at a size that fits; what happened is kept in the plugin state

local ecs = require("ecs")
local tty = require("tty")

local BACKGROUND = 0xFF000000
local results = {}

-- a grid of 10 by 3 cells
local grid = tty.new(10, 3)
tty.colors(grid, 0xFFFFFFFF, BACKGROUND)
tty.clear(grid)

-- print writes at the cursor: a new line moves to the next row, a full row wraps, and the last row scrolls the grid up
tty.print(grid, "one\ntwo\n")
results.afterTwo = { tty.cursor(grid) }
tty.print(grid, "three\nfour is long")
results.afterScroll = { tty.cursor(grid) }

-- put writes at a cell, and leaves the cursor
tty.put(grid, 8, 0, "XYZ")
results.size = { tty.size(grid) }

ecs.panel.registerType({
  name = "teletype.screen",
  title = "Screen",
  create = function(panel)
    return { panel = panel }
  end,
  draw = function(_, surface)
    surface:fill(0, 0, surface.width, surface.height, BACKGROUND)

    -- the grid takes the panel's size at a font size
    local columns, rows = tty.fit(surface.width / surface.scale, surface.height / surface.scale, 16)
    results.fits = columns > 10 and rows > 3
    tty.draw(surface, grid, 0, 0, 16, true)

    -- the first cell holds a letter, so some of its pixels are not the background
    local cellWidth, cellHeight = tty.cell(16)
    local inked = 0

    for y = 0, math.floor(cellHeight * surface.scale) - 1 do
      for x = 0, math.floor(cellWidth * surface.scale) - 1 do
        inked = inked + (surface:getPixel(x, y) ~= BACKGROUND and 1 or 0)
      end
    end

    results.inked = inked > 0
    results.cell = cellWidth > 0 and cellHeight > cellWidth
  end,
})

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
