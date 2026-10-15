---@meta
-- The types of fs's tables, for editors. fs never runs this file: OpenECS --definitions adds it to fs's definitions (OpenECS DESIGN 10.10).

---What a path is.
---@class fs.Info
---@field type "file"|"folder"|"other"
---@field size integer In bytes.
---@field modified integer When it last changed, in seconds since 1970.

---An entry of a folder: its name, and what it is.
---@class fs.Entry: fs.Info
---@field name string
