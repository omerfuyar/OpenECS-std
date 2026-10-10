-- one wall of images
return {
  format = 1,
  name = "gallery",
  version = "0.1.0",
  app = { id = "openecs.test.gallery", name = "Gallery" },
  depends = { gallery = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Wall", windows = { { panels = { { type = "gallery.wall" } } } } },
  },
}
