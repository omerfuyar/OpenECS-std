# OpenECS-std TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [DESIGN.md](DESIGN.md) and remove it from here.

## Tasks

- **ui for plugins in C.** ui's callbacks are Lua functions. A C plugin cannot give one, and ui cannot call a C plugin's functions by name, because a plugin uses only the functions of the plugins it depends on (OpenECS DESIGN 10.5).
- **ui elements.** Text that wraps, moving the cursor in a field, keyboard focus between elements, lists that choose, menus and popups.
- **image.** GIF animation, and pictures that a plugin makes or changes, not only files.
- **audio.** Streaming long files instead of reading them whole, and effects such as panning.
- **net.** UDP, and TLS for secure connections.
- **gltf.** Images inside a `.glb` file, skins and animations.
- **tty.** Bold and underlined cells, and wide characters that take two cells.
