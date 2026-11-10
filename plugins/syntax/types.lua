---@meta
-- The types of syntax's tables, for editors. syntax never runs this file: OpenECS --definitions adds it to syntax's definitions (OpenECS DESIGN 10.10).

---A highlighted range of a text. Positions are of bytes, from 1, as Lua's string.find gives them.
---@class syntax.Range
---@field start integer
---@field stop integer
---@field kind "keyword"|"string"|"comment"|"number"|"function"|"type"|"constant"|"punctuation"
