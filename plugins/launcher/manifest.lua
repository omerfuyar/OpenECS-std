---@type ecs.Manifest
return {
  name = "launcher",
  version = "0.1.0",
  api = 2,
  description = "Lists the presets and saved sessions, and opens the one the user chooses",
  depends = { ui = "0.1" },
  type = "lua",
}
