# Triggers: conditional triggers executed by the extender

Status: **implemented as the `Triggers` plugin (`plugins/triggers`), unit-tested
against synthetic memory and byte patterns, NOT YET RUN IN THE GAME.** Every
game fact below is static analysis of build `526b6ab4` (0.10.6.1, 2026-11-06)
unless marked otherwise. The plugin ships with `Mode=log`, which installs the
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
| `<mod>\extender\conditions.json` | `triggers[]`: `{id, on: {cast: A} \| {attack: true}, cast: B, chance, when, on_unavailable}` | On the event, if `when` holds (three-valued; unknown fails closed unless `on_unavailable: active`) and the chance rolls, queue `B` on the holder. |
| `<mod>\extender\effects.json` | `triggers[]` with `on: {cast}` or `{attack}` | `effects::ResolveEvent`: the evaluator-owned parts (choose / delay / repeat-every / non-native `when`) resolve to generated group abilities, which are queued. Delayed casts are dropped and logged (no turn source yet). |

Several rules may listen to the same ability or event (that is the point:
"multiple triggers"); each is evaluated and rolled independently, up to
`MaxCastsPerEvent` casts per event. Rules are evaluated for **every** entity that
casts or attacks, not only the player: a rule on an NPC's ability fires for the
NPC.

Predicates the live state source can answer today (anything else is unknown; a
file that `requires` an unbacked predicate is unavailable as a whole):

| Predicate | Source |
| --- | --- |
| `health_percent`, `target_health_percent` | Health component: `+0x08` current / `+0x0C` maximum |
| `stamina_percent` | Stamina component: `(maximum − used) / maximum` with used `+0x08`, maximum `+0x0C` (the attack code refuses when `cost + used >= maximum`) |
| `holder_tag`, `target_tag` | Tags component: `vector<string>` of tag indices into core_2 `tag` ("5" = Player), mapped back to names; unknown indices match by their digits |
| `weapon_class` | Equipment slot 9 (right hand), else 8 (left hand): the item's Weapon component `+0x08` = `weapon_type` index; no weapon = `Unarmed` |
| `shield` | an item in slots 8, 9, 17, 18 whose Item component `+0x08` = 14 (`Shield`) |
| `equipped` | slot filled / `item_type` by name; `item` and `item_tag` are **unknown** (the live registry does not expose an item's data id) |
| `skill_level` | Skills component: `std::map<string,int>` at `+0x08` (skill id → level) |

Not backed yet (unknown, fail closed): `party`, `companions`, `stacker`,
`inventory_count`, `time_of_day`, `season`, `biome`, `in_combat`,
`in_settlement`. The `target` of a cast event is `AbilityUser+0x1A0` (the
entity the game would also cast a queued ability at); of an attack event it is
the attacked entity.

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

Other collectors found on the way (not hooked yet): `OnDeath::update
0x6D0D00` collects 101/102 (kill events), `0x411C30` 97 (`on_max_stacks`),
`0x6CAD00` 98 (`on_attack_stack`), `0x33D140` 124 (`heal_on_damage_type`).

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
| Equipment | 21 | 0x150 (`0x284B0`) | `int slots[20]` at `+0x08`, `eq_slot` order, entity id or 0 |
| Health | 28 | 0x40 (`0x282E0`) | floats `+0x08` current, `+0x0C` maximum (save `Health;105\|105\|…`) |
| Item | 32 | 0x18 (`0x3B030`) | `+0x08` `item_type` index |
| Skills | 63 | 0x88 (`0x12DF40`) | `map<string,int>` at `+0x08` = level (`+0x18` potential, `+0x28` progress) |
| Stamina | 65 | 0x28 (`0x85450`) | floats `+0x08` used, `+0x0C` maximum (save `Stamina;0\|10000\|…` is ×100) |
| Tags | 68 | 0x20 (`0x42E80`) | `vector<string>` `+0x08`/`+0x10` (save `Tags;3\|5\|1\|9`) |
| Weapon | 76 | 0x68 (`0x1838A0`) | `+0x08` `weapon_type` index |

`ECStorage<T>` itself: pages vector `+0x08/+0x10` (24-byte entries, 0x2000
ints each, `-1` = absent), dense array `+0x38/+0x40`; the same page-table walk
the ability-bar plugins already use.

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
| `LogEvents` | 1 | one line per cast / attack event the hooks see (capped by `MaxLogLines` per session); firings and problems always log |
| `MaxLogLines` | 400 | |
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
   - `active: game build 526b6ab4, mode=log, N rule(s) …, cast hook installed, attack hook installed`,
   - on the first event: `state source … AbilityUser=ok Equipment=ok Health=ok Item=ok Skills=ok Stamina=ok Tags=ok Weapon=ok; queueing possible`,
   - `cast event: entity P cast 'arendeth_hydromancy_water_splash' (target T): 1 rule(s) matched, 1 allowed, …; health=100 stamina=.. weapon=Unarmed shield=no tags=3 skills=11`,
   - `rule <mod>/wounded_splash: would queue 'arendeth_hydromancy_spray' on entity P (Mode=log …)`,
   - `attack event: entity P attacked E (weapon entity W): …`.
   Check the state values against the character sheet (health %, stamina %,
   weapon class, tag count 3 for the player, skill count).
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

- `target` for cast events is the AbilityUser's current target (`+0x1A0`), not
  the tile the ability was aimed at; `target_*` predicates are unknown when it is 0.
- No turn / kill / damaged / dodge / block / crit events yet. Candidates are in
  section 3.1 (`OnDeath::update` for kills, `ProgressEffects::update 0x5AE060`
  for turns); each needs the same anchor-and-verify treatment.
- Predicates listed as not backed in section 2. `stacker` needs the stack
  storage (probably the Effects component, type 20, stride 0x70, or the
  `ability_stackers` runtime object) located first.
- `equipped.item` needs an entity → data id mapping (not found in the
  components read so far).
- Whether a queued cast from a rule can itself trigger rules: the engine clears
  triggers for queued casts through `AbilityUser+0x258`; the plugin inherits
  that (it only runs where the engine would run its own on-cast triggers).
