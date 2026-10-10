# OpenECS-std design

How OpenECS's standard plugins and first-party Lua plugins are built. They follow OpenECS's design and conventions: [OpenECS DESIGN.md](https://github.com/omerfuyar/OpenECS/blob/dev/DESIGN.md), which this file names as "OpenECS DESIGN".

## Contents

1. Rules
2. draw
3. ui
4. tty
5. The settings window
6. The launcher

## 1. Rules

- The plugins follow the rules of standard plugins (OpenECS DESIGN 20).
- Each plugin is a folder of `plugins/`, with its manifest. `presets/` holds first-party presets, and `tests/` the tests of these plugins, with their own presets and test plugins.
- `build.c` is the build of this repository. OpenECS's build includes it when this repository is checked out in its `std/` folder (OpenECS DESIGN 17.7): it builds each plugin into `bin/plugins/`, copies the presets into `bin/presets/`, and, with `--tests`, the tests beside OpenECS's own.
- Tests and test plugins have names that OpenECS's tests do not use, because both are copied into one folder.
- Plugins name their dependencies in their manifests and get their functions with `require` (OpenECS DESIGN 10.5). Their signatures name their parameters, for definition files (OpenECS DESIGN 10.10).

## 2. draw

- The `draw` plugin draws shapes, text and images into the pixels surface of another plugin's panel. A panel's `Draw` passes its surface handle (OpenECS DESIGN 10.6) to draw's functions.
- Positions and sizes are in layout units; draw multiplies them by the surface's scale. Colours are ARGB integers.
- It is a native plugin. It calls SDL3 and SDL3_ttf itself (OpenECS DESIGN 17.2): it wraps the surface's pixels in an SDL surface for each call, and draws text with one SDL3_ttf surface text engine, which keeps the glyphs it has drawn.
- Its font is the core's `ecs.font`, at the size the caller asks for; a relative path starts at the executable's folder.
- It reads image files, PNG, JPG or BMP, with SDL, the first time they are drawn, and keeps them until it shuts down. A relative path starts at the executable's folder; a plugin draws its own images by its folder (OpenECS DESIGN 11.3). A file that cannot be read is reported once.
- A surface can have a clip rectangle: drawing into the surface stays inside it until `unclip`. A plugin that clips unclips before its `Draw` returns.
- Its functions:

  | Function         | Does                                                                                                                           |
  | ---------------- | ------------------------------------------------------------------------------------------------------------------------------ |
  | `draw.fill`      | Fills a rectangle with a colour, blending by its alpha.                                                                        |
  | `draw.outline`   | Draws the edge of a rectangle, in a thickness and a colour.                                                                    |
  | `draw.text`      | Draws text at `x, y`, the top left of its line, in a size and a colour, and gives its width.                                   |
  | `draw.measure`   | Gives the width and the line height of a text in a size, without drawing it.                                                   |
  | `draw.color`     | Reads a colour: `"#RRGGBB"`, `"#RRGGBBAA"`, or the name of a colour of the core's theme, such as `"text"` for `ecs.colorText`. |
  | `draw.image`     | Draws an image file stretched to a rectangle. Gives false if the file cannot be read.                                          |
  | `draw.imageSize` | Gives an image file's width and height in pixels. Gives false if the file cannot be read.                                      |
  | `draw.clip`      | Limits later drawing into the surface to a rectangle. A new clip replaces the old one.                                         |
  | `draw.unclip`    | Lets drawing reach the whole surface again.                                                                                    |

- Their signatures are in the plugin's code and its definition files (OpenECS DESIGN 10.10).
- A colour that cannot be read is reported, and gives opaque magenta, so the mistake shows.

## 3. ui

- The `ui` plugin builds user interfaces from elements, lays them out, draws them with draw (2), and calls their callbacks. It is a Lua plugin, so it keeps the Lua functions it is given as callbacks.
- A panel describes its elements again each time it draws: `ui.show(panel, surface, root)` lays them out in the surface and draws them. Its `event` passes each event to `ui.event(panel, event)`, which handles the events that reach an element and gives true for them. Its `destroy` calls `ui.forget(panel)`.
- An element is a table that a function of ui makes, with the element's properties:

  | Element                            | Is                                                                                                                                                                                                                           |
  | ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `ui.column(props, children)`       | Children below each other.                                                                                                                                                                                                   |
  | `ui.row(props, children)`          | Children beside each other.                                                                                                                                                                                                  |
  | `ui.scroll(props, children)`       | Children below each other, which scroll, with a scrollbar when they do not fit. It needs an `id`.                                                                                                                            |
  | `ui.text(text, props)`             | A line of text, cut with an ellipsis when it does not fit.                                                                                                                                                                   |
  | `ui.button(label, props)`          | A button. `onClick()` runs when it is clicked.                                                                                                                                                                               |
  | `ui.check(on, props)`              | A check box, with an optional `label`. `onChange(on)` gets the new state.                                                                                                                                                    |
  | `ui.choice(value, choices, props)` | One of a list of texts; a click moves to the next. `onChange(value)` gets it.                                                                                                                                                |
  | `ui.field(text, props)`            | A line of text that the user types. It needs an `id`. `onChange(text)` gets each edit, and `onSubmit(text)` the text when Return is pressed; `onSubmit` gives false to keep typing. `valid = false` marks the text as wrong. |
  | `ui.key(combination, props)`       | A key combination. It needs an `id`. A click, then a key press, changes it: `onChange(combination)`.                                                                                                                         |
  | `ui.image(path, props)`            | An image file, at its size.                                                                                                                                                                                                  |
  | `ui.space(props)`                  | Empty room, which grows.                                                                                                                                                                                                     |

- Properties of every element: `id`, `width` and `height` (a fixed size), `grow` (a share of the room left along its parent's direction), `stretch` (whether it takes its parent's whole cross size), `padding`, `gap` and `align` (`"start"`, `"center"` or `"end"`, for containers), `background`, `hoverBackground` and `border` (colours, or names of theme colours), `color` and `size` (for text). A column or a row with `onClick` is clicked like a button.
- An element without an `id` gets one from its place in the tree, so it keeps its state from one draw to the next as long as the tree keeps its shape.
- Layout: each element gets the size it needs; then the room left along a container is shared by the children that grow, by their shares. Containers, and fields, choices and key elements in a column, stretch across their parent; other elements keep their size, aligned by the parent's `align`.
- A click is a press and a release of the left button on the same element. The element under the pointer shows it. The wheel scrolls the scroll element under the pointer, and the scrollbar's thumb can be dragged; a press beside the thumb moves it there.
- A field or key element that has the keys takes text input (OpenECS DESIGN 4.6): text and Backspace edit a field, Return submits it, and Escape, or a click elsewhere, drops what was typed. `ui.edit(panel, id)` gives an element the keys, as a click does, and `ui.focus(panel)` gives the id of the element that has them.
- `ui.rect(panel, id)` gives an element's rectangle in the last layout, and `ui.reveal(panel, scroll, child)` scrolls a scroll element so a child shows.
- Positions are in layout units, and colours are the core's theme's. ui fills the surface with the theme's background first.

## 4. tty

- The `tty` plugin keeps grids of characters and draws them into the pixels surface of another plugin's panel, at any font size. Terminals, consoles and logs build on it.
- It is a native plugin. A grid is a handle of type `tty.grid` (OpenECS DESIGN 10.6): `tty.new(columns, rows)` makes one, and Lua's garbage collector, or the plugin that made it, frees it.
- A cell holds one character, a Unicode code point, and its foreground and background colours, ARGB. A grid starts with every cell empty: a space in light grey on the core's dark background.
- `tty.put(grid, column, row, text)` writes text from a cell to the right, and cuts it at the grid's edge. `tty.print(grid, text)` writes at the cursor, as a terminal does: a new line moves the cursor to the start of the next row, a full row goes on in the next one, a tab moves to the next column that is a multiple of 8, and writing past the last row moves every row up by one.
- `tty.colors(grid, foreground, background)` sets the colours that later writing uses. `tty.clear(grid)` empties every cell and moves the cursor to the top left. `tty.setCursor` and `tty.cursor` move the cursor and give where it is. `tty.resize` changes a grid's size and keeps the cells that still fit, at the same places, and `tty.size` gives it.
- `tty.draw(surface, grid, x, y, size, cursor)` draws a grid with its top left at `x, y`, in layout units, with the font at a size; with `cursor`, a line under the cursor's cell shows it. `tty.cell(size)` gives the size of a cell, and `tty.fit(width, height, size)` how many columns and rows fit a rectangle, so a panel can make its grid as large as itself.
- The font is the setting `tty.font`: a monospace TrueType file, whose relative path starts at the executable's folder. Empty, its default, means the font tty ships with in its folder, Roboto Mono, under the SIL Open Font License, which is beside it.
- It draws each character once in each colour and size, and keeps the drawing for later cells, up to 4096 of them; then it starts again.

## 5. The settings window

- The first-party Lua plugin `settings` is built with ui (3). The core's settings file loads it in every tool (OpenECS DESIGN 12.3), and binds `,` after the prefix to `settings.open` (OpenECS DESIGN 7.8), which opens the window as a panel of type `settings.window`, or shows the one that is open.
- The window lists every declared setting under a title for its owner: the core's first, then each plugin's, each by name. A row shows the setting's name and description on its left, and a control with its value in effect on its right. The two sides share the row's width, so the controls line up and grow with the window.
- The control depends on the type: a check box for a `bool`, a choice for a `choice`, a key element for a `key`, a field with the colour beside it for a `color`, and a field for the other types. A `list` or a `table` shows as a Lua table, such as `{ 1, 2 }`.
- A field's text must read as a value of its setting's type, and shows as wrong while it does not: a number, a colour such as `#18191C`, or, for a `list` or a `table`, a Lua table, which is read with no access to anything. Return saves a text that can be read; a text that cannot is reported, and typing goes on.
- The window changes values in the user's settings file (OpenECS DESIGN 12.2): a step back or forward toggles a `bool`, cycles a `choice`, and adds or takes 1 from an `integer` or a `number`.
- The functions `settings.up`, `settings.down`, `settings.previous`, `settings.next` and `settings.edit` choose a setting and change it: `settings.edit` toggles a `bool`, cycles a `choice`, and gives the other controls the keys. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down, Left, Right and Return. While a field has the keys, they leave it alone, and `settings.edit` saves what is typed, as Return does.
- The list scrolls under the window's title (3). Choosing a setting with the keys scrolls it into view, and a click chooses the setting under it.
- The panel's saved state is the chosen setting.

## 6. The launcher

- When the command line names no preset and no session, OpenECS starts with the first-party preset `launcher`. Its app id is `openecs.launcher`, it sets `listed = false`, and it shows one panel of the first-party Lua plugin `launcher`.
- The panel type `launcher.list` lists the presets, then the saved sessions (OpenECS DESIGN 13.7). Presets whose tool has a last session come first, the most recently used first; the others follow by name. Sessions are listed newest first.
- An entry shows the tool's name. A session's entry also shows the file's name and when it was saved.
- The panel reads both lists when it is created and each time it is shown again.
- The functions `launcher.up`, `launcher.down` and `launcher.open` choose an entry and open it. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down and Return. A click on an entry opens it, and the wheel scrolls the list.
- The panel's saved state is the chosen entry, so the launcher chooses it again at the next start.
- It is built with ui (3).
- A preset opens with `ecs.session.openPreset`, a session with `ecs.session.open` (OpenECS DESIGN 13.5).

---

## Glossary

**Ellipsis.** The three dots, …, that end a text that is cut.

**Thumb (scrollbar).** The part of a scrollbar that shows which part of the content is in view, and that the user drags to scroll.
