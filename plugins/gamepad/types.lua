---@meta
-- The types of gamepad's tables, for editors. gamepad never runs this file: OpenECS --definitions adds it to gamepad's definitions (OpenECS DESIGN 10.10).

---A gamepad that is plugged in.
---@class gamepad.Info
---@field id integer What open takes, and what its events carry.
---@field name string
---@field type string Such as "xbox360", "ps5" or "unknown".
