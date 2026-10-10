-- the viewer plugin, without panels
return {
  format = 1,
  name = "viewer",
  version = "0.1.0",
  app = { id = "openecs.test.viewer", name = "Viewer" },
  depends = { viewer = "0.1" },
  pluginsDir = "../plugins",
  workspaces = { { name = "Empty", windows = { {} } } },
}
