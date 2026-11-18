---@type ecs.Manifest
return {
  name = "viewer",
  version = "0.1.0",
  api = 3,
  type = "lua",
  description = "Shows a file as what it is: an image, a sound or a text",
  depends = { ui = "0.1", image = "0.1", audio = "0.1", mixer = "0.1", syntax = "0.1", fs = "0.1" },
}
