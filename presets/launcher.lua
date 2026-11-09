-- what OpenECS starts with when the command line names no preset and no session: a list of presets and saved sessions
---@type ecs.Preset
return {
  format = 2,
  name = "launcher",
  version = "0.1.0",
  app = { id = "openecs.launcher", name = "OpenECS", categories = { "Development" } },
  listed = false,
  depends = { launcher = "0.1" },
  workspaces = {
    { name = "Launcher", windows = { { panels = { { type = "launcher.list" } } } } },
  },
}
