-- a wall that draws the plugin's own images, a PNG and an SVG, stretched, then reads its pixels; the results are kept in the plugin state

local ecs = require("ecs")
local image = require("image")

local results = {}

-- files are read once, so the same path gives the same picture
local green = image.load(ecs.plugin.folder .. "green.png")
local square = image.load(ecs.plugin.folder .. "square.svg")
results.same = image.load(ecs.plugin.folder .. "green.png") == green
results.missing = image.load("nope.png") == nil
results.greenSize = { image.size(green) }
results.squareSize = { image.size(square) }
results.greenPixel = image.pixel(green, 1, 1)

ecs.panel.registerType({
  name = "gallery.wall",
  title = "Wall",
  create = function()
    return {}
  end,
  draw = function(_, surface)
    surface:fill(0, 0, surface.width, surface.height, 0xFF000000)
    image.draw(surface, green, 10, 10, 20, 20)
    image.draw(surface, square, 40, 10, 40, 40)
    results.drawnGreen = surface:getPixel(math.floor(20 * surface.scale), math.floor(20 * surface.scale))
    results.drawnSquare = surface:getPixel(math.floor(60 * surface.scale), math.floor(30 * surface.scale))
  end,
})

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
