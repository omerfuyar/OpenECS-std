# AGENTS.md

Instructions for AI agents working in this repository. Read this file fully before doing anything.

This repository is part of OpenECS. OpenECS's [AGENTS.md](https://github.com/omerfuyar/OpenECS/blob/dev/AGENTS.md) applies here in full: how to work with the owner, the boundaries, git and writing documents. Read it first; when this repository is checked out in OpenECS's `std/` folder, it is `../AGENTS.md`. This file names only what is different here.

## Where things are

| File                   | What it holds                                                                   |
| ---------------------- | ------------------------------------------------------------------------------- |
| [README.md](README.md) | Short introduction, and how to build and test.                                  |
| [DESIGN.md](DESIGN.md) | How the plugins are built. OpenECS's DESIGN.md holds the conventions they follow. |
| [TODO.md](TODO.md)     | Open questions and pending tasks.                                               |

OpenECS's OVERVIEW.md holds the product decisions for these plugins too. Read it, OpenECS's DESIGN.md and this DESIGN.md before you propose or change anything.

| Path         | What it holds                                                          |
| ------------ | ---------------------------------------------------------------------- |
| `plugins/`   | The plugins, one folder each (DESIGN.md, section 1).                   |
| `presets/`   | First-party presets.                                                   |
| `tests/`     | Tests of the plugins, with their presets and test plugins.             |
| `build.c`    | The build of this repository, which OpenECS's build includes.          |
| `.github/`   | Checks and rulesets.                                                   |
| `LICENSE.md` | The license, zlib.                                                     |

## Differences

- Work in a checkout of OpenECS with its submodules, in its `std/` folder, so you build and test with OpenECS (README.md).
- Releases are OpenECS's: this repository has no tags or release descriptions of its own. OpenECS's submodule names the commit that ships.
- After a change here is merged into `dev`, move OpenECS's `std/` submodule to it in a pull request to OpenECS.
- A change to OpenECS's plugin interface may need a change here; make both, and merge OpenECS's first.
