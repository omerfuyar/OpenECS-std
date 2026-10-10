return {
  name = "filer",
  version = "0.1.0",
  api = 1,
  description = "Makes, lists, copies, moves and removes files with the fs plugin and Lua's io library, for tests/fs.lua",
  depends = { fs = "0.1" },
  lua = "init.lua",
}
