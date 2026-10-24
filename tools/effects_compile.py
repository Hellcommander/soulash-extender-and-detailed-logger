"""Validate and compile an extender effects file (spec 1, docs/effects.md).

  python -I tools/effects_compile.py <effects.json> [--mod <mod folder>] [--emit <folder>]
  python -I tools/effects_compile.py --self-test

An effects file describes spells as trees of composable effect nodes (all,
chance, repeat, choose, delay, when, and typed leaves) plus event triggers.
Everything the engine can express is compiled to ordinary Soulash 2 data:
abilities, passives with the engine's own trigger_ability_on_cast /
on_attack_cast / on_kill_create_item rows, milestones that grant them, and a
spec-1 skill-conditions file for the gates that remain. Nodes the engine
cannot express (exact choice, delays, repeats over turns, conditions beyond
weapon/tag/party, per-turn and on-hit events, teleport, experience, removing
stacks) are reported as evaluator-owned: their abilities are still generated
so the extender's evaluator (src/effects.cpp) can cast them, but nothing in the
engine data points at them, so without the evaluator they never fire.

Standard library only; never reads the game executable. Exit code 0 when
there are no errors. Nothing it emits has been run in the real game.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate_conditions as cond  # noqa: E402  (same folder, trusted)

MAX_SPELLS = 256
MAX_NODES = 64
MAX_DEPTH = 8
MAX_LIST = 32
ID_PATTERN = r"[A-Za-z0-9_]{1,64}"

TARGETS = ["self", "aoe_self", "health", "actor", "tile", "aoe_tile", "aoe_target", "line", "cone",
           "line_health", "corpse", "non_actor", "line_cone", "back_and_forth", "random",
           "horizontal_line"]
# Dredmor spell types -> engine targets.
TARGET_ALIASES = {"target": "health", "targetmonster": "health", "targetfloor": "tile",
                  "targetemptyfloor": "tile", "fireball": "aoe_tile", "area": "aoe_self",
                  "beam": "line", "targetcorpse": "corpse", "missile": "line_health",
                  "adjacent": "tile"}
SHAPES = {"circle": 1, "sides": 2, "square": 3, "ring": 4}
DAMAGE_TYPES = ["physical", "fire", "frost", "electricity", "death", "poison", "holy", "acid",
                "true", "nature"]
STATUS_KEYS = {"stun": "stun", "immobilize": "immobilize", "fear": "fear", "bleed": "bleed",
               "burn": "burn", "poison": "poison_effect", "frostbite": "frostbite",
               "silence": "silence", "vulnerable": "vulnerable", "disarm": "disarm", "hide": "hide",
               "cage": "cage", "burn_stamina": "burn_stamina"}
STATS = ["strength", "dexterity", "endurance", "intelligence", "willpower"]
MODIFIERS = ["resistance", "thorns", "damage_reduce", "damage_reduce_percent", "damage_bonus",
             "damage_bonus_percent", "attack_speed", "movement_speed", "sight", "magic_power",
             "hit_chance", "parry_chance", "deflection", "light_source", "regeneration"]
# Leaves the engine cannot express at all: evaluator (or impossible) only.
EVALUATOR_LEAVES = {"remove_stacks": "remove_stacks", "teleport": "teleport", "grant_xp": "grant_xp"}
# Leaves whose sub-ability should target the caster rather than relay the root target.
SELF_LEAVES = {"buff", "stacks", "remove_stacks", "stamina", "cooldowns", "aura"}
NATIVE_LEAVES = ["damage", "heal", "stamina", "status", "knockback", "pull", "swap", "lunge",
                 "charge", "step_aside", "buff", "summon", "create_item", "stacks", "cleanse",
                 "cooldowns", "transform", "aura", "cast"]
LEAVES = set(NATIVE_LEAVES) | set(EVALUATOR_LEAVES)
COMBINATORS = {"all", "chance", "repeat", "choose", "delay", "when"}
EVENTS = ["cast", "attack", "kill", "turn", "damaged", "dodge", "block", "crit"]
NATIVE_EVENTS = {"cast", "attack"}
CAPABILITIES = ["choose", "delay", "repeat", "remove_stacks", "teleport", "grant_xp", "on_kill",
                "on_turn", "on_damaged", "on_dodge", "on_block", "on_crit"]
SPELL_KEYS = {"id", "ability", "name", "description", "image", "animation", "skill", "target",
              "radius", "shape", "range", "min_range", "cooldown", "cast_time", "cost",
              "requirements", "slots", "level", "effects", "note"}
TRIGGER_KEYS = {"id", "on", "chance", "when", "on_unavailable", "do", "level", "note"}

Report = cond.Report
is_number = cond.is_number


# ---------------------------------------------------------------------------
# Nodes
# ---------------------------------------------------------------------------
class Node:
    """One effect node. kind is a combinator name or 'leaf'; for leaves `leaf`
    is the leaf name and `args` its arguments."""

    def __init__(self, kind, path, args=None, leaf=None):
        self.kind = kind
        self.path = path            # tuple of child indices from the spell root
        self.args = args
        self.leaf = leaf
        self.children = []
        self.chance = 1.0           # chance / choose weight share applied to children
        self.count = 1              # repeat
        self.every = None           # repeat over turns
        self.delay = 0
        self.when = None            # condition expression
        self.on_unavailable = "inactive"
        self.approximate = False
        self.native = True          # set by classify()
        self.reason = ""

    def path_text(self):
        return "_".join(str(i) for i in self.path) if self.path else "root"


def _check_amount(value, where, report):
    if is_number(value):
        return value >= 0 or report.error(where, "amount must not be negative")
    if (isinstance(value, list) and len(value) == 2 and all(is_number(v) for v in value)
            and 0 <= value[0] <= value[1]):
        return True
    report.error(where, "amount must be a number or [min, max] with 0 <= min <= max")
    return False


def _check_object(value, where, report, required=(), allowed=()):
    if not isinstance(value, dict):
        report.error(where, "expects an object")
        return False
    extra = set(value) - set(allowed) - set(required)
    if extra:
        report.error(where, "unknown keys %s" % sorted(extra))
        return False
    for key in required:
        if key not in value:
            report.error(where, "missing %r" % key)
            return False
    return True


def _check_id(value, where, report, label):
    return cond.id_string(value, where, report, label)


def _check_leaf(name, args, where, report):
    """Type-checks one leaf's arguments. Returns True when well-formed."""
    if name == "damage":
        if not _check_object(args, where, report, ("amount", "type"),
                             ("scaling", "weapon", "leech", "pierce", "chain", "ignore_party")):
            return False
        ok = _check_amount(args["amount"], where, report)
        if not isinstance(args["type"], str) or args["type"].lower() not in DAMAGE_TYPES:
            report.error(where, "unknown damage type %r (one of %s)" % (args["type"], DAMAGE_TYPES))
            ok = False
        for key in ("scaling", "weapon", "leech"):
            if key in args and (not is_number(args[key]) or args[key] < 0):
                report.error(where, "%s must be a non-negative number" % key)
                ok = False
        if "leech" in args and is_number(args["leech"]) and args["leech"] > 1:
            report.error(where, "leech is a fraction 0..1")
            ok = False
        for key in ("pierce", "ignore_party"):
            if key in args and not isinstance(args[key], bool):
                report.error(where, "%s must be true or false" % key)
                ok = False
        if "chain" in args and (not isinstance(args["chain"], int) or args["chain"] < 1):
            report.error(where, "chain must be a positive integer")
            ok = False
        return ok
    if name == "heal":
        return _check_object(args, where, report, ("amount",)) and _check_amount(args["amount"], where, report)
    if name == "stamina" or name == "grant_xp":
        if not is_number(args) or (name == "grant_xp" and args <= 0):
            report.error(where, "expects a number")
            return False
        return True
    if name == "status":
        if not isinstance(args, dict) or not args:
            report.error(where, "expects an object of status name -> value")
            return False
        ok = True
        for key, value in args.items():
            if key not in STATUS_KEYS:
                report.error(where, "unknown status %r (one of %s)" % (key, sorted(STATUS_KEYS)))
                ok = False
            elif not is_number(value) or value < 0:
                report.error(where, "status %s must be a non-negative number" % key)
                ok = False
        return ok
    if name in ("knockback", "pull"):
        if not isinstance(args, int) or isinstance(args, bool) or args < 1:
            report.error(where, "expects a positive integer (tiles)")
            return False
        return True
    if name in ("swap", "lunge", "charge", "step_aside"):
        if args is not True:
            report.error(where, "expects true")
            return False
        return True
    if name == "buff":
        if not _check_object(args, where, report, ("duration",), ("stats", "modifiers")):
            return False
        ok = True
        if not is_number(args["duration"]) or args["duration"] <= 0:
            report.error(where, "duration must be a positive number of turns")
            ok = False
        for group, universe in (("stats", STATS), ("modifiers", MODIFIERS)):
            if group in args:
                if not isinstance(args[group], dict) or not args[group]:
                    report.error(where, "%s expects a non-empty object" % group)
                    ok = False
                    continue
                for key, value in args[group].items():
                    if key not in universe:
                        report.error(where, "unknown %s key %r (one of %s)" % (group[:-1], key, universe))
                        ok = False
                    elif not is_number(value):
                        report.error(where, "%s.%s must be a number" % (group, key))
                        ok = False
        if "stats" not in args and "modifiers" not in args:
            report.error(where, "buff needs stats and/or modifiers")
            ok = False
        return ok
    if name == "summon":
        if not _check_object(args, where, report, ("entity",), ("count", "on_move")):
            return False
        ok = _check_id(args["entity"], where, report, "entity")
        if "count" in args and (not isinstance(args["count"], int) or args["count"] < 1):
            report.error(where, "count must be a positive integer")
            ok = False
        if "on_move" in args and not isinstance(args["on_move"], bool):
            report.error(where, "on_move must be true or false")
            ok = False
        return ok
    if name == "create_item":
        if not _check_object(args, where, report, ("item",), ("count",)):
            return False
        ok = _check_id(args["item"], where, report, "item")
        if "count" in args and (not isinstance(args["count"], int) or args["count"] < 1):
            report.error(where, "count must be a positive integer")
            ok = False
        return ok
    if name in ("stacks", "remove_stacks"):
        if not _check_object(args, where, report, ("stacker",), ("count",)):
            return False
        ok = _check_id(args["stacker"], where, report, "stacker")
        minimum = 0 if name == "remove_stacks" else 1
        if "count" in args and (not isinstance(args["count"], int) or args["count"] < minimum):
            report.error(where, "count must be an integer >= %d" % minimum)
            ok = False
        return ok
    if name == "cleanse":
        if not isinstance(args, list) or not args or len(args) > MAX_LIST \
                or any(not isinstance(a, str) or not a for a in args):
            report.error(where, "expects a list of effect names")
            return False
        return True
    if name == "cooldowns":
        return (_check_object(args, where, report, ("clear",))
                and (isinstance(args["clear"], int) and args["clear"] >= 1
                     or report.error(where, "clear must be a positive integer")))
    if name == "transform":
        return _check_object(args, where, report, ("entity",)) and _check_id(args["entity"], where, report, "entity")
    if name == "aura":
        return _check_object(args, where, report, ("ability",)) and _check_id(args["ability"], where, report, "ability")
    if name == "cast":
        if not _check_object(args, where, report, ("spell",), ("chance", "delay", "when")):
            return False
        ok = _check_id(args["spell"], where, report, "spell")
        if "chance" in args and (not is_number(args["chance"]) or not 0 < args["chance"] <= 1):
            report.error(where, "chance must be above 0 and at most 1")
            ok = False
        if "delay" in args and (not isinstance(args["delay"], int) or not 1 <= args["delay"] <= 1000):
            report.error(where, "delay must be an integer 1..1000 (turns)")
            ok = False
        return ok
    if name == "teleport":
        if args != "target":
            report.error(where, 'expects "target"')
            return False
        return True
    report.error(where, "unknown leaf %r" % name)
    return False


def build_node(expr, path, where, depth, state, report):
    """Validates one effect node and returns a Node (or None on error)."""
    state["nodes"] += 1
    if state["nodes"] > MAX_NODES:
        if not state.get("size_reported"):
            report.error(where, "more than %d nodes" % MAX_NODES)
            state["size_reported"] = True
        return None
    if depth > MAX_DEPTH:
        report.error(where, "deeper than %d levels" % MAX_DEPTH)
        return None
    if not isinstance(expr, dict) or not expr:
        report.error(where, "an effect is an object with one effect key")
        return None
    keys = set(expr) - {"note"}
    heads = [k for k in keys if k in COMBINATORS or k in LEAVES]
    if len(heads) != 1:
        report.error(where, "exactly one effect key expected, found %s" % sorted(keys))
        return None
    head = heads[0]
    modifiers = {"chance": {"do"}, "repeat": {"do", "every"}, "choose": {"approximate"},
                 "delay": {"do"}, "when": {"do", "on_unavailable"}, "all": set()}
    allowed = modifiers.get(head, set())
    extra = keys - {head} - allowed
    if extra:
        report.error(where, "unknown keys %s next to %r" % (sorted(extra), head))
        return None

    if head == "all":
        items = expr["all"]
        if not isinstance(items, list) or not items or len(items) > MAX_LIST:
            report.error(where, "all needs a list of 1..%d effects" % MAX_LIST)
            return None
        node = Node("all", path)
        for index, item in enumerate(items):
            child = build_node(item, path + (index,), "%s.all[%d]" % (where, index), depth + 1, state, report)
            if child is not None:
                node.children.append(child)
        return node

    if head in ("chance", "repeat", "delay", "when"):
        if "do" not in expr:
            report.error(where, "%r needs 'do'" % head)
            return None
        node = Node(head, path)
        value = expr[head]
        if head == "chance":
            if not is_number(value) or not 0 < value <= 1:
                report.error(where, "chance must be above 0 and at most 1")
                return None
            node.chance = float(value)
        elif head == "repeat":
            if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= MAX_LIST:
                report.error(where, "repeat must be an integer 1..%d" % MAX_LIST)
                return None
            node.count = value
            if "every" in expr:
                every = expr["every"]
                if not isinstance(every, int) or isinstance(every, bool) or not 1 <= every <= 1000:
                    report.error(where, "every must be an integer 1..1000 (turns)")
                    return None
                node.every = every
        elif head == "delay":
            if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= 1000:
                report.error(where, "delay must be an integer 1..1000 (turns)")
                return None
            node.delay = value
        else:
            leaves = []
            cond.walk(value, where + ".when", 1, {"nodes": 0}, report, leaves)
            node.when = value
            policy = expr.get("on_unavailable", "inactive")
            if policy not in ("inactive", "active"):
                report.error(where, "on_unavailable must be inactive or active")
                return None
            node.on_unavailable = policy
        child = build_node(expr["do"], path + (0,), where + ".do", depth + 1, state, report)
        if child is None:
            return None
        node.children.append(child)
        return node

    if head == "choose":
        options = expr["choose"]
        if not isinstance(options, list) or not options or len(options) > MAX_LIST:
            report.error(where, "choose needs a list of 1..%d options" % MAX_LIST)
            return None
        node = Node("choose", path)
        if "approximate" in expr:
            if not isinstance(expr["approximate"], bool):
                report.error(where, "approximate must be true or false")
                return None
            node.approximate = expr["approximate"]
        total = 0.0
        weights = []
        for index, option in enumerate(options):
            owhere = "%s.choose[%d]" % (where, index)
            if not _check_object(option, owhere, report, ("do",), ("weight",)):
                return None
            weight = option.get("weight", 1)
            if not is_number(weight) or weight <= 0:
                report.error(owhere, "weight must be a positive number")
                return None
            weights.append(float(weight))
            total += float(weight)
            child = build_node(option["do"], path + (index,), owhere + ".do", depth + 1, state, report)
            if child is None:
                return None
            node.children.append(child)
        for child, weight in zip(node.children, weights):
            child.chance = weight / total   # the option's share; used by approximate
        return node

    # Leaf.
    args = expr[head]
    if not _check_leaf(head, args, where + "." + head, report):
        return None
    if head == "cast" and ("chance" in args or "delay" in args or "when" in args):
        # Sugar: when(delay(chance(cast))) with the same path.
        inner = Node("leaf", path, {"spell": args["spell"]}, "cast")
        node = inner
        if "chance" in args:
            wrapper = Node("chance", path)
            wrapper.chance = float(args["chance"])
            wrapper.children.append(node)
            node = wrapper
        if "delay" in args:
            wrapper = Node("delay", path)
            wrapper.delay = args["delay"]
            wrapper.children.append(node)
            node = wrapper
        if "when" in args:
            leaves = []
            cond.walk(args["when"], where + ".cast.when", 1, {"nodes": 0}, report, leaves)
            wrapper = Node("when", path)
            wrapper.when = args["when"]
            wrapper.children.append(node)
            node = wrapper
        return node
    return Node("leaf", path, args, head)


# ---------------------------------------------------------------------------
# Classification: which backend owns a node
# ---------------------------------------------------------------------------
def fully_native_condition(expr):
    """A condition every leaf of which hoists to a native passive key. Mirrors
    validate_conditions.native_keys_for_passive: top-level all or one leaf;
    weapon_class with a single name, holder_tag, party."""
    keys, remaining = cond.native_keys_for_passive(expr)
    return remaining is None and bool(keys)


def classify(node, parent_native=True):
    """Marks node.native for the whole tree (docs/effects.md section 4.2).
    A node is native when its parent is native and it is itself expressible."""
    if not parent_native:
        node.native = False
        node.reason = "inside an evaluator-owned node"
    elif node.kind == "leaf":
        if node.leaf in EVALUATOR_LEAVES:
            node.native = False
            node.reason = "%s has no engine effect" % node.leaf
        else:
            node.native = True
    elif node.kind in ("all", "chance"):
        node.native = True
    elif node.kind == "repeat":
        node.native = node.every is None
        node.reason = "" if node.native else "repeat over turns"
    elif node.kind == "choose":
        node.native = node.approximate
        node.reason = "" if node.native else "exact one-of choice"
    elif node.kind == "delay":
        node.native = False
        node.reason = "delayed effect"
    elif node.kind == "when":
        node.native = fully_native_condition(node.when)
        node.reason = "" if node.native else "condition needs the evaluator"
    for child in node.children:
        classify(child, node.native)
    # A native combinator whose only child is evaluator-owned has nothing native
    # to do; keep it native (it emits nothing) so ownership stays simple.
    return node


def walk_nodes(node):
    yield node
    for child in node.children:
        yield from walk_nodes(child)


def required_capabilities(node):
    out = set()
    for item in walk_nodes(node):
        if item.kind == "choose" and not item.approximate:
            out.add("choose")
        elif item.kind == "delay":
            out.add("delay")
        elif item.kind == "repeat" and item.every is not None:
            out.add("repeat")
        elif item.kind == "leaf" and item.leaf in EVALUATOR_LEAVES:
            out.add(EVALUATOR_LEAVES[item.leaf])
        if item.kind == "when":
            names = set()
            cond.used_predicates(item.when, names)
            out |= names
    return out


# ---------------------------------------------------------------------------
# Compilation
# ---------------------------------------------------------------------------
def leaf_keys(leaf, args):
    """Native leaf -> (effects, effects_keys, duration)."""
    effects, keys, duration = {}, {}, None
    if leaf == "damage":
        effects["damage"] = list(args["amount"]) if isinstance(args["amount"], list) else [args["amount"], args["amount"]]
        effects["damage_type"] = args["type"].lower()
        if "scaling" in args:
            effects["magic_power_damage"] = args["scaling"]
        if "weapon" in args:
            effects["damage_weapon"] = args["weapon"]
        if "leech" in args:
            effects["damage_as_life"] = args["leech"]
        if args.get("pierce"):
            effects["pierce"] = 1
        if "chain" in args:
            effects["chain"] = args["chain"]
        if args.get("ignore_party"):
            effects["ignore_party"] = 1
    elif leaf == "heal":
        effects["heal"] = list(args["amount"]) if isinstance(args["amount"], list) else [args["amount"], args["amount"]]
    elif leaf == "stamina":
        effects["stamina"] = args
    elif leaf == "status":
        for name, value in args.items():
            effects[STATUS_KEYS[name]] = value
    elif leaf == "knockback":
        effects["knockback"] = args
    elif leaf == "pull":
        effects["pull_target"] = args
    elif leaf in ("swap", "lunge", "charge", "step_aside"):
        effects[{"swap": "trade_places", "lunge": "lunge", "charge": "charge", "step_aside": "move_to_side"}[leaf]] = 1
    elif leaf == "buff":
        duration = args["duration"]
        for group in ("stats", "modifiers"):
            for name, value in args.get(group, {}).items():
                effects[name] = value
    elif leaf == "summon":
        keys["summon"] = args["entity"]
        effects["summon_count"] = args.get("count", 1)
        if args.get("on_move"):
            keys["summon_on_move"] = args["entity"]
    elif leaf == "create_item":
        keys["summon"] = args["item"]
        effects["summon_count"] = args.get("count", 1)
    elif leaf == "stacks":
        keys["stacker"] = args["stacker"]
        effects["stacker_count"] = args.get("count", 1)
    elif leaf == "cleanse":
        effects["neutralize"] = list(args)
    elif leaf == "cooldowns":
        effects["clear_cooldowns"] = args["clear"]
    elif leaf == "transform":
        keys["transform"] = args["entity"]
    elif leaf == "aura":
        keys["aura"] = args["ability"]
    return effects, keys, duration


class Group:
    """Leaves merged into one ability."""

    def __init__(self, ability_id, path, self_target):
        self.ability_id = ability_id
        self.path = path
        self.effects = {}
        self.keys = {}
        self.duration = None
        self.self_target = self_target
        self.leaves = []

    def can_take(self, effects, keys, duration):
        if any(k in self.effects for k in effects) or any(k in self.keys for k in keys):
            return False
        if duration is not None and self.duration is not None and duration != self.duration:
            return False
        return True

    def take(self, leaf, effects, keys, duration):
        self.effects.update(effects)
        self.keys.update(keys)
        if duration is not None:
            self.duration = duration
        self.leaves.append(leaf)
        self.self_target = self.self_target and leaf in SELF_LEAVES


class SpellCompiler:
    def __init__(self, document, spell, index, report, local_spells):
        self.document = document
        self.spell = spell
        self.report = report
        self.where = "spells[%d]" % index
        self.mod = document["mod"]
        self.attached = "ability" in spell
        self.root_id = spell["ability"] if self.attached else "%s_%s" % (self.mod, spell["id"])
        self.local_spells = local_spells
        self.abilities = []        # generated ability JSON objects
        self.rows = []             # (passive_id, effect row) native trigger rows
        self.passives = {}         # passive id -> passive object
        self.conditions = []       # remaining gating conditions
        self.report_rows = []      # per-node report
        self.warnings = []

    # -- helpers ------------------------------------------------------------
    def passive_for(self, gates):
        """One chain passive per (root, gate set). Gates are hoisted native keys."""
        if not gates:
            pid = "ext_fx_" + self.root_id
        else:
            pid = "ext_fx_%s__%s" % (self.root_id, gates["__path"])
        if pid not in self.passives:
            passive = {"id": pid, "name": "Effects: %s" % (self.spell.get("name") or self.root_id),
                       "image": 0, "effects": []}
            for key, value in gates.items():
                if key != "__path":
                    passive[key] = value
            passive["description"] = "Generated by tools/effects_compile.py; grants the chained effects of %s." % self.root_id
            self.passives[pid] = passive
        return pid

    def resolve_cast(self, spell_id):
        if spell_id in self.local_spells:
            return "%s_%s" % (self.mod, spell_id)
        return spell_id

    def sub_ability(self, group):
        root = self.spell
        target = root.get("target", "self")
        target = TARGET_ALIASES.get(target, target)
        ability = {"id": group.ability_id,
                   "name": "%s (%s)" % (root.get("name") or self.root_id, group.path),
                   "description": "Generated sub-effect of %s: %s." % (self.root_id, ", ".join(group.leaves)),
                   "image": root.get("image", 0),
                   "animation": root.get("animation", ""),
                   "skill": root.get("skill", self.mod),
                   "cast_time": 0.0, "cooldown": 0.0,
                   "cost": {"health": 0, "stamina": 0},
                   "duration": group.duration if group.duration is not None else 0.0,
                   "range": 0}
        if group.self_target or target == "self":
            ability["target"] = "self"
        elif target == "aoe_self":
            ability["target"] = "aoe_self"
        else:
            ability["target"] = target
            ability["range"] = root.get("range", 0)
            self.warnings.append("%s: sub-ability %s relays the root target %r; where a triggered cast lands is unverified"
                                 % (self.where, group.ability_id, target))
        effects = dict(group.effects)
        if ability["target"].startswith("aoe") or target.startswith("aoe"):
            if "radius" in root and ability["target"] != "self":
                effects.setdefault("aoe_range", root["radius"])
                effects.setdefault("aoe_shape", SHAPES[root.get("shape", "circle")])
        ability["effects"] = effects
        if group.keys:
            ability["effects_keys"] = dict(group.keys)
        return ability

    def root_ability(self, group):
        root = self.spell
        target = root.get("target", "self")
        target = TARGET_ALIASES.get(target, target)
        effects = dict(group.effects)
        if "radius" in root:
            effects["aoe_range"] = root["radius"]
            effects["aoe_shape"] = SHAPES[root.get("shape", "circle")]
        ability = {"id": self.root_id, "name": root.get("name", self.root_id),
                   "description": root.get("description", ""), "image": root.get("image", 0),
                   "animation": root.get("animation", ""), "skill": root.get("skill", self.mod),
                   "cast_time": float(root.get("cast_time", 0)), "cooldown": float(root.get("cooldown", 0)),
                   "cost": {"health": root.get("cost", {}).get("health", 0), "stamina": root.get("cost", {}).get("stamina", 0)},
                   "duration": group.duration if group.duration is not None else float(root.get("duration", 0.0)),
                   "range": root.get("range", 0), "target": target, "effects": effects}
        if "min_range" in root:
            ability["min_range"] = root["min_range"]
        if group.keys:
            ability["effects_keys"] = dict(group.keys)
        if "requirements" in root:
            ability["requirements"] = {k: [cond.ABILITY_REQUIREMENT_IDS[n.lower()] for n in v]
                                       for k, v in root["requirements"].items()}
        if "slots" in root:
            ability["slots"] = root["slots"]
        return ability

    # -- native emission ------------------------------------------------------
    def emit_group_leaves(self, node, group_path, leaves, root_group):
        """Merges leaves into abilities. root_group is the Group that absorbs the
        first leaves (the root ability, or a fresh sub-ability). Returns groups."""
        groups = [root_group]
        splits = 0
        for leaf in leaves:
            effects, keys, duration = leaf_keys(leaf.leaf, leaf.args)
            target = None
            for group in groups:
                if group.can_take(effects, keys, duration):
                    target = group
                    break
            if target is None:
                splits += 1
                target = Group("%s__%s_x%d" % (self.root_id, group_path, splits), "%s_x%d" % (group_path, splits), True)
                groups.append(target)
            target.take(leaf.leaf, effects, keys, duration)
        return groups

    def compile_native(self, node, chance, gates, is_root):
        """Compiles a native node. Non-root groups become sub-abilities chained
        from the root ability with `chance` on the passive identified by gates."""
        if not node.native:
            self.compile_evaluator(node)
            return
        if node.kind == "leaf" and node.leaf == "cast":
            target = self.resolve_cast(node.args["spell"])
            self.add_row(gates, target, chance, node)
            return
        if node.kind == "all" or node.kind == "leaf":
            leaves = [c for c in node.children if c.kind == "leaf" and c.native and c.leaf != "cast"] \
                if node.kind == "all" else [node]
            others = [c for c in node.children if c not in leaves] if node.kind == "all" else []
            if leaves:
                if is_root and not self.attached:
                    base = Group(self.root_id, "root", False)
                    groups = self.emit_group_leaves(node, "root", leaves, base)
                    self.abilities.append(self.root_ability(groups[0]))
                    for extra in groups[1:]:
                        self.abilities.append(self.sub_ability(extra))
                        self.add_row(gates, extra.ability_id, chance, node, note="split of conflicting keys")
                else:
                    path = node.path_text() if node.path else "r"
                    base = Group("%s__%s" % (self.root_id, path), path, True)
                    groups = self.emit_group_leaves(node, path, leaves, base)
                    for group in groups:
                        self.abilities.append(self.sub_ability(group))
                        self.add_row(gates, group.ability_id, chance, node)
            elif is_root and not self.attached:
                # A root with no direct leaves still needs the ability the player casts.
                self.abilities.append(self.root_ability(Group(self.root_id, "root", False)))
            for child in others:
                self.compile_native(child, chance, gates, False)
            self.report_rows.append({"path": node.path_text(), "node": node.kind if node.kind != "leaf" else node.leaf,
                                     "backend": "native", "reason": ""})
            return
        if node.kind == "chance":
            self.report_rows.append({"path": node.path_text(), "node": "chance", "backend": "native", "reason": ""})
            self.compile_native(node.children[0], chance * node.chance, gates, False)
            return
        if node.kind == "repeat":
            self.report_rows.append({"path": node.path_text(), "node": "repeat", "backend": "native",
                                     "reason": "%d identical trigger rows (unverified that all fire)" % node.count})
            for _ in range(node.count):
                self.compile_native(node.children[0], chance, gates, False)
            return
        if node.kind == "choose":
            self.report_rows.append({"path": node.path_text(), "node": "choose", "backend": "native",
                                     "reason": "approximate: independent chances, 0..%d options fire" % len(node.children)})
            for child in node.children:
                self.compile_native(child, chance * child.chance, gates, False)
            return
        if node.kind == "when":
            keys, _remaining = cond.native_keys_for_passive(node.when)
            merged = dict(gates)
            for key, value in keys.items():
                if key in merged and merged[key] != value:
                    # Two different native gates on one path cannot both be keys of one passive.
                    node.native = False
                    node.reason = "conflicting native gates on one path"
                    for child in walk_nodes(node):
                        child.native = False
                    self.compile_evaluator(node)
                    return
                merged[key] = value
            merged["__path"] = node.path_text()
            self.report_rows.append({"path": node.path_text(), "node": "when", "backend": "native",
                                     "reason": "hoisted to %s" % sorted(k for k in keys)})
            self.compile_native(node.children[0], chance, merged, False)
            return
        raise AssertionError("unreachable node kind %r" % node.kind)

    def add_row(self, gates, target_ability, chance, node, note=""):
        if self.spell.get("__event") is not None:
            # Trigger rule: rows fire on the event rather than on a root cast.
            passive_id = self.passive_for(gates)
            self.passives[passive_id]["effects"].append(self.event_row(target_ability, chance))
            return
        passive_id = self.passive_for(gates)
        self.passives[passive_id]["effects"].append(
            {"effect": "trigger_ability_on_cast", "value": self.root_id,
             "secondary_value": target_ability, "third_value": round(chance, 6)})
        if target_ability == self.root_id:
            self.warnings.append("%s: %s casts itself (node %s); a triggered cast triggering again is unverified"
                                 % (self.where, self.root_id, node.path_text()))

    def event_row(self, target_ability, chance):
        event = self.spell["__event"]
        if "cast" in event:
            return {"effect": "trigger_ability_on_cast", "value": event["cast"],
                    "secondary_value": target_ability, "third_value": round(chance, 6)}
        if "attack" in event:
            return {"effect": "on_attack_cast", "value": target_ability, "secondary_value": round(chance, 6)}
        raise AssertionError("non-native event reached add_row")

    # -- evaluator emission -----------------------------------------------------
    def compile_evaluator(self, node):
        """Generates the group abilities under an evaluator-owned node (so the
        evaluator can cast them) without any native row pointing at them."""
        kind = node.kind if node.kind != "leaf" else node.leaf
        backend = "impossible" if (node.kind == "leaf" and node.leaf in ("teleport", "grant_xp")) else "evaluator"
        self.report_rows.append({"path": node.path_text(), "node": kind, "backend": backend, "reason": node.reason})
        if node.kind == "leaf":
            if node.leaf == "cast" or node.leaf in EVALUATOR_LEAVES:
                return
            path = node.path_text()
            group = Group("%s__%s" % (self.root_id, path), path, True)
            effects, keys, duration = leaf_keys(node.leaf, node.args)
            group.take(node.leaf, effects, keys, duration)
            self.abilities.append(self.sub_ability(group))
            return
        if node.kind == "all":
            leaves = [c for c in node.children if c.kind == "leaf" and c.leaf != "cast" and c.leaf not in EVALUATOR_LEAVES]
            if leaves:
                path = node.path_text() if node.path else "r"
                groups = self.emit_group_leaves(node, path, leaves, Group("%s__%s" % (self.root_id, path), path, True))
                for group in groups:
                    self.abilities.append(self.sub_ability(group))
            for child in node.children:
                if child not in leaves:
                    self.compile_evaluator(child)
            return
        for child in node.children:
            self.compile_evaluator(child)

    # -- entry ---------------------------------------------------------------------
    def run(self, root):
        classify(root)
        if self.attached:
            # Nothing is generated for the existing ability itself.
            self.report_rows.append({"path": "root", "node": "attach", "backend": "native",
                                     "reason": "effects chained from existing ability %s" % self.root_id})
        self.compile_native(root, 1.0, {}, True)
        if self.attached and not self.passives and not any(not n.native for n in walk_nodes(root)):
            self.warnings.append("%s: attached spell generates nothing" % self.where)
        return self


def compile_trigger(document, trigger, index, report, local_spells, node):
    """Compiles one event rule: native events become passive rows, others are
    evaluator-owned (an ability is still generated for their `do`)."""
    event = trigger["on"]
    event_name = next(iter(event))
    pseudo = {"ability": "%s_ext_trigger_%s" % (document["mod"], trigger["id"]),
              "name": "Trigger %s" % trigger["id"], "target": "self", "__event": event}
    compiler = SpellCompiler(document, pseudo, index, report, local_spells)
    compiler.where = "triggers[%d](%s)" % (index, trigger["id"])
    compiler.root_id = pseudo["ability"]
    classify(node)
    chance = trigger.get("chance", 1)
    gates = {}
    remaining = None
    if "when" in trigger:
        keys, remaining = cond.native_keys_for_passive(trigger["when"])
        gates = dict(keys)
        gates["__path"] = "gate"
    if event_name in NATIVE_EVENTS:
        compiler.compile_native(node, chance, gates, False)
        backend = "native"
    elif event_name == "kill" and node.kind == "leaf" and node.leaf == "create_item" and chance == 1:
        pid = compiler.passive_for(gates)
        compiler.passives[pid]["effects"].append(
            {"effect": "on_kill_create_item", "value": node.args["item"], "secondary_value": 1.0})
        compiler.report_rows.append({"path": "root", "node": "create_item", "backend": "native",
                                     "reason": "on_kill_create_item"})
        backend = "native"
    elif event_name == "kill" and node.kind == "leaf" and node.leaf == "create_item":
        pid = compiler.passive_for(gates)
        compiler.passives[pid]["effects"].append(
            {"effect": "on_kill_create_item", "value": node.args["item"], "secondary_value": round(chance, 6)})
        compiler.report_rows.append({"path": "root", "node": "create_item", "backend": "native",
                                     "reason": "on_kill_create_item with chance"})
        backend = "native"
    else:
        for item in walk_nodes(node):
            item.native = False
            item.reason = item.reason or "event %s has no native hook" % event_name
        compiler.compile_evaluator(node)
        backend = "evaluator"
        remaining = trigger.get("when")   # the whole condition is the evaluator's
        gates = {}
    if remaining is not None and backend == "native":
        pid = compiler.passive_for(gates)
        condition = {"id": "ext_trigger_" + trigger["id"].lower(), "target": {"kind": "passive", "id": pid},
                     "when": remaining}
        if "on_unavailable" in trigger:
            condition["on_unavailable"] = trigger["on_unavailable"]
        compiler.conditions.append(condition)
        names = set()
        cond.used_predicates(remaining, names)
        compiler.report_rows.append({"path": "when", "node": "condition", "backend": "evaluator",
                                     "reason": "gate %s stays a skill-condition on the passive" % sorted(names)})
    compiler.report_rows.insert(0, {"path": "on", "node": event_name, "backend": backend,
                                    "reason": "" if backend == "native" else "needs capability on_%s" % event_name})
    return compiler


def validate_spell(spell, index, report, seen_ids, known_targets):
    where = "spells[%d]" % index
    if not isinstance(spell, dict):
        report.error(where, "must be an object")
        return None
    extra = set(spell) - SPELL_KEYS
    if extra:
        report.error(where, "unknown keys %s" % sorted(extra))
    if ("id" in spell) == ("ability" in spell):
        report.error(where, "exactly one of 'id' (new ability) or 'ability' (attach to an existing one)")
        return None
    if "id" in spell:
        if not isinstance(spell["id"], str) or not re.fullmatch(ID_PATTERN, spell["id"]):
            report.error(where, "id must match %s" % ID_PATTERN)
            return None
        if spell["id"] in seen_ids:
            report.error(where, "duplicate spell id %r" % spell["id"])
            return None
        seen_ids.add(spell["id"])
        where = "%s(%s)" % (where, spell["id"])
    else:
        if not cond.id_string(spell["ability"], where, report, "ability"):
            return None
        if known_targets is not None and spell["ability"] not in known_targets["ability"]:
            report.error(where, "no ability %r in the mod to attach to" % spell["ability"])
        where = "%s(%s)" % (where, spell["ability"])
        for key in ("target", "radius", "shape", "range", "min_range", "cooldown", "cast_time", "cost",
                    "requirements", "slots", "name", "image", "animation"):
            if key in spell and key not in ("name", "image", "animation", "target", "radius", "shape", "range"):
                report.warn(where, "%s is ignored when attaching to an existing ability" % key)
    target = spell.get("target", "self")
    if not isinstance(target, str) or TARGET_ALIASES.get(target, target) not in TARGETS:
        report.error(where, "unknown target %r (engine names %s or Dredmor aliases %s)"
                     % (target, TARGETS, sorted(TARGET_ALIASES)))
    if "shape" in spell and spell["shape"] not in SHAPES:
        report.error(where, "shape must be one of %s" % sorted(SHAPES))
    if "radius" in spell and (not isinstance(spell["radius"], int) or not 0 <= spell["radius"] <= 32):
        report.error(where, "radius must be an integer 0..32")
    for key in ("range", "min_range", "cooldown", "cast_time"):
        if key in spell and (not is_number(spell[key]) or spell[key] < 0):
            report.error(where, "%s must be a non-negative number" % key)
    if "image" in spell and (not isinstance(spell["image"], int) or spell["image"] < 0):
        report.error(where, "image must be a non-negative integer")
    if "level" in spell and (not isinstance(spell["level"], int) or spell["level"] < 1):
        report.error(where, "level must be a positive integer")
    if "cost" in spell:
        if isinstance(spell["cost"], dict) and set(spell["cost"]) <= {"health", "stamina"}:
            for value in spell["cost"].values():
                if not is_number(value) or value < 0:
                    report.error(where, "cost values must be non-negative numbers")
        else:
            report.error(where, "cost must be an object with health and/or stamina")
    if "requirements" in spell:
        req = spell["requirements"]
        if not isinstance(req, dict) or not set(req) <= {"one_of", "always"} or not req:
            report.error(where, "requirements must be {one_of: [...]} and/or {always: [...]}")
        else:
            for key, names in req.items():
                if not isinstance(names, list) or not names or \
                        any(not isinstance(n, str) or n.lower() not in cond.ABILITY_REQUIREMENT_IDS for n in names):
                    report.error(where, "requirements.%s must list weapon classes %s"
                                 % (key, sorted(cond.ABILITY_REQUIREMENT_IDS)))
    effects = spell.get("effects")
    if not isinstance(effects, list) or not effects or len(effects) > MAX_LIST:
        report.error(where, "effects must be a list of 1..%d effect nodes" % MAX_LIST)
        return None
    state = {"nodes": 0}
    root = build_node({"all": effects}, (), where + ".effects", 0, state, report)
    return root


def validate_trigger(trigger, index, report, seen_ids, known_targets):
    where = "triggers[%d]" % index
    if not isinstance(trigger, dict):
        report.error(where, "must be an object")
        return None
    extra = set(trigger) - TRIGGER_KEYS
    if extra:
        report.error(where, "unknown keys %s" % sorted(extra))
    tid = trigger.get("id")
    if not isinstance(tid, str) or not re.fullmatch(ID_PATTERN, tid):
        report.error(where, "id must match %s" % ID_PATTERN)
        return None
    if tid in seen_ids:
        report.error(where, "duplicate id %r" % tid)
        return None
    seen_ids.add(tid)
    where = "%s(%s)" % (where, tid)
    on = trigger.get("on")
    if not isinstance(on, dict) or len(on) != 1 or next(iter(on)) not in EVENTS:
        report.error(where, "on must be one of %s" % ["{%s: ...}" % e for e in EVENTS])
        return None
    event = next(iter(on))
    if event == "cast":
        if not cond.id_string(on["cast"], where, report, "on.cast"):
            return None
        if known_targets is not None and on["cast"] not in known_targets["ability"]:
            report.warn(where, "on.cast %r is not an ability of this mod (fine if it is another mod's)" % on["cast"])
    elif on[event] is not True:
        report.error(where, "on.%s must be true" % event)
        return None
    chance = trigger.get("chance", 1)
    if not is_number(chance) or not 0 < chance <= 1:
        report.error(where, "chance must be above 0 and at most 1")
        return None
    if trigger.get("on_unavailable", "inactive") not in ("inactive", "active"):
        report.error(where, "on_unavailable must be inactive or active")
        return None
    if "when" in trigger:
        cond.walk(trigger["when"], where + ".when", 1, {"nodes": 0}, report, [])
    if "do" not in trigger:
        report.error(where, "missing 'do'")
        return None
    return build_node(trigger["do"], (), where + ".do", 1, {"nodes": 0}, report)


def validate(document, report, mod_folder=None):
    """Validates and compiles. Returns (compilers, summary) where compilers is a
    list of SpellCompiler results (empty when there are errors)."""
    if not isinstance(document, dict):
        report.error("file", "top level must be an object")
        return [], []
    extra = set(document) - {"spec", "mod", "requires", "spells", "triggers"}
    if extra:
        report.error("file", "unknown keys %s" % sorted(extra))
    if document.get("spec") != 1:
        report.error("file", "spec must be 1")
    mod = document.get("mod")
    if not isinstance(mod, str) or not re.fullmatch(ID_PATTERN, mod):
        report.error("file", "mod must be the mod id (%s)" % ID_PATTERN)
        return [], []
    requires = document.get("requires", [])
    if not isinstance(requires, list) or any(not isinstance(r, str) for r in requires):
        report.error("file", "requires must be a list of names")
        requires = []
    for name in requires:
        if name not in CAPABILITIES and name not in cond.PREDICATES:
            report.error("file", "requires names an unknown capability or predicate %r" % name)
    spells = document.get("spells", [])
    triggers = document.get("triggers", [])
    if not isinstance(spells, list) or len(spells) > MAX_SPELLS:
        report.error("file", "spells must be a list of at most %d" % MAX_SPELLS)
        return [], []
    if not isinstance(triggers, list) or len(triggers) > MAX_SPELLS:
        report.error("file", "triggers must be a list of at most %d" % MAX_SPELLS)
        return [], []
    known_targets = cond.mod_target_ids(mod_folder) if mod_folder else None
    local_spells = {s["id"] for s in spells if isinstance(s, dict) and isinstance(s.get("id"), str)}

    seen_ids = set()
    roots = []
    for index, spell in enumerate(spells):
        root = validate_spell(spell, index, report, seen_ids, known_targets)
        roots.append((spell, root))
    trigger_roots = []
    for index, trigger in enumerate(triggers):
        trigger_roots.append((trigger, validate_trigger(trigger, index, report, seen_ids, known_targets)))
    if report.errors:
        return [], []

    compilers = []
    needed = set()
    summary = []
    for index, (spell, root) in enumerate(roots):
        compiler = SpellCompiler(document, spell, index, report, local_spells).run(root)
        needed |= required_capabilities(root)
        for item in walk_nodes(root):
            if item.kind == "leaf" and item.leaf == "cast" and known_targets is not None:
                target = compiler.resolve_cast(item.args["spell"])
                if target not in known_targets["ability"] and item.args["spell"] not in local_spells \
                        and not any(target == c.root_id for c in compilers + [compiler]):
                    report.warn(compiler.where, "cast %r is not an ability of this mod or this file" % item.args["spell"])
        compilers.append(compiler)
        summary.append({"id": compiler.root_id, "kind": "attach" if compiler.attached else "spell",
                        "nodes": compiler.report_rows})
    for index, (trigger, node) in enumerate(trigger_roots):
        compiler = compile_trigger(document, trigger, index, report, local_spells, node)
        event = next(iter(trigger["on"]))
        if event not in NATIVE_EVENTS and not (event == "kill" and node.kind == "leaf" and node.leaf == "create_item"):
            needed.add("on_" + event)
        needed |= required_capabilities(node)
        if "when" in trigger:
            names = set()
            cond.used_predicates(trigger["when"], names)
            needed |= names
        compilers.append(compiler)
        summary.append({"id": "trigger " + trigger["id"], "kind": "trigger", "nodes": compiler.report_rows})
    for compiler in compilers:
        report.warnings.extend(compiler.warnings)
    missing = sorted(needed - set(requires))
    if missing:
        report.warn("file", "capabilities/predicates used but not listed in requires: %s" % missing)
    return compilers, summary


def emit(document, compilers, folder):
    abilities_folder = os.path.join(folder, "abilities")
    os.makedirs(abilities_folder, exist_ok=True)
    passives, milestones, conditions, report_rows = [], [], [], []
    count = 0
    for compiler in compilers:
        for ability in compiler.abilities:
            with open(os.path.join(abilities_folder, ability["id"] + ".json"), "w", encoding="utf-8") as handle:
                json.dump(ability, handle, indent="\t")
                handle.write("\n")
            count += 1
        level = compiler.spell.get("level", 1)
        for passive in compiler.passives.values():
            passives.append(passive)
            milestones.append({"id": passive["id"], "name": passive["name"],
                               "requirements": {"milestones": [], "skill": level},
                               "rewards": [{"passive": passive["id"]}],
                               "skill_id": compiler.spell.get("skill", document["mod"]),
                               "steps": [], "steps_type": 0})
        conditions.extend(compiler.conditions)
        report_rows.append({"id": compiler.root_id, "nodes": compiler.report_rows})
    for name, payload in (("generated_passives.json", passives), ("generated_milestones.json", milestones),
                          ("effects_report.json", report_rows)):
        with open(os.path.join(folder, name), "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2)
            handle.write("\n")
    requires = sorted({name for c in conditions for name in cond._leaf_names(c["when"])})
    with open(os.path.join(folder, "generated_conditions.json"), "w", encoding="utf-8") as handle:
        json.dump({"spec": 1, "requires": requires, "conditions": conditions}, handle, indent=2)
        handle.write("\n")
    return count, len(passives), len(conditions)


def run(path, mod_folder, emit_folder=None):
    report = Report()
    try:
        document = json.load(open(path, encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        print("ERROR: cannot read %s: %s" % (path, error))
        return 1
    compilers, summary = validate(document, report, mod_folder)
    for entry in summary:
        backends = [n["backend"] for n in entry["nodes"]]
        print("%-52s %-8s native=%d evaluator=%d impossible=%d%s" % (
            entry["id"], entry["kind"], backends.count("native"), backends.count("evaluator"),
            backends.count("impossible"), "  (fully native)" if set(backends) == {"native"} else ""))
        for node in entry["nodes"]:
            if node["backend"] != "native" or node["reason"]:
                print("    %-12s %-14s %-10s %s" % (node["path"], node["node"], node["backend"], node["reason"]))
    for message in report.warnings:
        print("WARNING: " + message)
    for message in report.errors:
        print("ERROR: " + message)
    print("%d spell(s)/trigger(s), %d error(s), %d warning(s)" % (len(summary), len(report.errors), len(report.warnings)))
    if emit_folder and not report.errors:
        generated = emit(document, compilers, emit_folder)
        print("emitted %d ability file(s), %d passive(s) and %d condition(s) to %s"
              % (generated[0], generated[1], generated[2], emit_folder))
    return 1 if report.errors else 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
def self_test():
    results = []

    def check(label, document, expect_errors, expect_substring=None):
        report = Report()
        validate(document, report)
        ok = bool(report.errors) == expect_errors
        if ok and expect_substring:
            ok = any(expect_substring in e for e in report.errors)
        if not ok:
            print("FAIL: %s -> %s" % (label, report.errors))
        results.append(ok)

    def doc(effects, **spell_fields):
        spell = {"id": "S", "target": "aoe_tile", "radius": 2, "range": 6, "effects": effects}
        spell.update(spell_fields)
        return {"spec": 1, "mod": "m", "spells": [spell]}

    def compiled(document):
        report = Report()
        compilers, _ = validate(document, report)
        if report.errors:
            print("FAIL: unexpected errors %s" % report.errors)
            results.append(False)
            return None
        return compilers

    def expect(label, condition, detail=""):
        if not condition:
            print("FAIL: %s %s" % (label, detail))
        results.append(bool(condition))

    dmg = {"damage": {"amount": [3, 6], "type": "fire", "scaling": 0.8}}
    # Structure and leaf typing.
    check("native damage spell", doc([dmg]), False)
    check("unknown top key", {"spec": 1, "mod": "m", "foo": 1}, True, "unknown keys")
    check("spec", {"spec": 2, "mod": "m"}, True, "spec must be 1")
    check("mod id", {"spec": 1, "mod": "bad id"}, True, "mod must be")
    check("id and ability together", doc([dmg], ability="x"), True, "exactly one of")
    check("bad target", doc([dmg], target="moon"), True, "unknown target")
    check("dredmor alias target", doc([dmg], target="fireball"), False)
    check("bad damage type", doc([{"damage": {"amount": 3, "type": "lava"}}]), True, "unknown damage type")
    check("bad amount", doc([{"damage": {"amount": [6, 3], "type": "fire"}}]), True, "amount must be")
    check("unknown leaf", doc([{"explode": 1}]), True, "exactly one effect key")
    check("two heads", doc([{"damage": {"amount": 1, "type": "fire"}, "heal": {"amount": 1}}]), True, "exactly one")
    check("unknown status", doc([{"status": {"blind": 1}}]), True, "unknown status")
    check("status ok", doc([{"status": {"stun": 1.5, "poison": 3}}]), False)
    check("buff needs stats", doc([{"buff": {"duration": 5}}]), True, "needs stats")
    check("buff unknown modifier", doc([{"buff": {"duration": 5, "modifiers": {"luck": 1}}}]), True, "unknown modifier")
    check("chance range", doc([{"chance": 1.5, "do": dmg}]), True, "chance must be")
    check("chance needs do", doc([{"chance": 0.5}]), True, "needs 'do'")
    check("choose weights", doc([{"choose": [{"weight": 0, "do": dmg}]}]), True, "weight must be")
    check("choose bad option key", doc([{"choose": [{"do": dmg, "percent": 5}]}]), True, "unknown keys")
    check("delay range", doc([{"delay": 0, "do": dmg}]), True, "delay must be")
    check("repeat range", doc([{"repeat": 99, "do": dmg}]), True, "repeat must be")
    check("when bad predicate", doc([{"when": {"moon": 1}, "do": dmg}]), True, "unknown predicate")
    check("cast ok", doc([{"cast": {"spell": "X", "chance": 0.5, "delay": 2}}]), False)
    check("cast bad chance", doc([{"cast": {"spell": "X", "chance": 2}}]), True, "chance must be")
    check("requirements names", doc([dmg], requirements={"one_of": ["Spoon"]}), True, "weapon classes")
    check("requires unknown", {"spec": 1, "mod": "m", "requires": ["magic"]}, True, "unknown capability")
    check("duplicate spell id", {"spec": 1, "mod": "m", "spells": [doc([dmg])["spells"][0], doc([dmg])["spells"][0]]},
          True, "duplicate spell id")
    check("effects empty", doc([]), True, "effects must be")
    deep = dmg
    for _ in range(MAX_DEPTH + 1):
        deep = {"chance": 0.5, "do": deep}
    check("depth limit", doc([deep]), True, "deeper than")
    check("node limit", doc([{"all": [dmg] * 30}, {"all": [dmg] * 30}, {"all": [dmg] * 10}]), True, "more than")

    # Triggers.
    def tdoc(**fields):
        trigger = {"id": "t", "on": {"attack": True}, "do": {"cast": {"spell": "B"}}}
        trigger.update(fields)
        return {"spec": 1, "mod": "m", "triggers": [trigger]}
    check("attack trigger", tdoc(), False)
    check("turn trigger", tdoc(on={"turn": True}), False)
    check("bad event", tdoc(on={"sneeze": True}), True, "on must be")
    check("event not true", tdoc(on={"kill": 1}), True, "must be true")
    check("trigger needs do", {"spec": 1, "mod": "m", "triggers": [{"id": "t", "on": {"attack": True}}]}, True, "missing 'do'")
    check("trigger id clash with spell",
          {"spec": 1, "mod": "m", "spells": [doc([dmg])["spells"][0]],
           "triggers": [{"id": "S", "on": {"attack": True}, "do": dmg}]}, True, "duplicate id")

    # Compilation: a fully native spell with merged and split leaves.
    compilers = compiled(doc([dmg, {"status": {"burn": 2}}, {"summon": {"entity": "Mine", "count": 3}},
                              {"damage": {"amount": 2, "type": "frost"}}]))
    if compilers:
        c = compilers[0]
        root = next(a for a in c.abilities if a["id"] == "m_S")
        expect("root merges leaves", root["effects"].get("damage") == [3, 6] and root["effects"].get("burn") == 2
               and root["effects"].get("summon_count") == 3 and root.get("effects_keys") == {"summon": "Mine"}
               and root["effects"].get("aoe_range") == 2 and root["effects"]["aoe_shape"] == 1
               and root["target"] == "aoe_tile", str(root))
        split = [a for a in c.abilities if a["id"] == "m_S__root_x1"]
        expect("conflicting damage splits into a chained sub-ability",
               len(split) == 1 and split[0]["effects"]["damage"] == [2, 2] and split[0]["effects"]["damage_type"] == "frost",
               str([a["id"] for a in c.abilities]))
        rows = c.passives.get("ext_fx_m_S", {}).get("effects", [])
        expect("split is chained with chance 1", rows == [{"effect": "trigger_ability_on_cast", "value": "m_S",
                                                           "secondary_value": "m_S__root_x1", "third_value": 1.0}], str(rows))
        expect("fully native report", all(r["backend"] == "native" for r in c.report_rows), str(c.report_rows))

    # Chance multiplies; cast resolves local spell ids; when hoists natively.
    compilers = compiled({"spec": 1, "mod": "m", "spells": [
        {"id": "A", "target": "self", "effects": [
            {"chance": 0.5, "do": {"chance": 0.5, "do": {"cast": {"spell": "B"}}}},
            {"when": {"all": [{"weapon_class": "Sword"}, {"party": "alone"}]}, "do": {"cast": {"spell": "Other_mod_x"}}},
            {"cast": {"spell": "A", "chance": 0.1}}]},
        {"id": "B", "target": "self", "effects": [{"heal": {"amount": 5}}]}]})
    if compilers:
        c = compilers[0]
        plain = c.passives["ext_fx_m_A"]["effects"]
        expect("nested chance multiplies and local id resolves",
               {"effect": "trigger_ability_on_cast", "value": "m_A", "secondary_value": "m_B", "third_value": 0.25} in plain, str(plain))
        expect("self-cast row", any(r["secondary_value"] == "m_A" and r["third_value"] == 0.1 for r in plain), str(plain))
        gated = c.passives.get("ext_fx_m_A__1")
        expect("fully native when becomes its own gated passive",
               gated is not None and gated.get("restrict_weapon_type") == "sword" and gated.get("apply_mode") == "solo_only"
               and gated["effects"][0]["secondary_value"] == "Other_mod_x", str(gated))
        expect("self-loop warning", any("casts itself" in w for w in c.warnings), str(c.warnings))
        expect("no conditions remain for native gates", c.conditions == [])

    # Evaluator ownership: choose (exact), delay, non-native when, evaluator leaves.
    compilers = compiled(doc([
        dmg,
        {"choose": [{"weight": 3, "do": {"cast": {"spell": "X"}}}, {"weight": 1, "do": {"status": {"stun": 1}}}]},
        {"delay": 2, "do": {"all": [{"buff": {"duration": 3, "stats": {"strength": 1}}}, {"stamina": 10}]}},
        {"when": {"health_percent": {"max": 50}}, "do": {"cast": {"spell": "Y"}}},
        {"remove_stacks": {"stacker": "flag", "count": 0}},
        {"teleport": "target"},
        {"repeat": 2, "every": 1, "do": {"cast": {"spell": "Z"}}}]))
    if compilers:
        c = compilers[0]
        by_path = {r["path"]: r for r in c.report_rows}
        expect("choose is evaluator", by_path["1"]["backend"] == "evaluator", str(by_path.get("1")))
        expect("choose children owned by evaluator", by_path["1_0"]["backend"] == "evaluator" and by_path["1_1"]["backend"] == "evaluator")
        expect("delay is evaluator", by_path["2"]["backend"] == "evaluator")
        expect("non-native when is evaluator", by_path["3"]["backend"] == "evaluator")
        expect("remove_stacks is evaluator", by_path["4"]["backend"] == "evaluator")
        expect("teleport is impossible", by_path["5"]["backend"] == "impossible")
        expect("repeat every is evaluator", by_path["6"]["backend"] == "evaluator")
        ids = {a["id"] for a in c.abilities}
        expect("evaluator leaf groups still get abilities",
               {"m_S", "m_S__1_1", "m_S__2_0"} <= ids and "m_S__3" not in ids, str(sorted(ids)))
        rows = c.passives["ext_fx_m_S"]["effects"] if "ext_fx_m_S" in c.passives else []
        expect("no native row points at evaluator-owned abilities",
               not any(r["secondary_value"] in ("m_S__1_1", "m_S__2_0", "X", "Y", "Z") for r in rows), str(rows))
        expect("sub-ability of self leaves targets self",
               next(a for a in c.abilities if a["id"] == "m_S__2_0")["target"] == "self")
        expect("capabilities computed",
               required_capabilities(classify(validate_spell(doc([dmg, {"delay": 1, "do": dmg}])["spells"][0], 0, Report(), set(), None)))
               == {"delay"})

    # Approximate choose compiles to native independent chances.
    compilers = compiled(doc([{"choose": [{"weight": 3, "do": {"cast": {"spell": "X"}}}, {"weight": 1, "do": {"cast": {"spell": "Y"}}}],
                               "approximate": True}]))
    if compilers:
        rows = compilers[0].passives["ext_fx_m_S"]["effects"]
        expect("approximate choose weights become chances",
               sorted((r["secondary_value"], r["third_value"]) for r in rows) == [("X", 0.75), ("Y", 0.25)], str(rows))
        expect("approximate choose is native", all(r["backend"] == "native" for r in compilers[0].report_rows))

    # Repeat without every: n rows.
    compilers = compiled(doc([{"repeat": 3, "do": {"cast": {"spell": "X"}}}]))
    if compilers:
        rows = compilers[0].passives["ext_fx_m_S"]["effects"]
        expect("repeat emits n rows", len(rows) == 3 and all(r["secondary_value"] == "X" for r in rows), str(rows))

    # Attach mode: nothing for the root ability; leaves go to __r.
    compilers = compiled({"spec": 1, "mod": "m", "spells": [{"ability": "m_Existing", "effects": [
        {"status": {"burn": 1}}, {"chance": 0.3, "do": {"cast": {"spell": "m_Existing"}}}]}]})
    if compilers:
        c = compilers[0]
        expect("attach generates no root ability", "m_Existing" not in {a["id"] for a in c.abilities}, str([a["id"] for a in c.abilities]))
        expect("attach root leaves become __r", any(a["id"] == "m_Existing__r" for a in c.abilities))
        rows = c.passives["ext_fx_m_Existing"]["effects"]
        expect("attach rows fire on the existing ability", all(r["value"] == "m_Existing" for r in rows) and len(rows) == 2, str(rows))

    # Triggers: native attack with hoisted gate + remaining condition; kill create_item; evaluator turn.
    compilers = compiled({"spec": 1, "mod": "m", "spells": [{"id": "P", "target": "aoe_self", "radius": 1, "effects": [dmg]}],
                          "triggers": [
        {"id": "t1", "on": {"attack": True}, "chance": 0.2,
         "when": {"all": [{"weapon_class": "Unarmed"}, {"stacker": {"id": "flag", "min": 1}}]},
         "do": {"cast": {"spell": "P"}}},
        {"id": "t2", "on": {"kill": True}, "when": {"holder_tag": "Player"}, "do": {"create_item": {"item": "Charge"}}},
        {"id": "t3", "on": {"turn": True}, "chance": 0.05, "do": {"cast": {"spell": "P"}}},
        {"id": "t4", "on": {"cast": "m_P"}, "do": {"all": [{"heal": {"amount": 2}}, {"stamina": 5}]}}]})
    if compilers:
        t1 = compilers[1]
        passive = t1.passives.get("ext_fx_m_ext_trigger_t1__gate")
        expect("attack trigger row uses on_attack_cast with secondary_value chance",
               passive is not None and passive["effects"] == [{"effect": "on_attack_cast", "value": "m_P", "secondary_value": 0.2}]
               and passive.get("restrict_weapon_type") == "unarmed", str(passive))
        expect("remaining gate becomes a condition on the passive",
               t1.conditions == [{"id": "ext_trigger_t1", "target": {"kind": "passive", "id": "ext_fx_m_ext_trigger_t1__gate"},
                                  "when": {"stacker": {"id": "flag", "min": 1}}}], str(t1.conditions))
        t2 = compilers[2]
        passive = t2.passives.get("ext_fx_m_ext_trigger_t2__gate")
        expect("kill create_item is native with restrict_tags",
               passive is not None and passive["effects"][0]["effect"] == "on_kill_create_item"
               and passive["effects"][0]["value"] == "Charge" and passive.get("restrict_tags") == "5", str(passive))
        t3 = compilers[3]
        expect("turn trigger is evaluator with no passives", not t3.passives and t3.report_rows[0]["backend"] == "evaluator", str(t3.report_rows))
        t4 = compilers[4]
        expect("cast trigger with effect group generates an ability and a row",
               any(a["id"] == "m_ext_trigger_t4__r" for a in t4.abilities)
               and t4.passives["ext_fx_m_ext_trigger_t4"]["effects"][0] == {"effect": "trigger_ability_on_cast", "value": "m_P",
                                                                             "secondary_value": "m_ext_trigger_t4__r", "third_value": 1.0},
               str((t4.abilities, t4.passives)))

    # Requires warning.
    report = Report()
    validate(doc([{"delay": 1, "do": dmg}]), report)
    expect("missing requires is a warning", any("not listed in requires" in w for w in report.warnings), str(report.warnings))

    # The shipped example.
    example = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "examples", "runic-magic-effects.json")
    if os.path.isfile(example):
        report = Report()
        compilers, summary = validate(json.load(open(example, encoding="utf-8-sig")), report)
        expect("example validates", not report.errors, str(report.errors))
        if compilers:
            by_id = {c.root_id: c for c in compilers}
            cinder = by_id.get("arendeth_runic_magic_Throw_Runeword_Cinder")
            expect("Cinder is fully native", cinder and all(r["backend"] == "native" for r in cinder.report_rows) and not cinder.passives)
            entropy = by_id.get("arendeth_runic_magic_Entropy_Surge")
            expect("Entropy choose is evaluator", entropy and any(r["path"] == "1" and r["backend"] == "evaluator" for r in entropy.report_rows))
            draw = by_id.get("arendeth_runic_magic_Draw_First_Circle_Exact")
            expect("approximate draw is native", draw and all(r["backend"] == "native" for r in draw.report_rows)
                   and len(draw.passives["ext_fx_arendeth_runic_magic_Draw_First_Circle_Exact"]["effects"]) == 4)
            cascade = by_id.get("arendeth_runic_magic_Cascade_Discharge")
            expect("Cascade attach: chance native, health gate evaluator",
                   cascade and {r["path"]: r["backend"] for r in cascade.report_rows}.get("0") == "native"
                   and {r["path"]: r["backend"] for r in cascade.report_rows}.get("1") == "evaluator")

    passed = all(results)
    print("PASS: effects compiler self-test (%d checks)" % len(results) if passed else "FAIL: effects compiler self-test")
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
