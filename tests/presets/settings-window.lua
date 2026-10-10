-- one board, the settings window, and the plugin that keeps the values of the settings the settings window changes
return {
  format = 1,
  name = "settings-window",
  version = "0.1.0",
  app = { id = "openecs.test.settingsWindow", name = "Settings window" },
  depends = { painter = "0.1", watcher = "0.1", settings = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Board", windows = { { panels = { { type = "painter.board" } } } } },
  },
}
