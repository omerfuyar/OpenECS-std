---@type ecs.Manifest
return {
  name = "keys",
  version = "0.1.0",
  api = 3,
  description = "The key list: every key binding of the tool, to change, remove or add one; and the keys after the prefix while the core waits for one",
  depends = { ui = "0.1", draw = "0.1" },
  type = "lua",
}
