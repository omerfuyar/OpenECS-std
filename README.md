# OpenECS-std

The standard plugins of [OpenECS](https://github.com/omerfuyar/OpenECS), and its first-party plugins and presets:

- **draw:** draws shapes, lines, triangles, images, turned or tinted, and text into other plugins' panels.
- **mixer:** plays sounds, many at once.
- **textgrid:** grids of characters for terminals, consoles, logs and editors, in colours, bold and underlined, with wide characters in two cells, drawn with draw at any size.
- **ui:** user-interface elements with layout, such as buttons, toggles, sliders, number fields, selectors, tabs, dropdowns and lists, with tooltips, which call back on clicks and edits; plugins in C describe them with macros in the manner of Clay's CLAY.
- **image:** reads PNG, JPG, GIF, SVG and other image files, and the frames of animated GIF files, into images that draw draws, and writes PNG, JPG, BMP and TGA files.
- **audio:** reads WAV, MP3, FLAC and Ogg Vorbis files into sounds that mixer plays, whole or as streams while they play.
- **gltf:** reads glTF 2.0 models, and gives their parts and vertex data.
- **net:** TCP connections and servers, TLS connections and servers, and UDP sockets, that never block the program.
- **web:** HTTP and HTTPS requests, and WebSocket connections, that never block the program.
- **fs:** lists, makes and watches folders, copies files and tells what a path is, which Lua's `io` and `os` cannot.
- **gpu:** draws into GPU surfaces on the GPU, from Lua or C: shapes, lines, textures and text, with the functions draw has for pixels, and vertex buffers with a plugin's own shaders.
- **cglm:** [cglm](https://github.com/recp/cglm)'s vectors, matrices, quaternions, cameras and noise for 3D scenes and shaders, in Lua; C plugins call cglm itself.
- **settings:** the settings window.
- **launcher:** picks a preset or session when OpenECS starts without one.
- **panels:** the panel list: every panel type of the tool, by plugin, to open one.

OpenECS ships them: this repository is a submodule of OpenECS, in its `std/` folder.

## Building and testing

Build and test inside a checkout of OpenECS, as OpenECS's README.md says:

```shell
git clone --recursive https://github.com/omerfuyar/OpenECS.git
cd OpenECS
.github/scripts/build.sh D
.github/scripts/test.sh build/Debug/bin/OpenECS
.github/scripts/test-wayland.sh build/Debug/bin/OpenECS
```

`test-wayland.sh` runs the tests on Wayland, in a headless sway, as OpenECS's README.md says.

OpenECS's build compiles and runs `shuild.c` from `std/`, which builds these plugins, unless OpenECS's build is given `--no-std`. The examples of [OpenECS-examples](https://github.com/omerfuyar/OpenECS-examples) are the tests: `test.sh` runs each example's test, and each plugin has an example.

Each plugin needs nothing but OpenECS: the libraries it uses are compiled into it. The build writes a `.luarc.json` here that names OpenECS's `include/` and the plugins' definition files, so an editor with LuaLS completes and checks the Lua plugins.

## Releases

This repository is released with OpenECS, at the same time and with the same version. Each release has a description in `.github/release-notes/`.

Each release also has these plugins built for Linux and Windows, with OpenECS of the same version: `plugins/`, `presets/` and the plugins' definition files in `include/`. Unpack it into an OpenECS folder of that version.
