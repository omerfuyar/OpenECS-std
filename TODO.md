# OpenECS-std TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [DESIGN.md](DESIGN.md) and remove it from here.

## Tasks

- **ui for plugins in C.** ui's callbacks are Lua functions. A C plugin cannot give one, and ui cannot call a C plugin's functions by name, because a plugin uses only the functions of the plugins it depends on (OpenECS DESIGN 10.5).
- **ui elements.** Text that wraps, moving the cursor in a field, keyboard focus between elements, trees, tables, tabs, tooltips, and a number field.
- **draw.** Antialiased edges, rounded rectangles, and images rotated or with a colour multiplied in.
- **image.** GIF animation, and writing images to files.
- **mixer.** Streaming long sounds instead of keeping them whole, and effects such as panning.
- **net.** TLS for secure connections.
- **web.** A plugin for HTTP and HTTPS requests, built on libcurl, which is approved as a dependency, with TLS from mbedTLS. It would run each request in the background and give the answer to a callback (OpenECS DESIGN 3.3).
- **gltf.** Images inside a `.glb` file, skins and animations.
- **textgrid.** Bold and underlined cells, and wide characters that take two cells.
- **fs.** Watching a folder for changes.
