# OpenECS-std design

How OpenECS's standard plugins and first-party Lua plugins are built. They follow OpenECS's design and conventions: [OpenECS DESIGN.md](https://github.com/omerfuyar/OpenECS/blob/dev/DESIGN.md), which this file names as "OpenECS DESIGN".

## Contents

1. Rules
2. draw
3. ui
4. tty
5. image
6. audio
7. net
8. gltf
9. fs
10. The settings window
11. The launcher

## 1. Rules

- The plugins follow the rules of standard plugins (OpenECS DESIGN 20).
- Each plugin is a folder of `plugins/`, with its manifest. `presets/` holds first-party presets, and `tests/` the tests of these plugins, with their own presets and test plugins.
- Plugins name their dependencies in their manifests and get their functions with `require` (OpenECS DESIGN 10.5). Their signatures name their parameters, for definition files (OpenECS DESIGN 10.10).
- A plugin needs nothing but OpenECS: it links no library. Its calls to SDL and SDL_ttf are resolved against the libraries beside OpenECS (OpenECS DESIGN 17.2), and the code of any other library it uses is compiled into it. Copying a plugin's folder into another OpenECS's `plugins/` is all it takes to install it.
- The libraries are submodules in `dependencies/`, which is never edited: stb_image and stb_vorbis from stb, nanosvg, dr_libs, cgltf and SDL_net. A plugin includes a library's code in one of its C files, after defining what the library asks for, such as `STB_IMAGE_IMPLEMENTATION`, so the code is compiled into the plugin and its functions stay inside it.
- SDL_image and SDL_mixer are not used: their own submodules bring large codec libraries, and these plugins need only what the small ones read.

### 1.1 Building

- `shuild.c` is the build of this repository. It runs inside a checkout of OpenECS, in its `std/` folder. OpenECS's build compiles it with shu and shuild from OpenECS's `dependencies/` into `shuild.ignore` in this folder, and runs it with the build type and OpenECS's build folder (OpenECS DESIGN 17.7).
- It builds each plugin into `bin/plugins/` and copies the presets into `bin/presets/`. With `--tests`, it copies `tests/` into `std/tests/` beside `bin/`, apart from OpenECS's own tests, and builds the test plugins there.
- A plugin's C files compile into its native library with OpenECS's flags: its warnings, and in Debug its static analyzer and sanitizers. The plugin sees OpenECS's plugin interface and SDL from the build's `include/` folder.
- `shuild.c` names, for each plugin that compiles a library in, the folders of `dependencies/` that its C files include. They are system include folders, so the library's warnings stay out of the build's. The static analyzer skips these plugins, because it takes too much memory on the libraries in them; the warnings and sanitizers stay.

### 1.2 Tests

- This repository's tests run on their own: OpenECS's `.github/scripts/test.sh` with `std` runs every test in `std/tests/`. Its checks build OpenECS's `dev` with this repository in `std/`, and run these tests only.
- A test loads only the plugins its preset names (OpenECS DESIGN 17.5), so a test's preset names every plugin the test uses, such as `settings` for the settings window.

### 1.3 Plugins

- A plugin keeps C allocations in SDL's allocator, and gives its libraries SDL's allocator where they take one.
- A relative path that a plugin's function reads starts at the executable's folder; a plugin reads its own files by its folder (OpenECS DESIGN 11.3).
- Lua plugins have Lua's standard libraries (OpenECS DESIGN 9.6): `io` reads and writes files, and `os` renames and removes them and gives the time. A standard plugin does not offer what they already do; fs (9) adds what they cannot.

## 2. draw

- The `draw` plugin draws shapes and text into the pixels surface of another plugin's panel. A panel's `Draw` passes its surface handle (OpenECS DESIGN 10.6) to draw's functions.
- Positions and sizes are in layout units; draw multiplies them by the surface's scale. Colours are ARGB integers.
- It is a native plugin. It calls SDL3 and SDL3_ttf itself (OpenECS DESIGN 17.2): it wraps the surface's pixels in an SDL surface for each call, and draws text with one SDL3_ttf surface text engine, which keeps the glyphs it has drawn.
- Its font is the core's `ecs.font`, at the size the caller asks for; a relative path starts at the executable's folder.
- A surface can have a clip rectangle: drawing into the surface stays inside it until `unclip`. A plugin that clips unclips before its `Draw` returns.
- Its functions:

  | Function       | Does                                                                                                                           |
  | -------------- | ------------------------------------------------------------------------------------------------------------------------------ |
  | `draw.fill`    | Fills a rectangle with a colour, blending by its alpha.                                                                        |
  | `draw.outline` | Draws the edge of a rectangle, in a thickness and a colour.                                                                    |
  | `draw.text`    | Draws text at `x, y`, the top left of its line, in a size and a colour, and gives its width.                                   |
  | `draw.measure` | Gives the width and the line height of a text in a size, without drawing it.                                                   |
  | `draw.color`   | Reads a colour: `"#RRGGBB"`, `"#RRGGBBAA"`, or the name of a colour of the core's theme, such as `"text"` for `ecs.colorText`. |
  | `draw.clip`    | Limits later drawing into the surface to a rectangle. A new clip replaces the old one.                                         |
  | `draw.unclip`  | Lets drawing reach the whole surface again.                                                                                    |

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
  | `ui.image(path, props)`            | An image file, at its size, drawn with image (5).                                                                                                                                                                            |
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

## 5. image

- The `image` plugin reads image files and draws them into the pixels surface of another plugin's panel. It is a native plugin.
- stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files; nanosvg reads SVG files. A GIF gives its first frame.
- `image.load(path)` reads a file the first time and gives a handle of type `image.picture` (OpenECS DESIGN 10.6); later calls with the same path give the same picture. Pictures are kept until the plugin shuts down. A file that cannot be read is reported and gives nil, and is tried again at the next call.
- `image.size(picture)` gives a picture's width and height in pixels; an SVG file's are its own size at 96 dots per inch. `image.pixel(picture, x, y)` gives a pixel's colour, ARGB.
- `image.draw(surface, picture, x, y, width, height)` draws a picture stretched to a rectangle, in layout units, blending by its alpha. An SVG file is drawn again at the size it is drawn at, so it stays sharp; the last drawing is kept for the next one at that size.

## 6. audio

- The `audio` plugin reads sound files and plays them on the computer's default output. It is a native plugin.
- dr_wav, dr_mp3 and dr_flac from dr_libs read WAV, MP3 and FLAC files, and stb_vorbis reads Ogg Vorbis files. A file is read whole into 32-bit float samples.
- `audio.load(path)` reads a file the first time and gives a handle of type `audio.sound`; later calls with the same path give the same sound. Sounds are kept until the plugin shuts down. A file that cannot be read is reported and gives nil.
- `audio.play(sound, volume, loop)` plays a sound and gives a voice: a number above 0 that stands for this playing of it. A volume of 1 is the sound's own. A sound can play in many voices at once. It gives 0 when there is no output to play on.
- `audio.stop(voice)`, `audio.stopAll()`, `audio.playing(voice)` and `audio.volume(voice, volume)` act on voices. A voice that has ended is forgotten; its number is not given again.
- The output opens the first time a sound plays. Each voice is an SDL audio stream bound to it, which SDL converts to the output's format and mixes with the others; a voice gives its stream samples as the stream asks for them.
- Tests and definitions run with SDL's dummy audio driver (OpenECS DESIGN 17.5), so they make no sound.

## 7. net

- The `net` plugin makes TCP connections and servers with SDL_net, which is compiled into it. It is a native plugin. Nothing it does blocks the program: a plugin asks for the state of a connection, and takes what arrived, when it wants, such as from a timer.
- `net.connect(host, port)` starts a connection and gives a handle of type `net.connection`. The host's name resolves in the background. `net.status(connection)` gives 1 once it is connected, 0 while it waits, and -1 if it failed or closed.
- `net.send(connection, data)` queues data to go out, and gives false if the connection has no socket yet or is closed. `net.receive(connection, most)` takes what has arrived, at most a number of bytes, and gives an empty buffer if nothing has. The buffer is kept until the next receive on the connection. A connection that the other side closed fails.
- `net.listen(port)` waits for connections on a port of every address of this computer and gives a handle of type `net.server`, or nil if it cannot; SDL_net listens on every family of addresses at once, and fails if one is missing, so then IPv4 alone is tried. `net.accept(server)` gives a connection that a client made, or nil.
- `net.close(connection)` closes a connection. Lua's garbage collector, or the plugin that made it, frees a connection or a server, and closes it.

## 8. gltf

- The `gltf` plugin reads glTF 2.0 models with cgltf: `.gltf` files with the files they name, and `.glb` files. It is a native plugin. It gives a model's parts and their data; drawing them is for the plugin that asks.
- `gltf.load(path)` reads a model and gives a handle of type `gltf.model`, which owns all of it. Each call reads the file again. A file that cannot be read, or that is not valid glTF, is reported and gives nil.
- `gltf.describe(model)` gives a table of the model's parts. Meshes, materials and nodes are lists, and they name each other by their numbers in those lists, from 1:

  | Field       | Holds                                                                                                                                                                                                                                                                                         |
  | ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `meshes`    | Each mesh's `name` and `primitives`. A primitive has a `mode` (`"triangles"`, `"triangleStrip"`, `"triangleFan"`, `"points"`, `"lines"`, `"lineLoop"` or `"lineStrip"`), a `material`, the names of its vertex `attributes`, such as `POSITION`, and its numbers of `vertices` and `indices`. |
  | `materials` | Each material's `name`, its base `color`, as four numbers from 0 to 1, and its `texture`, the base colour's image file, when it is a file.                                                                                                                                                    |
  | `nodes`     | Each node's `name`, its `mesh`, its `children`, and its `matrix`: 16 numbers, column by column, that place it in its parent.                                                                                                                                                                  |
  | `roots`     | The nodes that the model's scene starts from.                                                                                                                                                                                                                                                 |

- `gltf.attribute(model, mesh, primitive, name)` gives a vertex attribute of a primitive as 32-bit floats, one after another for each vertex: three for a `POSITION`, two for a `TEXCOORD_0`. Whole numbers and sparse data are converted. `gltf.indices(model, mesh, primitive)` gives its indices as 32-bit whole numbers, from 0; a primitive without indices gives its vertices in order. Both give an empty buffer for a missing mesh, primitive or attribute, and their buffer is kept until the next of these calls on the model.
- In Lua a buffer is a string, which `string.unpack` reads, such as `string.unpack("<fff", positions)` for the first position.

## 9. fs

- The `fs` plugin does with files and folders what Lua's `io` and `os` cannot: it lists folders, tells what a path is, makes folders, and copies, moves and removes files and folders. It is a native plugin, built on SDL's filesystem functions, so it needs no library.
- `fs.info(path)` gives a table of what a path is: its `type`, `"file"`, `"folder"` or `"other"`, its `size` in bytes, and `modified`, when it last changed, in seconds since 1970. A path that does not exist gives nil.
- `fs.list(folder, pattern)` gives the entries of a folder whose names match a pattern, sorted by name: each a table of its `name` and the fields of `fs.info`. In a pattern, `*` stands for any characters but `/`, and `?` for one; an empty pattern matches every name. The entries of the folder's folders are not listed. A folder that cannot be read gives nil.
- `fs.makeFolder(path)` makes a folder, and the folders above it that are missing. `fs.copy(from, to)` copies a file, replacing what is at the new path. `fs.move(from, to)` moves or renames a file or a folder. `fs.remove(path)` removes a file or an empty folder. Each gives false when it fails.
- `fs.folder(name)` gives a folder of the computer, ending with `/`: `executable`, the executable's folder, or one of the user's folders, `home`, `desktop`, `documents`, `downloads`, `music`, `pictures`, `videos`, `screenshots` or `templates`. A folder the system does not have gives nil.
- Reading and writing what files hold is Lua's `io` in Lua, and SDL's `SDL_LoadFile` and `SDL_SaveFile` in C.

## 10. The settings window

- The first-party Lua plugin `settings` is built with ui (3). The core's settings file loads it in every tool (OpenECS DESIGN 12.3), and binds `,` after the prefix to `settings.open` (OpenECS DESIGN 7.8), which opens the window as a panel of type `settings.window`, or shows the one that is open.
- The window lists every declared setting under a title for its owner: the core's first, then each plugin's, each by name. A row shows the setting's name and description on its left, and a control with its value in effect on its right. The two sides share the row's width, so the controls line up and grow with the window.
- The control depends on the type: a check box for a `bool`, a choice for a `choice`, a key element for a `key`, a field with the colour beside it for a `color`, and a field for the other types. A `list` or a `table` shows as a Lua table, such as `{ 1, 2 }`.
- A field's text must read as a value of its setting's type, and shows as wrong while it does not: a number, a colour such as `#18191C`, or, for a `list` or a `table`, a Lua table, which is read with no access to anything. Return saves a text that can be read; a text that cannot is reported, and typing goes on.
- The window changes values in the user's settings file (OpenECS DESIGN 12.2): a step back or forward toggles a `bool`, cycles a `choice`, and adds or takes 1 from an `integer` or a `number`.
- The functions `settings.up`, `settings.down`, `settings.previous`, `settings.next` and `settings.edit` choose a setting and change it: `settings.edit` toggles a `bool`, cycles a `choice`, and gives the other controls the keys. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down, Left, Right and Return. While a field has the keys, they leave it alone, and `settings.edit` saves what is typed, as Return does.
- The list scrolls under the window's title (3). Choosing a setting with the keys scrolls it into view, and a click chooses the setting under it.
- The panel's saved state is the chosen setting.

## 11. The launcher

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
