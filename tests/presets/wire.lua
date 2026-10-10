-- the wire plugin, without panels
return {
  format = 1,
  name = "wire",
  version = "0.1.0",
  app = { id = "openecs.test.wire", name = "Wire" },
  depends = { wire = "0.1" },
  pluginsDir = "../plugins",
  workspaces = { { name = "Empty", windows = { {} } } },
}
