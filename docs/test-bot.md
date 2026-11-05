# TestBot: scripted test runs with achievements and stats blocked

`plugins/test_bot` builds `TestBot.dll`, a built-in test bot. It replays a
step script (keys, modifier state, mouse wheel, clicks, waits, "wait until in
the game world", checks on the ability-bar set) inside the real game. **While
it is enabled, Steam achievements and stats are blocked for the whole session.**
Test runs therefore never unlock, clear or upload anything.

It is **off by default** and, like every native plugin, refused unless pinned
in `[PluginHashes]` when `RequireHash=1`.

## Turning it on

1. Pin the hash the build prints (`TestBot=<sha256>`) in the game's
   `SoulashExtender.ini` under `[PluginHashes]`.
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
| `wait_world [timeout_ms]` | wait until the player is in the game world (default 120000). A timeout aborts the run |
| `key <key> [hold_ms]` | press and release (default hold 50) |
| `key_down <key>` / `key_up <key>` | half a key press |
| `mod <none\|ctrl\|shift\|alt>[+...]` | set SDL's modifier state (`SDL_SetModState`; what `SDL_GetModState` reports, e.g. to AbilityBarWheel) |
| `wheel <notches>` | mouse wheel, `+` up / `-` down, one notch per frame |
| `click <x> <y> [left\|right\|middle]` | move the mouse there (window pixels) and click |
| `expect_bar_set <n>` | check `AbilityUser+0x68`; failure is counted (`PASS`/`FAIL line N` in the log) |
| `log <text>` | marker line in the log |

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

## Not verified yet

Everything above is static analysis plus synthetic tests. The first live run
should confirm: `achievement/stat blocking armed`, an
`achievements+stats BLOCKED on STEAMUSERSTATS_INTERFACE_VERSION013` line
(which appears when the game first asks for the interface), and the example
script ending with `-> PASS`.
