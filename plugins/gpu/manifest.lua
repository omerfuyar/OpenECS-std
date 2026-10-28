---@type ecs.Manifest
return {
  name = "gpu",
  version = "0.1.0",
  api = 2,
  description = "Draws shapes, lines, textures, text in draw's fonts, and vertex buffers with shaders of their own, into GPU surfaces, on the GPU; a standard plugin",
  depends = { draw = "0.1" },
  type = "native",
}
