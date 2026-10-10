# OpenECS-std

The standard plugins of [OpenECS](https://github.com/omerfuyar/OpenECS), and its first-party plugins and presets:

- **draw:** draws shapes and text into other plugins' panels.
- **ui:** user-interface elements with layout, which call back on clicks and edits.
- **tty:** grids of characters for terminals, consoles and logs, drawn at any size.
- **image:** reads PNG, JPG, GIF, SVG and other image files, and draws them.
- **audio:** reads WAV, MP3, FLAC and Ogg Vorbis files, and plays them.
- **net:** TCP connections and servers that never block the program.
- **gltf:** reads glTF 2.0 models, and gives their parts and vertex data.
- **fs:** lists, makes, copies, moves and removes files and folders, which Lua's `io` cannot.
- **settings:** the settings window.
- **launcher:** picks a preset or session when OpenECS starts without one.

OpenECS ships them: this repository is a submodule of OpenECS, in its `std/` folder.

## Building and testing

Build and test inside a checkout of OpenECS, as OpenECS's README.md says:

```shell
git clone --recursive https://github.com/omerfuyar/OpenECS.git
cd OpenECS
.github/scripts/build.sh D
.github/scripts/test.sh build/Debug/bin/OpenECS std
```

OpenECS's build compiles and runs `shuild.c` from `std/`, which builds these plugins, unless OpenECS's build is given `--no-std`. `test.sh` with `std` runs this repository's tests, apart from OpenECS's own.

Each plugin needs nothing but OpenECS: the libraries it uses are compiled into it.

## Releases

This repository is released with OpenECS, at the same time and with the same version. Each release has a description in `.github/release-notes/`.
