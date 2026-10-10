-- the filer plugin, without panels
return {
  format = 1,
  name = "filer",
  version = "0.1.0",
  app = { id = "openecs.test.filer", name = "Filer" },
  depends = { filer = "0.1" },
  pluginsDir = "../plugins",
  workspaces = { { name = "Empty", windows = { {} } } },
}
