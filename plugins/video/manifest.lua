---@type ecs.Manifest
return {
  name = "video",
  version = "0.1.0",
  api = 2,
  description = "Plays MPEG-1 video files with their MP2 sound: frames drawn with draw, sound played with mixer",
  depends = { draw = "0.1", mixer = "0.1" },
  type = "native",
}
