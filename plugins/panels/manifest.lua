---@type ecs.Manifest
return {
  name = "panels",
  version = "0.1.0",
  api = 1,
  description = "The panel list: every panel type of the tool, by plugin, to open one",
  depends = { ui = "0.1" },
  type = "lua",
}
