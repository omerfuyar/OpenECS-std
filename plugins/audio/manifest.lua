---@type ecs.Manifest
return {
  name = "audio",
  version = "0.1.0",
  api = 1,
  description = "Reads sound files, WAV, MP3, FLAC and OGG, into sounds that mixer plays; a standard plugin",
  depends = { mixer = "0.1" },
  type = "native",
}
