// Rule evaluation for the Triggers plugin (docs/triggers.md section 2): which
// extender abilities should be queued for a holder when an event happens.
// Pure (no game access, no Windows calls) so it is unit-tested; the plugin
// supplies the event, the state snapshot and the random source.
//
// Sources, per mod:
//   * <mod>\extender\conditions.json `triggers` (docs/skill-conditions.md):
//     {on: {cast: A} | {attack|kill|turn|damaged|dodge|block|crit: true},
//     cast: B, chance, when}. With the plugin installed the extender itself is
//     the executor: the rule fires only while `when` holds (IsTriggerAllowed,
//     fail closed) and the chance rolls.
//   * <mod>\extender\effects.json `triggers` (docs/effects.md 4.3): the
//     evaluator-owned parts of triggers, through ResolveEvent. Casts with a
//     delay are returned separately (Outcome::delayed); the plugin fires them
//     after that many turns of the holder, or drops them when it has no turn
//     source.
#pragma once

#include "conditions.hpp"
#include "effects.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace soulash_plugins::trigger_rules
{
    // Order matters: EventName indexes by it.
    enum class EventKind { Cast, Attack, Kill, Turn, Damaged, Dodge, Block, Crit };
    constexpr unsigned int EventKindCount = 8;

    inline const char* EventName(EventKind kind)
    {
        static const char* const names[EventKindCount] = {
            "cast", "attack", "kill", "turn", "damaged", "dodge", "block", "crit"};
        const auto index = static_cast<unsigned int>(kind);
        return index < EventKindCount ? names[index] : "?";
    }

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
        unsigned int delayTurns = 0;   // 0 = now; otherwise after that many turns of the holder
    };

    struct Outcome
    {
        unsigned matched = 0;              // rules whose event matched
        unsigned allowed = 0;              // ... whose condition held
        unsigned blockedByCondition = 0;   // false or unknown (fail closed)
        unsigned blockedByChance = 0;
        std::vector<Cast> casts;           // queue now
        std::vector<Cast> delayed;         // queue after Cast::delayTurns turns of the holder
        std::vector<std::string> notes;    // dropped casts, unresolved parts
    };

    // Predicates the live game registry can answer (plugins/common/game_state.hpp).
    // A file that `requires` anything else is unavailable and fails closed.
    inline const std::set<std::string>& BackedPredicates()
    {
        static const std::set<std::string> backed = {
            "weapon_class", "shield", "holder_tag", "target_tag", "health_percent", "stamina_percent",
            "target_health_percent", "equipped", "skill_level", "stacker", "party", "companions", "time_of_day",
            "season", "inventory_count"};
        return backed;
    }

    inline bool Matches(const soulash::conditions::Trigger& trigger, const Event& event)
    {
        if (trigger.onKind != EventName(event.kind)) return false;
        return event.kind != EventKind::Cast || trigger.onAbility == event.abilityId;
    }

    inline bool Matches(const soulash::effects::Trigger& trigger, const Event& event)
    {
        if (trigger.eventKind != EventName(event.kind)) return false;
        return event.kind != EventKind::Cast || trigger.eventAbility == event.abilityId;
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

    // What the resolver may rely on: its own cast realisations, the backed
    // predicates and every event kind (the host only delivers events it hooks).
    inline std::set<std::string> ResolverCapabilities()
    {
        std::set<std::string> capabilities = soulash::effects::SupportedCapabilities();
        for (const std::string& predicate : BackedPredicates()) capabilities.insert(predicate);
        for (unsigned int kind = 0; kind < EventKindCount; ++kind)
        {
            capabilities.insert(std::string("on_") + EventName(static_cast<EventKind>(kind)));
        }
        return capabilities;
    }

    // turnSource: the host can fire a cast after N turns of the holder; without
    // it delayed casts are dropped and noted.
    inline Outcome Evaluate(const std::vector<ModRules>& mods, const Event& event,
        const soulash::conditions::Snapshot& state, const std::function<double()>& rng, unsigned maxCasts,
        bool turnSource = false)
    {
        Outcome out;
        const std::set<std::string> capabilities = ResolverCapabilities();
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
                        out.casts.push_back({trigger.castAbility, trigger.id, mod.mod, 0});
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
                            if (!turnSource)
                            {
                                out.notes.push_back(mod.mod + "/" + trigger.id + ": delayed cast of " + cast.abilityId
                                    + " dropped (no turn source)");
                            }
                            else if (out.delayed.size() < maxCasts)
                            {
                                out.delayed.push_back({cast.abilityId, trigger.id, mod.mod,
                                    static_cast<unsigned int>(cast.delayTurns)});
                            }
                            else
                            {
                                out.notes.push_back(mod.mod + "/" + trigger.id + ": over the per-event delayed cast limit");
                            }
                            continue;
                        }
                        if (out.casts.size() < maxCasts)
                        {
                            out.casts.push_back({cast.abilityId, trigger.id, mod.mod, 0});
                        }
                    }
                }
            }
        }
        return out;
    }
}
