---@type ecs.Manifest
return {
  name = "keys",
  version = "0.1.0",
  api = 2,
  description = "The key list: every key binding of the tool, to change, remove or add one",
  depends = { ui = "0.1" },
  type = "lua",
}
