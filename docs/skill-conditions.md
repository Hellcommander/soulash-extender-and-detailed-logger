# Skill conditions: predicate vocabulary (spec v1)

Status: **specification plus an evaluator core**. `src/conditions.cpp` loads these files and evaluates them against a state snapshot (tested with synthetic snapshots), but nothing feeds it real game state yet and no game hook is involved. This document, `skill-conditions.schema.json`,
`examples/runic-magic-conditions.json` and `tools/validate_conditions.py` define
and check the data format so mods can be written now and an evaluator (a native
plugin, or the author's planned Lua layer) can implement it later.

## Why a separate vocabulary

The engine's own condition vocabulary is closed (see `skill-schema.md`):

- a passive can be gated by weapon type, shield, holder tags, enemy tags and
  alone/party mode;
- an ability can be gated by a weapon-class requirement.

Nothing else can switch a passive or ability on or off: health, stamina,
statuses, equipped items, inventory, time of day, season, biome, combat,
skill levels. Someone has asked for such conditions, so this spec defines a
small, declarative, side-effect-free language for them.

## Principles

1. **Data only.** A condition is JSON. It contains no code, no scripting, and
   cannot call anything. It can be validated offline.
2. **Fail closed.** If a condition cannot be evaluated (no evaluator, an unknown
   predicate, a missing capability) its target is *inactive* by default. A
   mod author may opt a specific condition into `"on_unavailable": "active"`.
3. **Native first.** Where the engine already has a key for a predicate, the
   condition is written once and a build tool may emit the native key too, so
   the engine enforces what it can without any evaluator.
4. **Pure and bounded.** A condition is a pure function of a snapshot of game
   state. At most 64 nodes, depth 8, 32 items per list, 64 characters per
   string. No loops, randomness, or state of its own.
5. **Versioned and negotiable.** The file declares `spec` and the predicates it
   `requires`; an evaluator declares which it supports.

## File

A mod may contain `extender/conditions.json`. (The engine tolerates unknown
folders and files in a mod folder: the runic mod already ships `tools/`,
`README.md` and notes.)

```json
{
  "spec": 1,
  "requires": ["health_percent", "time_of_day"],
  "conditions": [
    {
      "id": "charge_harvest_wounded",
      "target": { "kind": "passive", "id": "arendeth_runic_magic_Charge_Harvest" },
      "when": {
        "all": [
          { "health_percent": { "max": 50 } },
          { "weapon_class": ["Unarmed", "Natural"] }
        ]
      },
      "on_unavailable": "inactive",
      "note": "Free-text for humans; ignored by evaluators."
    }
  ]
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `spec` | yes | Integer, `1` for this document. |
| `requires` | no | Predicate names that must be supported; otherwise every condition in the file is unavailable. |
| `conditions[].id` | yes | Unique within the file; `[a-z0-9_]{1,64}`. |
| `conditions[].target` | yes | `{kind, id}`. `kind` is `passive` or `ability` in v1 (`skill` is reserved). `id` is the full data id. |
| `conditions[].when` | yes | An expression (below). |
| `conditions[].on_unavailable` | no | `inactive` (default) or `active`. |
| `conditions[].note` | no | Ignored. |

At most one condition per target per file. Two mods that condition the same
target are combined with AND (every condition must hold).

## Expressions

```
Expr := { "all": [Expr, ...] }      // AND, at least one item
      | { "any": [Expr, ...] }      // OR, at least one item
      | { "not": Expr }
      | { "<predicate>": <args> }   // a leaf: exactly one key
```

Evaluation is three-valued (Kleene): `true`, `false`, `unknown`. A leaf is
`unknown` when its predicate is not supported or its input is not available.
`all` is false if any item is false, otherwise unknown if any is unknown;
`any` is true if any is true, otherwise unknown if any is unknown; `not`
keeps `unknown`. A condition whose result is `unknown` follows `on_unavailable`.

## Predicates (v1)

*Holder* is the entity that has the passive or casts the ability. *Target* is
the ability's target, or the enemy in the current exchange for a passive.
Percentages are 0 to 100 of the maximum. Names are matched case-insensitively
against the game's own lists in `core_2/messages.json`.

| Predicate | Arguments | True when | Native key (passive / ability) |
| --- | --- | --- | --- |
| `weapon_class` | name or list of names from `weapon_type` except `Any` | the holder's wielded weapon class is one of them | `restrict_weapon_type` (one name) / `requirements.one_of` |
| `shield` | boolean | the holder has (true) or lacks (false) a shield | `restrict_shield` (encoding unverified) / `requirements.always: [7]` for true |
| `holder_tag` | name or list from `tag` | the holder has any of the tags | `restrict_tags` / none |
| `target_tag` | name or list from `tag` | the target has any of the tags | `restrict_tags_enemy` (encoding unverified) / none |
| `party` | `"alone"` or `"in_party"` | the holder is alone / in a party | `apply_mode` `solo_only` / `party_only` / none |
| `health_percent` | `{min?, max?}` (at least one) | holder health is within the range | none |
| `stamina_percent` | `{min?, max?}` | holder stamina is within the range | none |
| `target_health_percent` | `{min?, max?}` | target health is within the range | none |
| `stacker` | `{id, min?, max?}` | holder has the stacker's stacks within the range (`min` defaults to 1, or to 0 when only `max` is given) | none |
| `equipped` | see below | an equipment slot matches | none |
| `inventory_count` | `{item, min?, max?}` (`min` defaults to 1, or to 0 when only `max` is given) | holder carries that many of the item | none |
| `skill_level` | `{skill, min?, max?}` | holder's level in the skill is within the range | none |
| `time_of_day` | name or list from `day_part` | the current day part is one of them | none |
| `season` | name or list from `year_part` | the current season is one of them | none |
| `biome` | name or list from `biomes` | the holder's tile biome is one of them | none |
| `in_combat` | boolean | the holder is in combat | none |
| `in_settlement` | boolean | the holder is inside a settlement | none |
| `companions` | `{min?, max?}` | the number of companions is within the range | none |

`equipped` takes one object with a `slot` (an `eq_slot` id such as
`right_hand`, `amulet`, `ring1`) and exactly one of: `empty` (boolean), `item`
(a data id), `item_type` (a name from `item_type`), or `item_tag` (an entity
tag string).

Ranges: `min` and `max` are inclusive numbers; `min` must not exceed `max`. For `stacker` and `inventory_count`, `min` defaults to 1 when no bound is given and to 0 when only `max` is given, so `{"max": 0}` means "none".

## Realisation strategies (informative)

The language does not fix how a target is switched. Two strategies are
compatible with the same files:

- **Tag bridge (no hook into passive evaluation).** The evaluator keeps a
  synthetic tag on the holder in step with the condition and the build tool
  adds a native `restrict_tags` on that tag to the passive. With no evaluator
  the tag is never set, so the passive is inactive: fail closed for free. Open
  question: whether `restrict_tags` accepts mod-defined tags or only the 23
  built-in ids, and whether tags can be changed at runtime from outside the
  engine.
- **Direct gate.** The evaluator is called where passives and abilities are
  applied. Needs those engine functions found and runtime-verified (see
  `skill-schema.md`).

## Compile-down and hoisting

For a top-level `all` (or a single leaf) every leaf that has a native key for
the target kind is a *necessary* condition, so a build tool may emit that
native key without changing meaning (it can only make the target more
restrictive in the same way the full condition is). Leaves under `any` or
`not` must not be hoisted. A condition made only of hoistable leaves is fully
native and needs no evaluator; the validator reports this per condition.

## Authoring rules the validator enforces

- unique condition ids and one condition per target;
- exactly one key in a leaf; known predicate names and well-formed arguments;
- names resolve against the game's lists; ranges are ordered and within 0..100
  for percentages;
- the size limits above; a warning when a predicate is used but missing from `requires`;
- with `--mod`, every target id exists in that mod's `passives.json` or
  `abilities` folder.

## Open questions

- Value encoding of `restrict_shield` and `restrict_tags_enemy` natively.
- Whether mod-defined tags work in `restrict_tags`.
- Exact timing of evaluation relative to turns, and how an evaluator is told a
  condition's inputs changed (needs the evaluation points to be found).
- Stacker and status ids come from `ability_stackers` data; their runtime
  identity is unverified.
