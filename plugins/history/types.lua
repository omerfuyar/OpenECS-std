---@meta
-- The types of history's tables, for editors. history never runs this file: OpenECS --definitions adds it to history's definitions
-- (OpenECS DESIGN 10.10).

---A record as history.state gives it.
---@class history.Record
---@field label string What the change was, as the History panel shows it.
---@field tags string[] The tags the record was pushed with, such as the files it changes.

---A history's records and how many are done.
---@class history.State
---@field name string The history's name.
---@field records history.Record[] The oldest first.
---@field position integer How many records are done; 0 when every record is undone.
