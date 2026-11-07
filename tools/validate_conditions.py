"""Validate a skill-conditions file (spec 1, docs/skill-conditions.md).

  python -I tools/validate_conditions.py <conditions.json> [--mod <mod folder>] [--emit <folder>]
  python -I tools/validate_conditions.py --self-test

Conditions gate a passive or ability; triggers are conditional triggers (an
ability fires on a cast or an attack only while a condition holds). --emit
writes generated_passives.json (one passive per trigger, with the engine's own
trigger effect and any native restrict_* keys the condition allows) and
generated_conditions.json (the remaining gating conditions, spec 1) into the
folder, ready to merge into the mod.

Checks structure, predicate names and arguments, names against the game's own
lists, size limits and `requires`. With --mod it also checks that each target
exists in that mod's passives.json or abilities folder. It reports, per
condition, which leaves are native-key hoistable. Standard library only; it
never reads the game executable. Exit code 0 when there are no errors.
"""
import json
import os
import re
import sys

MAX_NODES = 64
MAX_DEPTH = 8
MAX_LIST = 32
MAX_STRING = 64

# Lists from core_2/messages.json (build 0.10.3.4). Matched case-insensitively.
WEAPON_TYPES = ["Axe", "Sword", "Mace", "Dagger", "Polearm", "Bow", "Unarmed", "Crossbow",
                "Improvised", "Natural", "Throwing"]  # "Any" is not a weapon class
TAGS = ["Goblin", "Human", "Animal", "Magic", "Undead", "Player", "Dwarf", "Elf", "Owlman",
        "Alive", "Troglodyte", "Troll", "Orc", "Fishman", "Demon", "Satyr", "Centaur",
        "Mutant", "Insectoid", "Dragon", "Skeleton", "Ghoul", "Zombie"]
DAY_PARTS = ["Midnight", "After Midnight", "Small Hours", "Early Morning", "Morning", "Midday",
             "Afternoon", "Late Afternoon", "Evening", "Late Evening", "Night", "Late Night"]
SEASONS = ["Spring", "Summer", "Autumn", "Winter"]
BIOMES = ["Shallow Ocean", "Ocean", "Deep Ocean", "Desert", "Savanna", "Tropical Rainforest",
          "Grasslands", "Forest", "Swamp", "Hills", "Mountains", "High Mountains", "Peaks",
          "Boreal Forest", "Tundra", "Snow"]
ITEM_TYPES = ["Any", "Head Armor", "Head Cloth", "Amulet", "Torso Armor", "Torso Cloth", "Belt",
              "Legs Armor", "Legs Cloth", "Boots", "Cape", "Gloves", "Ring", "Weapon", "Shield",
              "Arrow", "Resource", "Usable", "Corpse", "Fish", "Food", "Bolt", "Bag",
              "Light Source", "Seed", "Note", "Golem Head", "Golem Torso", "Golem Legs",
              "Golem Hand", "Quiver", "Soul", "Meat Head", "Meat Legs", "Meat Hand"]
EQ_SLOTS = ["backpack", "head_armor", "head_cloth", "amulet", "cape", "ammo", "torso_armor",
            "torso_cloth", "left_hand", "right_hand", "gloves", "ring1", "ring2", "belt"]
ABILITY_REQUIREMENT_IDS = {"axe": 1, "sword": 2, "mace": 3, "dagger": 4, "polearm": 5,
                           "unarmed": 8, "improvised": 9, "natural": 10, "bow": 11,
                           "crossbow": 12, "throwing": 13}


class Report:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, where, message):
        self.errors.append("%s: %s" % (where, message))

    def warn(self, where, message):
        self.warnings.append("%s: %s" % (where, message))


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def name_list(value, universe, where, report, label):
    """Accepts a name or a list of names; returns the lower-cased list or None."""
    items = value if isinstance(value, list) else [value]
    if not items or len(items) > MAX_LIST:
        report.error(where, "%s needs 1..%d names" % (label, MAX_LIST))
        return None
    known = {u.lower() for u in universe}
    out = []
    for item in items:
        if not isinstance(item, str) or len(item) > MAX_STRING:
            report.error(where, "%s entries must be strings of at most %d characters" % (label, MAX_STRING))
            return None
        if item.lower() not in known:
            report.error(where, "unknown %s %r" % (label, item))
            return None
        out.append(item.lower())
    return out


def check_range(value, where, report, percent, allowed=("min", "max")):
    if not isinstance(value, dict) or not value:
        report.error(where, "expects an object with min and/or max")
        return False
    extra = set(value) - set(allowed)
    if extra:
        report.error(where, "unknown keys %s" % sorted(extra))
        return False
    for key in ("min", "max"):
        if key in value:
            if not is_number(value[key]):
                report.error(where, "%s must be a number" % key)
                return False
            if percent and not 0 <= value[key] <= 100:
                report.error(where, "%s must be within 0..100" % key)
                return False
    if "min" in value and "max" in value and value["min"] > value["max"]:
        report.error(where, "min exceeds max")
        return False
    if "min" not in value and "max" not in value:
        report.error(where, "needs min and/or max")
        return False
    return True


def id_string(value, where, report, label):
    if not isinstance(value, str) or not value or len(value) > 128:
        report.error(where, "%s must be a non-empty string of at most 128 characters" % label)
        return False
    return True


# Predicate table: name -> (validator, native kinds).
def p_weapon_class(args, where, report):
    names = name_list(args, WEAPON_TYPES, where, report, "weapon class")
    return names is not None


def p_bool(args, where, report):
    if not isinstance(args, bool):
        report.error(where, "expects true or false")
        return False
    return True


def p_tag(args, where, report):
    return name_list(args, TAGS, where, report, "tag") is not None


def p_party(args, where, report):
    if args not in ("alone", "in_party"):
        report.error(where, 'expects "alone" or "in_party"')
        return False
    return True


def p_percent(args, where, report):
    return check_range(args, where, report, True)


def p_count(args, where, report):
    return check_range(args, where, report, False)


def p_ranged_id(key):
    def check(args, where, report):
        if not isinstance(args, dict) or key not in args:
            report.error(where, "expects an object with %r" % key)
            return False
        if not id_string(args[key], where, report, key):
            return False
        rest = {k: v for k, v in args.items() if k != key}
        if not rest and key != "id":
            return True
        if rest and not check_range(rest, where, report, False):
            return False
        return True
    return check


def p_equipped(args, where, report):
    if not isinstance(args, dict) or "slot" not in args:
        report.error(where, "expects an object with 'slot'")
        return False
    if str(args["slot"]).lower() not in EQ_SLOTS:
        report.error(where, "unknown equipment slot %r" % (args["slot"],))
        return False
    selectors = [k for k in args if k != "slot"]
    if len(selectors) != 1 or selectors[0] not in ("empty", "item", "item_type", "item_tag"):
        report.error(where, "needs exactly one of empty, item, item_type, item_tag")
        return False
    key, value = selectors[0], args[selectors[0]]
    if key == "empty":
        return p_bool(value, where, report)
    if key == "item_type":
        return name_list(value, ITEM_TYPES, where, report, "item type") is not None
    return id_string(value, where, report, key)


def p_day_part(args, where, report):
    return name_list(args, DAY_PARTS, where, report, "day part") is not None


def p_season(args, where, report):
    return name_list(args, SEASONS, where, report, "season") is not None


def p_biome(args, where, report):
    return name_list(args, BIOMES, where, report, "biome") is not None


PREDICATES = {
    "weapon_class": (p_weapon_class, {"passive", "ability"}),
    "shield": (p_bool, {"passive", "ability"}),
    "holder_tag": (p_tag, {"passive"}),
    "target_tag": (p_tag, {"passive"}),
    "party": (p_party, {"passive"}),
    "health_percent": (p_percent, set()),
    "stamina_percent": (p_percent, set()),
    "target_health_percent": (p_percent, set()),
    "stacker": (p_ranged_id("id"), set()),
    "equipped": (p_equipped, set()),
    "inventory_count": (p_ranged_id("item"), set()),
    "skill_level": (p_ranged_id("skill"), set()),
    "time_of_day": (p_day_part, set()),
    "season": (p_season, set()),
    "biome": (p_biome, set()),
    "in_combat": (p_bool, set()),
    "in_settlement": (p_bool, set()),
    "companions": (p_count, set()),
}


def walk(expr, where, depth, state, report, leaves):
    state["nodes"] += 1
    if state["nodes"] > MAX_NODES:
        if not state.get("size_reported"):
            report.error(where, "more than %d nodes" % MAX_NODES)
            state["size_reported"] = True
        return
    if depth > MAX_DEPTH:
        report.error(where, "deeper than %d levels" % MAX_DEPTH)
        return
    if not isinstance(expr, dict) or len(expr) != 1:
        report.error(where, "an expression is an object with exactly one key")
        return
    key, value = next(iter(expr.items()))
    if key in ("all", "any"):
        if not isinstance(value, list) or not value or len(value) > MAX_LIST:
            report.error(where, "%r needs a list of 1..%d expressions" % (key, MAX_LIST))
            return
        for index, item in enumerate(value):
            walk(item, "%s.%s[%d]" % (where, key, index), depth + 1, state, report, leaves)
    elif key == "not":
        walk(value, where + ".not", depth + 1, state, report, leaves)
    elif key in PREDICATES:
        checker, _native = PREDICATES[key]
        if checker(value, "%s.%s" % (where, key), report):
            leaves.append((key, value))
    else:
        report.error(where, "unknown predicate %r" % key)


def hoistable(expr, kind):
    """Leaves that are necessary conditions (top-level all, or a single leaf)
    and have a native key for `kind`."""
    top = list(expr.items())[0]
    if top[0] == "all":
        candidates = [item for item in top[1] if len(item) == 1 and next(iter(item)) in PREDICATES]
    elif top[0] in PREDICATES:
        candidates = [expr]
    else:
        return []
    out = []
    for leaf in candidates:
        name = next(iter(leaf))
        if kind in PREDICATES[name][1]:
            out.append(name)
    return out


def used_predicates(expr, acc):
    if isinstance(expr, dict) and len(expr) == 1:
        key, value = next(iter(expr.items()))
        if key in ("all", "any"):
            for item in value if isinstance(value, list) else []:
                used_predicates(item, acc)
        elif key == "not":
            used_predicates(value, acc)
        else:
            acc.add(key)


TRIGGER_KEYS = {"id", "on", "cast", "chance", "when", "on_unavailable", "note"}


def trigger_passive_id(trigger_id):
    return "ext_trigger_" + trigger_id


def native_keys_for_passive(expr):
    """Native restrict_* keys implied by necessary leaves, plus the leaves that
    remain for an evaluator. Only top-level `all` (or a single leaf) is hoisted."""
    keys = {}
    if not isinstance(expr, dict) or len(expr) != 1:
        return keys, expr
    name, value = next(iter(expr.items()))
    leaves = value if name == "all" else [expr]
    remaining = []
    for leaf in leaves:
        leaf_name, args = next(iter(leaf.items()))
        if leaf_name == "weapon_class" and "restrict_weapon_type" not in keys \
                and (isinstance(args, str) or (isinstance(args, list) and len(args) == 1)):
            keys["restrict_weapon_type"] = (args if isinstance(args, str) else args[0]).lower()
        elif leaf_name == "holder_tag" and "restrict_tags" not in keys:
            names = args if isinstance(args, list) else [args]
            ids = [str([t.lower() for t in TAGS].index(n.lower())) for n in names]
            keys["restrict_tags"] = ids[0] if len(ids) == 1 else ids
        elif leaf_name == "party" and "apply_mode" not in keys:
            keys["apply_mode"] = "solo_only" if args == "alone" else "party_only"
        else:
            remaining.append(leaf)
    if name != "all" and not remaining:
        return keys, None
    if not remaining:
        return keys, None
    return keys, (remaining[0] if len(remaining) == 1 else {"all": remaining})


def compile_trigger(trigger):
    """Returns (passive, condition-or-None) for a validated trigger."""
    chance = trigger.get("chance", 1)
    on = trigger["on"]
    if "cast" in on:
        effect = {"effect": "trigger_ability_on_cast", "value": on["cast"],
                  "secondary_value": trigger["cast"], "third_value": chance}
    else:
        # on_attack_cast takes its chance in secondary_value (core_2 messages.json id 100;
        # every installed mod encodes it that way), unlike trigger_ability_on_cast.
        effect = {"effect": "on_attack_cast", "value": trigger["cast"], "secondary_value": chance}
    keys, remaining = native_keys_for_passive(trigger["when"])
    passive = {"id": trigger_passive_id(trigger["id"]), "name": "Conditional trigger " + trigger["id"],
               "effects": [effect]}
    passive.update(keys)
    condition = None
    if remaining is not None:
        condition = {"id": trigger["id"], "target": {"kind": "passive", "id": passive["id"]},
                     "when": remaining}
        if "on_unavailable" in trigger:
            condition["on_unavailable"] = trigger["on_unavailable"]
    return passive, condition


def validate_trigger(trigger, where, report, seen_ids, known_targets, used):
    if not isinstance(trigger, dict):
        report.error(where, "must be an object")
        return None
    extra = set(trigger) - TRIGGER_KEYS
    if extra:
        report.error(where, "unknown keys %s" % sorted(extra))
    tid = trigger.get("id")
    if not isinstance(tid, str) or not re.fullmatch(r"[a-z0-9_]{1,64}", tid):
        report.error(where, "id must match [a-z0-9_]{1,64}")
        return None
    if tid in seen_ids:
        report.error(where, "duplicate id %r" % tid)
        return None
    seen_ids.add(tid)
    where = "%s(%s)" % (where, tid)

    on = trigger.get("on")
    if (not isinstance(on, dict) or len(on) != 1
            or next(iter(on)) not in ("cast", "attack")):
        report.error(where, "on must be {cast: <ability id>} or {attack: true}")
        return None
    if "cast" in on:
        if not id_string(on["cast"], where, report, "on.cast"):
            return None
        if known_targets is not None and on["cast"] not in known_targets["ability"]:
            report.warn(where, "on.cast %r is not an ability of this mod (fine if it is another mod's)" % on["cast"])
    elif on["attack"] is not True:
        report.error(where, "on.attack must be true")
        return None
    if not id_string(trigger.get("cast"), where, report, "cast"):
        return None
    if known_targets is not None and trigger["cast"] not in known_targets["ability"]:
        report.warn(where, "cast %r is not an ability of this mod (fine if it is another mod's)" % trigger["cast"])
    chance = trigger.get("chance", 1)
    if not is_number(chance) or not 0 < chance <= 1:
        report.error(where, "chance must be a number above 0 and at most 1")
        return None
    if trigger.get("on_unavailable", "inactive") not in ("inactive", "active"):
        report.error(where, "on_unavailable must be inactive or active")
        return None
    expr = trigger.get("when")
    if expr is None:
        report.error(where, "missing 'when' (a trigger without a condition needs no extender)")
        return None
    leaves = []
    walk(expr, where + ".when", 1, {"nodes": 0}, report, leaves)
    names = set()
    used_predicates(expr, names)
    used.update(names)
    return trigger


def mod_target_ids(mod_folder):
    ids = {"passive": set(), "ability": set()}
    passives = os.path.join(mod_folder, "passives.json")
    if os.path.isfile(passives):
        for item in json.load(open(passives, encoding="utf-8-sig")):
            if isinstance(item, dict) and "id" in item:
                ids["passive"].add(item["id"])
    abilities = os.path.join(mod_folder, "abilities")
    if os.path.isdir(abilities):
        for name in os.listdir(abilities):
            if name.lower().endswith(".json"):
                try:
                    item = json.load(open(os.path.join(abilities, name), encoding="utf-8-sig"))
                except ValueError:
                    continue
                if isinstance(item, dict) and "id" in item:
                    ids["ability"].add(item["id"])
    return ids


def validate(document, report, mod_folder=None):
    summary = []
    if not isinstance(document, dict):
        report.error("file", "top level must be an object")
        return summary
    extra = set(document) - {"spec", "requires", "conditions", "triggers"}
    if extra:
        report.error("file", "unknown keys %s" % sorted(extra))
    if document.get("spec") != 1:
        report.error("file", "spec must be 1")
    requires = document.get("requires", [])
    if not isinstance(requires, list) or any(not isinstance(r, str) for r in requires):
        report.error("file", "requires must be a list of predicate names")
        requires = []
    for name in requires:
        if name not in PREDICATES:
            report.error("file", "requires names an unknown predicate %r" % name)
    conditions = document.get("conditions", [])
    if not isinstance(conditions, list):
        report.error("file", "conditions must be a list")
        return summary
    triggers = document.get("triggers", [])
    if not isinstance(triggers, list):
        report.error("file", "triggers must be a list")
        return summary

    known_targets = mod_target_ids(mod_folder) if mod_folder else None
    seen_ids = set()
    seen_targets = set()
    used = set()
    for index, condition in enumerate(conditions):
        where = "conditions[%d]" % index
        if not isinstance(condition, dict):
            report.error(where, "must be an object")
            continue
        extra = set(condition) - {"id", "target", "when", "on_unavailable", "note"}
        if extra:
            report.error(where, "unknown keys %s" % sorted(extra))
        cid = condition.get("id")
        if not isinstance(cid, str) or not re.fullmatch(r"[a-z0-9_]{1,64}", cid):
            report.error(where, "id must match [a-z0-9_]{1,64}")
        elif cid in seen_ids:
            report.error(where, "duplicate id %r" % cid)
        else:
            seen_ids.add(cid)
            where = "%s(%s)" % (where, cid)

        target = condition.get("target")
        kind = None
        if (not isinstance(target, dict) or set(target) != {"kind", "id"}
                or target.get("kind") not in ("passive", "ability")):
            report.error(where, "target must be {kind: passive|ability, id}")
        else:
            kind = target["kind"]
            if not id_string(target["id"], where, report, "target id"):
                kind = None
            else:
                key = (kind, target["id"])
                if key in seen_targets:
                    report.error(where, "second condition for the same target")
                seen_targets.add(key)
                if known_targets is not None and target["id"] not in known_targets[kind]:
                    report.error(where, "no %s with id %r in the mod" % (kind, target["id"]))
        if condition.get("on_unavailable", "inactive") not in ("inactive", "active"):
            report.error(where, "on_unavailable must be inactive or active")

        leaves = []
        state = {"nodes": 0}
        expr = condition.get("when")
        if expr is None:
            report.error(where, "missing 'when'")
            continue
        walk(expr, where + ".when", 1, state, report, leaves)
        names = set()
        used_predicates(expr, names)
        used |= names
        if kind is not None and not report.errors:
            native = hoistable(expr, kind) if isinstance(expr, dict) and len(expr) == 1 else []
            all_names = [name for name, _ in leaves]
            evaluated = [name for name in all_names if name not in native]
            summary.append({"id": cid, "kind": kind, "native": native,
                            "needs_evaluator": evaluated,
                            "fully_native": not evaluated and len(native) == len(all_names)})
            if (kind == "ability" and "weapon_class" in native
                    and isinstance(expr, dict)):
                pass
    for index, trigger in enumerate(triggers):
        checked = validate_trigger(trigger, "triggers[%d]" % index, report, seen_ids,
                                   known_targets, used)
        if checked is not None and not report.errors:
            passive, condition = compile_trigger(checked)
            evaluated = []
            if condition is not None:
                leaves = []
                walk(condition["when"], "x", 1, {"nodes": 0}, Report(), leaves)
                evaluated = [name for name, _ in leaves]
            summary.append({"id": checked["id"], "kind": "trigger",
                            "native": sorted(k for k in passive if k.startswith(("restrict_", "apply_"))),
                            "needs_evaluator": evaluated,
                            "fully_native": condition is None})
    missing = sorted(used - set(requires) - {n for n in used if n not in PREDICATES})
    if missing:
        report.warn("file", "predicates used but not listed in requires: %s" % missing)
    return summary


def emit(document, folder):
    os.makedirs(folder, exist_ok=True)
    passives, conditions = [], list(document.get("conditions", []))
    for trigger in document.get("triggers", []):
        passive, condition = compile_trigger(trigger)
        passives.append(passive)
        if condition is not None:
            conditions.append(condition)
    with open(os.path.join(folder, "generated_passives.json"), "w", encoding="utf-8") as handle:
        json.dump(passives, handle, indent=2)
        handle.write("\n")
    requires = sorted({name for c in conditions for name in _leaf_names(c["when"])})
    with open(os.path.join(folder, "generated_conditions.json"), "w", encoding="utf-8") as handle:
        json.dump({"spec": 1, "requires": requires, "conditions": conditions}, handle, indent=2)
        handle.write("\n")
    return len(passives), len(conditions)


def _leaf_names(expr):
    names = set()
    used_predicates(expr, names)
    return names


def run(path, mod_folder, emit_folder=None):
    report = Report()
    try:
        document = json.load(open(path, encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        print("ERROR: cannot read %s: %s" % (path, error))
        return 1
    summary = validate(document, report, mod_folder)
    for entry in summary:
        print("%-36s %-8s native=%s evaluator=%s%s" % (
            entry["id"], entry["kind"], entry["native"] or "-", entry["needs_evaluator"] or "-",
            "  (fully native)" if entry["fully_native"] else ""))
    for message in report.warnings:
        print("WARNING: " + message)
    for message in report.errors:
        print("ERROR: " + message)
    print("%d condition(s)/trigger(s), %d error(s), %d warning(s)" % (
        len(summary), len(report.errors), len(report.warnings)))
    if emit_folder and not report.errors:
        generated = emit(document, emit_folder)
        print("emitted %d passive(s) and %d condition(s) to %s" % (generated[0], generated[1], emit_folder))
    return 1 if report.errors else 0


def self_test():
    def check(label, document, expect_errors, expect_substring=None):
        report = Report()
        validate(document, report)
        ok = bool(report.errors) == expect_errors
        if ok and expect_substring:
            ok = any(expect_substring in e for e in report.errors)
        if not ok:
            print("FAIL: %s -> %s" % (label, report.errors))
        return ok

    def doc(when, kind="passive", extra=None):
        condition = {"id": "c1", "target": {"kind": kind, "id": "x"}, "when": when}
        condition.update(extra or {})
        return {"spec": 1, "conditions": [condition]}

    results = [
        check("single native leaf", doc({"weapon_class": "Sword"}), False),
        check("health range", doc({"health_percent": {"max": 50}}), False),
        check("nested all/any/not", doc({"all": [{"any": [{"in_combat": True},
              {"not": {"shield": True}}]}, {"time_of_day": ["Night"]}]}), False),
        check("equipped empty", doc({"equipped": {"slot": "right_hand", "empty": True}}), False),
        check("equipped item_type", doc({"equipped": {"slot": "amulet", "item_type": "Amulet"}}), False),
        check("ability weapon class", doc({"weapon_class": ["Bow", "Crossbow"]}, "ability"), False),
        check("unknown predicate", doc({"moon_phase": "full"}), True, "unknown predicate"),
        check("two keys in a leaf", doc({"in_combat": True, "shield": True}), True, "exactly one key"),
        check("unknown weapon", doc({"weapon_class": "Spoon"}), True, "unknown weapon class"),
        check("Any is not a weapon class", doc({"weapon_class": "Any"}), True, "unknown weapon class"),
        check("percent out of range", doc({"health_percent": {"max": 150}}), True, "0..100"),
        check("min above max", doc({"health_percent": {"min": 80, "max": 20}}), True, "min exceeds max"),
        check("empty range", doc({"health_percent": {}}), True),
        check("bad slot", doc({"equipped": {"slot": "tail", "empty": True}}), True, "equipment slot"),
        check("two selectors", doc({"equipped": {"slot": "belt", "empty": True, "item": "x"}}), True,
              "exactly one"),
        check("empty all", doc({"all": []}), True, "1..32"),
        check("spec version", {"spec": 2, "conditions": []}, True, "spec must be 1"),
        check("bad on_unavailable", doc({"in_combat": True}, extra={"on_unavailable": "maybe"}), True,
              "on_unavailable"),
        check("holder_tag only for passives is accepted on passives",
              doc({"holder_tag": ["Undead", "Demon"]}), False),
    ]

    deep = {"in_combat": True}
    for _ in range(MAX_DEPTH + 1):
        deep = {"not": deep}
    results.append(check("depth limit", doc(deep), True, "deeper than"))

    wide = {"all": [{"in_combat": True}] * 30}
    wide = {"all": [wide, wide, wide]}
    results.append(check("node limit", doc(wide), True, "nodes"))

    duplicate = doc({"in_combat": True})
    duplicate["conditions"].append(dict(duplicate["conditions"][0], id="c2"))
    results.append(check("same target twice", duplicate, True, "same target"))

    # Hoisting report.
    report = Report()
    summary = validate(doc({"all": [{"weapon_class": "Axe"}, {"health_percent": {"max": 40}}]}), report)
    hoist_ok = (summary and summary[0]["native"] == ["weapon_class"]
                and summary[0]["needs_evaluator"] == ["health_percent"])
    if not hoist_ok:
        print("FAIL: hoisting report -> %s" % summary)
    results.append(bool(hoist_ok))

    report = Report()
    summary = validate(doc({"any": [{"weapon_class": "Axe"}, {"health_percent": {"max": 40}}]}), report)
    no_hoist_under_any = summary and summary[0]["native"] == []
    if not no_hoist_under_any:
        print("FAIL: leaves under any must not be hoisted -> %s" % summary)
    results.append(bool(no_hoist_under_any))

    report = Report()
    summary = validate(doc({"all": [{"weapon_class": "Sword"}, {"party": "alone"}]}), report)
    fully = summary and summary[0]["fully_native"]
    if not fully:
        print("FAIL: all-native condition should be fully native -> %s" % summary)
    results.append(bool(fully))

    def tdoc(**fields):
        trigger = {"id": "t1", "on": {"cast": "A"}, "cast": "B", "chance": 0.25,
                   "when": {"health_percent": {"max": 50}}}
        trigger.update(fields)
        return {"spec": 1, "triggers": [trigger]}

    results += [
        check("valid cast trigger", tdoc(), False),
        check("valid attack trigger", tdoc(on={"attack": True}), False),
        check("trigger without when", tdoc(when=None) if False else
              {"spec": 1, "triggers": [{"id": "t", "on": {"cast": "A"}, "cast": "B"}]}, True, "missing 'when'"),
        check("trigger bad on", tdoc(on={"kill": True}), True, "on must be"),
        check("trigger two on keys", tdoc(on={"cast": "A", "attack": True}), True, "on must be"),
        check("trigger attack false", tdoc(on={"attack": False}), True, "must be true"),
        check("trigger chance zero", tdoc(chance=0), True, "chance"),
        check("trigger chance above one", tdoc(chance=1.5), True, "chance"),
        check("trigger missing cast", {"spec": 1, "triggers": [{"id": "t", "on": {"attack": True},
              "when": {"in_combat": True}}]}, True, "cast"),
        check("trigger unknown predicate", tdoc(when={"moon": 1}), True, "unknown predicate"),
        check("trigger duplicate id with a condition",
              {"spec": 1,
               "conditions": [{"id": "t1", "target": {"kind": "passive", "id": "x"}, "when": {"in_combat": True}}],
               "triggers": [{"id": "t1", "on": {"attack": True}, "cast": "B", "when": {"in_combat": True}}]},
              True, "duplicate id"),
    ]

    # Compilation.
    passive, condition = compile_trigger({"id": "t1", "on": {"cast": "A"}, "cast": "B", "chance": 0.25,
                                          "when": {"all": [{"weapon_class": "Sword"},
                                                           {"health_percent": {"max": 50}}]}})
    compile_ok = (passive["effects"][0] == {"effect": "trigger_ability_on_cast", "value": "A",
                                            "secondary_value": "B", "third_value": 0.25}
                  and passive["restrict_weapon_type"] == "sword" and passive["id"] == "ext_trigger_t1"
                  and condition is not None and condition["when"] == {"health_percent": {"max": 50}}
                  and condition["target"] == {"kind": "passive", "id": "ext_trigger_t1"})
    if not compile_ok:
        print("FAIL: trigger compile -> %s %s" % (passive, condition))
    results.append(compile_ok)
    passive, condition = compile_trigger({"id": "t2", "on": {"attack": True}, "cast": "B",
                                          "when": {"all": [{"holder_tag": "Undead"}, {"party": "alone"}]}})
    native_ok = (passive["effects"][0] == {"effect": "on_attack_cast", "value": "B", "secondary_value": 1}
                 and passive["restrict_tags"] == "4" and passive["apply_mode"] == "solo_only"
                 and condition is None)
    if not native_ok:
        print("FAIL: fully native trigger -> %s %s" % (passive, condition))
    results.append(native_ok)
    passive, condition = compile_trigger({"id": "t3", "on": {"attack": True}, "cast": "B",
                                          "when": {"any": [{"weapon_class": "Axe"}, {"in_combat": True}]}})
    any_ok = "restrict_weapon_type" not in passive and condition is not None
    if not any_ok:
        print("FAIL: any must not be hoisted -> %s %s" % (passive, condition))
    results.append(any_ok)

    passed = all(results)
    print("PASS: skill-conditions validator self-test (%d checks)" % len(results) if passed
          else "FAIL: skill-conditions validator self-test")
    return 0 if passed else 1


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    args = argv[1:]
    mod = None
    emit_folder = None
    if "--mod" in args:
        index = args.index("--mod")
        mod = args[index + 1]
        del args[index:index + 2]
    if "--emit" in args:
        index = args.index("--emit")
        emit_folder = args[index + 1]
        del args[index:index + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    return run(args[0], mod, emit_folder)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
