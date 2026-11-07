# Triggers: conditional triggers executed by the extender

Status: **implemented as the `Triggers` plugin (`plugins/triggers`), unit-tested
against synthetic memory and byte patterns; first runs in the game on
2026-11-07 (TestBot-driven, `Mode=log`, build 526b6ab4): all eight hooks
located and installed, cast / attack / kill / turn / damaged / dodge events and
delayed casts seen with live state, crit and block not yet observed (no
critical hit and no deflection passive in the test fight); `Mode=active` not
yet tried.** Events:
cast, attack, kill, turn, damaged, dodge, block, crit; delayed casts wait for
the holder's turns. Every game fact below is static analysis of build
`526b6ab4` (0.10.6.1, 2026-11-06/07) unless marked otherwise. The plugin ships with `Mode=log`, which installs the
hooks and logs what would happen without queueing anything; the owner flips
`Mode=active` after the first run looks right (section 6).

## 1. Why the extender owns triggers

The engine chains abilities only through a handful of passive effects:
`trigger_ability_on_cast` (effect 117: when ability A is cast, cast B with a
flat chance) and `on_attack_cast` (effect 100: on every attack, cast B with a
chance). Nothing can make them conditional, the in-game mod editor exposes only
stackers (and, per the owner, can botch abilities that carry real stackers when
a mod is opened in it), and `trigger_ability` / `trigger_ability_on_turn` do not
exist in the executable. So the extender now does the whole job itself:

- rules are **data in the mod's `extender\` folder** (`conditions.json`
  `triggers`, `effects.json` `triggers`; docs/skill-conditions.md and
  docs/effects.md), which the editor never rewrites;
- the plugin hooks the two engine routines that resolve passive triggers and
  runs the extender's rules there, with a **state snapshot of the holder** taken
  from the game's own component registry;
- firing uses the **engine's own primitive**: a copy of the ability from the
  ability registry is pushed onto the holder's queued-ability list, and the game
  casts it on its own thread, at the holder's current target, exactly as it
  does for its own trigger passives.

Compile-down (`tools/validate_conditions.py --emit`) still exists for people
without the extender, but a mod that ships both the generated trigger passives
and the plugin would fire twice: with the plugin installed, do not merge
`generated_passives.json` for `triggers`.

This is pure JSON data plus a native plugin. The extender's sandboxed Lua
(`SoulashExtender\Scripts`, `ext.*` API) is not involved and nothing here claims
any name the game's planned official Lua support (December) may use.

## 2. Rules

Per mod, the plugin loads at start-up:

| File | What is used | Semantics |
| --- | --- | --- |
| `<mod>\extender\conditions.json` | `triggers[]`: `{id, on: {cast: A} \| {<event>: true}, cast: B, chance, when, on_unavailable}` | On the event, if `when` holds (three-valued; unknown fails closed unless `on_unavailable: active`) and the chance rolls, queue `B` on the holder. |
| `<mod>\extender\effects.json` | `triggers[]` with any `on` event | `effects::ResolveEvent`: the evaluator-owned parts (choose / delay / repeat-every / non-native `when`) resolve to generated group abilities, which are queued. A cast with a `delay` waits for that many **turn ticks of the holder** (section 3.1), then is queued. |

Events, with the holder (whose state `when` sees and who casts `B`) and the
target (`target_*` predicates; also the entity the queued ability is aimed at
when the holder's own target is that entity):

| `on` | Fires when | Holder | Target |
| --- | --- | --- | --- |
| `{cast: A}` | the holder cast ability `A` | caster | the caster's current target (`AbilityUser+0x1A0`) |
| `{attack: true}` | after every melee / ranged attack, hit or miss | attacker | attacked entity |
| `{kill: true}` | an entity died with a last attacker recorded | killer | victim |
| `{turn: true}` | once per game turn for every entity with an Effects component (every creature) | that entity | its current target |
| `{damaged: true}` | an attack or ability did `> 0` damage | damaged entity | attacker |
| `{dodge: true}` | an attack missed / was parried (the engine grants Parry skill experience) | defender | attacker |
| `{block: true}` | the defender's deflection passive stopped an attack before it was resolved | defender | attacker |
| `{crit: true}` | an attack resolution ended with at least one critical hit | attacker | defender |

Several rules may listen to the same ability or event (that is the point:
"multiple triggers"); each is evaluated and rolled independently, up to
`MaxCastsPerEvent` casts per event. Rules are evaluated for **every** entity
the event happens to, not only the player: a rule on an NPC's ability fires for
the NPC, and a `turn` rule is evaluated for every creature every turn. Scope
such rules with `holder_tag: Player`, a `stacker` or an `equipped` test; the
evaluation itself is cheap (a few component reads per entity).

Predicates the live state source can answer today (anything else is unknown; a
file that `requires` an unbacked predicate is unavailable as a whole):

| Predicate | Source |
| --- | --- |
| `health_percent`, `target_health_percent` | Health component: `+0x08` current / `+0x0C` maximum |
| `stamina_percent` | Stamina component: `(maximum − used) / maximum` with used `+0x08`, maximum `+0x0C` (the attack code refuses when `cost + used >= maximum`) |
| `holder_tag`, `target_tag` | Tags component: `vector<string>` of tag indices into core_2 `tag` ("5" = Player), mapped back to names; unknown indices match by their digits |
| `weapon_class` | Equipment slot 9 (right hand), else 8 (left hand): the item's Weapon component `+0x08` = `weapon_type` index; no weapon = `Unarmed` |
| `shield` | an item in slots 8, 9, 17, 18 whose Item component `+0x08` = 14 (`Shield`) |
| `equipped` | slot filled / `item_type` by name; `item` = the item entity's data id from its Name component (`+0x08`, e.g. `803` for core_2 Wheat Bread, `core_2_Ice_Rod`); `item_tag` = the item entity's Tags component |
| `inventory_count` | Equipment `+0x70/+0x78`: `vector<int>` of the item entities in the backpack (what `AStashItems` walks); each adds its Stackable `+0x08` count, or 1 without a Stackable component, under its data id. Unknown when the Name storage is unavailable. |
| `skill_level` | Skills component: `std::map<string,int>` at `+0x08` (skill id → level) |
| `stacker` | Effects component: `std::map<string,int>` stacks at `+0x38`, `std::map<string,float>` expiry time at `+0x48`; a stack whose expiry lies before the clock counts as 0 (the engine's own on_max_stacks code resets it then). Without a clock the entity's last tick time (`+0x6C`) stands in. |
| `party`, `companions` | Party component (type 50, 0x10 bytes) is carried by the player's companions, not by the player (the owner's `player.sav` has no `Party;` line; `DrawParty` lists the Party storage's entity ids). Player: `in_party` while any companion exists, `companions` = their number. Companion: `in_party`, `companions` = the other members plus the player. Anyone else: `alone`, 0. |
| `time_of_day`, `season` | the game clock struct (`0x205F0()` in 526b6ab4, located from `ProgressEffects::update`): `+0x20` day-part index (what the UI hands to the day-part translator `0x4CFEF0`; verified in game: 5 = Midday with the sun orb), `+0x14` = 1440 turns per day, `+0x18` time in turns, `+0x00` year, `+0x04` day of the year (1-based), `+0x10` days per year (100). Names from core_2 `day_part` / `year_part`. The exe has no year-part translator (only `day_part` is looked up and `+0x28` is a byte flag), so the season is derived: four equal parts of the year, day 1-25 Spring, 26-50 Summer, 51-75 Autumn, 76-100 Winter (day 50 of the test save logged `Summer`). |

Not backed yet (unknown, fail closed): `biome`, `in_combat`, `in_settlement`.

## 3. Game facts the plugin relies on (build 526b6ab4)

All RVAs reproduce with `tools/asm_fn.py` / `tools/pe_xrefs.py`; the plugin
never uses them directly (section 4).

### 3.1 Event hook points

| What | RVA | Evidence |
| --- | --- | --- |
| `UseAbility::update` | `0x73E690` | RTTI `UseAbility` slot 1; the only user of `"UseAbility: Entity {} is casting {}. Applying effects."` |
| **TriggerOnCast(ctx, casterId, const std::string* abilityId)** | `0x413E80` | called at `0x73F80E` right after that log line, only when `AbilityUser+0x258 == 0`; collects passives with effect **117** (`mov [rsp+20h],75h` → collector `0x2A6400`), compares each row's `value` with the cast id, rolls `third_value` against `rand()/2^53`, looks the secondary ability up and queues it |
| `EntityAttack` attack resolution | `0x6CAD00` | the only user of `"EntityAttack: Attacked entity {}"`; called from `EntityAttack::update 0x6C9D10` |
| **TriggerOnAttack(ctx, attackerId, targetId, weaponEntityId)** | `0x411670` | called at `0x6CBEAD` after every attack (hit or miss converge at `0x6CBBA8` first); collects effect **100**; reads the weapon's `Weapon+0x08` type, 9 (= `Any`) when there is no weapon entity |
| "Queued Ability" consumer | `0x744050` | pops `AbilityUser+0x230`, destroys the Ability (`~Ability 0x6DA60`, `operator delete 0x81E2E0` with size `0x198`), casts at the position deque `+0x188` or the target `+0x1A0` |
| passive effect name → id | `0x5238A6` | the resolver's table equals core_2 `messages.json` `passive_effects` ids (117 `trigger_ability_on_cast`, 100 `on_attack_cast`, 101 `on_kill_create_item`, …) |

The other events (each located and hooked on its own; a missing one only
disables that event):

| Event | Hooked routine | RVA | Evidence |
| --- | --- | --- | --- |
| `kill` | **KillTrigger(system, killerId, victimId, position*, effectId)** | `0x6D5E70` | `OnDeath::update 0x6D0D00` (RTTI `OnDeath` slot 1; the only user of `"killing an ally"`) walks dead entities (Health `+0x08 <= 0`), takes the killer from Health `+0x30`, and calls it at `0x6D130D` with effect 101 (`on_kill_any_create_item`) and at `0x6D133D` with 102 when the victim has component 51 (Persona). The routine checks `hasComponent(killer, 44 Name)` / `(killer, 21 Equipment)` and collects the passives. The plugin fires on the 101 call only. |
| `turn` | **TurnTick(system, entityId)** | `0x5AFAD0` | `ProgressEffects::update 0x5AE060` (RTTI `ProgressEffects` slot 1, registered by `0x4378D0`) loops the Effects storage's entity ids; per entity, when the clock (`0x205F0()`, `+0x18`) has passed `Effects+0x6C + 1.0`, it calls `0x5AF4F0(system, entity, effects)`, `0x5AFAD0(system, entity)` (collects passive 63), `0x5B0220(system, entity, effects)` and adds 1.0 to `+0x6C`: one tick per entity per game turn. |
| `dodge` | **ParryExperience(ctx, attackerId, defenderId, float)** | `0x4BA1E0` | the only user of `"core_2_Parry"`; called from the attack resolution only on the miss path (`0x6CB401`, after the roll `rand/2^53 > success rate` at `0x6CB193`), grants the defender Parry skill experience. |
| `damaged` | **DamageDealt(ctx, attackerId, targetId, int damage)** | `0x4B9FB0` | called at `0x6CBBD0` right after hit and miss paths converge (`0x6CBBA8`) with the damage the resolution just subtracted from the target's Health (`[rbp+60h]`, computed by `0x4BC2F0`); also called by the ability-damage code (`0x33D140`, `0x33B670`, `0x34ECA0`, `0x8E420`). The plugin fires when `damage > 0`. |
| `crit` | **AttackEnd(system, attack*, weaponType, bool critical)** | `0x6CED40` | the last call of the attack resolution (`0x6CC0E2`): `cmp [rbp+64h],0; setg r9b` passes "at least one critical hit"; the attack struct holds the attacker at `+0` and the target at `+0x90` (`r13 = rsi+0x90`). Collects passives 30 and 112. |
| `block` | **Deflect(system, attackerId, attackQueue*, r9) → bool** | `0x64B110` | `EntityAttack::update 0x6C9D10` (the only user of `"EntityAttack: Attack performed."`) calls it at `0x6CA6EE` before resolving an attack; it reads the current queue entry `queue.data[(queue.capacity-1) & queue.index]`, the target at entry `+0x4C`, and rolls the target's deflection passive (42) from its Effects. A true result ends in `"EntityAttack: Attack deflected."` (`0x6CEB00`) and no attack. |
| clock | **GameClock() → const struct*** | `0x205F0` | thread-safe static init of the struct at `0xB01DE8`: `+0x14 = 1440`, `+0x18` time (float turns), `+0x1C = 100.0`, `+0x20 = 4` (Morning) at start, `+0x28 = 0` (Spring). |

Other collectors found on the way (not hooked): `0x411C30` 97
(`on_max_stacks`), `0x6CAD00` 98 (`on_attack_stack`), `0x33D140` 124
(`heal_on_damage_type`). Melee damage is applied inline by the attack
resolution (`Health+0x08 -= damage` at `0x6CB928`), ability damage by
`0x33D140`, damage over time by `0x5AE760`; there is no single damage function,
which is why `damaged` hooks the shared post-damage call instead.

### 3.2 The cast primitive

Inside `TriggerOnCast`, for every row that fires (`0x413FEE`-`0x4140D3`):

```
registry = AbilityRegistry()                 ; 0x8B4A0, static std::map<std::string, Ability> at 0xB0F490
Ability tmp = registry.at(secondary_value)   ; 0x4249F0(map, &out, &id): a DEFAULT (empty id) Ability when missing
Ability* a = new(0x198)                      ; 0x81E020
copy(a, &tmp)                                ; 0x77520
abilityUser->queue.push_back(a)              ; std::deque<Ability*> at AbilityUser+0x230 (_Growmap 0x2BD350, blocks of 2)
```

The plugin does the same with one simplification: it constructs the lookup
result straight into the heap object (`map::at` writes its by-value result to
the pointer it is given), checks that the copied id equals the requested one
(an unknown id yields an empty Ability, which is destroyed and freed with the
game's own `~Ability` and `operator delete`), and pushes with the deque code in
`plugins/common/ability_queue.hpp`, which mirrors the engine's inline
`push_back` and calls the engine's `_Growmap`. Memory stays in the game's heap.

### 3.3 State source: the component registry

`ctx+0x100` is a `std::vector<ECStorageBase*>` indexed by **component-type
id**, in core_2 `messages.json` `component_type` order (the registry
constructor `0x436EC0` pushes one storage per type; `hasComponent(ctx, id,
type) = 0x678950` uses the same ids: 76 = Weapon before reading `Weapon+0x08`,
44 = Name, 53 = Position …). The known UI code agrees: index 1 = Ability user
(`[registry+8]`), 52 = Player (`+0x1A0`), 53 = Position (`+0x1A8`).

| Type | Index | Stride (getter) | Fields used |
| --- | --- | --- | --- |
| Ability user | 1 | 0x260 (`0x92360`) | `+0x1A0` target id, `+0x230` queue, `+0x258` no-triggers flag |
| Effects | 20 | 0x70 (`0x92160`) | `+0x08/+0x10` active effects (0x1A0 each), `+0x38` stacks map, `+0x48` stack expiry map, `+0x6C` last tick time (save `Effects;0\|\|-1.000000\|154.200012\|0\|0\|0`) |
| Equipment | 21 | 0x150 (`0x284B0`) | `int slots[20]` at `+0x08`, `eq_slot` order, entity id or 0; `vector<int>` backpack item entities at `+0x70/+0x78` (serialised after the slots as count and ids) |
| Name | 44 | 0xF0 (`0x27EB0`) | `+0x08` data id string, `+0x28` display name (save `Name;803\|Wheat Bread\|…`) |
| Party | 50 | 0x10 (`0x27CC0`) | `+0x08` int, `+0x0C` hold-position flag; presence = party member (companions only) |
| Player | 52 | 0x130 (`0x28100`) | presence = the player entity |
| Stackable | 64 | 0x10 (`0x27CC0`) | `+0x08` stack count |
| Health | 28 | 0x40 (`0x282E0`) | floats `+0x08` current, `+0x0C` maximum (save `Health;105\|105\|…`) |
| Item | 32 | 0x18 (`0x3B030`) | `+0x08` `item_type` index |
| Skills | 63 | 0x88 (`0x12DF40`) | `map<string,int>` at `+0x08` = level (`+0x18` potential, `+0x28` progress) |
| Stamina | 65 | 0x28 (`0x85450`) | floats `+0x08` used, `+0x0C` maximum (save `Stamina;0\|10000\|…` is ×100) |
| Tags | 68 | 0x20 (`0x42E80`) | `vector<string>` `+0x08`/`+0x10` (save `Tags;3\|5\|1\|9`) |
| Weapon | 76 | 0x68 (`0x1838A0`) | `+0x08` `weapon_type` index |

`ECStorage<T>` itself: pages vector `+0x08/+0x10` (24-byte entries, 0x2000
ints each, `-1` = absent), entity-id vector `+0x20/+0x28` (the dense order;
`ProgressEffects::update` and `DrawParty` iterate it), dense array
`+0x38/+0x40`; the same page-table walk the ability-bar plugins already use.

## 4. How the plugin survives game updates

Nothing is keyed by exe hash:

1. The three anchor functions come from the engine's own log strings
   (`findFunctionsReferencingString`): the cast log line, `"EntityAttack:
   Attacked entity {}"`, `"Queued Ability"`.
2. `plugins/common/trigger_layout.hpp` finds the call sites by their
   instruction bytes (the `cmp byte [rax+258h],0 … call` sequence, the
   `mov r9d,[rsi+60h] … call` sequence), follows the `rel32` to the trigger
   routines, and verifies those routines check effect 117 / 100, allocate
   `0x198` bytes, use the `+0x230..+0x250` deque fields and the `hasComponent(…,
   76)` call. From the same bytes it takes the ability registry getter,
   `map::at`, `operator new`/`delete`, `_Growmap` and `~Ability`. Any mismatch →
   `inactive`, nothing hooked.
3. Every component storage is checked by RTTI (`.?AV?$ECStorage@UHealth@@@ASEngine@@`
   must be the storage's vtable) and every component by its own RTTI vtable
   (`.?AUHealth@@`) before a field is read; a mismatch makes that component
   unknown (fail closed), never a wild read. Strides are the only constants
   carried over, and the vtable check catches a changed stride at the first
   entity.
4. Hooks are `installDetourAuto` detours (14/15 position-independent prologue
   bytes on both routines in 526b6ab4); the original runs first, so the
   engine's own passives keep their behaviour.
5. Every hook body runs under `__try`; a fault logs once, reports `DEGRADED`
   and disables the plugin for the session.

## 5. Configuration (`<game>\SoulashExtender\Plugins\Triggers.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | 0 = load nothing, hook nothing |
| `Mode` | `log` | `log`: hooks installed, events and would-be casts logged, nothing queued. `active`: queue. |
| `LogEvents` | 1 | one line per cast / attack / kill / damaged / dodge / block / crit event the hooks see (capped by `MaxLogLines` per session); firings and problems always log |
| `LogTurnEvents` | 0 | turn ticks happen for every creature every turn. 1: logged for the creatures a turn rule was allowed for (a rule with `holder_tag: Player` logs exactly the player's turns); 2: every creature a turn rule matched, allowed or not (hundreds of lines per turn) |
| `MaxLogLines` | 400 | |
| `LogExceptions` | 1 | log every first-chance C++ exception in the process: type, `what()`, a short stack (`game+0xRVA`), whether a Triggers detour was on the stack. The game's own crash path turns an unhandled one into a deliberate null write (`0x6F3A10`: `xor eax,eax; mov [rax],0Dh`) that says nothing about the throw; this line does |
| `MaxExceptionLines` | 40 | cap for those lines per session |
| `MaxCastsPerEvent` | 8 | upper bound on queued abilities per event, all rules together |

The build deploys `Triggers.dll` and the INI template (existing INI kept) and
pins the hash like the other plugins.

## 6. First run (owner)

1. Put a rule in a mod, e.g. `<game>\data\mods\<mod>\extender\conditions.json`:
   ```json
   { "spec": 1, "triggers": [ { "id": "wounded_splash", "on": { "cast": "arendeth_hydromancy_water_splash" },
     "cast": "arendeth_hydromancy_spray", "chance": 1, "when": { "health_percent": { "max": 100 } } } ] }
   ```
2. Start the game (Mode=log), load a save, cast that ability a few times and
   make one melee attack. Expected `Triggers:` lines in `logs\proxy-crash.log`:
   - `game code located (RVAs): on-cast trigger 0x413E80, on-attack trigger 0x411670, ability registry 0x8B4A0, map::at 0x4249F0, operator new 0x81E020, operator delete 0x81E2E0, deque growmap 0x2BD350, ~Ability 0x6DA60` (on 526b6ab4),
   - `event code located (RVAs, 0 = not on this build): kill trigger 0x6D5E70, turn tick 0x5AFAD0, parry 0x4BA1E0, damage dealt 0x4B9FB0, attack end 0x6CED40, deflect 0x64B110, game clock 0x205F0`,
   - `active: game build 526b6ab4, mode=log, N rule(s) …; events hooked: cast attack kill turn damaged dodge block crit; delayed casts wait for the holder's turns`,
   - on the first event: `state source … AbilityUser=ok Effects=ok Equipment=ok Health=ok Item=ok Name=ok Party=ok Player=ok Skills=ok Stackable=ok Stamina=ok Tags=ok Weapon=ok; queueing possible`,
   - `rules folder: <path> (default | [Conditions] Directory | SOULASH2_MODS_DIRECTORY)` and `c++ exception logging on …`,
   - `game clock: time T turns, day part D (<name>), year part Y (<name>)`: the day part must match the sun orb; the season is derived from the day of the year,
   - `cast event: entity P cast 'arendeth_hydromancy_water_splash' (target T): 1 rule(s) matched, 1 allowed, …; health=100 stamina=.. weapon=Unarmed shield=no tags=3 skills=11 stacks=0 party=alone companions=0 time=Morning/Spring inventory=N` (N = distinct item ids in the backpack),
   - `rule <mod>/wounded_splash: would queue 'arendeth_hydromancy_spray' on entity P (Mode=log …)`, or `would NOT queue … not in the game's ability registry` for an id the game does not know (checked in log mode too),
   - `c++ exception on thread T: type <mangled>, what "…"; inside a Triggers hook: yes|no; stack: …` whenever anything in the process throws; `inside a Triggers hook: no` with a `game+…` stack is the game's own,
   - `attack event: entity P attacked E (weapon entity W): …`, and after the fight `damaged event: entity E took N damage from P`, `dodge event: …`, `crit event: …`, `kill event: entity P killed E`.
   Check the state values against the character sheet (health %, stamina %,
   weapon class, tag count 3 for the player, skill count, stacks on the status
   bar, party size, day part and season on the clock). To see turn ticks, add a
   rule with `"on": {"turn": true}` and `"when": {"holder_tag": "Player"}` and
   set `LogTurnEvents=1`: exactly one `turn event: entity P` line per turn.
3. If that all matches, set `Mode=active`, restart, cast again: expect
   `rule …: queued 'arendeth_hydromancy_spray' on entity P (target T)` and the
   spray going off right after the splash. The engine's own log
   (`logs\entity_*.log` when enabled) shows `Queued Ability` casts.
4. Rollback: `Mode=log`, or `Enabled=0`, or delete `Triggers.dll`. Nothing on
   disk is changed by the plugin.

What a failure looks like: `inactive: game build … does not match the verified
trigger code (…)` (nothing hooked; send the line), `state source … WRONG VTABLE`
(a component layout changed; conditions on it are unknown), `rule …: ability
'x' is not in the game's ability registry` (typo in the rule or a mod not
loaded), `fault 0x… while handling a cast event; plugin disabled` (send the log).

## 7. Open points

- `target` for cast and turn events is the AbilityUser's current target
  (`+0x1A0`), not the tile the ability was aimed at; `target_*` predicates are
  unknown when it is 0.
- `damaged` covers melee / ranged hits and ability damage through the shared
  post-damage call; damage over time (`0x5AE760`) and movement damage do not go
  through it. `block` is the deflection passive (there is no shield block in
  Soulash 2's attack code); `dodge` is the engine's miss / parry outcome.
- The season is derived from the day of the year (no year-part translator in
  the exe); if the game ever shows seasons, compare.
- `crit` and `block` have not fired in a run yet (no critical hit happened in
  the test fights; the deflection passive was not on anyone). Their hooks
  install and are logged as hooked.
- A `kill` whose killer is already dead (a poison kill credited to a corpse)
  has an all-unknown snapshot and fails closed: `kill event: entity K killed V:
  … 1 blocked by condition … health=?0 …`. Expected.
- The engine itself throws `fmt::format_error` "invalid format specifier" from
  its heal message builder (`0x33B670`, " (reverse heal)"): ability effect
  message 4 (`'{} regains {:.0f} health.'` in core_2 `messages.json`) is
  formatted with an **int** amount on that path, and fmt rejects `.0f` for an
  int. Seen twice in the Enchanted Book fight of the test save; the game caught
  it both times on the second run, and the first run ended in the game's
  terminate path (unhandled C++ exception → null write at `0x6F3A10`) in the
  same fight. Not caused by the extender (`inside a Triggers hook: no`, stack
  entirely `game+…`). No mod overrides that message.
- Non-creature casters show up: `cast event: entity 2 cast '122' (target 2)`
  (an entity with a numeric ability id; `entity 2` also kills with a Sword:
  the world or a caravan actor, not the player). Rules on `cast` match by
  ability id, so this is noise only.
- Predicates still not backed: `biome` and `in_settlement` need the current
  region's world tile (`region.sav` is `x|y|z|regionId`, the biome lives in
  `world.sav`'s tile records and in the world object, not in the region map);
  `in_combat` has no runtime flag found yet (the AI's combat goals would have
  to be read).
- `inventory_count` counts the backpack list only (Equipment `+0x70`), not
  items in other containers or equipped slots.
- Whether a queued cast from a rule can itself trigger rules: the engine clears
  triggers for queued casts through `AbilityUser+0x258`; the plugin inherits
  that (it only runs where the engine would run its own on-cast triggers).
- Delayed casts are pending-list entries in the plugin (up to 256), not saved:
  they are lost when the game is closed.
