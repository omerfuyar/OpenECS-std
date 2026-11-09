---@meta
-- The types of regex's tables, for editors. regex never runs this file: OpenECS --definitions adds it to regex's definitions (OpenECS DESIGN 10.10).

---A match of a pattern in a text. Positions are of bytes, from 1, as Lua's string.find gives them.
---@class regex.Match
---@field start integer The first byte of the match.
---@field stop integer The last byte of the match; start - 1 for an empty match.
---@field text string What the pattern matched.
---@field groups (string|false)[] What each group in parentheses matched, in order; false for a group that matched nothing.
---@field named table<string, string> What each named group, (?<name>...), matched.
