# AGENTS.md

Instructions for AI agents working in this repository. Read this file fully before doing anything.

This repository is part of OpenECS. OpenECS's [AGENTS.md](https://github.com/omerfuyar/OpenECS/blob/dev/AGENTS.md) applies here in full: how to work with the owner, the boundaries, git and writing documents. Read it first; when this repository is checked out in OpenECS's `std/` folder, it is `../AGENTS.md`. This file names only what is different here.

## Where things are

| File                   | What it holds                                                                     |
| ---------------------- | --------------------------------------------------------------------------------- |
| [README.md](README.md) | Short introduction, and how to build and test.                                    |
| [DESIGN.md](DESIGN.md) | How the plugins are built. OpenECS's DESIGN.md holds the conventions they follow. |

OpenECS's OVERVIEW.md holds the product decisions for these plugins too, and OpenECS's [TODO.md](https://github.com/omerfuyar/OpenECS/blob/dev/TODO.md) their open questions and pending tasks, in its section Standard plugins. Before you propose or change anything, read the Contents of it, of OpenECS's DESIGN.md and of this DESIGN.md, then every section the task touches. Read OpenECS's DESIGN.md section 1 and this DESIGN.md section 1 before you write code.

| Path            | What it holds                                                                                                                                                                                              |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `plugins/`      | The plugins, one folder each (DESIGN.md, section 1).                                                                                                                                                       |
| `presets/`      | First-party presets.                                                                                                                                                                                       |
| `shuild.c`      | The build of this repository, which OpenECS's build compiles and runs.                                                                                                                                     |
| `.github/`      | Checks, rulesets and the agents' check script.                                                                                                                                                             |
| `.clang-format` | How clang-format formats the C code, as OpenECS's does.                                                                                                                                                    |
| `.clang-tidy`   | What clang-tidy checks in the C code: names (DESIGN.md, section 1.3) and common bugs.                                                                                                                      |
| `.claude/`      | Claude Code's settings and notes: it formats each C file it edits and never edits `dependencies/`; `notes.md` says why the tools are set up as they are. OpenECS's `.claude/skills/` holds the procedures. |
| `LICENSE.md`    | The license, zlib.                                                                                                                                                                                         |

## Differences

- Work in a checkout of OpenECS with its submodules, in its `std/` folder, so you build and test with OpenECS (README.md).
- Releases are made with OpenECS's, at the same time and with the same version, as OpenECS's AGENTS.md says: `dev` is merged into `main` with a pull request that the owner reviews, and the owner tags it. A release's description is `.github/release-notes/vVERSION.md`; it says what changed in this repository, and OpenECS's description links to it. OpenECS's submodule names the commit that ships.
- Before each commit, run `.github/scripts/check.sh` here, in place of OpenECS's: it formats the changed C files of `plugins/` and checks them with clang-tidy. It needs OpenECS's Debug build first.
- After a change here is merged into `dev`, move OpenECS's `std/` submodule to it in a pull request to OpenECS.
- A change to OpenECS's plugin interface may need a change here; make both, and merge OpenECS's first.
