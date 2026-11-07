#pragma once

// Loader and resolver for extender effects files (spec 1, docs/effects.md).
//
// The compiler (tools/effects_compile.py) turns a spell's effect tree into
// engine data for everything the engine can do itself. This module covers the
// rest: it loads the same file strictly, classifies every node as native or
// evaluator-owned by the same rule as the compiler, and `Resolve` walks the
// evaluator-owned parts to answer "which generated abilities should the holder
// cast now or in N turns". It never touches the game; a future hook source
// supplies the snapshot and performs the casts. Everything fails closed: a
// missing capability, an unknown condition or a leaf with no cast realisation
// yields no cast and is listed as unresolved.

#include "conditions.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace soulash::effects
{
    struct Node
    {
        enum class Kind { All, Chance, Repeat, Choose, Delay, When, Leaf } kind = Kind::All;
        std::vector<Node> children;
        std::vector<int> path;              // child indices from the spell root
        std::string leaf;                   // Leaf: effect name
        conditions::Json arguments;         // Leaf: arguments
        double chance = 1.0;                // Chance; Choose children: weight share
        int count = 1;                      // Repeat
        int every = 0;                      // Repeat: turns between repetitions (0 = at once)
        int delay = 0;                      // Delay
        bool approximate = false;           // Choose
        conditions::Node when;              // When
        bool activeWhenUnavailable = false; // When
        bool native = true;                 // backend ownership (docs/effects.md 4.2)

        std::string PathText() const;       // "root", "1_0", ...
    };

    struct Spell
    {
        std::string id;          // local id ("" when attached)
        std::string abilityId;   // generated root ability, or the existing one when attached
        bool attached = false;
        Node root;               // an All node
    };

    struct Trigger
    {
        std::string id;
        std::string eventKind;    // cast, attack, kill, turn, damaged, dodge, block, crit
        std::string eventAbility; // for cast
        double chance = 1.0;
        bool hasWhen = false;
        conditions::Node when;
        bool activeWhenUnavailable = false;
        Node action;
        std::string abilityId;    // <mod>_ext_trigger_<id>: root for generated groups
        bool nativeEvent = false; // cast / attack (and kill+create_item) are the engine's
    };

    class EffectSet
    {
    public:
        // Strict load. Limits: 256 KiB, 256 spells and triggers, 64 nodes and
        // depth 8 per tree, 32 items per list. Unknown keys are errors.
        bool Load(const std::string& text, std::string& error);

        const std::string& Mod() const { return mod_; }
        const std::vector<Spell>& Spells() const { return spells_; }
        const std::vector<Trigger>& Triggers() const { return triggers_; }
        const std::vector<std::string>& Requires() const { return requires_; }
        const Spell* FindSpell(const std::string& abilityId) const;

        // Resolves a `cast` target: a local spell id becomes <mod>_<id>.
        std::string CastTarget(const std::string& spellOrAbility) const;

    private:
        std::string mod_;
        std::vector<Spell> spells_;
        std::vector<Trigger> triggers_;
        std::vector<std::string> requires_;
    };

    struct Cast
    {
        std::string abilityId;
        int delayTurns = 0;
    };

    struct Unresolved
    {
        std::string path;
        std::string reason;
    };

    struct Resolution
    {
        std::vector<Cast> casts;
        std::vector<Unresolved> unresolved;
    };

    // Uniform random number in [0, 1).
    using Rng = std::function<double()>;

    // Capabilities this resolver realises as casts: choose, delay, repeat.
    // Leaves without a cast realisation (remove_stacks, teleport, grant_xp) and
    // non-native events need a host primitive that does not exist yet.
    const std::set<std::string>& SupportedCapabilities();

    // The root ability of `spell` was cast: which generated abilities follow?
    // Native parts emit nothing (the engine does them). `capabilities` is what
    // the host supports; `requires` entries outside it make the whole set
    // unavailable.
    Resolution Resolve(
        const EffectSet& set, const Spell& spell, const conditions::Snapshot& state, const Rng& rng,
        const std::set<std::string>& capabilities = SupportedCapabilities());

    // The trigger's event happened for the holder.
    Resolution ResolveEvent(
        const EffectSet& set, const Trigger& trigger, const conditions::Snapshot& state, const Rng& rng,
        const std::set<std::string>& capabilities = SupportedCapabilities());

    // Generated ability ids for the leaf groups of an evaluator-owned All node
    // (the compiler's first-fit merge: conflicting keys split into _x1, _x2 ...).
    std::vector<std::string> GroupAbilityIds(const std::string& rootAbilityId, const Node& all);
}
