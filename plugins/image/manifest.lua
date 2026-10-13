---@type ecs.Manifest
return {
  name = "image",
  version = "0.1.0",
  api = 1,
  description = "Reads image files, PNG, JPG, GIF, BMP, TGA, PSD, HDR, PNM and SVG, into images that draw draws; a standard plugin",
  depends = { draw = "0.1" },
  type = "native",
}
