---@meta
-- The types of sqlite's tables, for editors. sqlite never runs this file: OpenECS --definitions adds it to sqlite's definitions (OpenECS DESIGN 10.10).

---A row that a query gives: its columns by name, as integers, numbers, texts and buffers; a NULL is left out.
---@class sqlite.Row
---@field [string] integer|number|string
