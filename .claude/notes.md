# Notes

Notes for agents and the owner that belong in no other document: why the tools are set up as they are. OpenECS's `.claude/notes.md` holds the notes that all the repositories share, and OpenECS's `.claude/skills/` the procedures; agents work in OpenECS's `std/` folder, where they apply.

## Tools

- `.github/scripts/check.sh` gives clang-tidy the flags of a plugin's Debug build by hand, with the include folders of every plugin's libraries, because shuild writes no compilation database. When `shuild.c` adds a library or changes a plugin's flags, change `check.sh` with it.
- glslang's build writes one of its headers into `.shu/Debug/cmake/<compiler>/glslang/include`, such as `gcc`, so clang-tidy checks `gpu` only after a Debug build.
- Checks that are off in `.clang-tidy`, besides OpenECS's (its notes say why):
  - `bugprone-suspicious-include`: a plugin includes the C file of a library it compiles in, as `audio` and `net` do.
  - `bugprone-casting-through-void`: inotify gives `fs` its events as bytes.
- The naming checks follow DESIGN.md, section 1.3: functions are PascalCase, such as `MixerFreeEcho`, or `ECSPlugin_Init`.
- TextGrid's table of wide characters is between `clang-format off` and `on`, so it keeps its columns.
