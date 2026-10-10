-- the fs plugin makes, lists, copies, moves and removes files and folders, which Lua's io library reads and writes
return {
  preset = "presets/filer.lua",
  run = function(test)
    local results = test.session().pluginState.filer.state
    test.match(results.made, true, "a folder and the one above it made")
    test.match({ results.info.type, results.info.size }, { "file", 5 }, "a file's type and size")
    assert(results.info.modified > 1e9, "a file's time: " .. tostring(results.info.modified))
    test.match(results.folderType, "folder", "a folder's type")
    test.match(results.missing, true, "a missing path")
    test.match(results.all, { "inner:folder", "notes.txt:file" }, "the list, by name")
    test.match(results.texts, 1, "the list of a pattern")
    test.match({ results.copied, results.moved, results.movedText }, { true, true, "hello" }, "a copy, moved")
    test.match({ results.removedFull, results.removedFile }, { false, true }, "removing")
    assert(results.executable:match("/$"), "the executable's folder: " .. tostring(results.executable))
    test.match(results.unknownFolder, true, "an unknown folder")
  end,
}
