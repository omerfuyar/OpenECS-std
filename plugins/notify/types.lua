---@meta
-- The types of notify's tables, for editors. notify never runs this file: OpenECS --definitions adds it to notify's definitions (OpenECS DESIGN 10.10).

---A desktop notification.
---@class notify.Notification
---@field title string
---@field message? string
---@field image? string An image file, such as a PNG, shown beside the message.
---@field priority? "low"|"normal"|"high"|"critical"
---@field buttons? notify.Button[]
---@field replaces? integer The id of a notification that this one takes the place of.
---@field transient? boolean true to leave it out of the system's list of notifications once it is gone.

---A button of a notification.
---@class notify.Button
---@field id string What notify.action carries; not "default", which a click on the notification itself carries.
---@field label? string Its text; its id if it has none.

---A tray icon.
---@class notify.Tray
---@field name string What its events carry.
---@field icon? string An image file, such as a PNG.
---@field tooltip? string
---@field entries? notify.TrayEntry[]

---An entry of a tray icon's menu; one without a label is a separator.
---@class notify.TrayEntry
---@field id? string What notify.trayEntry carries; its label if it has none.
---@field label? string
---@field checked? boolean Makes the entry a checkbox, checked or not.
---@field disabled? boolean
