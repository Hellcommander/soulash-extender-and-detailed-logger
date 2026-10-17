#include "conditions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace soulash::conditions
{
    namespace
    {
        constexpr std::size_t MaximumDocument = 64 * 1024;
        constexpr int MaximumJsonDepth = 32;
        constexpr int MaximumExpressionDepth = 8;
        constexpr int MaximumNodes = 64;
        constexpr std::size_t MaximumList = 32;

        std::string Lower(const std::string& value)
        {
            std::string out = value;
            for (char& c : out)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        // ---- strict JSON ------------------------------------------------
        class Parser
        {
        public:
            Parser(const std::string& text) : text_(text) {}

            bool Parse(Json& out, std::string& error)
            {
                SkipSpace();
                if (!Value(out, 0))
                {
                    error = error_.empty() ? "invalid JSON" : error_;
                    return false;
                }
                SkipSpace();
                if (position_ != text_.size())
                {
                    error = "unexpected text after the document";
                    return false;
                }
                return true;
            }

        private:
            bool Fail(const char* message)
            {
                if (error_.empty())
                {
                    error_ = std::string(message) + " at offset " + std::to_string(position_);
                }
                return false;
            }

            void SkipSpace()
            {
                while (position_ < text_.size()
                    && (text_[position_] == ' ' || text_[position_] == '\t'
                        || text_[position_] == '\n' || text_[position_] == '\r'))
                {
                    ++position_;
                }
            }

            bool Literal(const char* word)
            {
                const std::size_t length = std::char_traits<char>::length(word);
                if (text_.compare(position_, length, word) != 0)
                {
                    return false;
                }
                position_ += length;
                return true;
            }

            bool String(std::string& out)
            {
                if (position_ >= text_.size() || text_[position_] != '"')
                {
                    return Fail("expected a string");
                }
                ++position_;
                out.clear();
                while (position_ < text_.size())
                {
                    const unsigned char c = static_cast<unsigned char>(text_[position_++]);
                    if (c == '"')
                    {
                        return true;
                    }
                    if (c < 0x20)
                    {
                        return Fail("control character in string");
                    }
                    if (c != '\\')
                    {
                        out.push_back(static_cast<char>(c));
                        continue;
                    }
                    if (position_ >= text_.size())
                    {
                        break;
                    }
                    const char escape = text_[position_++];
                    switch (escape)
                    {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        if (position_ + 4 > text_.size())
                        {
                            return Fail("short unicode escape");
                        }
                        unsigned code = 0;
                        for (int digit = 0; digit < 4; ++digit)
                        {
                            const char h = text_[position_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else return Fail("bad unicode escape");
                        }
                        if (code >= 0xD800 && code <= 0xDFFF)
                        {
                            return Fail("surrogate pairs are not supported");
                        }
                        if (code < 0x80)
                        {
                            out.push_back(static_cast<char>(code));
                        }
                        else if (code < 0x800)
                        {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default:
                        return Fail("bad escape");
                    }
                }
                return Fail("unterminated string");
            }

            bool Number(Json& out)
            {
                const std::size_t start = position_;
                if (position_ < text_.size() && text_[position_] == '-') ++position_;
                std::size_t digits = 0;
                while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    ++position_;
                    ++digits;
                }
                if (digits == 0)
                {
                    return Fail("expected a number");
                }
                if (position_ < text_.size() && text_[position_] == '.')
                {
                    ++position_;
                    std::size_t fraction = 0;
                    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                    {
                        ++position_;
                        ++fraction;
                    }
                    if (fraction == 0) return Fail("bad fraction");
                }
                if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E'))
                {
                    ++position_;
                    if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
                    std::size_t exponent = 0;
                    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                    {
                        ++position_;
                        ++exponent;
                    }
                    if (exponent == 0) return Fail("bad exponent");
                }
                out.type = Json::Type::Number;
                out.number = std::strtod(text_.substr(start, position_ - start).c_str(), nullptr);
                return std::isfinite(out.number) ? true : Fail("number out of range");
            }

            bool Value(Json& out, int depth)
            {
                if (depth > MaximumJsonDepth)
                {
                    return Fail("nesting too deep");
                }
                if (position_ >= text_.size())
                {
                    return Fail("unexpected end");
                }
                const char c = text_[position_];
                if (c == '{')
                {
                    ++position_;
                    out.type = Json::Type::Object;
                    SkipSpace();
                    if (position_ < text_.size() && text_[position_] == '}')
                    {
                        ++position_;
                        return true;
                    }
                    for (;;)
                    {
                        SkipSpace();
                        std::string key;
                        if (!String(key)) return false;
                        for (const auto& member : out.members)
                        {
                            if (member.first == key) return Fail("duplicate object key");
                        }
                        SkipSpace();
                        if (position_ >= text_.size() || text_[position_++] != ':') return Fail("expected ':'");
                        SkipSpace();
                        Json value;
                        if (!Value(value, depth + 1)) return false;
                        out.members.emplace_back(std::move(key), std::move(value));
                        SkipSpace();
                        if (position_ >= text_.size()) return Fail("unterminated object");
                        const char next = text_[position_++];
                        if (next == '}') return true;
                        if (next != ',') return Fail("expected ',' or '}'");
                    }
                }
                if (c == '[')
                {
                    ++position_;
                    out.type = Json::Type::Array;
                    SkipSpace();
                    if (position_ < text_.size() && text_[position_] == ']')
                    {
                        ++position_;
                        return true;
                    }
                    for (;;)
                    {
                        SkipSpace();
                        Json value;
                        if (!Value(value, depth + 1)) return false;
                        out.items.push_back(std::move(value));
                        SkipSpace();
                        if (position_ >= text_.size()) return Fail("unterminated array");
                        const char next = text_[position_++];
                        if (next == ']') return true;
                        if (next != ',') return Fail("expected ',' or ']'");
                    }
                }
                if (c == '"')
                {
                    out.type = Json::Type::String;
                    return String(out.text);
                }
                if (Literal("true")) { out.type = Json::Type::Bool; out.boolean = true; return true; }
                if (Literal("false")) { out.type = Json::Type::Bool; out.boolean = false; return true; }
                if (Literal("null")) { out.type = Json::Type::Null; return true; }
                return Number(out);
            }

            const std::string& text_;
            std::size_t position_ = 0;
            std::string error_;
        };

        // ---- document -> conditions ---------------------------------------
        bool BuildNode(const Json& json, int depth, int& nodes, Node& out, std::string& error)
        {
            if (++nodes > MaximumNodes)
            {
                error = "expression has more than 64 nodes";
                return false;
            }
            if (depth > MaximumExpressionDepth)
            {
                error = "expression is deeper than 8 levels";
                return false;
            }
            if (json.type != Json::Type::Object || json.members.size() != 1)
            {
                error = "an expression is an object with exactly one key";
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
                out.kind = key == "all" ? Node::Kind::All : Node::Kind::Any;
                out.children.resize(value.items.size());
                for (std::size_t index = 0; index < value.items.size(); ++index)
                {
                    if (!BuildNode(value.items[index], depth + 1, nodes, out.children[index], error))
                    {
                        return false;
                    }
                }
                return true;
            }
            if (key == "not")
            {
                out.kind = Node::Kind::Not;
                out.children.resize(1);
                return BuildNode(value, depth + 1, nodes, out.children[0], error);
            }
            out.kind = Node::Kind::Leaf;
            out.predicate = key;
            out.arguments = value;
            return true;
        }

        const Json* Field(const Json& object, const char* name)
        {
            return object.type == Json::Type::Object ? object.Find(name) : nullptr;
        }

        // ---- leaves -------------------------------------------------------
        Tri FromBool(bool value) { return value ? Tri::True : Tri::False; }

        // Name or list of names -> lower-cased set; false when malformed.
        bool NameSet(const Json& json, std::set<std::string>& out)
        {
            out.clear();
            if (json.type == Json::Type::String)
            {
                out.insert(Lower(json.text));
                return true;
            }
            if (json.type != Json::Type::Array || json.items.empty() || json.items.size() > MaximumList)
            {
                return false;
            }
            for (const Json& item : json.items)
            {
                if (item.type != Json::Type::String) return false;
                out.insert(Lower(item.text));
            }
            return true;
        }

        std::set<std::string> LowerSet(const std::set<std::string>& values)
        {
            std::set<std::string> out;
            for (const std::string& value : values) out.insert(Lower(value));
            return out;
        }

        Tri AnyOfName(const Json& args, const Optional<std::string>& actual)
        {
            std::set<std::string> wanted;
            if (!NameSet(args, wanted) || !actual.has) return Tri::Unknown;
            return FromBool(wanted.count(Lower(actual.value)) != 0);
        }

        Tri AnyOfTags(const Json& args, const Optional<std::set<std::string>>& actual)
        {
            std::set<std::string> wanted;
            if (!NameSet(args, wanted) || !actual.has) return Tri::Unknown;
            const std::set<std::string> held = LowerSet(actual.value);
            for (const std::string& name : wanted)
            {
                if (held.count(name) != 0) return Tri::True;
            }
            return Tri::False;
        }

        // {min?, max?} object (inclusive), default minimum applied by the caller.
        bool Range(const Json& range, double defaultMin, double& lo, double& hi, bool requireBound)
        {
            lo = defaultMin;
            hi = INFINITY;
            if (range.type != Json::Type::Object) return false;
            bool bounded = false;
            for (const auto& [key, value] : range.members)
            {
                if ((key == "min" || key == "max") && value.type == Json::Type::Number)
                {
                    (key == "min" ? lo : hi) = value.number;
                    bounded = true;
                }
            }
            return !requireBound || bounded;
        }

        Tri InRange(const Json& args, const Optional<double>& actual)
        {
            double lo = -INFINITY, hi = INFINITY;
            if (!Range(args, -INFINITY, lo, hi, true) || !actual.has) return Tri::Unknown;
            return FromBool(actual.value >= lo && actual.value <= hi);
        }

        // {<idKey>: "x", min?, max?} against a count map (absent id = 0).
        Tri CountOf(
            const Json& args, const char* idKey, double defaultMin,
            const Optional<std::map<std::string, double>>& counts)
        {
            const Json* id = Field(args, idKey);
            double lo = 0, hi = 0;
            if (id == nullptr || id->type != Json::Type::String || !counts.has
                || !Range(args, defaultMin, lo, hi, false))
            {
                return Tri::Unknown;
            }
            // With only a max, "at most N" reads naturally from zero.
            if (args.Find("max") != nullptr && args.Find("min") == nullptr)
            {
                lo = 0;
            }
            const auto found = counts.value.find(id->text);
            const double count = found == counts.value.end() ? 0.0 : found->second;
            return FromBool(count >= lo && count <= hi);
        }

        Tri Equipped(const Json& args, const Snapshot& state)
        {
            const Json* slot = Field(args, "slot");
            if (slot == nullptr || slot->type != Json::Type::String || !state.equipped.has
                || args.members.size() != 2)
            {
                return Tri::Unknown;
            }
            const auto found = state.equipped.value.find(slot->text);
            const bool empty = found == state.equipped.value.end();
            for (const auto& [key, value] : args.members)
            {
                if (key == "slot") continue;
                if (key == "empty" && value.type == Json::Type::Bool)
                {
                    return FromBool(empty == value.boolean);
                }
                if (empty || value.type != Json::Type::String)
                {
                    return value.type == Json::Type::String ? Tri::False : Tri::Unknown;
                }
                if (key == "item") return FromBool(found->second.item == value.text);
                if (key == "item_type") return FromBool(Lower(found->second.itemType) == Lower(value.text));
                if (key == "item_tag")
                {
                    for (const std::string& tag : found->second.itemTags)
                    {
                        if (tag == value.text) return Tri::True;
                    }
                    return Tri::False;
                }
            }
            return Tri::Unknown;
        }

        Tri Leaf(const std::string& name, const Json& args, const Snapshot& s)
        {
            if (name == "weapon_class") return AnyOfName(args, s.weaponClass);
            if (name == "shield")
            {
                if (args.type != Json::Type::Bool || !s.shield.has) return Tri::Unknown;
                return FromBool(s.shield.value == args.boolean);
            }
            if (name == "holder_tag") return AnyOfTags(args, s.holderTags);
            if (name == "target_tag") return AnyOfTags(args, s.targetTags);
            if (name == "party")
            {
                if (args.type != Json::Type::String || !s.party.has) return Tri::Unknown;
                return FromBool(Lower(s.party.value) == Lower(args.text));
            }
            if (name == "health_percent") return InRange(args, s.healthPercent);
            if (name == "stamina_percent") return InRange(args, s.staminaPercent);
            if (name == "target_health_percent") return InRange(args, s.targetHealthPercent);
            if (name == "stacker") return CountOf(args, "id", 1, s.stackers);
            if (name == "inventory_count") return CountOf(args, "item", 1, s.inventory);
            if (name == "skill_level") return CountOf(args, "skill", 0, s.skills);
            if (name == "equipped") return Equipped(args, s);
            if (name == "time_of_day") return AnyOfName(args, s.dayPart);
            if (name == "season") return AnyOfName(args, s.season);
            if (name == "biome") return AnyOfName(args, s.biome);
            if (name == "in_combat")
            {
                if (args.type != Json::Type::Bool || !s.inCombat.has) return Tri::Unknown;
                return FromBool(s.inCombat.value == args.boolean);
            }
            if (name == "in_settlement")
            {
                if (args.type != Json::Type::Bool || !s.inSettlement.has) return Tri::Unknown;
                return FromBool(s.inSettlement.value == args.boolean);
            }
            if (name == "companions") return InRange(args, s.companions);
            return Tri::Unknown; // unsupported predicate
        }
    }

    const Json* Json::Find(const std::string& key) const
    {
        for (const auto& member : members)
        {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }

    bool ConditionSet::Load(const std::string& text, std::string& error)
    {
        items_.clear();
        requires_.clear();
        if (text.size() > MaximumDocument)
        {
            error = "document exceeds 64 KiB";
            return false;
        }
        Json root;
        if (!Parser(text).Parse(root, error))
        {
            return false;
        }
        if (root.type != Json::Type::Object)
        {
            error = "top level must be an object";
            return false;
        }
        for (const auto& [key, value] : root.members)
        {
            if (key != "spec" && key != "requires" && key != "conditions")
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
        if (const Json* required = root.Find("requires"))
        {
            if (required->type != Json::Type::Array)
            {
                error = "requires must be a list";
                return false;
            }
            for (const Json& item : required->items)
            {
                if (item.type != Json::Type::String)
                {
                    error = "requires entries must be strings";
                    return false;
                }
                requires_.push_back(item.text);
            }
        }
        const Json* conditions = root.Find("conditions");
        if (conditions == nullptr || conditions->type != Json::Type::Array)
        {
            error = "conditions must be a list";
            return false;
        }

        std::set<std::string> ids;
        std::set<std::string> targets;
        for (const Json& entry : conditions->items)
        {
            Condition condition;
            const Json* id = Field(entry, "id");
            const Json* target = Field(entry, "target");
            const Json* when = Field(entry, "when");
            if (id == nullptr || id->type != Json::Type::String || target == nullptr
                || target->type != Json::Type::Object || when == nullptr)
            {
                error = "each condition needs id, target and when";
                return false;
            }
            const Json* kind = target->Find("kind");
            const Json* targetId = target->Find("id");
            if (kind == nullptr || kind->type != Json::Type::String
                || (kind->text != "passive" && kind->text != "ability")
                || targetId == nullptr || targetId->type != Json::Type::String || targetId->text.empty())
            {
                error = "target must be {kind: passive|ability, id}";
                return false;
            }
            condition.id = id->text;
            condition.targetKind = kind->text;
            condition.targetId = targetId->text;
            if (!ids.insert(condition.id).second)
            {
                error = "duplicate condition id '" + condition.id + "'";
                return false;
            }
            if (!targets.insert(condition.targetKind + ":" + condition.targetId).second)
            {
                error = "second condition for the same target";
                return false;
            }
            if (const Json* policy = entry.Find("on_unavailable"))
            {
                if (policy->type != Json::Type::String
                    || (policy->text != "inactive" && policy->text != "active"))
                {
                    error = "on_unavailable must be inactive or active";
                    return false;
                }
                condition.activeWhenUnavailable = policy->text == "active";
            }
            int nodes = 0;
            if (!BuildNode(*when, 1, nodes, condition.when, error))
            {
                return false;
            }
            items_.push_back(std::move(condition));
        }
        error.clear();
        return true;
    }

    const Condition* ConditionSet::Find(const std::string& targetKind, const std::string& targetId) const
    {
        for (const Condition& condition : items_)
        {
            if (condition.targetKind == targetKind && condition.targetId == targetId)
            {
                return &condition;
            }
        }
        return nullptr;
    }

    const std::set<std::string>& SupportedPredicates()
    {
        static const std::set<std::string> supported = {
            "weapon_class", "shield", "holder_tag", "target_tag", "party", "health_percent",
            "stamina_percent", "target_health_percent", "stacker", "equipped", "inventory_count",
            "skill_level", "time_of_day", "season", "biome", "in_combat", "in_settlement",
            "companions"};
        return supported;
    }

    Tri Evaluate(const Node& expression, const Snapshot& state)
    {
        switch (expression.kind)
        {
        case Node::Kind::Leaf:
            return Leaf(expression.predicate, expression.arguments, state);
        case Node::Kind::Not:
        {
            const Tri inner = Evaluate(expression.children[0], state);
            return inner == Tri::Unknown ? Tri::Unknown : (inner == Tri::True ? Tri::False : Tri::True);
        }
        case Node::Kind::All:
        {
            bool unknown = false;
            for (const Node& child : expression.children)
            {
                const Tri value = Evaluate(child, state);
                if (value == Tri::False) return Tri::False;
                unknown = unknown || value == Tri::Unknown;
            }
            return unknown ? Tri::Unknown : Tri::True;
        }
        case Node::Kind::Any:
        {
            bool unknown = false;
            for (const Node& child : expression.children)
            {
                const Tri value = Evaluate(child, state);
                if (value == Tri::True) return Tri::True;
                unknown = unknown || value == Tri::Unknown;
            }
            return unknown ? Tri::Unknown : Tri::False;
        }
        }
        return Tri::Unknown;
    }

    bool IsActive(
        const ConditionSet& set,
        const Condition& condition,
        const Snapshot& state,
        const std::set<std::string>& supported)
    {
        for (const std::string& name : set.Requires())
        {
            if (supported.count(name) == 0)
            {
                return condition.activeWhenUnavailable;
            }
        }
        switch (Evaluate(condition.when, state))
        {
        case Tri::True: return true;
        case Tri::False: return false;
        case Tri::Unknown: break;
        }
        return condition.activeWhenUnavailable;
    }
}
