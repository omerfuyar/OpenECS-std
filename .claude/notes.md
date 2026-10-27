# Notes

Notes for agents and the owner that belong in no other document: why the tools are set up as they are. OpenECS's `.claude/notes.md` holds the notes that all the repositories share, and OpenECS's `.claude/skills/` the procedures; agents work in OpenECS's `std/` folder, where they apply.

## Tools

- `.github/scripts/check.sh` runs clang-tidy on `compile_commands.json`, which shuild writes here in OpenECS's build, as OpenECS's notes say.
- Checks that are off in `.clang-tidy`, besides OpenECS's (its notes say why):
  - `bugprone-suspicious-include`: a plugin includes the C file of a library it compiles in, as `audio` and `net` do.
  - `bugprone-casting-through-void`: inotify gives `fs` its events as bytes.
- The naming checks follow DESIGN.md, section 1.3: functions are PascalCase, such as `MixerFreeEcho`, or `ECSPlugin_Init`.
- TextGrid's table of wide characters is between `clang-format off` and `on`, so it keeps its columns.
