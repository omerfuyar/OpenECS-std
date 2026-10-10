-- works in a folder of its own, with fs for folders and Lua's io for what files hold; what happened is kept in the plugin state

local ecs = require("ecs")
local fs = require("fs")

local results = {}
local root = ecs.plugin.folder .. "scratch/"

-- a previous run's folder is removed first, the files before their folders
for _, entry in ipairs(fs.list(root .. "inner/", "") or {}) do
  fs.remove(root .. "inner/" .. entry.name)
end

for _, entry in ipairs(fs.list(root, "") or {}) do
  fs.remove(root .. entry.name)
end

fs.remove(root)

-- makeFolder makes the folders above too; Lua's io writes the files
results.made = fs.makeFolder(root .. "inner/")
local file = assert(io.open(root .. "notes.txt", "w"))
file:write("hello")
file:close()
assert(io.open(root .. "inner/data.bin", "w")):close()

results.info = fs.info(root .. "notes.txt")
results.folderType = fs.info(root .. "inner").type
results.missing = fs.info(root .. "nope") == nil

-- a list is sorted by name, and a pattern chooses names
results.all = {}

for _, entry in ipairs(fs.list(root, "")) do
  results.all[#results.all + 1] = entry.name .. ":" .. entry.type
end

results.texts = #fs.list(root, "*.txt")
results.copied = fs.copy(root .. "notes.txt", root .. "copy.txt")
results.moved = fs.move(root .. "copy.txt", root .. "inner/moved.txt")
local moved = assert(io.open(root .. "inner/moved.txt", "r"))
results.movedText = moved:read("a")
moved:close()

-- a folder that holds files is not removed
results.removedFull = fs.remove(root .. "inner")
results.removedFile = fs.remove(root .. "notes.txt")
results.executable = fs.folder("executable")
results.unknownFolder = fs.folder("nowhere") == nil

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
