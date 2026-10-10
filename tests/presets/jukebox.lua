-- the jukebox plugin, without panels
return {
  format = 1,
  name = "jukebox",
  version = "0.1.0",
  app = { id = "openecs.test.jukebox", name = "Jukebox" },
  depends = { jukebox = "0.1" },
  pluginsDir = "../plugins",
  workspaces = { { name = "Empty", windows = { {} } } },
}
