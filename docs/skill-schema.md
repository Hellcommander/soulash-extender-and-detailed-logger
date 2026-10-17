# Skill, passive and ability schema (static analysis)

What the Soulash 2 executable itself recognises in skill, passive and ability
data, and what that means for "conditions for skills". Everything here comes
from reading `Soulash 2.exe` (build `d10cec5f`, version 0.10.3.4) and the
installed `core_2` data. Nothing was run. Reproduce with `tools/pe_xrefs.py`
(`fnstr` and `has` modes).

## Method and limits

- **Parser key sets.** JSON loaders reference each key as a string literal, so
  the strings referenced by a loader function are its accepted keys.
- **Name existence.** A key or effect name is "known" if it appears as a data
  string or, for strings of 15 characters or fewer, as an inline immediate in
  code. Both are checked (`tools/pe_xrefs.py <exe> has <key>`).
- **Existence is not behaviour.** A known name tells you the engine will not
  ignore it; it does not tell you what it does, what values are valid, or when
  it is evaluated. Semantics need an in-game test or runtime analysis.
- Three keys the runic mod uses (`game_required`, `steps`, `steps_type`) were not
  located by this method, but all three are used by `core_2` itself, so they
  are real; the method has false negatives.

## Passive loader (function RVA `0x509430`)

A passive accepts exactly these keys:

`id`, `name`, `description`, `image`, `restrict_weapon_type`, `restrict_shield`,
`apply_mode` (`party_only` / `solo_only`), `restrict_tags`, `restrict_tags_enemy`,
`effects` (each `effect`, `value`, `secondary_value`, `third_value`).

This is the whole passive **condition vocabulary**: weapon type, shield,
whether the holder is alone or in a party, tags on the holder, tags on the
enemy. Nothing else can gate a passive in data.

Passive effect names are resolved in the function at `0x514ad6` and elsewhere.
`core_2` uses 131 distinct passive effects. Condition-like effects that exist:
`damage_reduction_missing_health`, `thorns_for_missing_health`,
`regeneration_low_health`, `heal_on_damage_type`, `on_attack_stack`,
`on_damage_type_stack`, `on_max_stacks`, `trigger_ability_on_cast`,
`on_attack_cast`, `on_kill_create_item`, `on_kill_persona_create_item`,
`death_under_effect_gain_health`, `death_under_effect_gain_stamina`,
`stamina_percent_on_kill`, `poison_transfer_on_kill`. Names present in the
executable but unused by `core_2`: `self_resistance`,
`uncover_entity_with_tag`.

## Ability loader (function RVA `0x420e50`)

Keys: `id`, `name`, `description`, `image`, `animation`, `cost`
(`health`, `stamina`, `item`, `ammo`, `skill`), `requirements`
(`one_of`, `always`), `slots`, `target`, `effects`, `effects_keys`, `cooldown`,
`item_type`, `min_range`, `range`, `cast_time`, `duration`.

`requirements` takes numeric ids from the game's own `ability_requirement`
list in `core_2/messages.json`; it is a **weapon-class** list only:

| id | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| meaning | None | Axe | Sword | Mace | Dagger | Polearm | Ranged | Shield | Unarmed | Improvised | Natural | Bow | Crossbow | Throw |

So `one_of: [11, 12]` means "Bow or Crossbow", as the runic mod already
assumes. `always` is the AND form.

Ability effect names (function `0x41e530`, 115 names) include:
`damage`, `damage_type`, `magic_power_damage`, `magic_power`,
`magic_power_per_entity`, `magic_power_per_tag` (with `restrict_to_tag`),
`damage_on_missing_health`, `damage_on_missing_stamina`, `dmg_on_cooldown`,
`dmg_per_companion`, `damage_without_companions`, `skill_level_damage`,
`distance_damage`, `reset_cooldown_on_kill`, `summon`, `summon_count`,
`summon_on_move`, `summon_weather`, `aura`, `stacker`, `effect_per_stack`,
`turn_damage_to_stacks`, `stacker_to_dot`, `ally_command`, `cage`, `transform`,
`corrupt`, `ignore_party`, `copy_to_party`, plus the usual combat statuses.

## Names the runic mod flagged as unverified

`damage_percent`, `on_kill_create_item`, `trigger_ability_on_cast`,
`magic_power_per_entity` and `restrict_tags` / `apply_mode` all exist in the
executable, so none of them is a silent no-op caused by a misspelling.
Whether they behave as the mod assumes (chaining, scaling, who counts as the
caster) is still unverified. Of 209 lowercase identifiers the mod uses, none
is unknown to the engine.

## Conditions for skills: what exists and what does not

| Wanted condition | Native support |
| --- | --- |
| Weapon class (axe, bow, ...) | Yes: ability `requirements`, passive `restrict_weapon_type` |
| Shield equipped / not | Passive `restrict_shield` (semantics unverified) |
| Holder or enemy carries a tag | Passive `restrict_tags`, `restrict_tags_enemy` |
| Alone vs in a party | Passive `apply_mode` |
| Health / stamina thresholds | Only as effect scaling (`damage_on_missing_health`, ...), not as an on/off gate |
| Item tags, equipped item ids, buff state, time of day, season, location | No |
| Arbitrary combination (and/or/not, counters) | No |

A real condition system therefore needs code: either a native plugin that
evaluates a predicate where passives or abilities are applied, or the author's
planned Lua layer. The data format cannot express it.

## Implications for the extender

1. Passive application and ability availability are the two evaluation points.
   Their loaders are known (`0x509430`, `0x420e50`); the code that *reads* the
   parsed fields at runtime is not yet identified. Finding it needs dynamic
   analysis (a debugger breakpoint on a field, or tracing which code reads the
   parsed struct) and cannot be done statically with confidence.
2. A conditions feature should be specified as data first (a small predicate
   vocabulary in mod JSON that an extender plugin or the Lua layer evaluates),
   so mods written now keep working when the engine support changes. That
   spec now exists: [skill-conditions.md](skill-conditions.md).
3. Until the evaluation points are found and runtime-verified, nothing here
   should be hooked in a shipped build.
