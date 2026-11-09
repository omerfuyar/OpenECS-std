---@type ecs.Manifest
return {
  name = "plugins",
  version = "0.1.0",
  api = 2,
  description = "The plugin list: every plugin in the plugin folders, to enable or disable one while the tool runs",
  depends = { ui = "0.1" },
  type = "lua",
}
