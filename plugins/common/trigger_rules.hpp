// Rule evaluation for the Triggers plugin (docs/triggers.md section 2): which
// extender abilities should be queued for a holder when an event happens.
// Pure (no game access, no Windows calls) so it is unit-tested; the plugin
// supplies the event, the state snapshot and the random source.
//
// Sources, per mod:
//   * <mod>\extender\conditions.json `triggers` (docs/skill-conditions.md):
//     {on: {cast: A} | {attack: true}, cast: B, chance, when}. With the plugin
//     installed the extender itself is the executor: the rule fires only while
//     `when` holds (IsTriggerAllowed, fail closed) and the chance rolls.
//   * <mod>\extender\effects.json `triggers` (docs/effects.md 4.3): the
//     evaluator-owned parts of cast / attack triggers, through ResolveEvent.
//     Casts with a delay are dropped (no turn source yet) and reported.
#pragma once

#include "conditions.hpp"
#include "effects.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace soulash_plugins::trigger_rules
{
    enum class EventKind { Cast, Attack };

    struct Event
    {
        EventKind kind = EventKind::Cast;
        std::string abilityId;   // Cast: the ability that was cast
    };

    struct ModRules
    {
        std::string mod;
        bool hasConditions = false;
        soulash::conditions::ConditionSet conditions;
        bool hasEffects = false;
        soulash::effects::EffectSet effects;
    };

    struct Cast
    {
        std::string abilityId;
        std::string rule;    // trigger id
        std::string mod;
    };

    struct Outcome
    {
        unsigned matched = 0;              // rules whose event matched
        unsigned allowed = 0;              // ... whose condition held
        unsigned blockedByCondition = 0;   // false or unknown (fail closed)
        unsigned blockedByChance = 0;
        std::vector<Cast> casts;
        std::vector<std::string> notes;    // dropped delayed casts, unresolved parts
    };

    // Predicates the live game registry can answer (plugins/common/game_state.hpp).
    // A file that `requires` anything else is unavailable and fails closed.
    inline const std::set<std::string>& BackedPredicates()
    {
        static const std::set<std::string> backed = {
            "weapon_class", "shield", "holder_tag", "target_tag", "health_percent", "stamina_percent",
            "target_health_percent", "equipped", "skill_level"};
        return backed;
    }

    inline bool Matches(const soulash::conditions::Trigger& trigger, const Event& event)
    {
        if (event.kind == EventKind::Cast)
        {
            return trigger.onKind == "cast" && trigger.onAbility == event.abilityId;
        }
        return trigger.onKind == "attack";
    }

    inline bool Matches(const soulash::effects::Trigger& trigger, const Event& event)
    {
        if (event.kind == EventKind::Cast)
        {
            return trigger.eventKind == "cast" && trigger.eventAbility == event.abilityId;
        }
        return trigger.eventKind == "attack";
    }

    // Cheap pre-check so events without rules cost no snapshot.
    inline bool AnyRuleFor(const std::vector<ModRules>& mods, const Event& event)
    {
        for (const ModRules& mod : mods)
        {
            if (mod.hasConditions)
            {
                for (const auto& trigger : mod.conditions.Triggers())
                {
                    if (Matches(trigger, event)) return true;
                }
            }
            if (mod.hasEffects)
            {
                for (const auto& trigger : mod.effects.Triggers())
                {
                    if (Matches(trigger, event)) return true;
                }
            }
        }
        return false;
    }

    inline Outcome Evaluate(const std::vector<ModRules>& mods, const Event& event,
        const soulash::conditions::Snapshot& state, const std::function<double()>& rng, unsigned maxCasts)
    {
        Outcome out;
        for (const ModRules& mod : mods)
        {
            if (mod.hasConditions)
            {
                for (const auto& trigger : mod.conditions.Triggers())
                {
                    if (!Matches(trigger, event)) continue;
                    ++out.matched;
                    if (!soulash::conditions::IsTriggerAllowed(mod.conditions, trigger, state, BackedPredicates()))
                    {
                        ++out.blockedByCondition;
                        continue;
                    }
                    ++out.allowed;
                    if (trigger.chance < 1.0 && !(rng() < trigger.chance))
                    {
                        ++out.blockedByChance;
                        continue;
                    }
                    if (out.casts.size() < maxCasts)
                    {
                        out.casts.push_back({trigger.castAbility, trigger.id, mod.mod});
                    }
                    else
                    {
                        out.notes.push_back(mod.mod + "/" + trigger.id + ": over the per-event cast limit");
                    }
                }
            }
            if (mod.hasEffects)
            {
                for (const auto& trigger : mod.effects.Triggers())
                {
                    if (!Matches(trigger, event)) continue;
                    ++out.matched;
                    std::set<std::string> capabilities = soulash::effects::SupportedCapabilities();
                    for (const std::string& predicate : BackedPredicates()) capabilities.insert(predicate);
                    const soulash::effects::Resolution resolution =
                        soulash::effects::ResolveEvent(mod.effects, trigger, state, rng, capabilities);
                    bool conditionBlocked = false;
                    for (const auto& unresolved : resolution.unresolved)
                    {
                        if (unresolved.path == "when") conditionBlocked = true;
                        out.notes.push_back(mod.mod + "/" + trigger.id + " " + unresolved.path + ": " + unresolved.reason);
                    }
                    if (conditionBlocked)
                    {
                        ++out.blockedByCondition;
                        continue;
                    }
                    ++out.allowed;
                    for (const auto& cast : resolution.casts)
                    {
                        if (cast.delayTurns > 0)
                        {
                            out.notes.push_back(mod.mod + "/" + trigger.id + ": delayed cast of " + cast.abilityId
                                + " dropped (no turn source yet)");
                            continue;
                        }
                        if (out.casts.size() < maxCasts)
                        {
                            out.casts.push_back({cast.abilityId, trigger.id, mod.mod});
                        }
                    }
                }
            }
        }
        return out;
    }
}
