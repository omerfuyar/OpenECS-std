# OpenECS-std

The standard plugins of [OpenECS](https://github.com/omerfuyar/OpenECS), and its first-party plugins and presets:

- **draw:** draws shapes, text and images into other plugins' panels.
- **ui:** user-interface elements with layout, which call back on clicks and edits.
- **settings:** the settings window.
- **launcher:** picks a preset or session when OpenECS starts without one.

OpenECS ships them: this repository is a submodule of OpenECS, in its `std/` folder.

## Building and testing

Build and test inside a checkout of OpenECS, as OpenECS's README.md says:

``` shell
git clone --recursive https://github.com/omerfuyar/OpenECS.git
cd OpenECS
.github/scripts/build.sh D
.github/scripts/test.sh build/Debug/bin/OpenECS
```

OpenECS's build includes `build.c` from `std/`, and its tests run these plugins' tests too.
