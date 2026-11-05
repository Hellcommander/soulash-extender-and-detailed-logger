# TestBot: scripted test runs with achievements and stats blocked

`plugins/test_bot` builds `TestBot.dll`, a built-in test bot. It replays a
step script (keys, modifier state, mouse wheel, clicks, waits, "wait until in
the game world", checks on the ability-bar set) inside the real game. **While
it is enabled, Steam achievements and stats are blocked for the whole session.**
Test runs therefore never unlock, clear or upload anything.

It is **off by default** and, like every native plugin, refused unless pinned
in `[PluginHashes]` when `RequireHash=1`.

## Turning it on

1. Pin the hash the build prints (`TestBot=<sha256>`) in the extender's
   `<game>\SoulashExtender.ini` under `[PluginHashes]` (the build does this
   itself when that section exists).
2. Copy a script into `<game>\SoulashExtender\Plugins\` (the build does not
   deploy scripts; the example is `.build\plugins\TestBot.script`, source
   `plugins/test_bot/TestBot.script`).
3. In `<game>\SoulashExtender\Plugins\TestBot.ini` set `Enabled=1`.
4. Start the game. Lines starting with `TestBot:` in `logs\proxy-crash.log`
   show what happened; the run ends with a line ending in `-> PASS` or `-> FAIL`.

Set `Enabled=0` and restart the game to earn achievements normally again.
Blocking cannot be switched off mid-session on purpose.

`TestBot.ini` options: `Script` (a plain file name in the plugin folder),
`StopOnFailure` (0: run to the end and count failures; 1: stop at the first),
`LogBlockedCalls` (one line per blocked Steam call, at most 200 per session).

## How achievements are blocked

The game fetches `ISteamUserStats` through its import of
`steam_api64.dll!SteamInternal_FindOrCreateUserInterface` (the accessor at
`0x809970` in build `60b72c10` requests `STEAMUSERSTATS_INTERFACE_VERSION013`).
TestBot wraps that import. When a `STEAMUSERSTATS_INTERFACE_*` object comes
back, it gives that object a private copy of its vtable in which the write methods
return `true` without doing anything:

| Method | v013 slot (from `steam_api64.dll`) |
| --- | --- |
| `SetStat(float)` / `SetStat(int32)` | 2 / 3 (MSVC orders overloads in reverse) |
| `UpdateAvgRateStat` | 4 |
| `SetAchievement` / `ClearAchievement` | 6 / 7 |
| `StoreStats` | 9 |
| `IndicateAchievementProgress` | 12 |
| `ResetAllStats` | 20 |

Read methods (`GetAchievement`, `GetStat`, ...) pass through unchanged. The slot
numbers are not hard-coded: at load TestBot decodes `steam_api64.dll`'s own flat
exports (`SteamAPI_ISteamUserStats_SetAchievement` etc. are
`mov rax,[rcx]; jmp [rax+slot]`), so a different steam_api64.dll is handled or
refused. `native-tests.exe --steam-stats-slots <steam_api64.dll>` prints them.

Blocking also stops stat uploads. Without that, achievements tied to stat
thresholds could still unlock on Steam's side.

**Fail closed:** blocking is installed before anything else. If any export is
missing or has an unexpected shape, or the import cannot be hooked, the bot
logs `REFUSING to run` and does nothing. Plugins load at the game's first
BugSplat call, before `SteamAPI_Init`, so the hook is in place before the
game can ask for the interface. As a second line of defence, the first time
the bot sees the game world it requests the interface itself (Steam returns
the same cached object the game holds) and patches it if needed. Each
patched object is logged as `achievements+stats BLOCKED on ...`, and each
stopped call as `BLOCKED SetAchievement(<id>) ...`.

Scope: this blocks what reaches Steam. Any achievement bookkeeping the game
keeps itself (there is an `Achievements` ECS component) is untouched.

## Script format

One command per line; `#` starts a comment; keywords are case-insensitive;
times are milliseconds. A script has at most 2000 steps and 64 KiB; a parse
error names the line and the bot does not run.

| Command | Effect |
| --- | --- |
| `wait <ms>` | pause (0..600000) |
| `wait_world [timeout_ms]` | wait until the player is in the game world (default 120000). A timeout aborts the run, and so does world detection being off (immediately, not after the timeout) |
| `key <key> [hold_ms]` | press and release (default hold 50) |
| `key_down <key>` / `key_up <key>` | half a key press |
| `mod <none\|ctrl\|shift\|alt>[+...]` | set SDL's modifier state (`SDL_SetModState`; what `SDL_GetModState` reports, e.g. to AbilityBarWheel) |
| `wheel <notches>` | mouse wheel, `+` up / `-` down, one notch per frame |
| `click <x> <y> [left\|right\|middle]` | move the mouse there (window pixels) and click |
| `expect_bar_set <n>` | check `AbilityUser+0x68`; failure is counted (`PASS`/`FAIL line N` in the log) |
| `log <text>` | marker line in the log |
| `snapshot <name> [count] [interval_ms]` | capture `count` frames (1..20, default 1), `interval_ms` apart (0..60000, default 500), as `<name>_01.png`, `<name>_02.png`, ... in the run folder. `<name>`: 1..32 of `A-Z a-z 0-9 _ -`. The step ends when every frame is captured |
| `mods_errors [max_errors]` | copy `<game>\logs\errors.log` from its **last** `Loading mods:` line into `results.txt` (the earlier core-only pass is skipped). With `max_errors`, more `[error]` lines than that is a counted FAIL |
| `wait_screen <Screen> [timeout_ms]` | wait until that screen class is the one being drawn (default 30000). A timeout aborts the run and names the screen drawn instead; screen detection being off or an unknown name aborts immediately |
| `expect_screen <Screen>` | check the screen drawn within the last second; counted PASS/FAIL |
| `log_screen` | log the current screen, the window size (what `click`/`move`/`drag` coordinates mean) and the drawable size (snapshot pixels) |
| `type <text>` | text input (`SDL_TEXTINPUT`), one character per frame, for the editors' text fields. Printable ASCII, at most 200 characters. Leading and trailing blanks are dropped and `#` starts a comment, so neither can be typed. Click into the field first; use `key backspace` / `key enter` for editing keys |
| `move <x> <y>` | move the mouse there (hover, tooltips) |
| `dclick <x> <y> [left\|right\|middle]` | double click (the second press carries `clicks=2`) |
| `drag <x1> <y1> <x2> <y2> [left\|right\|middle]` | press at 1, move to 2 in 10 steps (one per frame; the motion events carry the held button), release at 2 |

Keys: `a`-`z`, `0`-`9`, `f1`-`f12`, `space`, `enter`/`return`, `escape`/`esc`,
`tab`, `backspace`, `up`/`down`/`left`/`right`, `lctrl`/`rctrl`/`lshift`/
`rshift`/`lalt`/`ralt`, or an SDL keycode (decimal, e.g. a keymap.json
`ase_key`, or `0x` hex).

Input goes in through `SDL_PushEvent`, so the game and the other plugins see
it exactly as they see real input (AbilityBarWheel swallows wheel notches, and
with `SuppressModifierKey=1` it hides Ctrl key events: an injected
`key lctrl` then does not reach the game either). The runner is clocked by a
pass-through `SDL_PollEvent` import hook on the game thread. `wait_world` and
`expect_bar_set` use a pass-through `DrawActionBar::update` hook, installed
only when `VerifyAbilityBarLayout` accepts the build. On any other build those
two commands fail and everything else still works.

## Run folder and results

Every run with `Enabled=1` creates
`<game>\SoulashExtender\TestBot\<UTC time, e.g. 20261105-143000Z>\` and
writes its path into `<game>\SoulashExtender\TestBot\latest.txt`. The run folder holds:

* `results.txt`: every TestBot log line with milliseconds since the run
  started, one `step N/M: line L <command>` line as each step begins,
  `PASS`/`FAIL` lines, blocked Steam calls, `errors.log` extracts and the final
  `script finished ... -> PASS|FAIL` summary;
* the snapshot PNGs (24-bit RGB, full drawable size, including the UI).

If the game exits normally while a script is still running, the last line is
`game exited during step N/M: script NOT finished`. A crash or kill leaves no
such line.

Snapshots hook the game's `SDL2.dll!SDL_GL_SwapWindow` import, but only when
the script contains a `snapshot` step. Just before a frame is presented, the
hook reads the finished back buffer with `glReadPixels`. It saves and
restores every piece of OpenGL state it touches (pack alignment, row length,
skips, read buffer, read framebuffer and pack buffer bindings). PNG encoding
and writing run on a separate thread, compressed with the game's own
`zlib1.dll` when it is loaded. At most 3 frames are written at a time, and a
4K frame costs about 25 MB of memory while it is being written. If no frame is
presented for 5 s, the step fails rather than hangs.

`mods_errors` reads the newest 4 MiB of `errors.log` and copies at most 3000
lines. Run it after `wait_world`. Before that, the game may still be in its
first, core-only loading pass.

Example (visual check of an AoE ability on the bar):

```
wait_world
key 3                      # select the ability in bar slot 3
click 960 540              # target a tile (window pixels)
snapshot storm 8 150       # 8 frames, 150 ms apart
wait 3000
snapshot storm_after 1
mods_errors
```

## Screens and the editors

`<Screen>` is the RTTI class name of one of the game's full screens. On build
60b72c10 these are accepted:

| Screen | What it is |
| --- | --- |
| `MainMenuScreen`, `OptionsScreen`, `CreditsScreen`, `ModsScreen` | main menu and its pages |
| `PlayerCreationScreen`, `WorldGenerationScreen`, `LoadingScreen`, `CemeteryScreen` | new game, loading, graveyard |
| `PreGameScreen` | not identified yet |
| `GameScreen` | the game world |
| `MapMakerScreen` | editor: map editor (F1); the editors open here |
| `EntityMakerScreen` | editor: entities (F2) |
| `AnimatorScreen` | editor: abilities and animations (F3): ability list, animation list and a live preview |
| `ProceduralMapMakerScreen` | editor: procedural maps (F5) |
| `DataTableScreen` | editor: data tables (F6; not visited yet, so the class is not confirmed) |

Confirmed in the game (build 60b72c10, window and drawable 1920x1080, first
live run 2026-11-05):

* **Way in:** `MainMenuScreen` → the Mods button (window pixels 244,202) →
  `ModsScreen` → a button on the Mods page (not identified yet) →
  `MapMakerScreen`. There is no separate editor menu screen; the editors open
  straight into the map editor.
* **Switching editors:** all editors share a button bar across the top
  (y ≈ 26) with function keys: F1 Map Editor, F2 Entities, F3 Abilities
  (`AnimatorScreen`, not `DataTableScreen`), F4 Main Menu (the game reloads all
  mods on the way out), F5 Procedural Maps, F6 Data Tables, F9 Reload Assets.
* **Escape** does nothing in any editor and changes no screen.
* **Entity editor** (`EntityMakerScreen`): buttons "Save entity (ctrl+s)" and
  "New entity"; Name field at about (738,111), Description at about (738,290).
* **Abilities editor** (`AnimatorScreen`): buttons "New Ability" and "New
  Animation"; Abilities search field at about (220,93), Animation search field
  at about (220,570).

The coordinates are window pixels at 1920x1080 and will differ at other window
sizes; `log_screen` logs the window size. To map something new, run a script
with `log_screen` and `snapshot` after each step and read `results.txt`, which
also logs every change as `screen: A -> B`.

How it works: every full screen is a class whose vtable slot 7 draws it, and
that function begins by clearing the frame. TestBot accepts a class only when
its slot-7 function (read from the exe file, so another plugin's hooks don't
get in the way) calls the game's `OPENGL32.dll!glClear` import within its
first 0x120 bytes. The three character-creation pages whose slot 7 is an empty
stub, and the in-game panels with shorter vtables, are skipped and listed in
the log. Each accepted slot 7 gets a 25-byte pass-through thunk that records
`this` and jumps on with every register and the stack untouched. Once per
frame the runner maps the last drawn object's vtable back to its class.
The hooks are installed only when the script uses `wait_screen`,
`expect_screen` or `log_screen`. The log line `screen detection on: draw slot
hooked on ...` lists the screens a script can name.

Reading or checking the values inside editor fields is not supported (that
needs per-widget reverse engineering). Check results visually with `snapshot`,
or through the files the editor saves.

Example (open the entity editor from the main menu, type a name, check
visually; coordinates for a 1920x1080 window):

```
wait_screen MainMenuScreen 120000
log_screen                     # window size for the coordinates below
click 244 202                  # main menu: Mods
wait_screen ModsScreen
snapshot mods_page             # find the editor button here, then click it:
# click <x> <y>                # Mods page: open the editors (button not identified yet)
wait_screen MapMakerScreen
key f2                         # Entities
wait_screen EntityMakerScreen
click 738 111                  # the Name field
key backspace
type Storm caller
snapshot entity_named
mods_errors
```

## Not verified yet

Everything above is static analysis plus synthetic tests. (The PNG writer is
checked by decoding its output with .NET. The OpenGL capture itself has never
run against the game.) The first live run should confirm: `achievement/stat blocking armed`, an
`achievements+stats BLOCKED on STEAMUSERSTATS_INTERFACE_VERSION013` line
(which appears when the game first asks for the interface), and the example
script ending with `-> PASS`.

First live run (2026-11-05, build 60b72c10, 49/49 steps, 0 failures): screen
detection, `snapshot` (12 valid PNGs), `wait_world`, achievement blocking and
`mods_errors` all came on and worked, and the screen changes above were logged.
That script did not use `type`, `move`, `dclick` or `drag`, so those are not
confirmed yet.
