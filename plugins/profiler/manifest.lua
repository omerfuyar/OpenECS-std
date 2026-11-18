---@type ecs.Manifest
return {
  name = "profiler",
  version = "0.1.0",
  api = 2,
  description = "The profiler: where each frame's time goes, by step of the main loop, by plugin and by panel",
  depends = { ui = "0.1" },
  type = "lua",
}
