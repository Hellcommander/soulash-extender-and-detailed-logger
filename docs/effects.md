# Effects: a composable, data-driven effect system (spec v1)

Status: **specification + compiler + evaluator core**. `tools/effects_compile.py`
validates an `extender/effects.json` file and compiles it to ordinary Soulash 2
ability, passive, milestone and conditions fragments. `src/effects.cpp` is the
strict loader and the resolver for the parts the engine cannot express, tested
with synthetic data only. **Nothing here has run in the real game**; every
engine behaviour below is marked native-verified (the key is known to the
executable and used by core_2 the same way), pattern (used by an installed mod,
not seen running) or unverified.

Dungeons of Dredmor's spell effects are the reference, because the owner's
Runic Magic mod is a Dredmor Runecaster in Soulash clothing and because
Dredmor is the best-known example of a *data-only* effect language in a
roguelike. Read: the [Spell Effects](https://github.com/dredmor-com/dungeons-of-dredmor/wiki/Spell-Effects),
[Spell System Reference](https://github.com/dredmor-com/dungeons-of-dredmor/wiki/Spell-System-Reference),
[Spell Types](https://github.com/dredmor-com/dungeons-of-dredmor/wiki/Spell-Types) and
[Buffs](https://github.com/dredmor-com/dungeons-of-dredmor/wiki/Buffs) pages, and
two mods that stretch that system to its limit: *Runecaster* (percent tables
in `Entropymine`, delayed `Prophecyblast` chains, `spawnitemfromlist`) and
*Compleat Essential Skills* (`requirebuffontrigger` + `removebuffbyname` used
63 times as a buff-flag state machine for stances, `triggerfromlist` as random
choice, 47 `targetblink` rows as a repeat loop).

## 1. What Dredmor has, and what it lacks

A Dredmor spell is `<spell name type>` (type = targeting) with a flat list of
`<effect type=... />` rows, an optional `<buff>` (stat changes, duration,
stacking, and buff-hooks such as `targetHitEffectBuff`, `playerHitEffectBuff`,
`targetKillBuff`, `dodgeBuff`, `triggerOnCast`) and `<anim>`. Every effect has
`percent` (chance), `taxa` (target family filter), `self`/`affectsCaster`.
Chaining is one effect, `trigger` (`spell`, `amount` = delay in turns,
`requirebuffontrigger[name]`, `requirebuffonnottrigger[name]`), plus
`triggerfromlist` (one of N, equal weights) and `dot` (a spell every turn for N
turns, optionally ending with a buff). Mines are a spell attribute
(`mine="1" mineradius mineTimer minePermanent`).

What modders had to work around:

| Want | Dredmor workaround |
| --- | --- |
| weighted random choice | many `trigger percent=` rows (not exclusive: 0..N fire) or `triggerfromlist` with duplicate options |
| state / modes | an invisible buff as a flag, `requirebuffontrigger`, `removebuffbyname` everywhere |
| conditions (health, equipment, time) | none; only buff flags and `taxa` |
| repeat N times | N identical rows |
| validation | an external XML validator; unknown attributes are silently ignored |
| sequencing | `amount` (delay) as the only ordering tool |

The "better than Dredmor" goals follow from that list: real combinators
(`all`, `chance`, `choose`, `delay`, `when`, `repeat`), gating by the existing
[skill-conditions](skill-conditions.md) expressions and by conditional
triggers, typed parameters with clear errors, and a compiler that reports per
node whether the engine does it natively or the evaluator must.

## 2. What Soulash 2 has natively

From `docs/skill-schema.md`, `core_2` and the 19 installed mods, confirmed with
`tools/pe_xrefs.py <exe> has <key>` against build 924e1ef3:

- **Ability** (`abilities/*.json`): `target` (one of `self`, `aoe_self`,
  `health`, `actor`, `tile`, `aoe_tile`, `aoe_target`, `line`, `cone`,
  `line_health`, `corpse`, `non_actor`, `line_cone`, `back_and_forth`,
  `random`, `horizontal_line`), `effects` (a flat map: each key once), and
  `effects_keys` (string-valued keys: `summon`, `stacker`, `aura`,
  `transform`, `summon_on_move`, `summon_weather`, `magic_power_per_entity`,
  `sense`), plus `cost`, `cooldown`, `cast_time`, `duration`, `range`,
  `min_range`, `requirements`, `slots`, `animation`.
- **Effect keys** (native-verified unless noted): `damage [min,max]`,
  `damage_type`, `magic_power_damage`, `damage_weapon`, `aoe_range`,
  `aoe_shape` (1 circle, 2 sides, 3 square, 4 ring), `aoe_break`, `pierce`,
  `chain`, `ignore_party`, `heal [min,max]`, `regeneration`, `stun`,
  `immobilize`, `fear`, `bleed`, `burn`, `poison_effect`, `frostbite`,
  `silence`, `vulnerable`, `disarm`, `hide`, `cage`, `knockback`,
  `pull_target`, `trade_places`, `lunge`, `charge`, `move_to_side`,
  `rollback`, `summon_count`, `stacker_count`, `neutralize [names]`,
  `clear_cooldowns`, `resistance`, `thorns`, `damage_reduce`,
  `damage_reduce_percent`, `damage_bonus`, `damage_bonus_percent`,
  `attack_speed`, `movement_speed`, `sight`, `magic_power`, `hit_chance`,
  `parry_chance`, `deflection`, `light_source`, `stamina`, the five
  statistics, `damage_as_life`, `burn_stamina`, `hit_self`, `transform`,
  `corrupt`, `copy_to_party`.
- **Passive effects** that chain abilities (`passives.json`):
  `trigger_ability_on_cast` (`value` = cast ability A, `secondary_value` =
  ability B to cast, `third_value` = chance; description id 117) and
  `on_attack_cast` (`value` = B, `secondary_value` = chance; id 100).
  `on_kill_create_item` / `on_kill_any_create_item` create an item on a kill.
  **Not found in the executable** (neither as a data string nor as an inline
  immediate): `trigger_ability` as an *ability* effect key (used by
  arendeth_astral_magic) and `trigger_ability_on_turn` (arendeth_hydromancy);
  treat both as silent no-ops until proven otherwise. `effect_keys` (a typo in
  one core_2 ability) is also unknown.
- **Stackers** (`ability_stackers.json`): a named counter with
  `max_stacks`, `duration`, `apply_caster` and passive-style `effects` while
  stacks are held. This is Dredmor's buff and buff-flag in one native object.
- **Temporary caster entities** (pattern, Runic Magic / arcane_ballistics):
  a summoned entity with `temporary: N`, `ability_user: [ability]` and no
  behaviours casts its ability when something comes near. That is a Dredmor
  mine. A 1-turn entity holding several abilities is the mod's "roller"
  (one random cast; which one the AI picks is unverified).
- **Items that cast** (pattern): `usable.ability` on an item entity makes
  using/throwing it cast an ability: Dredmor's rune items.

Not expressible in data at all: exact one-of-N choice, delays, repeat-with-
state, conditions beyond weapon class / tags / party, teleport-to-tile,
experience, mana-like resources other than stamina, removing specific stacks,
anything on turn/hit/damaged/dodge/kill events other than the three passives
above. Those are **evaluator effects** in this spec.

## 3. Concept mapping

| Dredmor | Spec v1 | Native Soulash target | Status |
| --- | --- | --- | --- |
| `<spell type=...>` | `spells[].target` | ability `target` (+ `aoe_range`, `aoe_shape`) | native-verified names; aliases `target`→`health`, `targetfloor`→`tile`, `fireball`→`aoe_tile`, `area`→`aoe_self`, `beam`→`line`, `targetcorpse`→`corpse` |
| `<requirements mp>` | `cost.stamina` | `cost` | native |
| `<requirements weapon/shield>` | `requirements` | `requirements.one_of/always` ids | native (table in skill-schema.md) |
| `damage <type>=n <type>F=f` | `{"damage": {amount, type, scaling}}` | `damage`, `damage_type`, `magic_power_damage` | native-verified |
| `heal amount amountF` | `{"heal": {amount}}` (+ `regeneration` in `buff`) | `heal`, `regeneration` | native-verified |
| `spellpoints` | `{"stamina": n}` | `stamina` | native key exists (semantics unverified) |
| `drain` | `{"damage": {..., "leech": f}}` | `damage_as_life` | pattern (Runic Hemorrhage) |
| `paralyze / lockdown / root / fear / blind / confuse / sleep / bleed / burn / oil` | `{"status": {name: value}}` | `stun`, `immobilize`, `fear`, `bleed`, `burn`, `poison`, `frostbite`, `silence`, `vulnerable`, `disarm`, `hide`, `cage` | native-verified keys; `blind`, `confuse`, `sleep` have no counterpart (`silence`/`fear` are the closest) |
| `knock / suction / swapwithmonster` | `knockback`, `pull`, `swap` | `knockback`, `pull_target`, `trade_places` | native-verified |
| `blink / teleport / displace` | `teleport` | none | **evaluator only** (impossible without hooks) |
| `summon monsterType` | `{"summon": {entity, count}}` | `effects_keys.summon` + `summon_count` | native-verified (count semantics unverified) |
| `create` (impassable object) | `summon` of a wall entity | same | pattern (Stone Spikes) |
| `spawn / spawnitematlocation` | `{"create_item": {item, count}}` | `summon` of an item entity | pattern (Conjure runes) |
| `spawnitemfromlist` | `choose` over `create_item` | evaluator (exact) | native only as `approximate` |
| `trigger spell` | `{"cast": {spell}}` | passive `trigger_ability_on_cast` row | native-verified key; *target of the triggered cast is unverified* |
| `trigger percent=p` | `{"chance": p, "do": ...}` | `third_value` | native-verified |
| `trigger amount=n` (delay) | `{"delay": n, "do": ...}` | none | **evaluator** |
| `triggerfromlist` | `{"choose": [{weight, do}]}` | none | **evaluator** (`approximate: true` → independent chances) |
| `requirebuffontrigger` (buff flag) | `{"when": {"stacker": {...}}, "do": ...}` + `{"stacks": {...}}` | stacker exists natively; the gate does not | **evaluator** gate, native state |
| `removebuffbyname` | `{"remove_stacks": {stacker, count}}` | none | **evaluator** |
| `dot` | `{"repeat": n, "every": 1, "do": ...}` | none | **evaluator** (native `burn`/`bleed`/`poison` cover plain damage over time) |
| `taxa` | `{"when": {"target_tag": ...}}` | passive `restrict_tags_enemy` (encoding unverified) | partly native |
| `uncurse / randomuncurse` | `{"cleanse": [effect names]}` | `neutralize` | native-verified |
| `resetcooldowns` | `{"cooldowns": {"clear": n}}` | `clear_cooldowns` | native key exists |
| `grantxp`, `drainMana`, `magicmapping`, `removemonster`, `manipulateobject`, item mutations | not in v1 | none | listed as impossible; `grant_xp` reserved as evaluator-only |
| `transformmonster` | `{"transform": {entity}}` | `effects_keys.transform` | native key (core_2 uses it) |
| mine (`mine="1" ...`) | `{"summon": {entity, count, duration}}` of a temporary caster | summon + the entity's own ability | pattern (Runic mines) |
| `<buff>` stats + time | `{"buff": {duration, stats, modifiers}}` | ability `duration` + stat keys | native-verified keys |
| `stacksize` | `stacks` + stacker `max_stacks` | native | native |
| `targetHitEffectBuff` / `triggerOnCast` | `triggers[].on = {attack} / {cast}` | `on_attack_cast` / `trigger_ability_on_cast` | native-verified |
| `targetKillBuff` | `triggers[].on = {kill}` | `on_kill_create_item` only when `do` is `create_item`; otherwise evaluator | partly native |
| `playerHitEffectBuff`, `dodgeBuff`, `blockBuff`, `counterBuff`, `criticalBuff` | `on = {damaged} / {dodge} / {block} / {crit}` | none | **evaluator** (needs a hook source) |
| every turn | `on = {turn}` | none (`trigger_ability_on_turn` is not in the exe) | **evaluator** |
| `self="1"` | `target: "self"` on the sub-spell | per ability | native |

## 4. The file

`<mod>/extender/effects.json`:

```json
{
  "spec": 1,
  "mod": "arendeth_runic_magic",
  "requires": ["choose", "delay", "stacker"],
  "spells": [ ... ],
  "triggers": [ ... ]
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `spec` | yes | `1`. |
| `mod` | yes | The mod id; generated ids are `<mod>_<spell id>`. |
| `requires` | no | Evaluator capabilities (`choose`, `delay`, `repeat`, `remove_stacks`, `teleport`, `grant_xp`, `on_turn`, `on_kill`, `on_damaged`, `on_dodge`, `on_block`, `on_crit`) and condition predicates the file needs. Without them every evaluator node is unavailable (fail closed). |
| `spells` | no | Spells (below), at most 256. |
| `triggers` | no | Event rules (below), at most 256. |

Limits: 256 KiB of text, 64 effect nodes and depth 8 per spell, 32 items per
list, 64 characters per id (ids `[A-Za-z0-9_]`, matching the mod's own
casing). Unknown keys anywhere are errors, not ignored.

### 4.1 Spell

```json
{
  "id": "Runeword_Entropy_Logic",
  "name": "Runeword: Entropy", "description": "...", "image": 62, "animation": "..._fx",
  "target": "aoe_tile", "radius": 3, "shape": "circle",
  "range": 7, "cooldown": 1, "cast_time": 0.3, "cost": {"stamina": 12},
  "requirements": {"one_of": ["Bow", "Crossbow"]},
  "effects": [ Effect, ... ]
}
```

Either `id` (a new ability is generated) or `ability` (attach to an existing
ability of the mod: only the chained effects are generated, the ability itself
is left alone). `target` accepts the engine names and the Dredmor aliases
above. `radius` → `aoe_range`, `shape` → `aoe_shape`. `effects` is a list that
behaves like `{"all": [...]}`.

### 4.2 Effect nodes

Exactly one key per node (as in conditions). Leaves:

| Leaf | Arguments | Compiles to |
| --- | --- | --- |
| `damage` | `amount` (n or `[min, max]`), `type` (damage type name), `scaling` (magic-power factor), `weapon` (weapon-damage factor), `leech` (fraction healed), `pierce`, `chain` (n), `ignore_party` | `damage`, `damage_type`, `magic_power_damage`, `damage_weapon`, `damage_as_life`, `pierce`, `chain`, `ignore_party` |
| `heal` | `amount` | `heal` |
| `stamina` | n | `stamina` |
| `status` | `{name: value, ...}` from `stun immobilize fear bleed burn poison frostbite silence vulnerable disarm hide cage burn_stamina` | the keys (`poison` → `poison_effect`) |
| `knockback` / `pull` | tiles (n) | `knockback` / `pull_target` |
| `swap` / `lunge` / `charge` / `step_aside` | `true` | `trade_places` / `lunge` / `charge` / `move_to_side` |
| `buff` | `duration` (turns), `stats` (`strength` ... `willpower`), `modifiers` (`resistance thorns damage_reduce damage_reduce_percent damage_bonus damage_bonus_percent attack_speed movement_speed sight magic_power hit_chance parry_chance deflection light_source regeneration`) | ability `duration` + the keys |
| `summon` | `entity` (id), `count`, `on_move` (bool) | `effects_keys.summon` (+ `summon_on_move`), `summon_count` |
| `create_item` | `item` (entity id), `count` | `summon` of the item entity (pattern) |
| `stacks` | `stacker` (id), `count` | `effects_keys.stacker`, `stacker_count` |
| `remove_stacks` | `stacker`, `count` (0 = all) | **evaluator** |
| `cleanse` | list of effect names | `neutralize` |
| `cooldowns` | `{"clear": n}` | `clear_cooldowns` |
| `transform` | `entity` | `effects_keys.transform` |
| `aura` | `ability` | `effects_keys.aura` |
| `cast` | `spell` (spell or ability id), `chance`, `delay`, `when` | passive `trigger_ability_on_cast` row; `delay`/`when` as the combinators below |
| `teleport` | `"target"` | **evaluator** (impossible natively) |
| `grant_xp` | n | **evaluator** (impossible natively) |

Combinators:

| Node | Meaning | Backend |
| --- | --- | --- |
| `{"all": [E...]}` | every child, together | native: leaves merge into one ability; conflicting keys split into chained sub-abilities (chance 1) |
| `{"chance": p, "do": E}` | E with probability p | native (`third_value`) |
| `{"repeat": n, "do": E}` (+ `"every": turns`) | E n times (optionally one per `every` turns) | native when `every` is absent (n trigger rows, unverified that identical rows all fire); evaluator with `every` |
| `{"choose": [{"weight": w, "do": E}...]}` | exactly one child, weighted | **evaluator**; `"approximate": true` compiles to independent chances w/Σw (0..N fire, expected 1) |
| `{"delay": turns, "do": E}` | E after `turns` turns | **evaluator** |
| `{"when": Expr, "do": E, "on_unavailable"?}` | E only while the condition holds | native when Expr is fully hoistable (`weapon_class` with one name, `holder_tag`, `party`; see skill-conditions.md) and E is native; otherwise evaluator |

Node ownership is the rule that makes the two backends composable without
double-firing: a node is **native** when it and all its ancestors are native
(`all`, `chance`, `repeat` without `every`, native leaves, `cast` without
`delay`, a fully native `when`). Everything at or below a `choose`, `delay`,
`repeat … every`, non-native `when` or evaluator leaf is **evaluator-owned**:
the generated passive never points at it, so without the evaluator it simply
never fires (fail closed), and the evaluator resolves the whole subtree
(including nested `chance`/`all`) and casts the resulting group abilities.
Native `chance`, `repeat` and `when` nodes *above* an evaluator-owned node are
applied by the evaluator as well, because the engine never received a row for
that child; for their native children the engine rolls and gates as usual.

### 4.3 Triggers (events)

```json
{ "id": "entropy_pulse_surge", "on": {"cast": "arendeth_runic_magic_Entropy_Vortex_Ring"},
  "chance": 0.25, "when": {"holder_tag": "Undead"}, "do": {"cast": {"spell": "Volt_Burst"}} }
```

`on` is one of `{cast: ability}`, `{attack: true}`, `{kill: true}`,
`{turn: true}`, `{damaged: true}`, `{dodge: true}`, `{block: true}`,
`{crit: true}`. `cast` and `attack` compile to the native passives (the same
rule as conditions.json triggers, with `do` allowed to be any effect node
instead of a single ability). `kill` with `do = create_item` compiles to
`on_kill_create_item`; every other event needs the evaluator and a hook source
that does not exist yet. A trigger without `when` and with a native `on` needs
no extender at all.

### 4.4 Generated ids

Sub-abilities are named by path so the compiler and the evaluator agree
without sharing a plan file: `<root ability id>__<indices>`, e.g.
`arendeth_runic_magic_Runeword_Entropy_Logic__1_0` is child 0 of child 1 of
the root; `__1_x2` is the third split of conflicting leaves in node 1. The
chain passive is `ext_fx_<root ability id>`; a gated `when` node gets its own
passive `ext_fx_<root>__<path>` so it can be gated independently. Milestones
`ext_fx_<root>` grant the passives at `level` (default 1).

## 5. Compiler output (`tools/effects_compile.py <file> --emit <folder>`)

- `abilities/<id>.json` for every generated root and sub-ability (merge into the
  mod's `abilities` folder; roots carry the spell's own fields, sub-abilities
  copy the root's `skill`, `animation`, `image` and target, with
  `cast_time 0`, `cooldown 0`, `cost 0`).
- `generated_passives.json`: chain passives with `trigger_ability_on_cast` /
  `on_attack_cast` / `on_kill_create_item` rows and hoisted `restrict_*` keys.
- `generated_milestones.json`: one per passive, so the skill actually grants it.
- `generated_conditions.json`: the gating conditions that remain (spec-1
  skill-conditions file).
- `effects_report.json`: every node with its path, backend (`native`,
  `evaluator`, `impossible`) and the reason, so an author sees exactly what
  works without the extender.

Semantics the compiler deliberately does *not* paper over: the target a
triggered cast lands on (unverified; sub-abilities default to `self` /
`aoe_self` for self-shaped roots and copy the root target otherwise, with a
warning), whether identical trigger rows all fire, and whether triggered casts
can trigger further casts (Runic Magic's own open question).

## 6. Evaluator (`src/effects.cpp`)

`EffectSet::Load` is the strict loader (same JSON parser and limits as
conditions). `Resolve(spell, state, rng, capabilities)` walks only the
evaluator-owned subtrees and returns `{abilityId, delayTurns}` casts;
`ResolveEvent(set, event, ...)` does the same for `triggers`. Both use the
conditions evaluator (`Evaluate`, Kleene three-valued) for `when`, and fail
closed on unknown conditions, unsupported leaves and missing capabilities.
The only primitive a future hook source must provide is **"make the holder
cast ability X now / in N turns"**, and the per-mod discovery at start-up logs
`effects <mod>: N spell(s), M trigger(s) loaded` so authors get real-
environment feedback today. The resolver has no game state behind it yet; see
todos.txt item 2 (a state source) for what unblocks it.

## 7. Open questions (all need the game)

1. Where a `trigger_ability_on_cast` cast lands (caster, original target, or
   nothing for targeted abilities).
2. Whether several identical trigger rows fire independently.
3. Whether a triggered cast can itself trigger (`cast` chains deeper than one).
4. `summon_count` semantics for item entities; whether `summon` of an item
   drops it on the floor or into the bag (Runic Magic assumes the floor).
5. Value encodings of `restrict_shield` and `restrict_tags_enemy`.
6. Whether `tags: ["!x"]` on a passive effect row (seen in arendeth_hydromancy)
   does anything.
