-- reads a model of one red triangle, and keeps what the gltf plugin gives in the plugin state

local ecs = require("ecs")
local gltf = require("gltf")

local results = {}
local model = gltf.load(ecs.plugin.folder .. "triangle.gltf")
results.missing = gltf.load("nope.gltf") == nil
results.info = gltf.describe(model)

-- the buffers hold little-endian floats and 32-bit indices
local positions = gltf.attribute(model, 1, 1, "POSITION")
results.positions = { string.unpack("<fffffffff", positions) }
results.positions[10] = nil
local indices = gltf.indices(model, 1, 1)
results.indices = { string.unpack("<I4I4I4", indices) }
results.indices[4] = nil
results.noAttribute = gltf.attribute(model, 1, 1, "NORMAL") == ""
results.noMesh = gltf.indices(model, 2, 1) == ""

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
