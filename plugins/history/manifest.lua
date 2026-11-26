---@type ecs.Manifest
return {
  name = "history",
  version = "0.1.0",
  api = 4,
  description = "Undo and redo for any tool: histories of records that plugins push, the keys, and a panel that lists the records",
  depends = { ui = "0.1" },
  type = "lua",
}
