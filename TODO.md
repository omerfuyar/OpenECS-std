# OpenECS-std TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [DESIGN.md](DESIGN.md) and remove it from here.

## Tasks

- **ui for plugins in C.** ui's callbacks are Lua functions. A C plugin cannot give one, and ui cannot call a C plugin's functions by name, because a plugin uses only the functions of the plugins it depends on (OpenECS DESIGN 10.5).
- **ui elements.** Text that wraps, moving the cursor in a field, keyboard focus between elements, lists that choose, menus and popups.
- **More plugins.** A character grid for terminals, consoles and logs; images in more formats; audio; networking; glTF models.
