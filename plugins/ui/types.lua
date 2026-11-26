---@meta
-- The types of ui's elements and their properties, for editors. ui never runs this file: OpenECS --definitions adds it to ui's definitions,
-- and the value<...> types of ui's signatures name these classes (OpenECS DESIGN 10.10).

---An element that a function of ui makes; ui.show lays it out and draws it.
---@class ui.Element

---A child of a container: an element, or false, which is left out, so a child shows on a condition.
---@alias ui.Child ui.Element|false

---An element's rectangle in the last layout, in layout units.
---@class ui.Rect
---@field x number
---@field y number
---@field width number
---@field height number

---A style: props of an element's look, kept in a variable to share them. An element's style prop takes one.
---@class ui.Style
---@field style? ui.Style The style it builds on; its own props win.
---@field width? number
---@field height? number
---@field grow? number
---@field stretch? boolean
---@field padding? number
---@field gap? number
---@field align? "start"|"center"|"end"
---@field background? ecs.Color
---@field hoverBackground? ecs.Color
---@field border? ecs.Color
---@field color? ecs.Color
---@field size? number
---@field font? string

---ui's own styles, which ui.styles gives.
---@class ui.Styles
---@field view ui.Style A view's root: room around it and between its parts, on the panel's colour.
---@field item ui.Style A row of a list: room around it and between its parts.
---@field title ui.Style A view's title.
---@field heading ui.Style A heading among items, in the accent.
---@field note ui.Style A note, dimmed and small.
---@field bar ui.Style A row of tools along a panel's top, a header of the panel's own controls, on the tab row's colour.

---The properties every element takes.
---@class ui.Props
---@field id? string Names the element, so it keeps its state, and ui.rect finds it; one from its place in the tree otherwise.
---@field style? ui.Style A style: the props of its look that it does not give itself.
---@field width? number A fixed width, in layout units.
---@field height? number A fixed height, in layout units.
---@field grow? number A share of the room left along the parent's direction.
---@field stretch? boolean Whether it takes the parent's whole cross size.
---@field padding? number Room inside a container's edges.
---@field gap? number Room between a container's children.
---@field align? "start"|"center"|"end" Where a container puts children that do not stretch.
---@field background? ecs.Color A colour, such as "#202226" or a theme colour, such as "tabRow".
---@field hoverBackground? ecs.Color The background while the pointer is over it.
---@field border? ecs.Color The colour of a line around it.
---@field color? ecs.Color The colour of its text.
---@field size? number The font size of its text, in layout units.
---@field font? string A font name of draw, such as "ui" or "mono".
---@field tooltip? string A text that shows when the pointer rests on it.
---@field keys? string The key combination that does what it does, such as "Ctrl+S", which its tooltip names after its text, dimmed.
---@field onMenu? fun(x: number, y: number) A release of the right button on it gives the place, in layout units, such as to open ui.menu there.
---@field onDrop? fun(dataType: string, value: any, x: number, y: number) Data dropped on it, which the panel accepts, gives its type, its value and the place, in layout units.
---@field onDragOver? fun(dataType: string, value: any, x: number, y: number): boolean? Whether data dragged over it may land there, asked at each move and at the drop: false refuses it, nothing or true takes it.
---@field selectable? boolean false keeps a list's child, such as a heading, from being an item.

---A column or a row: with onClick, it is clicked like a button.
---@class ui.ContainerProps: ui.Props
---@field onClick? fun()
---@field wrap? boolean A row in a column puts the children that do not fit the rest of a line on the next line.

---A scroll element; it needs an id, which keeps its offset.
---@class ui.ScrollProps: ui.Props
---@field id string

---@class ui.ListProps: ui.ScrollProps
---@field selected? integer|integer[] The chosen item's number, from 1, or a list of the chosen ones' numbers, each highlighted; Up and Down move from the first.
---@field count? integer A long list's number of items, when its children are only the items that ui.range names; the others take their room without being built.
---@field first? integer The number of a long list's first child, from 1, as ui.range gives it.
---@field onSelect? fun(item: integer, how: "toggle"|"range"|nil, between: integer[]?) A press on an item gives its number; with Ctrl, how is "toggle", and with Shift "range", with the numbers from the first chosen item to it.
---@field onActivate? fun(item: integer) A click on an item gives its number.
---@field onMenu? fun(item: integer?, x: number, y: number) A right click gives the item under it, which a right press chose if it was not, or nil beside the items, and the place.
---@field onDrag? fun(item: integer): string?, any A press on an item and a move of the drag threshold give the item; the type and the value that it gives are dragged as the core's data, and nil drags nothing.
---@field onDrop? fun(item: integer?, where: "before"|"after"|"into"|nil, dataType: string, value: any) Data dropped on an item gives it, and whether it landed on its upper or lower half, or the place that onDragOver gave; nil and nil beside the items.
---@field onDragOver? fun(item: integer?, where: "before"|"after"|nil, dataType: string, value: any): boolean|"before"|"after"|"into"|nil Whether data dragged there may land, asked at each move and at the drop with what onDrop would get: false refuses it, nothing or true takes it, and over an item another where gives it that place, which the mark shows and onDrop gets.

---@class ui.GroupProps: ui.Props
---@field title? string

---A section: a heading with an arrow over its children. The plugin keeps whether it is open.
---@class ui.SectionProps: ui.Props
---@field open? boolean Whether its children show; true if missing.
---@field onToggle? fun(open: boolean) A click on the heading gives the new state.

---A colour element; a click opens a picker.
---@class ui.ColorProps: ui.Props
---@field alpha? boolean Whether the picker has an alpha bar, and gives "#RRGGBBAA".
---@field onChange? fun(color: string) Gets the colour a press in the picker ends on, as "#RRGGBB", or "#RRGGBBAA" with alpha.

---A column of a table.
---@class ui.TableColumn
---@field id string The key of the column's cells in each row.
---@field title? string The title in the header; the id if missing.
---@field width? number A fixed width; the widest cell's otherwise.

---A row of a table: its cells' values by column id.
---@alias ui.TableRow table<string, any>

---@class ui.TableProps: ui.Props
---@field id string
---@field selected? integer|integer[] The chosen row's number, from 1, or a list of the chosen ones' numbers.
---@field onSelect? fun(row: integer, how: "toggle"|"range"|nil, between: integer[]?) As a list's.
---@field onActivate? fun(row: integer)
---@field onMenu? fun(row: integer?, x: number, y: number)
---@field onDrag? fun(row: integer): string?, any As a list's.
---@field onDrop? fun(row: integer?, where: "before"|"after"|"into"|nil, dataType: string, value: any) As a list's.
---@field onDragOver? fun(row: integer?, where: "before"|"after"|nil, dataType: string, value: any): boolean|"before"|"after"|"into"|nil As a list's.
---@field onSort? fun(column: string) A click on a title gives its column's id.
---@field sort? string The id of the column the rows are sorted by, which shows an arrow.
---@field ascending? boolean The arrow points up.

---A node of a tree; one with children, even none, has an arrow.
---@class ui.TreeNode
---@field id any
---@field label? ui.Text The id as text if missing.
---@field color? ecs.Color
---@field children? ui.TreeNode[]
---@field marks? ui.TreeMark[] Short labels at the end of the node's row, each on or off, such as an eye and a lock.

---A mark of a tree's node: a click on it gives the tree's onMark its new state, and does not choose the node.
---@class ui.TreeMark
---@field id any
---@field label ui.Text What the row shows, such as an icon: in the accent colour while on, dimmed while off, as its icons are.
---@field on boolean
---@field tooltip? string
---@field keys? string The key combination that turns it too, which its tooltip names after its text.

---@class ui.TreeProps: ui.Props
---@field id string
---@field open? table<any, boolean> The open nodes, by id.
---@field selected? any The chosen node's id, or a list of the chosen ones' ids, each highlighted; Up and Down move from the first.
---@field onSelect? fun(id: any, how: "toggle"|"range"|nil, between: any[]?) As a list's, with ids; between holds the shown nodes from the first chosen one to it.
---@field onActivate? fun(id: any)
---@field onMenu? fun(id: any, x: number, y: number) A right click gives the node's id, or nil beside the nodes, and the place.
---@field onDrag? fun(id: any): string?, any As a list's, with the node's id.
---@field onDrop? fun(id: any, where: "before"|"into"|"after"|nil, dataType: string, value: any) Data dropped on a node gives its id, and where it landed: before it on its upper quarter, after it on its lower quarter, and into it between, or the place that onDragOver gave; nil and nil beside the nodes.
---@field onDragOver? fun(id: any, where: "before"|"into"|"after"|nil, dataType: string, value: any): boolean|"before"|"after"|"into"|nil As a list's, with the node's id.
---@field onToggle? fun(id: any, open: boolean) A click on an arrow gives the node's id and its new state.
---@field onMark? fun(id: any, mark: any, on: boolean) A click on a node's mark gives the node's id, the mark's id and its new state.

---A graph's bar: a number, or a list of numbers that stack.
---@alias ui.GraphValue number|number[]

---A line across a graph at a value, such as a frame's budget.
---@class ui.GraphLine
---@field value number
---@field color? ecs.Color "textDim" if missing.

---@class ui.GraphProps: ui.Props
---@field max? number The top of the scale; the highest bar's total if missing. A bar above it is cut.
---@field colors? ecs.Color[] The colour of each part of a stacked bar, in order, again from the first when a bar has more parts; "accent" if missing.
---@field lines? ui.GraphLine[] Lines across the graph at values.

---@class ui.SeparatorProps: ui.Props
---@field vertical? boolean A line up a row, rather than across a column.

---@class ui.TextProps: ui.Props
---@field wrap? boolean Breaks into lines at the width of its column, or its own width.
---@field wrapMark? string What a text that wraps shows, in "textDim", at the start of each line after a break, such as "↪ "; those lines start after it. A new line of the text's own is not marked.
---@field maxLines? integer The most lines it shows: it wraps, as with wrap = true, and the last line it shows ends with an ellipsis when more follow.
---@field gutter? string What a text shows, in "textDim", before its first line, such as the number of a code's line; each of its lines starts after the gutter's width, and a wrapMark stands at the gutter's end, where its text would.
---@field onLink? fun(link: string) A click on a span with a link, or Return while the text has the keys, gives the link.

---A part of a text in its own style, or a picture a line high: an icon, or an image. draw makes bold, italic, underline and struck text from
---the text's font.
---@class ui.Span
---@field text? string Empty for a picture.
---@field icon? string An icon, as ui.icon names it: eye, eyeClosed, lock or lockOpen, in the span's colour.
---@field image? string The path of an image, which fits a square a line high.
---@field bold? boolean
---@field italic? boolean
---@field mono? boolean In the mono font, for code.
---@field underline? boolean
---@field strike? boolean
---@field color? ecs.Color The span's colour; the text's if missing, and the accent for a link.
---@field link? string Underlined, in the accent; a click gives it to the text's onLink.

---A text, or a list of spans, each a text, a table, or an icon or an image element: what a text, a button, a tree's node and mark, and a
---section's title show, such as { ui.icon("lock"), " Locked" }.
---@alias ui.Text string|(string|ui.Span|ui.Element)[]

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
---@field onChange? fun(value: number) Gets each value of a drag, and of Left and Right.
---@field onSubmit? fun(value: number) Gets the value that a drag's release ends on, and each of Left and Right.

---A selector, tabs or a dropdown.
---@class ui.ChoiceProps: ui.Props
---@field onSelect? fun(choice: integer) Gets the number of the chosen one, from 1.
---@field folders? string A dropdown's separator of folders, such as ".": its choices are paths, shown a folder at a time.

---A field; it needs an id, which keeps what is typed.
---@class ui.FieldProps: ui.Props
---@field id string
---@field onChange? fun(text: string) Gets each edit.
---@field onSubmit? fun(text: string): boolean? Gets the text when Return is pressed; false keeps typing.
---@field valid? boolean false marks the text as wrong.
---@field placeholder? string The text an empty field shows, dimmed, until it is typed in, such as Search.

---@class ui.NumberProps: ui.Props
---@field id string
---@field min? number
---@field max? number
---@field step? number
---@field onChange? fun(value: number) Gets the number at each edit that is one.
---@field onSubmit? fun(value: number) Gets the number when Return is pressed.

---@class ui.KeyProps: ui.Props
---@field id string
---@field onChange? fun(combination: string)

---A path; it needs an id, which keeps what is typed.
---@class ui.SearchProps: ui.Props
---@field id? string The field's id; "search" if missing.
---@field onChange? fun(query: string) Gets each edit, and "" when the search is cleared.
---@field onSubmit? fun(query: string): boolean? Gets the query when Return is pressed.
---@field placeholder? string The text the empty field shows; "Search" if missing.
---@field results? string The id of the list that shows what matches; Up and Down choose in it while the search has the keys.

---@class ui.PathProps: ui.FieldProps
---@field dialog? "openFile"|"saveFile"|"openFolder" The file dialog its button opens; "openFile" if missing.
---@field pattern? string The extensions the dialog shows, such as "ttf;otf"; every file if missing.

---An element as a plugin in C describes it, which ui.showDescription builds with the element function of its type.
---@class ui.Description
---@field type string The element's type, which names its function, such as "column" or "button".
---@field props? ui.Props The element's props; its callbacks are C functions, onDrag, onDragOver, onDrop and onMenu as tables of caller and token, or none; a column or row with clickable = true is clicked like a button, onActivate = true
---and onSort = true ask a list, table or tree for those callbacks, and a field with valid = false refuses Return.
---@field value? boolean|number|string The value its function takes first: a text, a label, a state, a number, a path, a section's title or a colour.
---@field choices? string[] A selector's, tabs' or dropdown's choices.
---@field columns? ui.TableColumn[] A table's columns.
---@field rows? ui.TableRow[] A table's rows.
---@field nodes? ui.TreeNode[] A tree's nodes.
---@field open? boolean|table<any, boolean> Whether a section is open, or a tree's open nodes, by id.
---@field selected? any A tree's chosen node's id, or the number of a selector's, tabs' or dropdown's chosen one.
---@field children? ui.Description[] A container's children.

---The element under the pointer, which ui.inspected gives.
---@class ui.Inspected
---@field type string The element's type, such as "button".
---@field id string The element's id: its own, or the one ui gave it from its place.
---@field x number
---@field y number
---@field width number
---@field height number

---What a callback of a described element got, which ui.take gives.
---@class ui.Taken
---@field value boolean|number|string|nil The callback's first argument: a new state, number, text or choice, an item's number, or a node's or column's id.
---@field open? boolean onToggle's second argument: whether the node is open now.

---An entry of a menu that ui.menu opens, or a line between entries.
---@class ui.MenuEntry
---@field label? string What the entry says.
---@field line? boolean True makes the entry a line between entries, which has nothing else.
---@field keys? string The keys that run the same, shown at the entry's right, such as "Ctrl+D".
---@field enabled? boolean False shows the entry dimmed, and it does not run; true if missing.
---@field onClick? fun() Runs when the entry is clicked, or chosen with Return; the menu closes first.
---@field entries? ui.MenuEntry[] A submenu's entries, which open in place of the menu's; the entry has no onClick then.
