#include "effects.hpp"

#include <algorithm>
#include <cmath>

namespace soulash::effects
{
    using conditions::Json;
    using conditions::Tri;

    namespace
    {
        constexpr std::size_t MaximumDocument = 256 * 1024;
        constexpr std::size_t MaximumSpells = 256;
        constexpr int MaximumNodes = 64;
        constexpr int MaximumDepth = 8;
        constexpr std::size_t MaximumList = 32;

        const std::set<std::string> kCombinators = {"all", "chance", "repeat", "choose", "delay", "when"};
        const std::set<std::string> kNativeLeaves = {
            "damage", "heal", "stamina", "status", "knockback", "pull", "swap", "lunge", "charge",
            "step_aside", "buff", "summon", "create_item", "stacks", "cleanse", "cooldowns", "transform",
            "aura", "cast"};
        const std::set<std::string> kEvaluatorLeaves = {"remove_stacks", "teleport", "grant_xp"};
        const std::set<std::string> kEvents = {"cast", "attack", "kill", "turn", "damaged", "dodge", "block", "crit"};
        const std::set<std::string> kCapabilities = {
            "choose", "delay", "repeat", "remove_stacks", "teleport", "grant_xp", "on_kill", "on_turn",
            "on_damaged", "on_dodge", "on_block", "on_crit"};
        // Hoistable condition predicates (validate_conditions.native_keys_for_passive).
        const std::set<std::string> kHoistable = {"weapon_class", "holder_tag", "party"};

        bool IsLeaf(const std::string& key)
        {
            return kNativeLeaves.count(key) != 0 || kEvaluatorLeaves.count(key) != 0;
        }

        bool IsInteger(const Json& value)
        {
            return value.type == Json::Type::Number && std::floor(value.number) == value.number;
        }

        std::string Join(const std::vector<int>& path)
        {
            if (path.empty()) return "root";
            std::string out;
            for (std::size_t index = 0; index < path.size(); ++index)
            {
                if (index != 0) out += '_';
                out += std::to_string(path[index]);
            }
            return out;
        }

        bool ValidId(const std::string& value)
        {
            if (value.empty() || value.size() > 64) return false;
            for (char c : value)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok) return false;
            }
            return true;
        }

        bool DataId(const Json* value)
        {
            return value != nullptr && value->type == Json::Type::String && !value->text.empty()
                && value->text.size() <= 128;
        }

        // Builds a conditions::Node from JSON (structure only; predicates are
        // checked offline by the validator and unknown ones evaluate to unknown).
        bool BuildCondition(const Json& json, int depth, int& nodes, conditions::Node& out, std::string& error)
        {
            if (++nodes > 64) { error = "condition has more than 64 nodes"; return false; }
            if (depth > 8) { error = "condition is deeper than 8 levels"; return false; }
            if (json.type != Json::Type::Object || json.members.size() != 1)
            {
                error = "a condition is an object with exactly one key";
                return false;
            }
            const auto& [key, value] = json.members[0];
            if (key == "all" || key == "any")
            {
                if (value.type != Json::Type::Array || value.items.empty() || value.items.size() > MaximumList)
                {
                    error = "all/any need a list of 1 to 32 expressions";
                    return false;
                }
                out.kind = key == "all" ? conditions::Node::Kind::All : conditions::Node::Kind::Any;
                out.children.resize(value.items.size());
                for (std::size_t index = 0; index < value.items.size(); ++index)
                {
                    if (!BuildCondition(value.items[index], depth + 1, nodes, out.children[index], error)) return false;
                }
                return true;
            }
            if (key == "not")
            {
                out.kind = conditions::Node::Kind::Not;
                out.children.resize(1);
                return BuildCondition(value, depth + 1, nodes, out.children[0], error);
            }
            out.kind = conditions::Node::Kind::Leaf;
            out.predicate = key;
            out.arguments = value;
            return true;
        }

        // Mirrors validate_conditions.native_keys_for_passive: every leaf of a
        // top-level all (or a single leaf) must hoist, each native key once.
        bool FullyNativeCondition(const Json& when)
        {
            if (when.type != Json::Type::Object || when.members.size() != 1) return false;
            const auto& [name, value] = when.members[0];
            std::vector<const Json*> leaves;
            if (name == "all")
            {
                if (value.type != Json::Type::Array) return false;
                for (const Json& item : value.items) leaves.push_back(&item);
            }
            else
            {
                leaves.push_back(&when);
            }
            if (leaves.empty()) return false;
            std::set<std::string> seen;
            for (const Json* leaf : leaves)
            {
                if (leaf->type != Json::Type::Object || leaf->members.size() != 1) return false;
                const auto& [predicate, args] = leaf->members[0];
                if (kHoistable.count(predicate) == 0 || seen.count(predicate) != 0) return false;
                if (predicate == "weapon_class"
                    && !(args.type == Json::Type::String
                        || (args.type == Json::Type::Array && args.items.size() == 1)))
                {
                    return false;
                }
                seen.insert(predicate);
            }
            return true;
        }

        // The native keys a hoisted condition puts on a passive (key -> value
        // text), used only to detect conflicting gates on one path.
        std::vector<std::pair<std::string, std::string>> HoistedGates(const Json& when)
        {
            std::vector<std::pair<std::string, std::string>> out;
            if (when.type != Json::Type::Object || when.members.size() != 1) return out;
            const auto& [name, value] = when.members[0];
            std::vector<const Json*> leaves;
            if (name == "all") { for (const Json& item : value.items) leaves.push_back(&item); }
            else leaves.push_back(&when);
            for (const Json* leaf : leaves)
            {
                const auto& [predicate, args] = leaf->members[0];
                std::string text;
                if (args.type == Json::Type::String) text = args.text;
                else if (args.type == Json::Type::Array)
                {
                    for (const Json& item : args.items) text += (item.type == Json::Type::String ? item.text : "?") + ",";
                }
                std::transform(text.begin(), text.end(), text.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                out.emplace_back(predicate, text);
            }
            return out;
        }

        // ---- tree building ------------------------------------------------
        struct Builder
        {
            int nodes = 0;
            std::string error;

            bool Fail(const std::string& where, const std::string& message)
            {
                if (error.empty()) error = where + ": " + message;
                return false;
            }

            bool Build(const Json& json, std::vector<int> path, const std::string& where, int depth, Node& out)
            {
                if (++nodes > MaximumNodes) return Fail(where, "more than 64 nodes");
                if (depth > MaximumDepth) return Fail(where, "deeper than 8 levels");
                if (json.type != Json::Type::Object || json.members.empty())
                {
                    return Fail(where, "an effect is an object with one effect key");
                }
                std::string head;
                for (const auto& [key, value] : json.members)
                {
                    if (kCombinators.count(key) != 0 || IsLeaf(key))
                    {
                        if (!head.empty()) return Fail(where, "exactly one effect key expected");
                        head = key;
                    }
                }
                if (head.empty()) return Fail(where, "exactly one effect key expected");
                for (const auto& [key, value] : json.members)
                {
                    if (key == head || key == "note") continue;
                    const bool allowed =
                        (head == "chance" && key == "do") || (head == "repeat" && (key == "do" || key == "every"))
                        || (head == "choose" && key == "approximate") || (head == "delay" && key == "do")
                        || (head == "when" && (key == "do" || key == "on_unavailable"));
                    if (!allowed) return Fail(where, "unknown key '" + key + "' next to '" + head + "'");
                }
                out.path = path;
                const Json& value = *json.Find(head);

                if (head == "all")
                {
                    if (value.type != Json::Type::Array || value.items.empty() || value.items.size() > MaximumList)
                    {
                        return Fail(where, "all needs a list of 1..32 effects");
                    }
                    out.kind = Node::Kind::All;
                    out.children.resize(value.items.size());
                    for (std::size_t index = 0; index < value.items.size(); ++index)
                    {
                        std::vector<int> childPath = path;
                        childPath.push_back(static_cast<int>(index));
                        if (!Build(value.items[index], childPath, where + ".all[" + std::to_string(index) + "]",
                                depth + 1, out.children[index]))
                        {
                            return false;
                        }
                    }
                    return true;
                }
                if (head == "chance" || head == "repeat" || head == "delay" || head == "when")
                {
                    const Json* body = json.Find("do");
                    if (body == nullptr) return Fail(where, "'" + head + "' needs 'do'");
                    if (head == "chance")
                    {
                        if (value.type != Json::Type::Number || !(value.number > 0.0 && value.number <= 1.0))
                        {
                            return Fail(where, "chance must be above 0 and at most 1");
                        }
                        out.kind = Node::Kind::Chance;
                        out.chance = value.number;
                    }
                    else if (head == "repeat")
                    {
                        if (!IsInteger(value) || value.number < 1 || value.number > 32)
                        {
                            return Fail(where, "repeat must be an integer 1..32");
                        }
                        out.kind = Node::Kind::Repeat;
                        out.count = static_cast<int>(value.number);
                        if (const Json* every = json.Find("every"))
                        {
                            if (!IsInteger(*every) || every->number < 1 || every->number > 1000)
                            {
                                return Fail(where, "every must be an integer 1..1000");
                            }
                            out.every = static_cast<int>(every->number);
                        }
                    }
                    else if (head == "delay")
                    {
                        if (!IsInteger(value) || value.number < 1 || value.number > 1000)
                        {
                            return Fail(where, "delay must be an integer 1..1000");
                        }
                        out.kind = Node::Kind::Delay;
                        out.delay = static_cast<int>(value.number);
                    }
                    else
                    {
                        out.kind = Node::Kind::When;
                        int conditionNodes = 0;
                        std::string conditionError;
                        if (!BuildCondition(value, 1, conditionNodes, out.when, conditionError))
                        {
                            return Fail(where + ".when", conditionError);
                        }
                        out.arguments = value;   // keep the JSON for hoisting checks
                        if (const Json* policy = json.Find("on_unavailable"))
                        {
                            if (policy->type != Json::Type::String
                                || (policy->text != "inactive" && policy->text != "active"))
                            {
                                return Fail(where, "on_unavailable must be inactive or active");
                            }
                            out.activeWhenUnavailable = policy->text == "active";
                        }
                    }
                    out.children.resize(1);
                    std::vector<int> childPath = path;
                    childPath.push_back(0);
                    return Build(*body, childPath, where + ".do", depth + 1, out.children[0]);
                }
                if (head == "choose")
                {
                    if (value.type != Json::Type::Array || value.items.empty() || value.items.size() > MaximumList)
                    {
                        return Fail(where, "choose needs a list of 1..32 options");
                    }
                    out.kind = Node::Kind::Choose;
                    if (const Json* approximate = json.Find("approximate"))
                    {
                        if (approximate->type != Json::Type::Bool) return Fail(where, "approximate must be true or false");
                        out.approximate = approximate->boolean;
                    }
                    double total = 0.0;
                    std::vector<double> weights;
                    out.children.resize(value.items.size());
                    for (std::size_t index = 0; index < value.items.size(); ++index)
                    {
                        const Json& option = value.items[index];
                        const std::string optionWhere = where + ".choose[" + std::to_string(index) + "]";
                        if (option.type != Json::Type::Object) return Fail(optionWhere, "expects an object");
                        double weight = 1.0;
                        const Json* body = nullptr;
                        for (const auto& [key, member] : option.members)
                        {
                            if (key == "weight")
                            {
                                if (member.type != Json::Type::Number || member.number <= 0.0)
                                {
                                    return Fail(optionWhere, "weight must be a positive number");
                                }
                                weight = member.number;
                            }
                            else if (key == "do") body = &member;
                            else return Fail(optionWhere, "unknown key '" + key + "'");
                        }
                        if (body == nullptr) return Fail(optionWhere, "missing 'do'");
                        weights.push_back(weight);
                        total += weight;
                        std::vector<int> childPath = path;
                        childPath.push_back(static_cast<int>(index));
                        if (!Build(*body, childPath, optionWhere + ".do", depth + 1, out.children[index])) return false;
                    }
                    for (std::size_t index = 0; index < weights.size(); ++index)
                    {
                        out.children[index].chance = weights[index] / total;
                    }
                    return true;
                }

                // Leaf. Argument typing is the validator's job offline; here only
                // what the resolver relies on is checked.
                if (head == "cast")
                {
                    if (value.type != Json::Type::Object || !DataId(value.Find("spell")))
                    {
                        return Fail(where + ".cast", "expects an object with 'spell'");
                    }
                    Node inner;
                    inner.kind = Node::Kind::Leaf;
                    inner.leaf = "cast";
                    inner.path = path;
                    inner.arguments = value;
                    Node* current = &inner;
                    Node wrapped;
                    bool haveWrapper = false;
                    auto wrap = [&](Node::Kind kind) {
                        Node wrapper;
                        wrapper.kind = kind;
                        wrapper.path = path;
                        wrapper.children.push_back(haveWrapper ? wrapped : inner);
                        wrapped = wrapper;
                        haveWrapper = true;
                        current = &wrapped;
                    };
                    for (const auto& [key, member] : value.members)
                    {
                        if (key == "spell") continue;
                        if (key == "chance")
                        {
                            if (member.type != Json::Type::Number || !(member.number > 0.0 && member.number <= 1.0))
                            {
                                return Fail(where + ".cast", "chance must be above 0 and at most 1");
                            }
                        }
                        else if (key == "delay")
                        {
                            if (!IsInteger(member) || member.number < 1 || member.number > 1000)
                            {
                                return Fail(where + ".cast", "delay must be an integer 1..1000");
                            }
                        }
                        else if (key != "when")
                        {
                            return Fail(where + ".cast", "unknown key '" + key + "'");
                        }
                    }
                    if (const Json* chance = value.Find("chance"))
                    {
                        wrap(Node::Kind::Chance);
                        current->chance = chance->number;
                    }
                    if (const Json* delay = value.Find("delay"))
                    {
                        wrap(Node::Kind::Delay);
                        current->delay = static_cast<int>(delay->number);
                    }
                    if (const Json* when = value.Find("when"))
                    {
                        wrap(Node::Kind::When);
                        int conditionNodes = 0;
                        std::string conditionError;
                        if (!BuildCondition(*when, 1, conditionNodes, current->when, conditionError))
                        {
                            return Fail(where + ".cast.when", conditionError);
                        }
                        current->arguments = *when;
                    }
                    out = haveWrapper ? wrapped : inner;
                    return true;
                }
                out.kind = Node::Kind::Leaf;
                out.leaf = head;
                out.arguments = value;
                return true;
            }
        };

        // ---- classification (docs/effects.md 4.2) --------------------------
        void Classify(Node& node, bool parentNative)
        {
            if (!parentNative) node.native = false;
            else
            {
                switch (node.kind)
                {
                case Node::Kind::Leaf: node.native = kEvaluatorLeaves.count(node.leaf) == 0; break;
                case Node::Kind::All:
                case Node::Kind::Chance: node.native = true; break;
                case Node::Kind::Repeat: node.native = node.every == 0; break;
                case Node::Kind::Choose: node.native = node.approximate; break;
                case Node::Kind::Delay: node.native = false; break;
                case Node::Kind::When: node.native = FullyNativeCondition(node.arguments); break;
                }
            }
            for (Node& child : node.children) Classify(child, node.native);
        }

        // Conflicting hoisted gates on one path make the inner `when` evaluator-owned
        // (the compiler does the same while emitting).
        void ResolveGateConflicts(Node& node, std::vector<std::pair<std::string, std::string>> gates)
        {
            if (node.native && node.kind == Node::Kind::When)
            {
                for (const auto& gate : HoistedGates(node.arguments))
                {
                    for (const auto& existing : gates)
                    {
                        if (existing.first == gate.first && existing.second != gate.second)
                        {
                            Classify(node, false);
                            return;
                        }
                    }
                    gates.push_back(gate);
                }
            }
            for (Node& child : node.children) ResolveGateConflicts(child, gates);
        }

        // ---- leaf key sets for the first-fit merge --------------------------
        // Only the key names matter (two leaves conflict when they share one);
        // values are the compiler's business. Must match leaf_keys() in Python.
        std::set<std::string> LeafKeys(const Node& leaf, double& duration)
        {
            std::set<std::string> keys;
            duration = NAN;
            const Json& args = leaf.arguments;
            auto has = [&](const char* name) { return args.type == Json::Type::Object && args.Find(name) != nullptr; };
            if (leaf.leaf == "damage")
            {
                keys = {"damage", "damage_type"};
                if (has("scaling")) keys.insert("magic_power_damage");
                if (has("weapon")) keys.insert("damage_weapon");
                if (has("leech")) keys.insert("damage_as_life");
                if (has("pierce")) keys.insert("pierce");
                if (has("chain")) keys.insert("chain");
                if (has("ignore_party")) keys.insert("ignore_party");
            }
            else if (leaf.leaf == "heal") keys = {"heal"};
            else if (leaf.leaf == "stamina") keys = {"stamina"};
            else if (leaf.leaf == "status")
            {
                if (args.type == Json::Type::Object)
                {
                    for (const auto& [name, value] : args.members) keys.insert(name == "poison" ? "poison_effect" : name);
                }
            }
            else if (leaf.leaf == "knockback") keys = {"knockback"};
            else if (leaf.leaf == "pull") keys = {"pull_target"};
            else if (leaf.leaf == "swap") keys = {"trade_places"};
            else if (leaf.leaf == "lunge") keys = {"lunge"};
            else if (leaf.leaf == "charge") keys = {"charge"};
            else if (leaf.leaf == "step_aside") keys = {"move_to_side"};
            else if (leaf.leaf == "buff")
            {
                if (args.type == Json::Type::Object)
                {
                    if (const Json* d = args.Find("duration")) duration = d->number;
                    for (const char* group : {"stats", "modifiers"})
                    {
                        if (const Json* members = args.Find(group))
                        {
                            for (const auto& [name, value] : members->members) keys.insert(name);
                        }
                    }
                }
            }
            else if (leaf.leaf == "summon")
            {
                keys = {"key:summon", "summon_count"};
                if (has("on_move")) keys.insert("key:summon_on_move");
            }
            else if (leaf.leaf == "create_item") keys = {"key:summon", "summon_count"};
            else if (leaf.leaf == "stacks") keys = {"key:stacker", "stacker_count"};
            else if (leaf.leaf == "cleanse") keys = {"neutralize"};
            else if (leaf.leaf == "cooldowns") keys = {"clear_cooldowns"};
            else if (leaf.leaf == "transform") keys = {"key:transform"};
            else if (leaf.leaf == "aura") keys = {"key:aura"};
            return keys;
        }

        bool IsGroupLeaf(const Node& node)
        {
            return node.kind == Node::Kind::Leaf && node.leaf != "cast" && kEvaluatorLeaves.count(node.leaf) == 0;
        }

        // ---- resolution ------------------------------------------------------
        struct Resolver
        {
            const EffectSet& set;
            const std::string& rootAbilityId;
            const conditions::Snapshot& state;
            const Rng& rng;
            const std::set<std::string>& capabilities;
            Resolution& out;

            void Skip(const Node& node, const std::string& reason)
            {
                out.unresolved.push_back({node.PathText(), reason});
            }

            bool Allowed(const Node& node)
            {
                const Tri result = conditions::Evaluate(node.when, state);
                if (result == Tri::True) return true;
                if (result == Tri::False) return false;
                if (node.activeWhenUnavailable) return true;
                Skip(node, "condition is unknown (fail closed)");
                return false;
            }

            // inEvaluator: an evaluator-owned ancestor has been entered, so
            // leaf groups are emitted as casts. Native ancestors only route.
            void Walk(const Node& node, int delay, bool inEvaluator)
            {
                const bool evaluator = inEvaluator || !node.native;
                switch (node.kind)
                {
                case Node::Kind::Leaf:
                    if (node.leaf == "cast")
                    {
                        if (evaluator)
                        {
                            out.casts.push_back({set.CastTarget(node.arguments.Find("spell")->text), delay});
                        }
                    }
                    else if (kEvaluatorLeaves.count(node.leaf) != 0)
                    {
                        if (capabilities.count(node.leaf) == 0) Skip(node, "capability " + node.leaf + " not supported");
                        else Skip(node, node.leaf + " has no cast realisation yet");
                    }
                    else if (evaluator)
                    {
                        out.casts.push_back({rootAbilityId + "__" + node.PathText(), delay});
                    }
                    return;
                case Node::Kind::All:
                    if (evaluator)
                    {
                        for (const std::string& id : GroupAbilityIds(rootAbilityId, node)) out.casts.push_back({id, delay});
                    }
                    for (const Node& child : node.children)
                    {
                        if (evaluator && IsGroupLeaf(child)) continue;   // merged above
                        Walk(child, delay, evaluator);
                    }
                    return;
                case Node::Kind::Chance:
                    if (rng() < node.chance) Walk(node.children[0], delay, evaluator);
                    return;
                case Node::Kind::Repeat:
                    if (node.every != 0 && capabilities.count("repeat") == 0)
                    {
                        Skip(node, "capability repeat not supported");
                        return;
                    }
                    for (int index = 0; index < node.count; ++index)
                    {
                        Walk(node.children[0], delay + node.every * index, evaluator);
                    }
                    return;
                case Node::Kind::Choose:
                    if (node.approximate)
                    {
                        for (const Node& child : node.children)
                        {
                            if (rng() < child.chance) Walk(child, delay, evaluator);
                        }
                        return;
                    }
                    if (capabilities.count("choose") == 0)
                    {
                        Skip(node, "capability choose not supported");
                        return;
                    }
                    {
                        double roll = rng();
                        for (const Node& child : node.children)
                        {
                            if (roll < child.chance || &child == &node.children.back())
                            {
                                Walk(child, delay, true);
                                return;
                            }
                            roll -= child.chance;
                        }
                    }
                    return;
                case Node::Kind::Delay:
                    if (capabilities.count("delay") == 0)
                    {
                        Skip(node, "capability delay not supported");
                        return;
                    }
                    Walk(node.children[0], delay + node.delay, true);
                    return;
                case Node::Kind::When:
                    if (Allowed(node)) Walk(node.children[0], delay, evaluator);
                    return;
                }
            }
        };

        bool RequirementsMet(const EffectSet& set, const std::set<std::string>& capabilities, Resolution& out)
        {
            bool ok = true;
            for (const std::string& name : set.Requires())
            {
                if (capabilities.count(name) == 0 && conditions::SupportedPredicates().count(name) == 0)
                {
                    out.unresolved.push_back({"requires", "required '" + name + "' is not supported"});
                    ok = false;
                }
            }
            return ok;
        }
    }

    std::string Node::PathText() const
    {
        return Join(path);
    }

    std::vector<std::string> GroupAbilityIds(const std::string& rootAbilityId, const Node& all)
    {
        struct Group { std::set<std::string> keys; double duration = NAN; };
        std::vector<Group> groups;
        for (const Node& child : all.children)
        {
            if (!IsGroupLeaf(child)) continue;
            double duration = NAN;
            const std::set<std::string> keys = LeafKeys(child, duration);
            Group* target = nullptr;
            if (groups.empty()) groups.emplace_back();
            for (Group& group : groups)
            {
                bool clash = false;
                for (const std::string& key : keys) clash = clash || group.keys.count(key) != 0;
                if (!clash && !std::isnan(duration) && !std::isnan(group.duration) && duration != group.duration) clash = true;
                if (!clash) { target = &group; break; }
            }
            if (target == nullptr)
            {
                groups.emplace_back();
                target = &groups.back();
            }
            target->keys.insert(keys.begin(), keys.end());
            if (!std::isnan(duration)) target->duration = duration;
        }
        std::vector<std::string> ids;
        if (groups.empty()) return ids;
        const std::string base = rootAbilityId + "__" + (all.path.empty() ? std::string("r") : all.PathText());
        ids.push_back(base);
        for (std::size_t index = 1; index < groups.size(); ++index)
        {
            ids.push_back(base + "_x" + std::to_string(index));
        }
        return ids;
    }

    const std::set<std::string>& SupportedCapabilities()
    {
        static const std::set<std::string> supported = {"choose", "delay", "repeat"};
        return supported;
    }

    const Spell* EffectSet::FindSpell(const std::string& abilityId) const
    {
        for (const Spell& spell : spells_)
        {
            if (spell.abilityId == abilityId) return &spell;
        }
        return nullptr;
    }

    std::string EffectSet::CastTarget(const std::string& spellOrAbility) const
    {
        for (const Spell& spell : spells_)
        {
            if (!spell.attached && spell.id == spellOrAbility) return spell.abilityId;
        }
        return spellOrAbility;
    }

    bool EffectSet::Load(const std::string& text, std::string& error)
    {
        mod_.clear();
        spells_.clear();
        triggers_.clear();
        requires_.clear();
        if (text.size() > MaximumDocument)
        {
            error = "document exceeds 256 KiB";
            return false;
        }
        Json root;
        if (!conditions::ParseJson(text, root, error)) return false;
        if (root.type != Json::Type::Object)
        {
            error = "top level must be an object";
            return false;
        }
        for (const auto& [key, value] : root.members)
        {
            if (key != "spec" && key != "mod" && key != "requires" && key != "spells" && key != "triggers")
            {
                error = "unknown top-level key '" + key + "'";
                return false;
            }
        }
        const Json* spec = root.Find("spec");
        if (spec == nullptr || spec->type != Json::Type::Number || spec->number != 1)
        {
            error = "spec must be 1";
            return false;
        }
        const Json* mod = root.Find("mod");
        if (mod == nullptr || mod->type != Json::Type::String || !ValidId(mod->text))
        {
            error = "mod must be the mod id";
            return false;
        }
        mod_ = mod->text;
        if (const Json* required = root.Find("requires"))
        {
            if (required->type != Json::Type::Array) { error = "requires must be a list"; return false; }
            for (const Json& item : required->items)
            {
                if (item.type != Json::Type::String) { error = "requires entries must be strings"; return false; }
                if (kCapabilities.count(item.text) == 0 && conditions::SupportedPredicates().count(item.text) == 0)
                {
                    error = "requires names an unknown capability or predicate '" + item.text + "'";
                    return false;
                }
                requires_.push_back(item.text);
            }
        }
        std::set<std::string> ids;
        if (const Json* spells = root.Find("spells"))
        {
            if (spells->type != Json::Type::Array || spells->items.size() > MaximumSpells)
            {
                error = "spells must be a list of at most 256";
                return false;
            }
            for (std::size_t index = 0; index < spells->items.size(); ++index)
            {
                const Json& json = spells->items[index];
                const std::string where = "spells[" + std::to_string(index) + "]";
                if (json.type != Json::Type::Object) { error = where + ": must be an object"; return false; }
                Spell spell;
                const Json* id = json.Find("id");
                const Json* ability = json.Find("ability");
                if ((id == nullptr) == (ability == nullptr))
                {
                    error = where + ": exactly one of 'id' or 'ability'";
                    return false;
                }
                static const std::set<std::string> spellKeys = {
                    "id", "ability", "name", "description", "image", "animation", "skill", "target", "radius",
                    "shape", "range", "min_range", "cooldown", "cast_time", "cost", "requirements", "slots",
                    "level", "effects", "note"};
                for (const auto& [key, value] : json.members)
                {
                    if (spellKeys.count(key) == 0) { error = where + ": unknown key '" + key + "'"; return false; }
                }
                if (id != nullptr)
                {
                    if (id->type != Json::Type::String || !ValidId(id->text))
                    {
                        error = where + ": id must match [A-Za-z0-9_]{1,64}";
                        return false;
                    }
                    if (!ids.insert(id->text).second) { error = where + ": duplicate id '" + id->text + "'"; return false; }
                    spell.id = id->text;
                    spell.abilityId = mod_ + "_" + id->text;
                }
                else
                {
                    if (!DataId(ability)) { error = where + ": ability must be a data id"; return false; }
                    spell.attached = true;
                    spell.abilityId = ability->text;
                }
                const Json* effects = json.Find("effects");
                if (effects == nullptr || effects->type != Json::Type::Array || effects->items.empty()
                    || effects->items.size() > MaximumList)
                {
                    error = where + ": effects must be a list of 1..32 effect nodes";
                    return false;
                }
                Json wrapped;
                wrapped.type = Json::Type::Object;
                wrapped.members.emplace_back("all", *effects);
                Builder builder;
                if (!builder.Build(wrapped, {}, where + ".effects", 0, spell.root))
                {
                    error = builder.error;
                    return false;
                }
                Classify(spell.root, true);
                ResolveGateConflicts(spell.root, {});
                spells_.push_back(std::move(spell));
            }
        }
        if (const Json* triggers = root.Find("triggers"))
        {
            if (triggers->type != Json::Type::Array || triggers->items.size() > MaximumSpells)
            {
                error = "triggers must be a list of at most 256";
                return false;
            }
            for (std::size_t index = 0; index < triggers->items.size(); ++index)
            {
                const Json& json = triggers->items[index];
                const std::string where = "triggers[" + std::to_string(index) + "]";
                if (json.type != Json::Type::Object) { error = where + ": must be an object"; return false; }
                static const std::set<std::string> triggerKeys = {
                    "id", "on", "chance", "when", "on_unavailable", "do", "level", "note"};
                for (const auto& [key, value] : json.members)
                {
                    if (triggerKeys.count(key) == 0) { error = where + ": unknown key '" + key + "'"; return false; }
                }
                Trigger trigger;
                const Json* id = json.Find("id");
                if (id == nullptr || id->type != Json::Type::String || !ValidId(id->text))
                {
                    error = where + ": id must match [A-Za-z0-9_]{1,64}";
                    return false;
                }
                if (!ids.insert(id->text).second) { error = where + ": duplicate id '" + id->text + "'"; return false; }
                trigger.id = id->text;
                trigger.abilityId = mod_ + "_ext_trigger_" + id->text;
                const Json* on = json.Find("on");
                if (on == nullptr || on->type != Json::Type::Object || on->members.size() != 1
                    || kEvents.count(on->members[0].first) == 0)
                {
                    error = where + ": on must name exactly one event";
                    return false;
                }
                trigger.eventKind = on->members[0].first;
                const Json& eventValue = on->members[0].second;
                if (trigger.eventKind == "cast")
                {
                    if (!DataId(&eventValue)) { error = where + ": on.cast must be an ability id"; return false; }
                    trigger.eventAbility = eventValue.text;
                }
                else if (eventValue.type != Json::Type::Bool || !eventValue.boolean)
                {
                    error = where + ": on." + trigger.eventKind + " must be true";
                    return false;
                }
                if (const Json* chance = json.Find("chance"))
                {
                    if (chance->type != Json::Type::Number || !(chance->number > 0.0 && chance->number <= 1.0))
                    {
                        error = where + ": chance must be above 0 and at most 1";
                        return false;
                    }
                    trigger.chance = chance->number;
                }
                if (const Json* policy = json.Find("on_unavailable"))
                {
                    if (policy->type != Json::Type::String || (policy->text != "inactive" && policy->text != "active"))
                    {
                        error = where + ": on_unavailable must be inactive or active";
                        return false;
                    }
                    trigger.activeWhenUnavailable = policy->text == "active";
                }
                if (const Json* when = json.Find("when"))
                {
                    int conditionNodes = 0;
                    std::string conditionError;
                    if (!BuildCondition(*when, 1, conditionNodes, trigger.when, conditionError))
                    {
                        error = where + ".when: " + conditionError;
                        return false;
                    }
                    trigger.hasWhen = true;
                }
                const Json* action = json.Find("do");
                if (action == nullptr) { error = where + ": missing 'do'"; return false; }
                Builder builder;
                if (!builder.Build(*action, {}, where + ".do", 1, trigger.action))
                {
                    error = builder.error;
                    return false;
                }
                const bool killCreate = trigger.eventKind == "kill" && trigger.action.kind == Node::Kind::Leaf
                    && trigger.action.leaf == "create_item";
                trigger.nativeEvent = trigger.eventKind == "cast" || trigger.eventKind == "attack" || killCreate;
                Classify(trigger.action, trigger.nativeEvent);
                if (trigger.nativeEvent) ResolveGateConflicts(trigger.action, {});
                triggers_.push_back(std::move(trigger));
            }
        }
        return true;
    }

    Resolution Resolve(
        const EffectSet& set, const Spell& spell, const conditions::Snapshot& state, const Rng& rng,
        const std::set<std::string>& capabilities)
    {
        Resolution out;
        if (!RequirementsMet(set, capabilities, out)) return out;
        Resolver resolver{set, spell.abilityId, state, rng, capabilities, out};
        resolver.Walk(spell.root, 0, false);
        return out;
    }

    Resolution ResolveEvent(
        const EffectSet& set, const Trigger& trigger, const conditions::Snapshot& state, const Rng& rng,
        const std::set<std::string>& capabilities)
    {
        Resolution out;
        if (!RequirementsMet(set, capabilities, out)) return out;
        if (!trigger.nativeEvent && capabilities.count("on_" + trigger.eventKind) == 0)
        {
            out.unresolved.push_back({"on", "capability on_" + trigger.eventKind + " not supported"});
            return out;
        }
        // The engine rolls the chance and enforces hoisted gates for its own
        // rows; for evaluator-owned parts the evaluator must do both.
        bool anyEvaluator = !trigger.nativeEvent;
        std::vector<const Node*> stack{&trigger.action};
        while (!stack.empty() && !anyEvaluator)
        {
            const Node* node = stack.back();
            stack.pop_back();
            if (!node->native) anyEvaluator = true;
            for (const Node& child : node->children) stack.push_back(&child);
        }
        if (!anyEvaluator) return out;
        if (trigger.chance < 1.0 && !(rng() < trigger.chance)) return out;
        if (trigger.hasWhen)
        {
            const Tri result = conditions::Evaluate(trigger.when, state);
            if (result == Tri::False) return out;
            if (result == Tri::Unknown && !trigger.activeWhenUnavailable)
            {
                out.unresolved.push_back({"when", "condition is unknown (fail closed)"});
                return out;
            }
        }
        Resolver resolver{set, trigger.abilityId, state, rng, capabilities, out};
        resolver.Walk(trigger.action, 0, !trigger.nativeEvent);
        return out;
    }
}
