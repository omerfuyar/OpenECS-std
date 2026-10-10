-- one screen that draws a tty grid
return {
  format = 1,
  name = "teletype",
  version = "0.1.0",
  app = { id = "openecs.test.teletype", name = "Teletype" },
  depends = { teletype = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Screen", windows = { { panels = { { type = "teletype.screen" } } } } },
  },
}
