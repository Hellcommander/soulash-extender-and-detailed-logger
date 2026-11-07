#pragma once

// Evaluator for skill-conditions files (spec 1, docs/skill-conditions.md).
//
// Pure data in, answer out: it reads a conditions document and a snapshot of
// game state that the host fills in, and reports whether a target should be
// active. It never touches the game. A snapshot field the host cannot supply is
// simply left unset and yields "unknown", which fails closed.

#include <map>
#include <set>
#include <string>
#include <vector>

namespace soulash::conditions
{
    enum class Tri { False, True, Unknown };

    template <class T>
    struct Optional
    {
        bool has = false;
        T value{};
        Optional() = default;
        Optional(const T& v) : has(true), value(v) {}
    };

    struct EquippedItem
    {
        std::string item;                 // data id
        std::string itemType;             // name from the game's item_type list
        std::set<std::string> itemTags;   // entity tag strings
    };

    // Names are compared case-insensitively. Unset fields are unknown.
    struct Snapshot
    {
        Optional<std::string> weaponClass;
        Optional<bool> shield;
        Optional<std::set<std::string>> holderTags;
        Optional<std::set<std::string>> targetTags;
        Optional<std::string> party;                          // "alone" or "in_party"
        Optional<double> healthPercent;
        Optional<double> staminaPercent;
        Optional<double> targetHealthPercent;
        Optional<std::map<std::string, double>> stackers;     // id -> stacks (absent id = 0)
        Optional<std::map<std::string, double>> inventory;    // item id -> count (absent = 0)
        Optional<std::map<std::string, double>> skills;       // skill id -> level (absent = 0)
        Optional<std::map<std::string, EquippedItem>> equipped;  // slot id -> item (absent = empty)
        Optional<std::string> dayPart;
        Optional<std::string> season;
        Optional<std::string> biome;
        Optional<bool> inCombat;
        Optional<bool> inSettlement;
        Optional<double> companions;
    };

    // Minimal JSON value (strict parser, see conditions.cpp).
    struct Json
    {
        enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
        bool boolean = false;
        double number = 0;
        std::string text;
        std::vector<Json> items;
        std::vector<std::pair<std::string, Json>> members;

        const Json* Find(const std::string& key) const;
    };

    struct Node
    {
        enum class Kind { All, Any, Not, Leaf } kind = Kind::Leaf;
        std::vector<Node> children;
        std::string predicate;   // Leaf only
        Json arguments;          // Leaf only
    };

    struct Condition
    {
        std::string id;
        std::string targetKind;  // "passive" or "ability"
        std::string targetId;
        Node when;
        bool activeWhenUnavailable = false;
    };

    // A conditional trigger: `cast` fires on `onKind` ("cast" of `onAbility`, or
    // "attack") with `chance`, only while `when` holds. Compiles to the
    // engine's own trigger effect plus a gating condition (see the validator's
    // --emit); this evaluator answers whether it is currently allowed to fire.
    struct Trigger
    {
        std::string id;
        std::string onKind;       // "cast" or "attack"
        std::string onAbility;    // for "cast"
        std::string castAbility;
        double chance = 1.0;
        Node when;
        bool activeWhenUnavailable = false;
    };

    class ConditionSet
    {
    public:
        // Parses and structurally validates a document. Limits: 64 KiB of text,
        // 64 nodes and depth 8 per expression, 32 items per list, unique ids and
        // targets. Predicate names and arguments are not checked here (use
        // tools/validate_conditions.py); an unknown predicate evaluates to unknown.
        bool Load(const std::string& text, std::string& error);

        const Condition* Find(const std::string& targetKind, const std::string& targetId) const;
        const std::vector<Condition>& Items() const { return items_; }
        const std::vector<Trigger>& Triggers() const { return triggers_; }
        const std::vector<std::string>& Requires() const { return requires_; }

    private:
        std::vector<Condition> items_;
        std::vector<Trigger> triggers_;
        std::vector<std::string> requires_;
    };

    // The strict JSON parser used by the loader (shared with src/effects.cpp).
    bool ParseJson(const std::string& text, Json& out, std::string& error);

    // Predicates this evaluator implements.
    const std::set<std::string>& SupportedPredicates();

    Tri Evaluate(const Node& expression, const Snapshot& state);

    // Should the target be active? Applies `on_unavailable` to unknown results
    // and when the file requires a predicate not in `supported`.
    bool IsActive(
        const ConditionSet& set,
        const Condition& condition,
        const Snapshot& state,
        const std::set<std::string>& supported = SupportedPredicates());

    // May this trigger fire right now? Same fail-closed rules as IsActive.
    bool IsTriggerAllowed(
        const ConditionSet& set,
        const Trigger& trigger,
        const Snapshot& state,
        const std::set<std::string>& supported = SupportedPredicates());
}
