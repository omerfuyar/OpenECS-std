---@meta
-- The types of ui's elements and their properties, for editors. ui never runs this file: OpenECS --definitions adds it to ui's definitions,
-- and the value<...> types of ui's signatures name these classes (OpenECS DESIGN 10.10).

---An element that a function of ui makes; ui.show lays it out and draws it.
---@class ui.Element

---An element's rectangle in the last layout, in layout units.
---@class ui.Rect
---@field x number
---@field y number
---@field width number
---@field height number

---The properties every element takes.
---@class ui.Props
---@field id? string Names the element, so it keeps its state, and ui.rect finds it; one from its place in the tree otherwise.
---@field width? number A fixed width, in layout units.
---@field height? number A fixed height, in layout units.
---@field grow? number A share of the room left along the parent's direction.
---@field stretch? boolean Whether it takes the parent's whole cross size.
---@field padding? number Room inside a container's edges.
---@field gap? number Room between a container's children.
---@field align? "start"|"center"|"end" Where a container puts children that do not stretch.
---@field background? string A colour, such as "#202226" or a theme colour, such as "tabRow".
---@field hoverBackground? string The background while the pointer is over it.
---@field border? string The colour of a line around it.
---@field color? string The colour of its text.
---@field size? number The font size of its text, in layout units.
---@field font? string A font name of draw, such as "ui" or "mono".
---@field tooltip? string A text that shows when the pointer rests on it.
---@field selectable? boolean false keeps a list's child, such as a heading, from being an item.

---A column or a row: with onClick, it is clicked like a button.
---@class ui.ContainerProps: ui.Props
---@field onClick? fun()

---A scroll element; it needs an id, which keeps its offset.
---@class ui.ScrollProps: ui.Props
---@field id string

---@class ui.ListProps: ui.ScrollProps
---@field selected? integer The chosen item's number, from 1.
---@field onSelect? fun(item: integer) A press on an item gives its number.
---@field onActivate? fun(item: integer) A click on an item gives its number.

---@class ui.GroupProps: ui.Props
---@field title? string

---A column of a table.
---@class ui.TableColumn
---@field id string The key of the column's cells in each row.
---@field title? string The title in the header; the id if missing.
---@field width? number A fixed width; the widest cell's otherwise.

---A row of a table: its cells' values by column id.
---@alias ui.TableRow table<string, any>

---@class ui.TableProps: ui.Props
---@field id string
---@field selected? integer The chosen row's number, from 1.
---@field onSelect? fun(row: integer)
---@field onActivate? fun(row: integer)
---@field onSort? fun(column: string) A click on a title gives its column's id.
---@field sort? string The id of the column the rows are sorted by, which shows an arrow.
---@field ascending? boolean The arrow points up.

---A node of a tree; one with children, even none, has an arrow.
---@class ui.TreeNode
---@field id any
---@field label? string The id as text if missing.
---@field color? string
---@field children? ui.TreeNode[]

---@class ui.TreeProps: ui.Props
---@field id string
---@field open? table<any, boolean> The open nodes, by id.
---@field selected? any The chosen node's id.
---@field onSelect? fun(id: any)
---@field onActivate? fun(id: any)
---@field onToggle? fun(id: any, open: boolean) A click on an arrow gives the node's id and its new state.

---@class ui.SeparatorProps: ui.Props
---@field vertical? boolean A line up a row, rather than across a column.

---@class ui.TextProps: ui.Props
---@field wrap? boolean Breaks into lines at the width of its column, or its own width.

---@class ui.ButtonProps: ui.Props
---@field onClick? fun()

---@class ui.ToggleProps: ui.Props
---@field label? string
---@field look? "check"|"switch"
---@field onChange? fun(on: boolean)

---@class ui.SliderProps: ui.Props
---@field min? number 0 if missing.
---@field max? number 1 if missing.
---@field step? number Rounds the value.
---@field onChange? fun(value: number)

---A selector, tabs or a dropdown.
---@class ui.ChoiceProps: ui.Props
---@field onChange? fun(choice: string)

---A field; it needs an id, which keeps what is typed.
---@class ui.FieldProps: ui.Props
---@field id string
---@field onChange? fun(text: string) Gets each edit.
---@field onSubmit? fun(text: string): boolean? Gets the text when Return is pressed; false keeps typing.
---@field valid? boolean false marks the text as wrong.

---@class ui.NumberProps: ui.Props
---@field id string
---@field min? number
---@field max? number
---@field step? number
---@field onChange? fun(value: number)

---@class ui.KeyProps: ui.Props
---@field id string
---@field onChange? fun(combination: string)

---An element as a plugin in C describes it, which ui.showDescription builds with the element function of its type.
---@class ui.Description
---@field type string The element's type, which names its function, such as "column" or "button".
---@field props? ui.Props The element's props, without callbacks; a column or row with clickable = true is clicked like a button.
---@field value? boolean|number|string The value its function takes first: a text, a label, a state, a number or a path.
---@field choices? string[] A selector's, tabs' or dropdown's choices.
---@field columns? ui.TableColumn[] A table's columns.
---@field rows? ui.TableRow[] A table's rows.
---@field nodes? ui.TreeNode[] A tree's nodes.
---@field children? ui.Description[] A container's children.

---What a callback of a described element got, which ui.take gives.
---@class ui.Taken
---@field value boolean|number|string|nil The callback's first argument: a new state, number, text or choice, an item's number, or a node's or column's id.
---@field open? boolean onToggle's second argument: whether the node is open now.
