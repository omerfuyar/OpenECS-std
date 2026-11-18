---@type ecs.Manifest
return {
  name = "console",
  version = "0.1.0",
  api = 3,
  type = "lua",
  description = "A log whose lines may name a file and a line, which a click opens in a code editor",
  depends = { ui = "0.1", process = "0.1" },
}
