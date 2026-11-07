# Runic Magic audit: Dredmor's Runecaster vs `arendeth_runic_magic` 0.14.5

Date: 2026-11-07. Sources read: the Runecaster mod (`Runecaster/mod/skillDB.xml`,
`spellDB.xml`, `itemDB.xml`, `craftDB.xml`, `monDB.xml`, v1.7 by Ruigi; it has no
readme) and the live Soulash 2 mod folder (143 abilities, 153 entities, 174
milestones after this audit, `passives.json`, `ability_stackers.json`,
`ability_amplifiers.json`, `tools/runic_registry.json`, `tools/build_runic.py`,
README, CHANGELOG, the owner's design notes). Checks: SkillCreator
`s2_quality.py`, `audit-mods`, `check-assets`, the extender's `pe_xrefs.py has`
against `Soulash 2.exe`, and `tools/effects_compile.py` for the proposal rules.
The game was not launched. Everything marked **unverified** needs an in-game run.

Three parts, in the order asked: (1) the comparison, (2) bugs and the fixes
applied, (3) the design proposal, not applied.

---

## 1. Comparison

### 1.1 What Runecaster is

Runecaster is a Dredmor wizard skill with 8 levels. Every level is a
rune-making spell, not an attack:

| Dredmor level | Ability | Spell | Mana (base / savvy bonus / min) | Result |
| --- | --- | --- | --- | --- |
| 0 | 1st Circle Runes | `1st Circle Rune` (template 30, anchored) | 10 / 0.235 / 5 | **one** rune from {Strength, Movement, Life, Self}, spawned on the floor |
| 1 | 2nd Circle Runes | `2nd Circle Rune` | 15 / 0.210 / 8 | one of {Matter, Energy, Fire, Water} |
| 2 | Mass 1st Circle Runes | template 09 (an area) | 20 / 0.175 / 10 | one rune per template tile, 1st circle |
| 3 | 3rd Circle Runes | | 25 / 0.175 / 13 | one of {Spirit, Harvest, Rigidity, Transformation} |
| 4 | Mass 2nd Circle Runes | template 09 | 40 / 0.210 / 20 | several 2nd-circle runes |
| 5 | 4th Circle Runes | | 40 / 0.150 / 20 | one of {Heavens, Purity, Wisdom, Destiny} |
| 6 | Mass 3rd Circle Runes | template 09 | 60 / 0.225 / 30 | several 3rd-circle runes |
| 7 | 5th Circle Runes | | 50 / 0.125 / 32 | one of {Ambition, Gods, Harmony, Chaos} |

Every level also gives +1 Sagacity. The runes themselves are **reagents**
(`<gem />`, price 0, `special="1"`): they cannot be thrown or used. Two
crafting tables turn them into spells:

- **Lathe** (n-Dimensional Lathe): 2 to 4 runes become a **Rune Word**, a
  stackable thrown weapon (`<weapon thrown=... hit="Runeword X" canTargetFloor="1">`).
  Throwing it casts the `Runeword X` spell where it lands. 21 runewords.
- **Alchemy** (Modular Alchemy Box): 2 runes of one kind become 1 rune of the
  next kind in the same circle (a closed cycle per circle: 20 recipes).

Loadout: Coat of Himonya (+2 dodge, +4 mana, +1 sagacity), 3 of each 1st-circle
rune, 4 Rune Word of Force, both toolkits. Flavour items: Coat of Lesko, the
"FREE ZORKMIDS" tome (Mega Midas on hit), Runecrunch Cereal (eating it spawns a
random low runeword). One monster: the Steel Statue (level 9 construct,
crushing 18, armour 10, block 20, resists, 1-in-3 on-hit "Fist of Steel" knockback).

### 1.2 Skill progression

| Runecaster | Soulash milestone | Faithful? |
| --- | --- | --- |
| 1st Circle (lvl 0) | `Draw First Circle` lvl 1 | order kept |
| 2nd Circle (1) | `Draw Second Circle` 8 | order kept |
| Mass 1st (2) | `Mass First Circle` 12 | order kept |
| 3rd Circle (3) | `Draw Third Circle` 16 | order kept |
| Mass 2nd (4) | `Mass Second Circle` 18 | order kept |
| 4th Circle (5) | `Draw Fourth Circle` 26 | order kept |
| Mass 3rd (6) | `Mass Third Circle` 28 | order kept |
| 5th Circle (7) | `Draw Fifth Circle` 38 | order kept |
| +1 Sagacity per level | +1 statistic every level 11 to 50, +10% all attributes at 30 (toolkit convention) | Soulash convention, fine |
| nothing else | 36 passives, 12 amplifiers, 8 hotbar spells, 109 recipe unlocks, 1 extra active (`Recall Echoes`) | see 1.7 |

The eight Runecaster abilities map one to one and in the right order. Costs are
stamina 4/5/6/7/8 (circles) and 10/12/15 (mass) with cooldowns 4 to 14 instead of
mana 10 to 60; Dredmor's circles have no cooldown.

### 1.3 Rune making: `spawnitemfromlist` vs Rune Roller

Dredmor: one cast spawns **exactly one** rune, uniformly from the circle's four,
on the floor next to you (template 30 is a single anchored tile; template 09
for the Mass versions is a ring of tiles, one rune each).

Soulash (`rollers.base: true` since 0.14.3): `Draw X Circle` is `aoe_self`,
range 1, and summons a 1-turn stationary caster entity `Rune Roller (X Circle)`
(tag `magic`, health 1, allies `player`) whose `ability_user` holds the circle's
four `Conjure_*` abilities. Each `Conjure_*` summons the **rune item entity**
next to the roller (that is how an item lands on the floor; the vanilla
"summon" of an item entity is the pattern the mod relies on and the owner has
seen it conjure in game). On top of that the `X Circle Resonance` passive adds
four independent 15% `trigger_ability_on_cast` rows (Draw → Conjure).

Divergences:

1. **How many runes a cast gives is not controlled.** A 1-turn AI caster with
   four zero-cost, zero-cooldown `aoe_self` abilities may fire one of them or
   all four before it expires; nothing in the data says "one". The owner's
   in-game report lists the roller "summoning Rune of Life/Movement/Self/Strength",
   which reads as all four. Plus 0 to 4 more from Resonance. Dredmor: exactly one.
2. **Mass circles conjure nothing themselves.** `Draw Mass X Circle` is
   `target: self` with only `stacker_count: 2`; every rune comes from the
   `Mass X Circle Resonance` passive (four independent 75% rows, expected 3,
   possibly 0). Dredmor: one rune per template tile, never zero.
3. Dredmor's circles cost mana and nothing else. Soulash's also add 1 or 2
   stacks of `Runic Focus` (+2 magic power per stack, max 5, 10 turns) and feed
   four more player passives: `Overdraw` (25% to also draw the circle below),
   `Cascade Logic` (30% `Cascade Discharge` per draw), `Bastion Logic`
   (25% `Ward Mend` per draw), `Scribe's Habit` (5% per attack per 1st-circle
   rune). None of this exists in Runecaster.

### 1.4 Runes and the Rune Charge

| | Runecaster | Soulash |
| --- | --- | --- |
| What a rune is | a reagent gem, cannot be used or thrown | an item (`type 16`) that is a crafting ingredient **and** can be powered |
| Powering | none | `Rune of X` + `Rune Charge` (crafted from resource type 6, 8 time) → `Powered Rune of X` (`type 17`, `usable.ability = Throw_X`) |
| Throwing a rune | impossible; only Rune Words are weapons | every one of the 26 powered runes is a thrown spell with its own effects (`Throw_*`, 26 abilities) |
| Transmutation | alchemy, 2 → 1 within the circle, closed cycle | Runeforge (`tools: [15]`), same 20 recipes, **same cycle, verified identical** |
| Fused runes | none | 6 hybrids (Wind, Blood, Venom, Thorns, Gravity, Void), uncraftable by circles |
| Starting kit | 3 of each 1st-circle rune, 4 Rune Word of Force, Coat of Himonya, 2 toolkits | 3 of each 1st-circle rune, 3 Rune Charges, 2 Runeword Force, a Robe (vanilla 183) |

The transmutation table is the one part of the crafting layer that is a
faithful port. The Powered Rune layer is the mod's own twist (documented as such
in the owner's design notes) and is the single largest departure from the
source: in Dredmor the rune economy exists only to feed runewords, and the
player has 21 spells; in Soulash the player has 26 rune spells + 47 runeword
spells + 8 hotbar spells + 8 tablets.

### 1.5 Runewords: recipe, level and effect

Dredmor recipe = lathe inputs; Dredmor level = the item's `level` attribute
(2/4/6/8/10). "Mine" in Dredmor means a glyph on the floor that fires its
effect on anything that steps on it (`minePermanent="1"` keeps re-firing for
`mineTimer` turns). Soulash "mine" means a stationary 60-turn AI caster entity
(20 health, tag `rune_mine`) that casts its burst (cooldown 6, range 3) at an
enemy that comes within sight range 3.

| Runeword | Dredmor recipe → Soulash recipe | Lvl D → S | Dredmor effect | Soulash effect | Verdict |
| --- | --- | --- | --- | --- | --- |
| Force | Strength+Movement → same | 2 → 3 | Forcemine (12 t, permanent): blasting 4+0.2, knock 2 | 8–14 physical r1 + knock 3, 3 Force Mines (5–9, knock 2) | faithful |
| Health | Strength+Life → same | 2 → 5 | healthmine (16 t): heal 2+0.2 to whoever steps on it | heal 10–18 r2, 3 Health Mines (heal 8–14) | faithful (targeting bug fixed, 2.1 B2) |
| Instability | Life+Movement → same | 2 → 7 | Instabilitymine: aethereal 4+0.3 + random **displace** | 6–10 true r2, knock 4, fear, 3 mines (knock 4 + fear) | approximation: no teleport primitive |
| Swiftness | Self+Movement → **Movement+Destiny** | 2 → 20 (now 26) | Swiftnessmine (60 t) buffs who stands on it: +5 nimble, +25 dodge; plus 300-turn buff on target | self buff 10 t: attack speed −0.1, move −0.15 | recipe changed; glyph-standing buff lost; a 2nd-circle word moved to 4th circle |
| Flames | Fire+Energy → **Fire+Fire** | 4 → 10 | Firefield + Firefield2 (template 10 ring, 16 t): conflag 3+0.2, burn | 14–22 fire r3, burn 5, 9 vanilla Burning Flames (561, 10 t) | effect faithful; recipe changed |
| Ice | Matter+Water → same | 4 → 12 | Icemine (33% Slipped: crush + paralyse) + Chillmine r2 | 14–22 frost r3, frostbite, immobilize, 3 Chill Mines | faithful |
| Storms | Energy+Strength+Movement → **Energy+Water** | 4 → 14 | Voltmine (12 t): voltaic 8+0.25, paralyse 2, 33% Stormbolt | 16–26 electric r3, stun, chain 3, 3 Volt Mines (chain 2) | effect faithful; recipe changed |
| Stone | Matter+Strength+Life → **Matter+Rigidity** | 4 → 16 | Stonewall ring (template 10): crush 18+0.5, knock 4, walls 48 t | 14–24 physical r2, immobilize, knock 3, 4 permanent Stone Spikes (631) | faithful; recipe changed |
| Teleportation | Spirit+Movement+Self → **Movement+Transformation+Self** | 6 → 32 | caster warps to the glyph (Xeuclid's Translation) + "Stop Hitting Yourself" buff | `trade_places` with the target + fear 2, r2 square | not faithful: swaps instead of teleporting; `Throw_Movement` (lunge 5) is closer to the source |
| Blossom | Life+Water+Harvest → **Harvest+Life** | 6 → 22 | Fruitblast: spawns food (Apple/Plum/Pear/Fairywodger/Night Cap), confuse 10 | heal 16–26, cleanse, 3 Bloom Mines (heal) | not faithful: no food, no confuse |
| Aurification | Life+Transformation+Matter+Harvest → **Gods+Ambition** | 6 → 46 | Auricmine (10 t): midas (kill → gold), transmutative 8+0.2, Golden Ratio curse | 18–30 true r3, vulnerable, silence, 3 Auric Mines (true + silence) | not faithful (no midas primitive); moved from 6 to the 5th circle |
| Decay | Life+Transformation+Water+Spirit → **Harvest+Rigidity** | 6 → 28 | Decaymine r2 (28 t): toxic/asphyx/putrefying + Maggotsmine (thaumite swarm) | 12–20 poison r3, 3 Decay Mines | half faithful: no swarm |
| Steel | Rigidity+Fire+Strength+Matter → **Matter+Transformation** | 6 → 34 | summon Steel Statue (a real golem that walks and fights, permanent) + Steel ring (slashing 8+0.5, bleed) | Steel Statue construct, **stationary**, 40 t, 120 hp, Steel Ring pulse every 4 t | half faithful: statue cannot move or melee |
| Light | Purity+Fire+Energy+Heavens → **Purity+Wisdom** | 8 → 24 (now 26) | Divinemine (24 t): Divine Smite on Demon/Undead only, Divine Power buff on the player standing on it; Divine Power Buff on the summon | 20–32 holy r3 + heal 10–16 to everyone in the blast, 3 Divine Mines (holy + heal, no party filter) | not faithful: hits everything, heals enemies |
| Death | Rigidity+Life+Spirit+Destiny → **Chaos+Rigidity+Spirit** | 8 → 40 | 350 necromantic to living taxa, **heals Undead 350**, then curses the caster (Curse of the Mind, Weakness, Soul Price, blinding, delayed) | 28–46 death r3, fear, vulnerable | not faithful: no undead clause, no price |
| Restoration | Transformation+Life+Spirit+Purity → **Life+Spirit** | 8 → 18 | Restoremine (60 t): heal 50+0.5, uncurse, mana 20+0.5 | heal 25–40 r3, cleanse poison, stamina 20, 3 Restoration Mines | faithful (targeting bug fixed) |
| Prophecy | Heavens+Wisdom+Self+Destiny → **Spirit+Heavens+Destiny** | 8 → 48 | Prophecy1: 7 Prophecyblasts at 2,4,…,14 turns around the target (aethereal 10+0.34, paralyse, bleed) + 13-turn marker buff | holy blast + Prophecy Beacon construct, 14 t, pulses every 2 t (holy 5–9, stun 0.5) | good approximation (7 pulses) |
| Love | Harmony+Purity+Water+Life → **Harmony+Self** | 10 → 42 | charm 24 turns + "Unresistable" (−100 resist) | heal 14–24 r2 + `corrupt` 5 turns, no party filter | half faithful: 5 vs 24 turns; heals the charmed |
| Power | Ambition+Spirit+Transformation+Self → **Strength+Ambition** | 10 → 30 (now 38) | 30-turn self buff: +100 crit, +10 melee, +100 haywire, +20 magic power (and a buff on the target too) | self buff 10 t: +50% damage, +10 magic power | approximation |
| Destruction | Gods+Destiny+Heavens+Fire → **Fire+Energy+Chaos** | 10 → 44 | Torus Blasts 1–3, Gog's Pyre, Runic Explosion (blasting 12 + conflag 12, flamefield), Big Dig | 30–48 fire r4, burn, stun, knock | faithful in spirit |
| Entropy | Chaos+Purity+Transformation+Energy → **Chaos+Energy+Transformation** | 10 → 50 | Entropymine (10 t, permanent): 28 `trigger percent` rows (2% to 25%), many spells at once incl. two heals and a 25% displace | Entropy Vortex construct (20 t), pulse every 2 t; `Entropy Vortex Surge` passive (lvl 50) rolls 9 independent rows (sum 1.55) | faithful mechanism (independent percent rows, like the source); smaller table |

Scores: 7 recipes identical (Force, Health, Instability, Ice, Entropy, and the
trivially reordered Flames/Blossom pairs if ingredients were the only point), 14
changed. Recipe sizes are 2 or 3 everywhere; Dredmor uses 2, 3 and 4. Levels
follow a 1-to-50 spread the toolkit expects, which is fine, but three runewords
were unlockable before their ingredients existed (fixed, B4).

### 1.6 Items not ported

Coat of Himonya, Coat of Lesko, FREE ZORKMIDS tome, Runecrunch Cereal, the
"Runeword of X" shop variants (price 750 to 15000). The Soulash mod has a
`Book of Runic Magic` and a `Runic Magic Hall` + Runic Adept NPCs instead,
which is the Soulash way to teach a skill.

### 1.7 Content that has no Runecaster source

26 extra runewords (Steam, Arc, Shrapnel, Magnet, Hemorrhage, Cyclone, Glacier,
Bulwark, Reaping, Pestilence, Veil, Tempest, Sanctuary, Singularity, Cascade,
Collapse, Bastion, Bloodfire, Wildfire, Hailstorm, Ironhide, Plaguewind, Anchor,
Starfall, Blight, Stormcall), 6 fused runes, 8 hotbar spells (`Cast_*`) with 8
craftable Spell Tablets, 12 Etch amplifiers, 7 attunement passives + stackers,
7 bag passives, `Runic Focus`, the rune-logic layer (Rune Echo, Cascade Node,
Collapse Seed, Ward Stone, `Recall Echoes`, `Runic Momentum`, `Cascade Logic`,
`Collapse Sequence`, `Bastion Logic`, `Chain Reaction`), 8 Rune Glyphs left by
rune throws, `Runic Constructs`, `Storm Sprite Swarm`, `Tempest Pylon Surge`,
`Overdraw`, `Scribe's Habit`, `Charge Harvest`, `Runic Strike`. By count this is
about three quarters of the mod. It is well made (every check passes, every
ability has FX and sound), but a player coming from Runecaster will not
recognise most of it, which is the "not faithful" the owner feels.

---

## 2. Bugs

### 2.1 Fixed in the mod folder

Validated afterwards: `s2_quality.py` 0/0/0, `audit-mods` 0 errors 0 warnings,
`check-assets` 0 issues, every JSON parses, every milestone filename matches
its level. The builder (`tools/build_runic.py`) and `tools/runic_registry.json`
were patched the same way so a rebuild cannot bring the bugs back.

**B1. 31 abilities could only be cast while wielding a bow or crossbow.**
`requirements.one_of: [11, 12]` is the game's `ability_requirement` weapon-class
list (11 Bow, 12 Crossbow; see `docs/skill-schema.md`). It was copied from the
arcane_ballistics trap pattern onto every Mine Burst (11), Rune Glyph release
(8), construct Ring (8) and player payload (`Cascade Discharge`,
`Collapse Implosion`, `Collapse Aftershock`, `Ward Mend`). The mine, glyph and
construct entities wield weapon type 5, so if the engine checks requirements
for AI casters none of them ever fired; the player payloads needed the player
to hold a bow. Fix: the `requirements` block removed from all 31; builder
`MINE_ABIL_REQ` and the two inline literals removed. (If `Mine Force Burst` was
seen *casting* in game today, the engine skips the check for AI casters and the
removal is merely harmless; the player payloads were still gated.) The same
pattern sits in `arendeth_arcane_ballistics`: follow-up for that mod, not touched.

**B2. Heal-only mines healed the enemy that triggered them.** `Mine Health
Burst`, `Mine Bloom Burst`, `Mine Restore Burst` and the Ward Mending Glyph's
`Cast Warding Rune Release` were `target: health`, range 3: the AI picks a
hostile target and casts a heal on it (or never casts, if the AI refuses to heal
enemies). Fix: `target: aoe_self`, `range: 0`, the pattern `Sanctuary Totem
Ring` already uses in this mod, so the glyph heals whoever stands around it, as
Dredmor's healthmine does. Builder mine loop patched (heal-only → `aoe_self`).

**B3. Magic power per standing construct was 200 per entity.** Vanilla Hell
gives 10 per Burning Flame and the mod's own `Echo Etch` gives 10 per Echo; the
README reads the number as a radius ("within 200 tiles"), which it is not. With
three mines standing each burst would have carried +600 magic power × 0.4 to 0.5
scaling. Fix: 36 rows (`Runic Constructs` 26, `Storm Sprite Swarm` 1,
`Runic Momentum` 9) set to 10; builder patched in three places. The *passive*
form of `magic_power_per_entity` is itself unconfirmed (the toolkit only
confirms the amplifier form), so this fixes the magnitude, not the question of
whether it works.

**B4. Three recipes unlocked before their ingredients could exist.** Swiftness
(lvl 20) needs Destiny (4th circle, 26); Light (24) needs Purity and Wisdom
(26); Power (30) needs Ambition (5th circle, 38). Fix: milestones moved to
26 / 26 / 38 (files renamed, `requirements.skill` updated) and the registry
levels changed to match. The README table still shows 20/24/30.

**B5. The Book of Runic Magic could not be crafted.** It has a `craftable`
component (vanilla runes as inputs) but nothing granted the recipe; every
vanilla skill book is unlocked by a level-44 recipe milestone. Fix: new
`44_Craft_Book_of_Runic_Magic.json`; builder adds the same milestone.

**B6. Hotbar layer removed (owner's decision, 2026-11-07, see 3.2).** Spells
come from thrown runes landing, not from the skill bar, so the following were
deleted from the live mod: the 8 `Cast_*` hotbar spells and their milestones,
the 8 Spell Tablets (entities + recipe milestones), all 12 Etch amplifiers
(`ability_amplifiers.json` deleted; four installed owner mods ship without that
file), `Recall Echoes`, the player-cast payloads `Cascade Discharge`,
`Collapse Implosion`, `Collapse Aftershock`, `Ward Mend` with the passives
`Cascade Logic`, `Collapse Sequence`, `Bastion Logic`, `Chain Reaction`, the
node entities Cascade Node / Collapse Seed / Ward Stone, and the runewords
Cascade, Collapse and Bastion that only fed that layer. With them went 19
animations and 16 sound rows plus their `.ogg` files that nothing else
referenced, 4 character tags, and 16 `fx_map.json` rows. 102 files removed;
a full copy of the folder from before the removal is at
`D:\games\Ai assisted toolkit\Tools\Output\Soulash2\_runic_removed_2026-11-07\`.
Kept: the 8 Rune Glyphs and their `Cast_*_Release` bursts (they are left by
thrown runes), Rune Echoes, `Runic Momentum`, `Runic Constructs`. Registry:
`casts[].hotbar: false`, `tablet` keys dropped, `augments` / `payloads` /
`logic.passives` / `logic.nodes` emptied, `echo.recall` dropped, three
runewords removed; builder honours `hotbar` and deletes
`ability_amplifiers.json` when there are no amplifiers. After the removal:
127 abilities, 139 entities, 138 milestones, 32 passives; `s2_quality.py`,
`audit-mods`, `check-assets` all clean, no orphan ability, no dangling id.
The README still describes the hotbar spells, tablets, Etches and rune logic.

### 2.2 Found, not changed (needs the owner or the game)

- **Rune Roller fires an unknown number of Conjures** (1.3). Not fixable in
  data; the proposal replaces it.
- **`tools/build_runic.py` is stale.** A rebuild into a scratch copy differed
  from the live folder in 178 files: the live mod has `aoe_self` on Draw and
  Conjure (builder writes `self`), `Throw_Self` through the `Warding Glyph
  Trigger` stacker and `Throw_Self_Glyph` (builder summons the glyph directly),
  the mod's own `glyph32` / `items` tilesheets and animated glyph frames, eight
  Runic Adept races in the hall, the sound table in `assets.json`. **Running
  the builder now would destroy that work.** Either port the hand edits into the
  builder or retire it; this audit patched it only so its known bugs stay fixed.
- **`effects_keys.magic_power_per_entity` on abilities carries no amount.**
  `Cascade Discharge`, `Collapse Implosion`, `Collapse Aftershock`, `Ward Mend`,
  `Recall Echoes`, `Cast Rune Burst` name the counted entity but no per-entity
  value; the toolkit lists the ability form under `effects` as a number. Whether
  the engine pairs a key with a default amount is unverified.
- **`damage_percent` on the seven Attunement passives** is not a vanilla passive
  effect and not in the toolkit catalog; the executable does contain the name as
  inline immediates (the same signature as `extra_equip_slot` and
  `on_damage_type_stack`, which are vanilla), so it is plausibly real. Four
  other owner mods use it. Unverified in play.
- **`Warding Glyph Trigger` stacker** has `max_stacks 1`, no duration and
  `max_stacks_cast_unlock: false` (same shape as vanilla Blessed Knuckles, max 5):
  whether the single stack is consumed on cast, so the next Rune of Self throw
  triggers again, is unverified.
- **Divine Mine / Runeword Light** damage and heal everything in the area (no
  party filter because `ignore_party` would also skip the heal). Design, not
  code, but it heals enemies.
- **Runic Adept NPCs** hold `Draw First Circle`, so a friendly adept in combat
  summons rollers and drops runes on the floor. Flavour; mention only.
- **Validator gap in the extender**: `validate_conditions.py` `TAGS` stops at
  Zombie; `Vampire` (tag 23) and the other late tags are rejected in
  `target_tag`. Not a mod bug.

### 2.3 Not found (checked, clean)

Wrong ids, duplicate passive ids, unreachable milestones, orphan abilities,
missing animations or sounds, icon indices past the sheets, empty tooltips,
`::token`s that resolve to nothing: all clean per `audit-mods`, `s2_quality.py`
and `check-assets`, plus a cross-reference script over milestones, abilities,
entities and recipes (every ability is either milestoned or reached through an
entity kit, a usable item, a passive row or a stacker; every recipe milestone
points at a craftable entity). `s2_editor_check.py` was not run: it generates a
TestBot script that drives the live game's F3 editor, which this audit may not
launch. The mod has no `extender\` folder, so there was nothing for
`validate_conditions.py` / `effects_compile.py` to check in the mod itself; both
were run on the repo examples (clean) and on the proposal file below.

---

## 3. Proposal: a faithful Runecaster on Soulash 2

### 3.1 The engine limits that shape every option

| Dredmor needs | Soulash 2 native | Extender (Triggers plugin, `extender\effects.json`) |
| --- | --- | --- |
| `spawnitemfromlist` (exactly one of N) | no; only N independent chance rows or an AI roller | `choose` (exact weighted one-of), evaluator-owned |
| `trigger amount=n` (delay) | no | `delay`, `repeat … every` (holder turn ticks) |
| mines firing on **step** | no step hook; stationary AI caster that casts at an enemy in range, or `aoe_self` pulses | nothing better; `on turn` + a proximity predicate would be needed (does not exist) |
| `displace` / teleport-to-glyph | `lunge`, `trade_places`, `charge` only | `teleport` is listed but evaluator-only and unimplemented |
| `taxa` filters (Demon / Undead) | `restrict_tags_enemy` (encoding unverified) | `when: {target_tag: [...]}` |
| buff only while standing on the glyph | no | `on turn` exists; a "near entity" predicate does not |
| midas, food spawning, charm 24 turns | no midas; `summon` of an item entity spawns items; `corrupt` with `duration` is charm | `create_item` |
| a golem that walks and fights | yes: a summon entity with `moveable` + AI (the Steel Statue here was built on the stationary trap pattern on purpose) | n/a |
| conditional triggers | stackers only (`on_max_stacks_cast`); `trigger_ability_on_cast` / `on_attack_cast` with a flat chance | cast / attack / kill / turn / damaged / dodge / block / crit with `when`; **log mode only so far, never run active** |

Everything below that says "extender" is therefore conditional on the first
active run of the Triggers plugin; every "native" change can ship today.

### 3.2 Decided: a rune is a thrown object that releases its spell where it lands

The owner confirmed on 2026-11-07 that the thrown rune or runeword releasing
its spell at the landing tile is the intended core, and that spells must not be
"a skill thing" on the hotbar. So:

- **Keep:** Powered Runes, the Rune Charge, every `Throw_*` ability, the
  runewords as throwables, mines and constructs left where the rune lands, the
  bag passives. The Circles stay on the skill bar only because they make runes,
  exactly as in Runecaster; they cast nothing.
- **Off-concept, to remove or move out of the base mod:** the eight hotbar
  spells (`Cast Rune Bolt`, `Frost Rune`, `Spark Rune`, `Ember Rune`,
  `Warding Rune`, `Binding Rune`, `Venom Rune`, `Rune Burst`), their eight
  Spell Tablets, the 12 Etch amplifiers that exist only to socket those spells,
  `Recall Echoes` (a hotbar nova), and the player-cast payload layer
  (`Cascade Discharge`, `Collapse Implosion`, `Collapse Aftershock`,
  `Ward Mend` with their logic passives), which fires spells from the player
  rather than from a landed rune. The Rune Glyphs the throws leave behind fit
  the concept and stay.
- **Then make the throwables faithful:** Dredmor's recipes and effects (3.4,
  3.5), exact one-of-four conjuring (3.3), and the 26 extra runewords either
  trimmed to a Dredmor-sized list or kept behind a registry flag (3.6).

Items marked (A) below described the rejected "runes as inert reagents" option
and no longer apply; they are left in place only so the reasoning is visible.

### 3.3 Circles and Mass Circles (native + extender)

1. **Native now:** remove the Rune Roller summon from `Draw X Circle` and make
   the four Resonance rows 25% each (`approximate` one-of-four: expected one
   rune, 0 to 4 possible). The toolkit validator accepts it; it is strictly
   better defined than roller + 15% rows. Keep `Runic Focus` or drop it (Dredmor
   gives nothing per cast).
2. **Extender:** attach an exact `choose` over the four `create_item`s to each
   `Draw X Circle` (compiled below as `Draw_First_Circle`): exactly one rune,
   Dredmor's `spawnitemfromlist`. Mass circles: `repeat 3` of the same `choose`
   (Dredmor's template 09 gives several). Then delete the five Rune Roller
   entities, the five Resonance and three Mass Resonance passives, and the 20
   `Conjure_*` abilities are only needed if `Scribe's Habit` stays.
3. **Owner's native workaround (2026-11-07): what the mod already does.** For
   the build without the extender the mechanism stays as it is: the throwable
   item carries a `usable.ability` (the Reactive Crafting pattern), the throw
   ability resolves where the item lands, and a one-turn entity left there
   (`temporary: 1.0`, one spell on cooldown 0) is a constant per-turn trigger
   that lives one turn, so it fires exactly once. The same shape makes the
   Rune Roller a native one-of-four for the circles. No data change is needed;
   only the in-game checks remain: (a) a one-turn temporary gets its turn before
   it expires; (b) the AI casts a zero-cost self-area ability with no enemy in
   sight (Sanctuary Totem already assumes this); (c) with four equal Conjures it
   picks at random rather than the first listed. Keep the four Conjures at
   identical cost and cooldown, cast `Draw First Circle` twenty times and count
   runes per cast and per kind. The extender `choose` in item 2 is only the
   fallback if (c) fails.
4. Costs: scale stamina to Dredmor's curve (circles 10/15/25/40/50 → e.g.
   4/6/8/10/12 stamina; mass 20/40/60 → 8/12/16) and drop the cooldowns, which
   Dredmor does not have; the limiter is bag space, not time.

### 3.4 Runeword recipes and levels (native, registry only)

Set `runewords[].ing` to the lathe inputs and spread Dredmor's 2/4/6/8/10 item
levels over the circles that supply the parts:

| Runeword | Recipe (Dredmor) | Suggested level |
| --- | --- | --- |
| Force | Strength+Movement | 3 |
| Health | Strength+Life | 5 |
| Instability | Life+Movement | 7 |
| Swiftness | Self+Movement | 9 |
| Stone | Matter+Strength+Life | 10 |
| Storms | Energy+Strength+Movement | 12 |
| Ice | Matter+Water | 13 |
| Flames | Fire+Energy | 14 |
| Teleportation | Spirit+Movement+Self | 17 |
| Blossom | Life+Water+Harvest | 18 |
| Aurification | Life+Transformation+Matter+Harvest | 20 |
| Decay | Life+Transformation+Water+Spirit | 22 |
| Steel | Rigidity+Fire+Strength+Matter | 24 |
| Light | Purity+Fire+Energy+Heavens | 27 |
| Death | Rigidity+Life+Spirit+Destiny | 29 |
| Restoration | Transformation+Life+Spirit+Purity | 31 |
| Prophecy | Heavens+Wisdom+Self+Destiny | 33 |
| Love | Harmony+Purity+Water+Life | 40 |
| Power | Ambition+Spirit+Transformation+Self | 42 |
| Destruction | Gods+Destiny+Heavens+Fire | 45 |
| Entropy | Chaos+Purity+Transformation+Energy | 48 |

Four-input recipes are plain `craftable.resources` rows; nothing in the engine
limits inputs to three. The registry's `runewords` row format already carries
the ingredient list, so this is data only. Loadout (A): 4 Runeword Force, no
charges, and a cloth chest piece standing in for the Coat of Himonya.

### 3.5 Runeword effects (per word; N = native, E = extender)

- **Swiftness** (N): leave a 60-turn `Swiftness Glyph` caster whose `aoe_self`
  pulse (cooldown 1, range 0, r1) applies a 2-turn attack/move speed buff, plus
  a long self buff on throw. That is the closest "only while you stand on it"
  the engine allows; an exact version needs an `on turn` + proximity predicate
  the extender does not have (listed in 3.1).
- **Teleportation** (N): replace `trade_places` with `lunge` to the target tile
  (as `Throw_Movement` already does) and add a short self buff (`dodge`/
  `resistance`) for "Stop Hitting Yourself". `target: aoe_tile`.
- **Light** (N/E): split into a `Divine Smite` burst with
  `restrict_tags_enemy` Demon/Undead (native, encoding unverified) or an
  extender `when: {target_tag: ["Demon", "Undead", "Skeleton", "Zombie", "Ghoul"]}`,
  a player-only `Divine Power` self buff (`resistance`, `sight`) on throw, and
  a Divine Glyph that pulses `aoe_self` heal to allies only (drop the enemy heal).
- **Death** (E, compiled below): keep the death blast; add `when target_tag`
  undead → heal; add `delay 3` → `Death_Price` self debuff (−2 strength and
  willpower, −5 magic power, 12 turns). Dredmor's 350 flat damage is
  "kill anything alive"; Soulash has no kill primitive, so scale to the top of
  the 5th circle instead (e.g. 60–90 death, `ignore_party`).
- **Love** (N): `corrupt` duration 5 → 24, remove the heal (it heals the
  charmed), add `vulnerable` for "Unresistable".
- **Blossom** (E, compiled below): `repeat 3` × `choose` over vanilla food
  `create_item`s, plus `fear` (confuse has no counterpart). Drop the heal mines.
- **Aurification** (N): no midas exists. Closest honest version: a mine whose
  burst does `damage_type: true` plus `vulnerable`, and `on_kill_create_item`
  of a gold item on the player while the Auric Mine stands cannot be expressed
  either; document it as the one runeword that cannot be ported.
- **Decay** (N): add `summon` of a vanilla insect swarm entity (Maggotsmine's
  thaumites) to the throw, `summon_count 3`, next to the mines.
- **Steel** (N): rebuild the Steel Statue as a walking summon: clone a vanilla
  golem (`clone-entity --source 2893` in the toolkit), level 9 stats, crushing
  18, armour 10, resists hyperborean/putrefying/conflagratory/toxic, a 33%
  `on_attack_cast` knockback ability for Fist of Steel, no `temporary`. Keep the
  Steel Ring as the throw's own blast.
- **Prophecy** (E, compiled below): `repeat 7, every 2` casting
  `Prophecy_Beacon_Ring` from the player at the target; retire the beacon
  construct so the blasts scale with the player, as in Dredmor.
- **Entropy** (E, compiled below): attach one exact weighted `choose` to the
  vortex pulse (one burst per pulse, weights = Dredmor's percents) and grow the
  table back toward 28 entries (add a heal, a displace stand-in `knockback 4`,
  a swarm summon). Native fallback: keep the independent rows but cap the sum
  near 1.0.
- **Power** (N): duration 10 → 30, `critical_hit_chance` and `hit_chance`
  instead of a flat +50% damage (Dredmor: +100 crit, +10 melee, +20 magic power).
- **Restoration / Health / Force / Ice / Storms / Stone / Flames /
  Destruction / Instability**: already faithful after B1/B2; only the recipe
  change applies.

### 3.6 Trim or move

Remove per the 3.2 decision: hotbar `Cast_*` spells, Spell Tablets, Etches,
`Recall Echoes`, the player-cast payloads and their logic passives (`Cascade
Logic`, `Collapse Sequence`, `Bastion Logic`, `Chain Reaction`), and with them
the Cascade Node, Collapse Seed and Ward Stone entities and the Cascade,
Collapse and Bastion runewords that only exist to feed that layer. Keep behind a
registry flag (`"dredmor_only": true` skipping those sections in
`build_runic.py`): fused runes, the remaining extra runewords, attunements,
Rune Echoes and `Runic Momentum`. The registry already has on/off switches for
rollers and nodes, so this is the same mechanism. Bag passives stay: they are the Soulash
answer to "say goodbye to your backpack space" and the owner's stated design.

### 3.7 The extender rule file (validated, not installed)

Written as `runic-magic-proposal-effects.json` in the session group's shared
notes folder (outside both the repo and the game folder) and compiled with
`tools/effects_compile.py --mod <mod>`: 8 spells and triggers, 0 errors,
0 warnings. Contents: exact one-of-four for
`Draw First Circle`; `repeat 3` × one-of-four for `Mass First Circle`; seven
delayed Prophecy blasts on `Throw Runeword Prophecy`; undead heal + delayed
Soul Price on `Throw Runeword Death`; a weighted ten-entry table on
`Entropy Vortex Ring`; food spawning on `Throw Runeword Blossom`; and a
per-turn Swiftness pulse trigger that documents the missing proximity
predicate. Every node that matters is evaluator-owned, so none of it fires until
the Triggers plugin runs with `Mode=active`; the native attach roots are
harmless no-ops before that. It should be installed as
`<mod>\extender\effects.json` only after the plugin's first active run looks
right in the log, per `docs/triggers.md` section 6.

### 3.8 Untested: amplifier slots on the item-cast throw abilities

Owner's suggestion (2026-11-07): the `Throw_*` abilities could carry `slots`
(`offensive` / `utility`) so Etch amplifiers socket into the thrown runes
themselves, which would put the removed augment layer back where the concept
wants it, on the thrown spell rather than on a hotbar spell. Item-cast
abilities do appear in the skill UI like any other ability (owner, 2026-11-07),
so the socket has a place to live; the only unknown is whether a slot set on
such an ability sticks and whether it bugs the ability. The README previously
asserted it cannot work; the owner now treats it as open. Cheapest
test: give `Throw Runeword Force` `slots: {offensive: 1, utility: 0}`, restore
one Etch (`Deep Etch`, +15% damage) from the pre-removal backup with a
milestone, and look for the socket in game and for the bonus in the hit. If it
sticks, the twelve Etches come back as runeword augments; if not, the idea is
closed and nothing else changes.

### 3.9 Verification order in game

1. Does a Mine Force Burst fire now that the bow requirement is gone (B1)? One
   Runeword Force throw next to a target dummy, watch the log for the burst.
2. Does a Health Mine heal the player standing next to it (B2)?
3. Cast `Draw First Circle` ten times and count runes per cast (1.3 item 1).
4. Flip `Mode=active` on the Triggers plugin and test `Draw_First_Circle` with
   the rule file: exactly one rune per cast.
