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
   `plugins/test_bot/TestBot.script`). Or skip this: without a script the bot
   idles and runs whatever is dropped into its inbox (see "Sending the bot
   scripts" below).
3. In `<game>\SoulashExtender\Plugins\TestBot.ini` set `Enabled=1`.
4. Start the game. Lines starting with `TestBot:` in `logs\proxy-crash.log`
   show what happened; the run ends with a line ending in `-> PASS` or `-> FAIL`.
   While the game runs, `<game>\SoulashExtender\TestBot\status.json` and
   `live.png` show the bot's state and the screen (see "Live feedback while
   the game runs" below), so nobody has to wait for the game to exit.

Set `Enabled=0` and restart the game to earn achievements normally again.
Blocking cannot be switched off mid-session on purpose.

`TestBot.ini` options: `Script` (a plain file name in the plugin folder),
`StopOnFailure` (0: run to the end and count failures; 1: stop at the first),
`LogBlockedCalls` (one line per blocked Steam call, at most 200 per session),
`LiveFrameMs` (how often `live.png` is refreshed; default 2000, minimum 250,
0 turns it off), `Inbox` (1: scripts dropped into `TestBot\inbox` run when the
bot is idle, see "Sending the bot scripts"; 0 closes the inbox).

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
| `type <text>` | typed text: per character a key down + `SDL_TEXTINPUT`, then the key up on the next frame (text events alone lose the second of two equal characters in a row), for the editors' text fields. Printable ASCII, at most 200 characters. Leading and trailing blanks are dropped and `#` starts a comment, so neither can be typed. Click into the field first; use `key backspace` / `key enter` for editing keys |
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
  started, one `step N/M: line L <command>` line as each step begins (the
  command with its arguments, defaults filled in, e.g. `click 244 202 left`),
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

## Live feedback while the game runs

Whoever drives a run (a person, or an agent that wrote the script) does not
have to wait for the game to exit. While `Enabled=1`,
`<game>\SoulashExtender\TestBot\` holds:

* `status.json`: the bot's state. It is rewritten through a temporary file
  and a rename (a reader never sees a partial file) on every change, at most
  every 100 ms, and at least once a second while the game pumps events. The
  first version is written as soon as the plugin loads, the last one
  (`"state":"exited"`) when the game exits normally.
* `live.png`: the current frame (24-bit RGB, drawable size), refreshed every
  `LiveFrameMs` milliseconds (default 2000, minimum 250, 0 = off) and replaced
  atomically the same way. It keeps refreshing after the script has finished,
  for as long as the game runs, and it does not need a script at all: with a
  missing or broken script the bot still shows the screen.
* `results.txt` in the run folder is written line by line without buffering,
  so it can be tailed (`Get-Content -Wait`).

`status.json` fields:

| Field | Meaning |
| --- | --- |
| `schema` | 1 |
| `updated_utc` | when the file was written. Older than about 3 s while the game process exists: the game is not pumping events (loading, a modal dialog, hung) |
| `run_ms` | milliseconds since the run folder was created (the clock used in `results.txt`) |
| `pid` | the game's process id |
| `state` | `running`; `waiting` (the step waits for something, see `waiting_for`); `finished`; `aborted` (a timeout, or `StopOnFailure=1`); `stopped` (a `stop` file in the inbox); `idle` (ready, no script yet: waiting for the inbox); `not_running` (the bot cannot run scripts this session: blocking refused, SDL hooks failed); `exited` (the game closed normally). Until the first job starts, `reason` carries the bot's last log line (why the INI script did not load, or why nothing can run) |
| `result` | `null` while the script runs, then `PASS`, `FAIL` or `STOPPED` (for the latest job) |
| `run_dir` | the run folder |
| `job` | the current or last script: `index` (1 = the first), `name`, `source` (`ini` or `inbox`); `null` before the first |
| `steps_total`, `steps_done` | script length and completed steps |
| `step` | the step in progress: `index` (1-based), `line`, `command` (the script line, normalised), `elapsed_ms`; `null` when no step runs |
| `waiting_for` | what the current step waits for and the time left before the run aborts: `wait_world` ("the game world (load a save or start a new game); 87 s before the run aborts"), `wait_screen` (the screen wanted and the one drawn now), `wait` (ms left), `snapshot` (frames captured so far); `null` otherwise |
| `checks`, `failures` | counts so far |
| `last_failure` | the last `FAIL` message (`line 12 (expect_screen): screen is X, expected Y`), or `null` |
| `screen_detection`, `screen` | whether screen detection is on, and the screen drawn within the last second (`null` without detection, or when nothing was drawn) |
| `world_detection`, `in_world`, `bar_set` | whether world detection is on; whether the player is in the game world; the current ability-bar set (`null` outside the world) |
| `window`, `drawable` | `[width, height]` of the window (`click` coordinates) and of the drawable (snapshot pixels), or `null` |
| `blocked_steam_calls` | achievement and stat calls stopped so far |
| `screen_capture`, `snapshots_written` | whether the frame hook is in place; PNGs written by `snapshot` steps so far |
| `live_frame` | `file`, `every_ms`, `frames` written, `age_ms` of the newest, `size`, `errors`; `null` when off |
| `inbox` | `dir`; `accepting` (true when the next dropped script would start at once); `pending` scripts waiting; `jobs_done`; `last_error` (a dropped script that could not run, with its parse error); `null` with `Inbox=0` |
| `loop_hz` | event-loop iterations per second, a rough frame rate (0 while the game is busy) |
| `log_tail` | the last 16 `results.txt` lines, oldest first |

The `exited` record is shorter: `schema`, `updated_utc`, `run_ms`, `pid`,
`state`, `script` (the state the last job was in: `running`, `finished`,
`aborted`, `stopped`, `idle`, `not_running`), `result`, `steps_done`,
`steps_total`, `checks`, `failures`. A crash or kill leaves the last heartbeat in place instead;
`pid` tells whether that process still exists.

Polling from PowerShell:

```powershell
$dir = 'D:\games\Steam\steamapps\common\Soulash 2\SoulashExtender\TestBot'
Get-Content "$dir\status.json" -Raw | ConvertFrom-Json | Select-Object state, result, waiting_for, screen, failures, @{n='step'; e={$_.step.command}}
```

How an agent uses it: start the game (or ask for it), then poll `status.json`
every second or two and read `live.png` when it needs to see the screen.

* `waiting` on `wait_world`: a person has to load a save; `waiting_for` says
  how long the bot will wait.
* `waiting` on `wait_screen` with the wrong screen in `waiting_for`: the
  script's last click missed; `live.png` shows where the mouse went.
* `aborted`: `last_failure` names the step and the reason.
* `finished`: `result` gives PASS or FAIL without opening `results.txt`;
  `log_tail` has the summary line.
* `not_running`: `reason` is the bot's own last log line (hash not pinned is
  not among them: then the plugin did not load at all and there is no file,
  see `compat-report.txt` and `logs\proxy-crash.log`).
* `updated_utc` stale with the process alive: the game is loading or stuck.
* Between runs, `live.png` lets the agent guide a person at the keyboard
  ("click the Mods button, top left") without snapshots in the script.

### Sending the bot scripts: the inbox

With `Inbox=1` (the default) the bot creates `<game>\SoulashExtender\TestBot\inbox\`
and, every 500 ms, looks there:

* **`*.script` files** are jobs. Whenever no script is running, the bot takes
  the first one in name order (`01_open_editor.script` before
  `02_name_entity.script`), moves it into the run folder as
  `jobNN_<name>.script` (so it runs once and the run keeps a copy), parses it
  and runs it like the INI script. Names: letters, digits, `_`, `-`, `.`;
  same limits as any script (64 KiB, 2000 steps). A script that does not
  parse is taken, skipped and reported in `status.json` as `inbox.last_error`
  and in `results.txt` as `inbox: <name> skipped: line N: ...`.
* **A file named `stop`** (no extension, any content) aborts the running
  script at the next poll. The job ends with `-> STOPPED`; the file is deleted.
  A stop does not release a held key or modifier: send `mod none` or `key_up`
  in the next script if the stopped one was mid-press.

Each job gets its own `job N (inbox) <name> ACTIVE: ...` line and its own
`script <name> finished ... -> PASS|FAIL` line in `results.txt`, and
`status.json` shows it under `job` with the usual `step`, `checks`,
`failures`, `result`. Snapshot and screen hooks that an inbox script needs
and the INI script did not are installed when that job starts; their
`screen capture ready` / `screen detection on` lines appear then.

The INI script is optional: with none, the bot logs `idle: waiting for a
script in the inbox` and `state` is `idle` until the first job. Achievements
and stats are blocked all the same, from the moment the bot is enabled.

A file still open for writing is left alone until it is closed (the bot opens
scripts without write sharing), but write the script under another name and
rename it, so the bot never sees half a file:

```powershell
$inbox = 'D:\games\Steam\steamapps\common\Soulash 2\SoulashExtender\TestBot\inbox'
@"
wait_screen MainMenuScreen
log_screen
click 244 202
wait_screen ModsScreen
snapshot mods_page
"@ | Set-Content "$inbox\01_mods_page.tmp" -Encoding ascii
Move-Item "$inbox\01_mods_page.tmp" "$inbox\01_mods_page.script"
```

Then poll `status.json` until `job.name` is `01_mods_page.script` and `state`
is `finished`, `aborted` or `stopped`; `result` and `log_tail` say how it
went, and the run folder has the snapshots. To interrupt:

```powershell
New-Item "$inbox\stop" -ItemType File | Out-Null
```

The inbox is the extender's own folder (like `SoulashExtender.ini`), not a
game file: writing there is within the read-only rule for the game's files.
Anyone who can write into the game folder can drive the bot this way, which
is no more than they could do by replacing the plugin itself.

Cost: a live frame reads the back buffer once per interval (a few
milliseconds at 1080p) and encodes the PNG on a worker thread. Only one live
frame is in flight at a time, a `snapshot` step has priority in the same
frame, and a read that fails (minimised window) is retried, while a fault
switches capture off for the session as for `snapshot`.

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
confirmed yet. The live feedback files (`status.json`, `live.png`) and the
inbox (both added 2026-11-06) have not run in the game yet either; the step
descriptions and JSON quoting they use are unit-tested.
