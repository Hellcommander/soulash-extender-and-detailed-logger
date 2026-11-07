# Variants: a plain-game version and an extender version of the same skill

Status: **implemented as the `Variants` plugin (`plugins/variants`), unit-tested
(`tests/variant_tests.inc`) and tested end to end against a stand-in game
process that imports the same file functions as Soulash 2. NOT YET RUN IN THE
GAME.** It ships with `Mode=log`: the hooks are installed and every extender
version the game *would* load is logged, but the game still loads the
plain-game files. The owner flips `Mode=active` after the first run looks
right (section 6).

## 1. What it is for

A mod should work for everyone, with or without the extender. But the deeper
version of a skill (conditional triggers, `effects.json` chains, `choose` /
`delay`, extender-only sub-abilities) cannot be expressed in plain game data,
and the plain-game workaround is often what you do **not** want with the
extender. The clearest example: `tools/validate_conditions.py --emit` and
`tools/effects_compile.py` compile triggers down to `trigger_ability_on_cast`
passives so they work in the plain game, while the `Triggers` plugin executes
the same rules itself, so a mod that ships both fires twice
(docs/triggers.md section 1).

Variants solve that: the mod keeps **one version for the plain game** in its
usual files, and puts **the extender version** of any file next to it. The
plain game never looks at it; the game with the extender loads it instead.

## 2. Layout

```
<mod>\
  mod.json
  abilities\Fireball.json            plain-game version (e.g. with compile-down passives in mind)
  passives.json                      plain-game passives, including generated_passives.json rows
  skills.json
  extender\
    conditions.json, effects.json    extender rules (docs/skill-conditions.md, docs/effects.md)
    variants\                        <- extender versions, same relative paths as the mod root
      abilities\Fireball.json        loaded INSTEAD of abilities\Fireball.json
      abilities\Fireball_Echo.json   extender-only: appears in abilities\ only with the extender
      passives.json                  loaded INSTEAD of passives.json (e.g. without the compile-down rows)
      milestones\                    a folder that exists only here: the whole folder is extender-only
        Fireball_Mastery.json
    editor_saves\                    created by the plugin, see section 4
```

Rules:

- A file in `variants\` whose twin exists in the mod is an **override**: the
  game reads the variant instead. It replaces the **whole file** (no JSON
  merge): copy the plain-game file and change what the extender version needs.
- A file or folder in `variants\` with no twin in the mod is an **addition**:
  the game sees it in the folder listing and reads it, as if it were in the mod.
  Use this for sub-abilities, milestones or passives that only the extender
  version references, so the plain game does not carry dead entries.
- Any data file the game reads through its own file calls can have a variant:
  abilities, passives, stackers, amplifiers, skills, milestones, entities,
  conversations, ... **Not** `mod.json` (refused) and nothing under
  `extender\` itself (the extender's own files have no plain-game version).
  Images and fonts are loaded through SDL, not through the hooked calls: do not
  expect variants of `.png` / `.ttf` to work.
- Folders nest up to 8 levels; at most 4096 entries per mod; folder and file
  kinds must match their twin (a file cannot replace a folder). Refused entries
  are logged with the reason and ignored; the rest of the mod still works.
- Ability ids stay what the files say. If the extender version of an ability
  keeps the same `id`, everything that references it (milestones, hotbar,
  saves, `Triggers` rules) keeps working with the deeper version. Give
  extender-only sub-abilities their own ids.

## 3. Choosing which version a player gets

| Player has | Game loads |
| --- | --- |
| no extender | the plain-game files; `extender\` is never read |
| extender without `Variants.dll`, or `Enabled=0` | the plain-game files |
| `Variants.dll`, `Mode=log` (default) | the plain-game files; the log lists what would change |
| `Variants.dll`, `Mode=active` | the extender versions and additions |

The choice is made when the game reads the files, i.e. at start-up and
whenever it reloads mods (returning to the main menu). Changing the INI needs a
restart. Saves only store ids, so a save made with one version loads with the
other; the abilities simply behave as the loaded version says.

## 4. The mod editor (why `editor_saves` exists)

The in-game mod editor does not save from the files on disk: it saves abilities
from the game's in-memory ability registry (build `526b6ab4`: `0x5473D0` reads
the registry through `0x8B4A0` / `map::at 0x4249F0`, then writes
`data/mods/{}/abilities/{}.json`, named by ability id). With variants active,
that registry holds the **extender** versions, so saving would overwrite the
plain-game files with extender data (and could write new files named by id
next to the original ones).

So while a mod has variants and `Mode=active`, **every write the game makes
inside that mod goes to `<mod>\extender\editor_saves\<same relative path>`**:
file writes, new folders, renames, copies and deletes. Deletes and renames act
on the `editor_saves` copy only. The mod's own files and its `variants\` are
never changed. Exceptions: `mod.json` at the mod root (the editor writes it from
its own mod info, not from loaded data) and anything under `extender\`. Mods
without variants are not affected at all. The first such write logs a hint:

```
Variants: the game is writing into <mod>, which has extender versions loaded (the mod editor?). Its writes go to <mod>\extender\editor_saves\ ...
```

To edit the **plain-game** version in the editor: `Mode=log` (or `Enabled=0`),
restart, edit, save. To edit the **extender** version: edit the files in
`variants\` by hand, or take what the editor left in `editor_saves\` and copy
the parts you want into `variants\`. This also keeps the owner's rule that the
editor never rewrites anything below `extender\` other than its own
`editor_saves\` output.

## 5. How it works (and why it survives game updates)

Soulash 2 reads mod data with the **dynamic UCRT** (`std::ifstream` →
`_wfsopen` / `_fsopen`, imported from `api-ms-win-crt-stdio-l1-1-0.dll`; the
JSON reader `0x4E93B0` "Unable to open file {}") and lists folders and handles
everything else with the **statically linked STL filesystem**, which calls
KERNEL32 directly (`0x823DD0`-`0x824FF8` in `526b6ab4`: `CreateFile2`,
`FindFirstFileExW`, `FindNextFileW`, `FindClose`, `GetFileAttributesExW`,
`MoveFileExW`, `CopyFile2`, `CreateDirectoryW`); `_stat64i32` / `_mkdir` come
from the UCRT as well. The ability loader `0x4ED7E0` lists `{}/abilities` and
reads each file.

The plugin replaces those **import slots of the game executable** with
pass-through functions (the same technique the ability-bar plugins use for
`SDL_PollEvent`). Nothing inside the game's code is patched, no RVA is used,
and nothing is keyed by an exe hash: slots are found by function name in the
import directory at load. A build that stops importing one of them loses only
that hook (`not hooked on this build: ...`); without `_wfsopen` and `_fsopen`
the plugin stays inactive.

Per call: the path is made absolute (`GetFullPathNameW`, the game uses paths
relative to its folder), canonicalised and looked up in an index built at
start-up from `<game>\data\mods\*\extender\variants\` (pure code in
`plugins/common/variant_index.hpp`). Reads of a mapped path open the variant
(falling back to the game's own path if the variant vanished); listings of a
folder with additions get them appended after the real entries, and listings
of an extender-only folder are served from the variant folder; writes are
handled as in section 4. Paths that do not contain `mods` skip all of that.
The index is immutable once the hooks are in; listing state is behind a lock.

Timing: the extender's plugins load on its own thread right after the game's
first BugSplat call, before SDL starts (docs/game-notes.md), and the mods are
read later. The plugin logs how long after process start its hooks went in and
the first mod file read it saw; if the game ever read mods before that, the
next mod reload (main menu) picks the variants up.

## 6. Configuration and first run

`<game>\SoulashExtender\Plugins\Variants.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | 0 = hook nothing; every mod loads its plain-game files |
| `Mode` | `log` | `log`: plain-game files load, would-be changes are logged. `active`: extender versions load, writes into variant mods go to `editor_saves` |
| `LogFiles` | 1 | one line the first time each variant is used |
| `MaxLogLines` | 300 | cap for per-file lines (problems and summaries always log) |

The build deploys `Variants.dll` and the INI template (existing INI kept) and
pins the hash like the other plugins. With no mod shipping `extender\variants\`
the plugin hooks nothing and reports "idle".

First run (owner):

1. Give one mod a variant, e.g. copy `abilities\<some ability>.json` to
   `extender\variants\abilities\` and change its `name` or `description` so
   the difference is visible in game; optionally add an extra
   `extender\variants\abilities\<new>.json`.
2. Start the game (`Mode=log`). Expected `Variants:` lines in
   `logs\proxy-crash.log`:
   - `<mod>: N extender version(s): O override(s), A added file(s), F added folder(s)`
   - `Mode=log (...): N extender version(s) in M mod(s) (...); hooked _wfsopen _fsopen _stat64i32 _mkdir CreateFile2 FindFirstFileExW FindNextFileW FindClose GetFileAttributesExW MoveFileExW CopyFile2 CreateDirectoryW; T ms after process start`
   - `first read inside a mod seen: ...\data\mods\<mod>\... (the hooks were in place before the game read it)`
   - `would use (Mode=log) loading extender version of <mod>\abilities\<file>` and, for an addition, `would use (Mode=log) listing extender-only file ...`
   The game itself must look exactly as before.
3. Set `Mode=active`, restart: the lines lose the `would use (Mode=log)`
   prefix and the changed name / description shows in game (skill window,
   ability tooltip); the extra ability exists (e.g. referenced by a milestone
   or a `Triggers` rule).
4. Optional editor check: open that mod in the mod editor and save; expect the
   `editor_saves` hint and `write <mod>\abilities\... -> <mod>\extender\editor_saves\...`
   lines, and the mod's own files unchanged.
5. Rollback: `Mode=log`, or `Enabled=0`, or delete `Variants.dll`. The plugin
   changes nothing on disk except creating `editor_saves\` content when the
   game writes into a variant mod in `Mode=active`.

## 7. Open points

- Not run in the game yet: whether every data loader goes through the hooked
  calls (the ability loader, the JSON reader and the editor do, statically) and
  whether the plugin's hooks are always in before the first mod load.
- Whole-file replacement only. A JSON merge patch (`variants\passives.patch.json`
  changing two passives of two hundred) would need the plugin to write merged
  files somewhere and is not implemented.
- `editor_saves\` is never cleaned up by the plugin; delete it when done.
- Steam Workshop publishing writes `steam_publish_id` into `mod.json`, which is
  allowed; anything else the publish flow writes into a variant mod would land
  in `editor_saves\`. Publish with `Mode=log` to be safe.
