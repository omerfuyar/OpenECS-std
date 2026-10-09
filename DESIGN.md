# OpenECS-std design

How OpenECS's standard plugins and first-party Lua plugins are built. They follow OpenECS's design and conventions: [OpenECS DESIGN.md](https://github.com/omerfuyar/OpenECS/blob/dev/DESIGN.md), which this file names as "OpenECS DESIGN".

## Contents

1. Rules
2. ui
3. The settings window
4. The launcher

## 1. Rules

- The plugins follow the rules of standard plugins (OpenECS DESIGN 20).
- Each plugin is a folder of `plugins/`, with its manifest. `presets/` holds first-party presets, and `tests/` the tests of these plugins, with their own presets and test plugins.
- `build.c` is the build of this repository. OpenECS's build includes it when this repository is checked out in its `std/` folder (OpenECS DESIGN 17.7): it builds each plugin into `bin/plugins/`, copies the presets into `bin/presets/`, and, with `--tests`, the tests beside OpenECS's own.
- Tests and test plugins have names that OpenECS's tests do not use, because both are copied into one folder.

## 2. ui

- The `ui` plugin draws shapes, text, images and user-interface elements into the pixels surface of another plugin's panel. A panel's `Draw` passes its surface handle (OpenECS DESIGN 10.6) to ui's functions.
- Positions and sizes are in layout units; ui multiplies them by the surface's scale. Colours are ARGB integers.
- It is a native plugin. It calls SDL3 and SDL3_ttf itself (OpenECS DESIGN 17.2): it wraps the surface's pixels in an SDL surface for each call, and draws text with one SDL3_ttf surface text engine, which keeps the glyphs it has drawn.
- Its font is the core's `ecs.font`, at the size the caller asks for; a relative path starts at the executable's folder. The labels of its elements are in the core's `ecs.fontSize`.
- It reads image files, PNG, JPG or BMP, with SDL, the first time they are drawn, and keeps them until it shuts down. A relative path starts at the executable's folder; a plugin draws its own images by its folder (OpenECS DESIGN 11.3). A file that cannot be read is reported once.
- Its elements are drawn in the colours of the core's theme. They are only drawn: the caller handles the input and keeps the state, such as whether a check box is on.
- Its functions:

  | Function       | Signature                                                                    | Does                                                                                                                                                                                 |
  | -------------- | ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
  | `ui.fill`      | `void(handle<ecs.surface>, float, float, float, float, int64)`               | Fills a rectangle `x, y, width, height` with a colour, blending by its alpha.                                                                                                        |
  | `ui.text`      | `float(handle<ecs.surface>, string, float, float, float, int64)`             | Draws text at `x, y`, the top left of its line, in a size and a colour, and gives its width.                                                                                         |
  | `ui.measure`   | `void(string, float, out float, out float)`                                  | Gives the width and the line height of a text in a size, without drawing it.                                                                                                         |
  | `ui.color`     | `int64(string)`                                                              | Reads a colour: `"#RRGGBB"`, `"#RRGGBBAA"`, or the name of a colour of the core's theme, such as `"text"` for `ecs.colorText`.                                                       |
  | `ui.outline`   | `void(handle<ecs.surface>, float, float, float, float, float, int64)`        | Draws the edge of a rectangle `x, y, width, height`, in a thickness and a colour.                                                                                                    |
  | `ui.image`     | `bool(handle<ecs.surface>, string, float, float, float, float)`              | Draws an image file stretched to a rectangle `x, y, width, height`. Gives false if the file cannot be read.                                                                          |
  | `ui.imageSize` | `bool(string, out float, out float)`                                         | Gives an image file's width and height in pixels. Gives false if the file cannot be read.                                                                                            |
  | `ui.button`    | `void(handle<ecs.surface>, string, float, float, float, float, int)`         | Draws a button with a label in a rectangle. The state is 0 for normal, 1 for under the pointer or chosen, and 2 for pressed.                                                         |
  | `ui.check`     | `void(handle<ecs.surface>, float, float, float, bool)`                       | Draws a check box at `x, y` in a size, filled when it is on.                                                                                                                         |
  | `ui.field`     | `float(handle<ecs.surface>, string, float, float, float, float, bool)`       | Draws a text field with a line of text in a rectangle. A focused field has a cursor after its text and keeps the cursor in view. Gives the cursor's x, such as for text input (OpenECS DESIGN 4.6). |
  | `ui.scrollbar` | `void(handle<ecs.surface>, float, float, float, float, float, float, float)` | Draws a vertical scrollbar in a rectangle, for content of a total length, of which a length is shown, scrolled by an offset. It draws nothing when all the content is shown.         |
  | `ui.thumb`     | `void(float, float, float, float, out float, out float)`                     | Gives where a scrollbar's thumb starts and how long it is, from the bar's length and the content's total, shown length and offset, so the caller can tell a press on it.             |

- A colour that cannot be read is reported, and gives opaque magenta, so the mistake shows.

## 3. The settings window

- The first-party Lua plugin `settings` draws with the ui plugin (2). The core's settings file loads it in every tool (OpenECS DESIGN 12.3), and binds `,` after the prefix to `settings.open` (OpenECS DESIGN 7.8), which opens the window as a panel of type `settings.window`, or shows the one that is open.
- The window lists every declared setting under a title for its owner: the core's first, then each plugin's, each by name. A row shows the setting's name, a control with its value in effect, and its description.
- The control depends on the type: a check box for a `bool`, a button for a `choice`, and a text field for the other types. A `list` or a `table` shows as a Lua table, such as `{ 1, 2 }`.
- The window changes values in the user's settings file (OpenECS DESIGN 12.2): a step back or forward toggles a `bool`, cycles a `choice`, and adds or takes 1 from an `integer` or a `number`.
- `settings.edit` toggles a `bool` and cycles a `choice` forward. For the other types it types the value with text input (OpenECS DESIGN 4.6): it starts typing, and saves the value typed. A number must read as one, and a `list` or a `table` as a Lua table, which is read with no access to anything. A value that cannot be read is reported, and typing goes on. Backspace deletes the last character, and Escape, or choosing another setting, drops what was typed.
- The functions `settings.up`, `settings.down`, `settings.previous`, `settings.next` and `settings.edit` choose a setting and change it. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down, Left, Right and Return.
- The list scrolls under the window's title, with a scrollbar when it is longer than the window. The wheel scrolls it, and so does dragging the scrollbar's thumb; a press beside the thumb moves the thumb there. Choosing a setting with the keys scrolls it into view.
- A click chooses the setting under it. A click on its control also runs `settings.edit`.
- The panel's saved state is the chosen setting.

## 4. The launcher

- When the command line names no preset and no session, OpenECS starts with the first-party preset `launcher`. Its app id is `openecs.launcher`, it sets `listed = false`, and it shows one panel of the first-party Lua plugin `launcher`.
- The panel type `launcher.list` lists the presets, then the saved sessions (OpenECS DESIGN 13.7). Presets whose tool has a last session come first, the most recently used first; the others follow by name. Sessions are listed newest first.
- An entry shows the tool's name. A session's entry also shows the file's name and when it was saved.
- The panel reads both lists when it is created and each time it is shown again.
- The functions `launcher.up`, `launcher.down` and `launcher.open` choose an entry and open it. The plugin binds them for its panel type (OpenECS DESIGN 7.8) to Up, Down and Return. The wheel chooses too, and a click on an entry opens it.
- The panel's saved state is the chosen entry, so the launcher chooses it again at the next start.
- It draws with the ui plugin (2).
- A preset opens with `ecs.session.openPreset`, a session with `ecs.session.open` (OpenECS DESIGN 13.5).

---

## Glossary

**Thumb (scrollbar).** The part of a scrollbar that shows which part of the content is in view, and that the user drags to scroll.
