# OpenECS-std design

How OpenECS's standard plugins and first-party Lua plugins are built. They follow OpenECS's design and conventions: [OpenECS DESIGN.md](https://github.com/omerfuyar/OpenECS/blob/dev/DESIGN.md), which this file names as "OpenECS DESIGN".

## Contents

1. Rules
2. draw
3. mixer
4. textgrid
5. ui
6. image
7. audio
8. gltf
9. net
10. fs
11. web
12. The settings window
13. The launcher
14. The panel list
15. gpu

## 1. Rules

- The plugins follow the rules of standard plugins (OpenECS DESIGN 20): each has one job, executors run things and loaders turn files into the executors' objects, and those objects back into files, and each is a generic building block.
- Each plugin is a folder of `plugins/`, with its manifest. `presets/` holds first-party presets.
- Plugins name their dependencies in their manifests and get their functions with `require` (OpenECS DESIGN 10.5). Their signatures name their parameters, for definition files (OpenECS DESIGN 10.10).
- In Lua, the functions that act on one of a plugin's handles are also methods of the handle (OpenECS DESIGN 10.6): `connection:send(data)` is `net.send(connection, data)`. A method's name leaves out the type, so `image:size()` is `draw.imageSize(image)`, `watch:stop()` is `fs.unwatch(watch)` and `socket:close()` is `web.socketClose(socket)`. C calls the functions. The definition files list each type's methods.
- A plugin needs nothing but OpenECS: it links no library. Its calls to SDL and SDL_ttf are resolved against the libraries beside OpenECS (OpenECS DESIGN 17.2), and the code of any other library it uses is compiled into it. Copying a plugin's folder into another OpenECS's `plugins/` is all it takes to install it.
- The libraries are submodules in `dependencies/`, which is never edited: stb_image and stb_vorbis from stb, nanosvg, dr_libs, cgltf, SDL_net, libcurl and mbedTLS. A plugin includes a library's code in one of its C files, after defining what the library asks for, such as `STB_IMAGE_IMPLEMENTATION`, so the code is compiled into the plugin and its functions stay inside it.
- A library of many C files, such as libcurl and mbedTLS, is compiled into a static archive instead, and the plugin's library takes the archive's code. Its files are compiled with hidden symbols, so its functions stay inside the plugin too.
- `dependencies/other/` holds the glue headers that configure a library in place of its own configure script, as OpenECS's `dependencies/other/` does.
- Submodules are at fixed commits, as OpenECS DESIGN 17.4 says. mbedTLS is at a release tag of its long-term branch, 3.6, because its branch lacks the C files that its release makes with Python scripts.
- SDL_image and SDL_mixer are not used: their own submodules bring large codec libraries, and these plugins need only what the small ones read.

### 1.1 Building

- `shuild.c` is the build of this repository. It runs inside a checkout of OpenECS, in its `std/` folder. OpenECS's build compiles it with shu and shuild from OpenECS's `dependencies/` into `shuild.ignore` in this folder, and runs it with the build type and OpenECS's build folder (OpenECS DESIGN 17.7).
- It builds each plugin into `bin/plugins/` and copies the presets into `bin/presets/`.
- Its build files are kept in `.shu/` here, and copies keep their files' times, so a second build compiles only the plugins whose files changed.
- A plugin's C files compile into its native library with OpenECS's flags: its warnings, and in Debug its static analyzer and sanitizers. The plugin sees OpenECS's plugin interface and SDL from the build's `include/` folder.
- `shuild.c` names, for each plugin that compiles a library in, the folders of `dependencies/` that its C files include. They are system include folders, so the library's warnings stay out of the build's. The static analyzer skips these plugins, because it takes too much memory on the libraries in them; the warnings and sanitizers stay.
- `shuild.c` also names the libraries that a plugin takes as archives. Like OpenECS's dependencies, they are compiled without the warnings, the analyzer and the sanitizers (OpenECS DESIGN 17.3), into `.shu/`. The plugin's own files keep the warnings and the sanitizers, and the analyzer too unless they include a library's code, as net's include SDL_net's. libcurl is compiled as one file that includes all of its C files, as its own unity builds are, because shuild finds the headers a file includes without the build's definitions, and libcurl's headers need them.

### 1.2 Tests

- The examples of OpenECS-examples are the tests of these plugins (OpenECS DESIGN 17.5): each plugin has an example that shows it, and that example's test checks it.
- This repository's checks build OpenECS's `dev` with this repository in `std/` and the examples, and run the examples' tests.
- A test loads only the plugins its preset names (OpenECS DESIGN 17.5), so an example's preset names every plugin it uses, such as `settings` for the settings window.

### 1.3 Plugins

- A plugin keeps C allocations in SDL's allocator, and gives its libraries SDL's allocator where they take one.
- A relative path that a plugin's function reads starts at the executable's folder; a plugin reads its own files by its folder (OpenECS DESIGN 11.3).
- Lua plugins have Lua's standard libraries (OpenECS DESIGN 9.6): `io` reads and writes files, and `os` renames and removes them and gives the time. A standard plugin does not offer what they already do; fs (10) adds what they cannot.
- An object that a plugin gives is a handle (OpenECS DESIGN 10.6): Lua's garbage collector frees it when no one uses it. A plugin whose objects hold much memory, such as images, sounds, models and grids, also has a `free` function, which frees one at once and makes its handles invalid.
- Names follow OpenECS DESIGN 9.9: a plugin declares by local names, and colours are texts in Lua and `ECSColor` in C (OpenECS DESIGN 5.2).

### 1.4 How panels draw

- The standard plugins draw on the CPU, into the pixels surface of the panel that calls it (OpenECS DESIGN 5.1): 32-bit ARGB pixels with premultiplied alpha. The core then uploads the surface to a texture and shows it with SDL's GPU renderer, with the core's own interface over it (OpenECS DESIGN 5.3). gpu is the one that draws on the GPU, into GPU surfaces (15).
- draw (2) is the one plugin that writes those pixels: shapes, images and text. textgrid (4) and ui (5) draw through draw, so a font or a way of blending is in one place.
- A panel that draws with the GPU instead uses a GPU surface and the sdl plugin's device (OpenECS DESIGN 5.4); no standard plugin does yet.

## 2. draw

- The `draw` plugin is the executor of 2D drawing: it draws shapes, images and text into the pixels surface of another plugin's panel. A panel's `Draw` passes its surface handle (OpenECS DESIGN 10.6) to draw's functions.
- Positions and sizes are in layout units; draw multiplies them by the surface's scale. Colours have the type `color` (OpenECS DESIGN 10.2), and draw blends them by their alpha.
- It is a native plugin. It draws shapes with its own small rasterizer, and calls SDL3 and SDL3_ttf for images and text (OpenECS DESIGN 17.2): it wraps the surface's pixels in an SDL surface, and draws text with one SDL3_ttf surface text engine, which keeps the glyphs it has drawn.
- Lines, circles and rounded rectangles have smooth edges: each pixel at an edge gets the part of the pixel the shape covers, from its distance to the edge. Rectangles keep sharp edges on whole pixels, and triangles fill each pixel whose centre is inside them, so the triangles of a mesh meet without seams.
- A surface can have a clip rectangle: drawing into the surface stays inside it until `unclip`. A plugin that clips unclips before its `Draw` returns.
- Its functions:

  | Function                                   | Does                                                                                                                                       |
  | ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------ |
  | `fill`, `outline`                          | Fill a rectangle, or draw its edge in a thickness.                                                                                         |
| `rounded`, `roundedOutline`                | Fill a rectangle whose corners are rounded by a radius, or draw its edge in a thickness.                                                   |
  | `line`, `circle`                           | Draw a line in a thickness, and a circle: a ring of a thickness, or filled with a thickness of 0.                                          |
  | `triangles`, `lines`                       | Fill triangles, or draw lines, between points: a buffer of x and y floats, and indices, three or two 32-bit numbers each, or none for points in order. |
  | `newImage`, `imageSize`, `imagePixel`, `imagePixels`, `freeImage` | Make an image of type `draw.image` from ARGB pixels, give its size, a pixel or all its pixels, and free it.          |
  | `image`                                    | Draw an image stretched to a rectangle.                                                                                                    |
  | `imageTransformed`                         | Draw an image stretched to a rectangle, turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour: white leaves it as it is, and a lower alpha fades it. Its edges are smooth. |
  | `font`                                     | Give a font of type `draw.font`: `"ui"`, the core's `ecs.font`; `"mono"`, a monospace font; or the path of a TrueType file.                |
  | `text`, `measure`                          | Draw text in a font at `x, y`, the top left of its line, in a size and a colour, and give its width; or give its width and line height.    |
  | `clip`, `unclip`                           | Limit later drawing into the surface to a rectangle, or let it reach the whole surface again.                                              |

- `"mono"` is the setting `draw.monoFont`: a monospace TrueType file, whose relative path starts at the executable's folder. Empty, its default, means the font draw ships with in its `fonts/` folder, Roboto Mono, under the SIL Open Font License, which is beside it.
- A font is opened at each size asked for, and kept; a change of `ecs.font` or `draw.monoFont` opens the fonts again.
- Their signatures are in the plugin's code and its definition files (OpenECS DESIGN 10.10).

## 3. mixer

- The `mixer` plugin is the executor of sound: it plays sounds on the computer's default output, many at once. It is a native plugin, built on SDL's audio streams.
- `mixer.newSound(channels, rate, samples)` makes a sound of type `mixer.sound` from 32-bit float samples, frame after frame. Loaders, such as audio (7), make sounds this way.
- `mixer.play(sound, volume, loop)` plays a sound and gives a voice: a number above 0 that stands for this playing of it. A volume of 1 is the sound's own. A sound can play in many voices at once. It gives 0 when there is no output to play on.
- `mixer.newStream(channels, rate)` makes a stream: a sound whose samples a plugin gives while it plays, such as a synthesizer's. `mixer.queue(stream, samples)` adds samples to its end, `mixer.queued(stream)` gives the seconds that wait to play, and `mixer.finish(stream)` tells that no more come, so its voice ends once it has played them. A voice that runs out of samples before then waits, silent, for more. A stream plays in one voice at a time, and does not loop.
- `mixer.pan(voice, pan)` moves a voice from left, -1, to right, 1: the far channel's gain falls toward 0, and at 0, the middle, both keep their whole gain. A mono sound plays in both channels, so it can be panned too.
- `mixer.stop(voice)`, `mixer.stopAll()`, `mixer.playing(voice)` and `mixer.volume(voice, volume)` act on voices, and `mixer.length(sound)` gives a sound's length in seconds. A voice that has ended is forgotten; its number is not given again.
- `mixer.filter(voice, lowpass, highpass)` filters one voice: `lowpass` cuts its sounds above a frequency in hertz, such as a sound behind a wall, and `highpass` those below one, such as a sound on a telephone. 0 turns a filter off. Each is a Butterworth biquad of the Audio EQ Cookbook, on each of the voice's channels, before SDL converts the voice to the output's rate.
- `mixer.echo(voice, delay, feedback, mix)` gives one voice an echo: what it played `delay` seconds ago, up to 2, comes back at `mix`, and each echo comes back again at `feedback` times as loud, up to 0.95. A delay or a mix of 0 takes the echo away. A voice with an echo plays on after its sound ends, until its echo is about a thousandth as loud, and 30 seconds at most; `mixer.playing` is true until then, and `mixer.stop` cuts it.
- `mixer.reverb(mix, size)` adds a reverb to everything that plays, as in a room. `mix` is how much of it is heard, from 0, none, to 1, and `size` is the room's, from 0 to 1: a larger room rings longer. It gives false when there is no output. It follows Freeverb: eight comb filters and four all-pass filters for each channel of the output, in SDL's post-mix callback, so it costs the same for any number of voices. As in Freeverb, samples that fade to almost nothing become 0, so a reverb that rings out over silence costs no more. A reverb turned on again starts silent, without what rang when it was turned off.
- `mixer.freeSound(sound)` frees a sound now; the voices that play it finish first, because each keeps the sound.
- `mixer.keepSound(sound)` keeps a sound alive, even when no one uses its handle, until `mixer.releaseSound(sound)`. A plugin that uses a sound after giving its handle away keeps it, as audio keeps the streams it feeds (OpenECS DESIGN 10.6).
- The output opens the first time a sound plays. Each voice is an SDL audio stream bound to it, which SDL converts to the output's format and mixes with the others; a voice gives its stream samples as the stream asks for them, and flushes the stream once it has given them all. A stream's samples wait in an SDL audio stream of their own, which the main thread fills and the voice empties.
- Tests and definitions run with SDL's dummy audio driver (OpenECS DESIGN 17.5), so they make no sound.

## 4. textgrid

- The `textgrid` plugin keeps grids of characters and draws them, with draw (2), into the pixels surface of another plugin's panel, at any font size. Terminals, consoles, logs and text editors build on it: they all place text on a grid.
- It is a native plugin. A grid is a handle of type `textgrid.grid`: `textgrid.new(columns, rows)` makes one, and `textgrid.free(grid)` or Lua's garbage collector frees it.
- A cell holds one character, a Unicode code point, its foreground and background colours, and its style. A grid starts with every cell empty: a space in the theme's text colour on its background, without a style.
- A wide character, of an East Asian script or most emoji, takes two cells, as in terminals; the right cell holds nothing. Writing over half of one empties its other half. A table of Unicode's wide ranges tells which characters are wide. Draw's default monospace font has no East Asian characters, so they show only in a font that has them, set with `draw.monoFont`.
- `textgrid.put(grid, column, row, text)` writes text from a cell to the right, and cuts it at the grid's edge. `textgrid.print(grid, text)` writes at the cursor, as a terminal does: a new line moves the cursor to the start of the next row, a full row goes on in the next one, a tab moves to the next column that is a multiple of 8, and writing past the last row moves every row up by one. A wide character that does not fit in the rest of a row goes to the next; `put` leaves it out. A grid of one column leaves wide characters out, and its cursor stays.
- `textgrid.colors(grid, foreground, background)` sets the colours that later writing uses, and `textgrid.style(grid, style)` the style: the words `bold` and `underline`, or an empty text for none. The monospace font has no bold face, so bold text is drawn twice, a pixel apart. `textgrid.get(grid, column, row)` gives a cell's character and style. `textgrid.clear(grid)` empties every cell and moves the cursor to the top left. `textgrid.setCursor` and `textgrid.cursor` move the cursor and give where it is. `textgrid.size` gives a grid's size, and `textgrid.resize` changes it. A row that `print` filled to the edge is marked as going on in the next row, so resizing joins such rows into their lines and lays the lines out again at the new width: a line wraps instead of being cut, and the cursor keeps its place in its line. When the lines take more rows than the grid has, the first rows go, so the cursor stays. `put` never wraps, so its rows stay apart.
- `textgrid.draw(surface, grid, x, y, size, cursor)` draws a grid with its top left at `x, y`, in layout units, in draw's `"mono"` font at a size; with `cursor`, a line under the cursor's cell shows it. Cells of the same colours and style in a row are drawn as one text, and a wide character alone, over its two cells. `textgrid.cell(size)` gives the size of a cell, and `textgrid.fit(width, height, size)` how many columns and rows fit a rectangle, so a panel can make its grid as large as itself.

## 5. ui

- The `ui` plugin builds user interfaces from elements, lays them out, draws them with draw (2), and calls their callbacks. It is a Lua plugin, so it keeps the Lua functions it is given as callbacks. Its elements are generic, in the spirit of raygui, Unity's UI and HTML, so tools build their own views from them; it has no views of one domain.
- A panel describes its elements again each time it draws: `ui.show(panel, surface, root)` lays them out in the surface and draws them. Its `event` passes each event to `ui.event(panel, event)`, which handles the events that reach an element and gives true for them. Its `destroy` calls `ui.forget(panel)`.
- An element is a table that a function of ui makes, with the element's properties:

  | Element                               | Is                                                                                                                                                                                                                           |
  | ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `column(props, children)`             | Children below each other.                                                                                                                                                                                                   |
  | `row(props, children)`                | Children beside each other.                                                                                                                                                                                                  |
  | `group(props, children)`              | Children below each other in a frame, under the `title` of its props.                                                                                                                                                        |
  | `scroll(props, children)`             | Children below each other, which scroll, with a scrollbar when they do not fit. It needs an `id`.                                                                                                                            |
  | `list(props, children)`               | A scroll element whose children are items to choose. `selected` is the chosen item's number; a press on an item gives its number to `onSelect`, and a click to `onActivate`. Children with `selectable = false`, such as headings, are not items. |
  | `table(props, columns, rows)`         | Rows of cells below a header of the columns' titles; the rows scroll and the header stays. It needs an `id`. A column is `{ id, title, width }`, and a row is a table of its cells' values by column id. A column is as wide as its widest cell, unless it gives a `width`. `selected` is the chosen row's number, and `onSelect` and `onActivate` get a row's number, as for a list. A click on a title gives the column's id to `onSort`; `sort` names the column the rows are sorted by, which shows an arrow, up with `ascending = true`. The plugin sorts the rows. |
  | `tree(props, nodes)`                  | A list of nodes, each below its parent and indented by its depth. It needs an `id`. A node is `{ id, label, children }`, and one with `children`, even none, has an arrow. `open` names the open nodes by id, as `{ src = true }`, and `selected` is the chosen node's id. A click on an arrow gives the node's id and its new state to `onToggle(id, open)`; a press beside it gives the id to `onSelect`, and a click to `onActivate`. The plugin keeps `open` and `selected`. |
  | `separator(props)`                    | A line across a column, or up a row.                                                                                                                                                                                         |
  | `text(text, props)`                   | A line of text, cut with an ellipsis when it does not fit. With `wrap = true`, it breaks into lines at spaces and at its own new lines, to fit the width of its column, or its own `width`.                                    |
  | `button(label, props)`                | A button. `onClick()` runs when it is clicked.                                                                                                                                                                               |
  | `toggle(on, props)`                   | On or off: a check box, or a switch with `look = "switch"`, with an optional `label`. `onChange(on)` gets the new state.                                                                                                      |
  | `slider(value, props)`                | A number from `min` to `max`, 0 and 1 if missing, rounded to `step` if given, which a press and a drag change. `onChange(value)` gets it.                                                                                     |
  | `selector(value, choices, props)`     | One of a few texts, side by side. `onChange(value)` gets the clicked one.                                                                                                                                                    |
  | `tabs(value, choices, props)`         | Tabs: a header for each of the choices, at its own width from the left, with a line under the chosen one. A click on a header gives it to `onChange(value)`, and Left and Right choose the next one, as for a selector. The panel describes below them what the chosen tab holds. |
  | `dropdown(value, choices, props)`     | One of a list of texts, which a click opens in a popup (OpenECS DESIGN 4.6); Up, Down and Return choose there too. `onChange(value)` gets the chosen one.                                                                     |
  | `field(text, props)`                  | A line of text that the user types. It needs an `id`. `onChange(text)` gets each edit, and `onSubmit(text)` the text when Return is pressed; `onSubmit` gives false to keep typing. `valid = false` marks the text as wrong. |
  | `number(value, props)`                | A field for a number, kept from `min` to `max` if they are given and rounded to `step` if it is given. It needs an `id`. Return gives the typed number to `onChange(value)`; a text that is not a number is marked and keeps the field typing. Up and Down change the number by `step`, or by 1, and give it at once. |
  | `key(combination, props)`             | A key combination. It needs an `id`. A click, then a key press, changes it: `onChange(combination)`.                                                                                                                         |
  | `image(path, props)`                  | An image file, at its size, read with image (6) at the surface's scale and drawn with draw.                                                                                                                                  |
  | `space(props)`                        | Empty room, which grows.                                                                                                                                                                                                     |

- Properties of every element: `id`, `width` and `height` (a fixed size), `grow` (a share of the room left along its parent's direction), `stretch` (whether it takes its parent's whole cross size), `padding`, `gap` and `align` (`"start"`, `"center"` or `"end"`, for containers), `background`, `hoverBackground` and `border` (colours), `color`, `size` and `font` (for text: a font name of draw), and `tooltip`, a text. A column or a row with `onClick` is clicked like a button.
- A tooltip shows below the pointer when the pointer rests on its element for half a second, wrapped to a width. Each move starts the half second again. It goes when the pointer leaves the element, at a press or a key, and after five seconds, since a panel hears nothing when the pointer leaves it. It is drawn inside the panel. `ui.tip(panel)` gives the id of the element whose tooltip shows.
- An element without an `id` gets one from its place in the tree, so it keeps its state from one draw to the next as long as the tree keeps its shape.
- Layout: each element gets the size it needs; then the room left along a container is shared by the children that grow, by their shares. Containers, separators, a list's items, and fields, key elements, sliders, selectors and dropdowns in a column, stretch across their parent; other elements keep their size, aligned by the parent's `align`.
- A click is a press and a release of the left button on the same element. The element under the pointer shows it. The wheel scrolls the scroll element under the pointer, and the scrollbar's thumb can be dragged; a press beside the thumb moves it there.
- A field or key element that has the keys takes text input (OpenECS DESIGN 4.6): text goes in at the field's cursor, which Left, Right, Home and End move, and Ctrl with Left and Right moves by words; Backspace and Delete take the character before and after it, or the word with Ctrl; Return submits the field, and Escape, or a click elsewhere, drops what was typed. A field selects text too: a move with Shift selects from the cursor, Ctrl+A selects all of it, and a drag selects what it passes over. A press puts the cursor at the pressed character, and with Shift selects up to there. Typing, Backspace and Delete replace the selection. Ctrl+C copies it to the clipboard, Ctrl+X cuts it, and Ctrl+V pastes text in its place, with its lines joined by spaces, as a field holds one line.
- `ui.edit(panel, id)` gives an element the keys, as a click does, and `ui.focus(panel)` gives the id of the field or key element that takes typing.
- Tab gives the keys to the next element that takes the pointer, in the order of the tree, and Shift+Tab to the one before; they go round at the ends, and scroll the element into view. That element shows a ring. Return or Space presses it as a click does, Left and Right change a selector or a slider, and Escape takes the keys away. A field or key element that gets the keys takes typing at once, and Tab leaves it. A press gives the pressed element the keys, without a ring, so Tab goes on from it. Keys that a plugin binds for its panel type (OpenECS DESIGN 7.8) reach the plugin first.
- `ui.move(panel, list, step)` chooses the item of a list a number of items from the chosen one, gives it to `onSelect`, and scrolls it into view; `ui.activate(panel, list)` gives the chosen item to `onActivate`. They work on a tree and a table too. `ui.expand(panel, tree, open)` opens or closes a tree's chosen node through `onToggle`. Opening an open node chooses its first child, and closing a node that is closed, or has no children, chooses its parent. A plugin binds its keys to its own functions, which call these.
- `ui.rect(panel, id)` gives an element's rectangle in the last layout, as a table of `x`, `y`, `width` and `height` in layout units, or nil when no element has the id, and `ui.reveal(panel, scroll, child)` scrolls a scroll element so a child shows.
- **C.** C gives ui no functions, so a plugin in C describes its elements, and ui builds them with the same element functions:
  - `ui.showDescription(panel, surface, description)` builds the elements of a description and shows them. A description is a table: `kind`, the element function's name, such as `"button"`; `props`, without callbacks; `value`, the first argument, such as a text or a state; `choices`, `columns`, `rows` or `nodes`; and `children`.
  - A described element with an id keeps what each of its callbacks gets, such as `onClick` or `onChange`. `ui.take(panel, id, callback)` takes it, as a table of `value` and, for `onToggle`, `open`, or gives nil if the callback did not run. What no one took is forgotten at the next `showDescription`, so each draw takes only what happened since the last. A column or row with `clickable = true` is clicked like a button.
  - ui's `header.h` ends `ui.h` (OpenECS DESIGN 10.10). Its macros describe the elements in an immediate style, in the manner of Clay's `CLAY`: `UI_SHOW(&ui, panel, surface) { UI_COLUMN(.padding = 8) { ... } }`. A container's block holds its children, and the props are designated initializers of `UiProps`.
  - The macros take what changed before they describe an element: `UI_BUTTON` gives whether the button was clicked, and `UI_TOGGLE(&on)`, `UI_SLIDER(&value)`, `UI_SELECTOR(&index, choices)`, `UI_FIELD(buffer, capacity)` and `UI_LIST(&selected)` write the new state into the plugin's variables. A button's label is its id if it has none.
  - In `UiProps`, a number left 0 or a text left nullptr is left to ui. `grow = UI_FIXED` keeps an element from growing, and `min` and `max` count when they differ.
  - `Ui_Init(&ui, plugin)` looks up ui's functions, from the plugin's `Init`, and `Ui_Event(&ui, panel, event)` gives a panel's event to ui. 3_ui shows the same controls in Lua and in C (OpenECS-examples).
- Positions are in layout units, and colours are the theme's unless an element names others. ui fills the surface with the theme's background first.
- Its sizes are its settings, in layout units, so the user can change them: `ui.paddingX`, `ui.paddingY`, `ui.checkSize`, `ui.fieldWidth`, `ui.scrollbarWidth`, `ui.thumbMin` and `ui.wheelStep`. `ui.margin`, `ui.rowPadding`, `ui.titleSize` and `ui.noteSize` are the sizes that the first-party views share (12, 13, 14), so they look alike.

## 6. image

- The `image` plugin is a loader: it reads image files into images of draw (2), which draws them, and writes those images to files. It is a native plugin.
- stb_image reads PNG, JPG, GIF, BMP, TGA, PSD, HDR and PNM files; nanosvg reads SVG files. `image.load` gives a GIF's first frame.
- `image.load(path, scale)` reads a file into a new image of type `draw.image`, at a scale of its own size: a raster file is stretched, and an SVG file, whose size is its own at 96 dots per inch, is drawn at that scale, so it stays sharp. A file that cannot be read is reported and gives nil.
- Each call reads the file again; a plugin that draws an image many times keeps it.
- `image.decode(bytes, scale)` reads an image from the bytes of its file, such as a PNG file's held in a model (8), at a scale as `load` does. Bytes that start with `<` are read as SVG. Bytes that are not an image are reported and give nil.
- `image.loadFrames(path, scale)` reads the frames of an animated GIF file, each into a new image, at a scale as `load` does. It gives a list of frames, each a table of its `image` and the `seconds` it shows for. Another image file gives one frame of 0 seconds. A plugin shows the frames one after another with a timer; image only reads them.
- stb_image reads out of its buffer for a GIF frame that restores the frame before it. So image changes that disposal, in its copy of the file, to restoring the background. stb restores the background as it was before the frame, so the frames look the same.
- `image.save(image, path)` writes an image of draw to a file, in the format its extension names: `.png`, `.jpg` or `.jpeg`, `.bmp` or `.tga`. stb_image_write writes them; a JPG file is written at a quality of 90 in 100. A relative path starts at the executable's folder. It gives false when the file cannot be written, and the reason is logged.
- `draw.imagePixels(image)` gives the pixels that `image.save` writes. In C they stay the image's, so a plugin reads them only while the image lives.

## 7. audio

- The `audio` plugin is a loader: it reads sound files into sounds of mixer (3), which plays them. It is a native plugin.
- dr_wav, dr_mp3 and dr_flac from dr_libs read WAV, MP3 and FLAC files, and stb_vorbis reads Ogg Vorbis files. A file is read whole into 32-bit float samples, or a piece at a time.
- `audio.load(path)` reads a file into a new sound of type `mixer.sound`. A file that cannot be read is reported and gives nil. Each call reads the file again.
- `audio.stream(path)` opens a long file, such as music, and gives a stream of mixer (3) that it feeds while it plays: a timer keeps half a second of the file waiting in the stream, read a quarter of a second at a time. At the file's end, audio finishes the stream, so its voice ends once it has played. audio keeps the stream until then, so a stream plays to its end even when no one uses its handle. A stream that never plays keeps its file open until audio quits.

## 8. gltf

- The `gltf` plugin is a loader: it reads glTF 2.0 models with cgltf, `.gltf` files with the files they name, and `.glb` files. It is a native plugin. It gives a model's parts and their data as buffers that draw's `triangles` and `lines` (2) take, or that a GPU plugin uploads.
- `gltf.load(path)` reads a model and gives a handle of type `gltf.model`, which owns all of it, and `gltf.free(model)` frees it now. Each call reads the file again. A file that cannot be read, or that is not valid glTF, is reported and gives nil.
- `gltf.describe(model)` gives a table of the model's parts. Meshes, materials and nodes are lists, and they name each other by their numbers in those lists, from 1:

  | Field       | Holds                                                                                                                                                                                                                                                                                         |
  | ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `meshes`    | Each mesh's `name` and `primitives`. A primitive has a `mode` (`"triangles"`, `"triangleStrip"`, `"triangleFan"`, `"points"`, `"lines"`, `"lineLoop"` or `"lineStrip"`), a `material`, the names of its vertex `attributes`, such as `POSITION`, and its numbers of `vertices` and `indices`. |
  | `materials` | Each material's `name`, its base `color`, as four numbers from 0 to 1, its `texture`, the base colour's image file, when it is a file, and its `image`, the number of the base colour's image in `images`.                                                                                     |
  | `images`    | Each image's `name`, its `mimeType`, such as `"image/png"`, when the model gives one, and its `file`, when it is a file. An image inside the model, as in a `.glb`, has no file.                                                                                                                |
  | `nodes`     | Each node's `name`, its `mesh`, its `children`, and its `matrix`: 16 numbers, column by column, that place it in its parent.                                                                                                                                                                  |
  | `animations` | Each animation's `name`, and its `duration` in seconds: the time of its last key.                                                                                                                                                                                                             |
  | `skins`     | Each skin's `name`, and its `joints`: the nodes whose moves bend the meshes it skins. A node with a skinned mesh names its `skin`.                                                                                                                                                            |
  | `roots`     | The nodes that the model's scene starts from.                                                                                                                                                                                                                                                 |

- `gltf.attribute(model, mesh, primitive, name)` gives a vertex attribute of a primitive as 32-bit floats, one after another for each vertex: three for a `POSITION`, two for a `TEXCOORD_0`. Whole numbers and sparse data are converted. `gltf.indices(model, mesh, primitive)` gives its indices as 32-bit whole numbers, from 0; a primitive without indices gives its vertices in order. Both give an empty buffer for a missing mesh, primitive or attribute, and their buffer is kept until the next call of `attribute`, `indices`, `image`, `pose` or `skin` on the model.
- `gltf.pose(model, animation, seconds)` gives each node's matrix in its parent at a time of an animation, 16 floats a node, in the order of `nodes`, as `matrix` is. Animation 0 gives the nodes' own matrices. Before an animation's first key a node takes the first key's value, and after the last the last's. Steps, linear keys, with rotations turned along the shorter way, and cubic splines are read as glTF says; morph weights are left out. A plugin loops an animation by giving the time modulo its `duration`.
- `gltf.skin(model, mesh, primitive, skin, animation, seconds)` gives a primitive's positions where a skin's joints move them at a time of an animation, three floats each, in the model's space, as glTF skins them: each vertex goes through the joints that its `JOINTS_0` names, by the shares that its `WEIGHTS_0` gives, after the inverse of where the joint was when the mesh was bound. A primitive without them, or a missing skin, gives its positions as they are. A joint past the skin's inverse bind matrices keeps the identity. The skinning runs on the CPU.
- `gltf.image(model, image)` gives the bytes of an image, such as a PNG file's, from its own file next to the model, or from inside the model. `image.decode` (6) makes a draw image of them. A missing image gives an empty buffer, and the buffer is kept until the next call of `attribute`, `indices`, `image`, `pose` or `skin` on the model.
- In Lua a buffer is a string, which `string.unpack` reads, such as `string.unpack("<fff", positions)` for the first position.

## 9. net

- The `net` plugin makes TCP connections and servers, TLS connections and servers, and UDP sockets, with SDL_net, which is compiled into it. It is a native plugin. Nothing it does blocks the program: a plugin asks for the state of a connection, and takes what arrived, when it wants, such as from a timer.
- **TCP.** `net.connect(host, port)` starts a connection and gives a handle of type `net.connection`. The host's name resolves in the background. `net.status(connection)` gives 1 once it is connected, 0 while it waits, and -1 if it failed or closed.
- `net.send(connection, data)` queues data to go out, and gives false if the connection has no socket yet or is closed. `net.receive(connection, most)` takes what has arrived, at most a number of bytes, and gives an empty buffer if nothing has. The buffer is kept until the next receive on the connection. A connection that the other side closed fails.
- `net.listen(port)` waits for connections on a port of every address of this computer and gives a handle of type `net.server`, or nil if it cannot. `net.accept(server)` gives a connection that a client made, or nil.
- **TLS.** `net.connectTLS(host, port)` starts a TLS connection, with mbedTLS, which is compiled into it too (1). Its status is 0 until the TLS handshake is done, and -1 if it fails, such as when the server's certificate is not trusted or not for the host. It checks the certificate against the system's trusted certificates, as web does (11). Then `send` and `receive` move the data in and out of TLS.
- `net.listenTLS(port, certificate, key)` waits for TLS connections, with a certificate and its private key from PEM files, whose relative paths start at the executable's folder. It gives nil if it cannot listen or cannot read them. The connections it accepts do their handshake as a client's do, and their status is 1 once it is done. A connection keeps its server's configuration, so it works on after the server's handle is gone.
- `net.trust(path)` adds the certificates of a PEM file to the trusted ones, for the TLS connections that do their handshake later, such as a test server's own certificate. It gives false if the file cannot be read.
- `net.close(connection)` closes a connection. Lua's garbage collector, or the plugin that made it, frees a connection or a server, and closes it.
- **UDP.** `net.udp(port)` opens a socket of type `net.udp` on a port of every address of this computer; port 0 picks a free one. Datagrams may be lost or come in another order, and nothing tells.
- `net.sendTo(socket, host, port, data)` sends a datagram. The host's name resolves in the background: the datagram waits, behind the ones sent before it, and leaves once the name resolves. A socket keeps the last 8 hosts it resolved, so sending to them again does not resolve them again.
- `net.receiveFrom(socket)` takes a datagram that has arrived, with the address and port it came from, and gives an empty buffer if none has. The buffer and the address are kept until the next `receiveFrom` on the socket.
- SDL_net listens on every family of addresses at once, and fails if one is missing; then IPv4 alone is tried, for servers and UDP sockets.

## 10. fs

- The `fs` plugin does with files and folders what Lua's `io` and `os` cannot: it lists folders, tells what a path is, makes folders, copies files and watches folders. It is a native plugin, built on SDL's filesystem functions and the system's own watching, so it needs no library. Moving and removing are `os.rename` and `os.remove`.
- `fs.info(path)` gives a table of what a path is: its `type`, `"file"`, `"folder"` or `"other"`, its `size` in bytes, and `modified`, when it last changed, in seconds since 1970. A path that does not exist gives nil.
- `fs.list(folder, pattern)` gives the entries of a folder whose names match a pattern, sorted by name: each a table of its `name` and the fields of `fs.info`. In a pattern, `*` stands for any characters but `/`, and `?` for one; an empty pattern matches every name. The entries of the folder's folders are not listed. A folder that cannot be read gives nil.
- `fs.makeFolder(path)` makes a folder, and the folders above it that are missing. `fs.copy(from, to)` copies a file, replacing what is at the new path. Each gives false when it fails.
- `fs.watch(folder, changed)` watches a folder and gives a handle of type `fs.watch`. When an entry of the folder is added, removed or changed, `changed` gets the folder, ending with `/`, the entry's name, and `"added"`, `"removed"` or `"changed"`. A rename is the old name removed and the new one added. The entries of the folder's folders are not watched. Changes are read ten times a second, up to 8 reads of them each time, so one write may come as several changes, and a busy folder never holds the program. `fs.unwatch(watch)` stops a watch now; a watch also stops when no one uses its handle. A folder that cannot be watched, or a file, gives nil.
- Watching uses the system's own way, as SDL has none: inotify on Linux. On other systems `fs.watch` gives nil and logs a warning.
- `fs.folder(name)` gives a folder of the computer, ending with `/`: `executable`, the executable's folder, or one of the user's folders, `home`, `desktop`, `documents`, `downloads`, `music`, `pictures`, `videos`, `screenshots` or `templates`. A folder the system does not have gives nil.
- Reading and writing what files hold is Lua's `io` in Lua, and SDL's `SDL_LoadFile` and `SDL_SaveFile` in C.

## 11. web

- The `web` plugin makes HTTP and HTTPS requests, and WebSocket connections. It is a native plugin: libcurl makes them, with TLS from mbedTLS, and both are compiled into it (1). libcurl is built for HTTP, HTTPS and WebSockets only, with HTTP/1.1, and resolves names on a thread of its own.
- Nothing it does blocks the program: one thread of the plugin runs every request, many at once.
- `web.get(url, done)` starts a GET request and gives a handle of type `web.request`. `web.request(method, url, headers, body, done)` starts a request with any method, such as `"POST"`; the headers are a list of texts, such as `{ "Content-Type: application/json" }`, and a body that is not empty is sent. A request that cannot be made gives nil; one to a URL that is not HTTP or HTTPS fails, as one to a host that cannot be reached does.
- `done`, if given, is a kept callback (OpenECS DESIGN 10.4): web calls it on the main thread with the request once it is done, and then releases it. The request goes on when its handle is dropped, and `done` gets it in a new handle. Without `done`, a plugin asks for the request's state when it wants, such as from a timer, as with net (9).
- `web.status(request)` gives 0 while the request runs, the answer's HTTP status once it is done, such as 200 or 404, and -1 if it failed, such as when the host cannot be reached or its certificate is not trusted. `web.error(request)` gives why it failed.
- Once it is done, `web.body(request)` gives the answer's body as a buffer, a string in Lua, and `web.header(request, name)` one of its headers, or nothing. A request follows up to 10 redirects, and these are the last answer's.
- `web.free(request)` frees a request now, and stops one that runs; its `done` is not called. Lua's garbage collector, or the plugin that made it, frees it otherwise.
- The setting `web.timeout` is the number of seconds a request may take before it fails; 0 means no limit. For a WebSocket connection, it limits only how long connecting takes.
- **WebSockets.** `web.socket(url, headers)` starts a connection to a `ws://` or `wss://` URL and gives a handle of type `web.socket`. As with net (9), nothing blocks: a plugin asks for the connection's state, and takes its messages, when it wants, such as from a timer. In Lua, the socket's methods do the rest; in C they are `socketStatus`, `socketError`, `socketSend`, `socketReceive` and `socketClose`.
- `socket:status()` gives 1 once the server took the connection, 0 while it connects, and -1 if it failed or closed. `socket:error()` gives why.
- `socket:send(data, text)` queues a message, as text or as bytes, and gives false if the connection failed or closed. A message sent while the connection connects waits, and goes once it is open. Each message goes in one frame.
- `socket:receive()` takes the oldest message that arrived, a buffer, and its kind, `"text"` or `"binary"`. It gives nothing as the kind if no message waits. A message that the server sent in many frames comes as one. The buffer is kept until the next receive on the connection.
- libcurl answers the server's pings. A close from the server, or the end of its connection, closes the connection.
- `socket:close()` closes a connection and frees it now. Lua's garbage collector, or the plugin that made it, frees it otherwise.
- HTTPS checks a server's certificate against the system's trusted certificates: the file that `SSL_CERT_FILE` names, or the one where Debian, Ubuntu, Arch, Fedora, openSUSE or Alpine keep it. The proxies that `http_proxy`, `https_proxy` and `no_proxy` name are used.
- The plugin gives libcurl and mbedTLS SDL's allocator.

## 12. The settings window

- The first-party Lua plugin `settings` is built with ui (5). The core's settings file loads it in every tool (OpenECS DESIGN 12.3), and binds `,` after the prefix to `settings.open` (OpenECS DESIGN 7.8), which opens the window as a panel of type `settings.window`, or shows the one that is open.
- The window lists every declared setting under a title for its owner: the core's first, then each plugin's, each by name. A row shows the setting's name and description on its left, and a control with its value in effect on its right. The two sides share the row's width, so the controls line up and grow with the window.
- The control depends on the type: a switch for a `bool`, a dropdown for a `choice`, a key element for a `key`, a field with the colour beside it for a `color`, and a field for the other types. A `list` or a `table` shows as a Lua table, such as `{ 1, 2 }`.
- A field's text must read as a value of its setting's type, and shows as wrong while it does not: a number, a colour such as `#18191C`, or, for a `list` or a `table`, a Lua table, which is read with no access to anything. Return saves a text that can be read; a text that cannot is reported, and typing goes on.
- The window changes values in the user's settings file (OpenECS DESIGN 12.2): a step back or forward toggles a `bool`, cycles a `choice`, and adds or takes 1 from an `integer` or a `number`.
- The functions `settings.up`, `settings.down`, `settings.previous`, `settings.next` and `settings.edit` choose a setting and change it: `settings.edit` toggles a `bool`, cycles a `choice`, and gives the other controls the keys. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down, Left, Right and Return. While a field has the keys, they leave it alone, and `settings.edit` saves what is typed, as Return does.
- The settings are a ui list (5), with each owner's title as a heading, under the window's title. Choosing a setting with the keys scrolls it into view, and a press chooses the setting under it, while its control gets the press too.
- The panel's saved state is the chosen setting.

## 13. The launcher

- When the command line names no preset and no session, OpenECS starts with the first-party preset `launcher`. Its app id is `openecs.launcher`, it sets `listed = false`, and it shows one panel of the first-party Lua plugin `launcher`.
- The panel type `launcher.list` lists the presets, then the saved sessions (OpenECS DESIGN 13.7). Presets whose tool has a last session come first, the most recently used first; the others follow by name. Sessions are listed newest first.
- An entry shows the tool's name. A session's entry also shows the file's name and when it was saved.
- The panel reads both lists when it is created and each time it is shown again.
- Every tool loads the plugin, as the core's settings file names it (OpenECS DESIGN 12.3). `launcher.open` opens the list in any tool, or gives an open one the focus, so the user can go to another tool; the core binds it to the prefix and `/`.
- The functions `launcher.up`, `launcher.down` and `launcher.start` choose an entry and start it. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down and Return. A click on an entry opens it, and the wheel scrolls the list.
- The panel's saved state is the chosen entry, so the launcher chooses it again at the next start.
- It is built with ui (5): the entries are a list, whose `onActivate` opens the entry.
- A preset opens with `ecs.session.openPreset`, a session with `ecs.session.open` (OpenECS DESIGN 13.5).

## 14. The panel list

- It shows every panel type of the tool's plugins, so the user finds what each plugin offers and opens it. The first-party Lua plugin `panels` gives the panel type `panels.list`, titled "Panels".
- The core's settings file loads it for every tool, and binds `panels.open` to Alt+W, then `.` (OpenECS DESIGN 12.3).
- It reads the list with `ecs.panel.types()` (OpenECS DESIGN 4.1), each time it is opened or shown, and leaves out its own type. The types are grouped by plugin, under the plugin's name and description; each row shows the type's title and name.
- `panels.open` opens the list, or focuses it if it is open. The panel that had the focus is where a chosen type opens: in its group, or where the core puts new panels if it has closed.
- A click on a row, or `panels.choose`, opens a panel of the type. `panels.up` and `panels.down` choose a row; the plugin binds them for its panel type to Up, Down and Return.
- The panel's saved state is the chosen type.
- It is built with ui (5): the types are a list, with each plugin's name and description as a heading, whose `onActivate` opens the type.

---

## Glossary

**Archive (static).** A file that holds compiled code, which a library or a program takes in when it is linked.

**Certificate.** A file with which a server proves who it is to HTTPS. It is trusted when an authority that the system trusts signed it.

**Datagram.** A message sent by UDP on its own, without a connection.

**Ellipsis.** The three dots, …, that end a text that is cut.

**PEM.** A text file that holds certificates or keys in base64, between lines such as `-----BEGIN CERTIFICATE-----`.

**Proxy.** A server that makes requests for other programs.

**Rasterizer.** Code that turns a shape into the pixels it covers.

**Redirect.** An answer that tells the client to ask another URL instead.

**Thumb (scrollbar).** The part of a scrollbar that shows which part of the content is in view, and that the user drags to scroll.

**TLS (Transport Layer Security).** The protocol that encrypts HTTPS and other connections, and checks the server's certificate.

**Unity build.** Compiling a library as one C file that includes all of its C files.

## 15. gpu

- The `gpu` plugin draws into the GPU surface of another plugin's panel, on the GPU (OpenECS DESIGN 5.4), from Lua and C alike. It is a native plugin. It draws shapes, lines and textures, with the functions draw has for pixels, and vertex buffers with a plugin's own shaders, through pipelines. It holds no scene: the plugin that draws keeps its buffers and says what to draw each frame.
- It uses OpenECS's GPU device, which `ECSGpu_GetDevice` gives (OpenECS DESIGN 9.8): shapes go through an offscreen SDL renderer of its own on the device, `SDL_CreateGPURenderer(device, nullptr)`, and pipelines through SDL's GPU API. Without a GPU device it still loads, and its functions log why they cannot draw.
- **Frames.** A GPU panel's `draw` starts a frame of its surface with `gpu.frame(surface)`, a handle of type `gpu.frame`, draws with its methods, and calls `frame:submit()` before `draw` returns. Submitting sends what the frame drew to the GPU before the core's renderer shows the texture. A frame cannot draw after it is submitted. One that is dropped without being submitted sends its pipelines' drawing at once, and its shapes with the next frame.
- What a frame draws keeps its order, shapes and pipelines mixed: draws with pipelines run in one render pass until a shape comes, and shapes go to the GPU before the next pass starts.
- Positions and sizes of shapes are in layout units; gpu multiplies them by the surface's scale. Colours have the type `color` (OpenECS DESIGN 10.2), and gpu blends them by their alpha.
- Its functions for shapes, each also a method of the frame or the texture in Lua:

  | Function                                                 | Does                                                                                                                                    |
  | -------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
  | `frame`, `submit`                                        | Start drawing into a GPU surface, and send what the frame drew to the GPU.                                                              |
  | `clear`                                                  | Fill the whole surface with a colour, replacing what it holds.                                                                         |
  | `fill`, `line`                                           | Fill a rectangle, and draw a line in a thickness.                                                                                      |
  | `triangles`                                              | Fill triangles between points, a buffer of x and y floats, with indices, three 32-bit numbers each, or none for points in threes, as draw's do. |
  | `mesh`                                                   | Fill triangles whose points each have a colour, an ARGB 32-bit number, which blends across each triangle. Later triangles cover earlier ones. |
  | `texture`, `updateTexture`, `textureSize`, `freeTexture` | Make a texture of type `gpu.texture` on the GPU from ARGB pixels with premultiplied alpha, as draw's images have; replace its pixels, give its size, and free it. |
  | `image`, `imageTransformed`                              | Draw a texture stretched to a rectangle, or also turned clockwise by an angle in degrees around its centre, with each pixel's colour multiplied by a colour. |

- A texture keeps its pixels on the GPU, so drawing it each frame costs no upload. `gpu.texture(width, height, image:pixels())` makes one of a draw image.
- **Pipelines.** A plugin draws with shaders of its own through three kinds of handle, made once and kept:
  - `gpu.buffer(usage, bytes)` makes a buffer of type `gpu.buffer` on the GPU: `"vertex"`, vertices one after another as a pipeline's attributes lay them out, or `"index"`, 32-bit indices. `buffer:update(bytes)` replaces its bytes, `buffer:size()` gives its size, and `buffer:free()` frees it.
  - `gpu.shader(desc)` makes a shader of type `gpu.shader` from compiled code: `stage`, `"vertex"` or `"fragment"`; the code as `spirv`, `dxil` or `msl`; its `entry` function, `"main"` if missing; and how many `uniforms` blocks and `samplers` it reads. The GPU's driver takes one kind of code, SPIR-V for Vulkan, DXIL for Direct3D 12 and MSL for Metal, and gpu uses the first one given that it takes. Uniform blocks are in set 1 of a vertex shader and set 3 of a fragment shader, and samplers in set 2, as SDL's GPU API sets them.
  - `gpu.pipeline(desc)` makes a pipeline of type `gpu.pipeline`: its `vertex` and `fragment` shaders; `attributes`, the list of a vertex's attributes at locations from 0, each `"float"`, `"float2"`, `"float3"`, `"float4"` or `"color"`, 4 bytes from 0 to 255; its `primitive`, `"triangles"` if missing; `depth`, whether it tests and writes the frame's depth, true if missing; `cull`, `"none"`, `"front"` or `"back"`, with counter-clockwise front faces; and `blend`, `"none"`, `"alpha"` or `"add"`.
- `frame:draw(pipeline, draw)` draws `draw.vertices`, a vertex buffer, with a pipeline: through `draw.indices`, an index buffer, if it has one, and `draw.count` of them, or all. `draw.vertexUniforms` and `draw.fragmentUniforms` are each shader's first uniform block, as std140 lays it out, such as from `string.pack`, and `draw.texture`, a `gpu.texture`, is the fragment shader's first sampler, sampled linearly.
- A frame's draws share one depth texture of its size, `D16_UNORM`, which its first draw clears, so the near primitives of every draw cover the far ones. gpu keeps one for each of the last four sizes it drew.
- Panel textures are `B8G8R8A8_UNORM`, so a pipeline's colour target is too.
- The types of the descriptions are in the plugin's `types.lua`, for editors (OpenECS DESIGN 10.10).
- Compiling shaders from source at run time, and text, are not yet in gpu (OpenECS TODO).
