---@meta
-- The types of compress's tables, for editors. compress never runs this file: OpenECS --definitions adds it to compress's definitions (OpenECS DESIGN 10.10).

---A file or folder of a zip archive.
---@class compress.ZipEntry
---@field name string Its path in the archive, with / between folders; a folder's ends with /.
---@field size integer Its size in bytes, unpacked.
---@field packedSize integer Its size in bytes in the archive.

---A file to write into a zip archive.
---@class compress.ZipFile
---@field name string Its path in the archive, with / between folders; a name that ends with / is a folder.
---@field data string? What the file holds; nothing for a folder.

---A file or folder of a tar archive.
---@class compress.TarFile
---@field name string Its path in the archive, with / between folders; a name that ends with / is a folder.
---@field data string? What the file holds; nothing for a folder.
---@field executable boolean? Whether the file is executable, as a program is on Linux.
