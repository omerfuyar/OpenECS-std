---@type ecs.Manifest
return {
  name = "ui",
  version = "0.1.0",
  api = 2,
  description = "User-interface elements with layout, which call back on clicks and edits; a standard plugin",
  depends = { draw = "0.1", image = "0.1" },
  type = "lua",
}
