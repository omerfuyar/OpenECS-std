---@type ecs.Manifest
return {
  name = "files",
  version = "0.1.0",
  api = 2,
  type = "lua",
  description = "A folder's files: it makes, renames, moves and deletes them, and another plugin says how they open",
  depends = { ui = "0.1", fs = "0.1" },
}
