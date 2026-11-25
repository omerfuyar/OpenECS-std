---@type ecs.Manifest
return {
  name = "panels",
  version = "0.1.0",
  api = 3,
  description = "The panel list: every panel type of the tool, by plugin, to open one; and a blank panel that holds a place in the layout",
  depends = { ui = "0.1" },
  type = "lua",
}
