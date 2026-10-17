"""Validate a skill-conditions file (spec 1, docs/skill-conditions.md).

  python -I tools/validate_conditions.py <conditions.json> [--mod <mod folder>]
  python -I tools/validate_conditions.py --self-test

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
    extra = set(document) - {"spec", "requires", "conditions"}
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
    conditions = document.get("conditions")
    if not isinstance(conditions, list):
        report.error("file", "conditions must be a list")
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
    missing = sorted(used - set(requires) - {n for n in used if n not in PREDICATES})
    if missing:
        report.warn("file", "predicates used but not listed in requires: %s" % missing)
    return summary


def run(path, mod_folder):
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
    print("%d condition(s), %d error(s), %d warning(s)" % (
        len(summary), len(report.errors), len(report.warnings)))
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

    passed = all(results)
    print("PASS: skill-conditions validator self-test (%d checks)" % len(results) if passed
          else "FAIL: skill-conditions validator self-test")
    return 0 if passed else 1


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    args = argv[1:]
    mod = None
    if "--mod" in args:
        index = args.index("--mod")
        mod = args[index + 1]
        del args[index:index + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    return run(args[0], mod)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
