-- one form built with the ui plugin
return {
  format = 1,
  name = "former",
  version = "0.1.0",
  app = { id = "openecs.test.former", name = "Former" },
  depends = { former = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Form", windows = { { panels = { { type = "former.form" } } } } },
  },
}
