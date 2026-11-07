// <mod>\extender\throws.json: what happens when the player really throws an
// item (the game's own Throw action, not an item ability) and it lands
// (docs/triggers.md section 8). Pure: parsing and lookup only; the Triggers
// plugin supplies the event and does the casting.
//
//   {
//     "throws": [
//       {
//         "id": "fire_rune",                                   optional, defaults to the first item
//         "item": "mymod_Rune_of_Fire" | ["id", ...],         the thrown item's data id(s)
//         "cast": "mymod_Fire_Burst" | ["id", ...],           1-8 abilities, cast by the thrower AT THE LANDING TILE
//         "chance": 1.0,                                      optional, 0-1, one roll for the whole rule
//         "consume": true,                                    optional (default true): the item is used up
//         "note": "free text"
//       }
//     ]
//   }
//
// Every cast is the engine's queued cast (as for the other Triggers rules),
// aimed at the landing tile instead of the holder's target. The engine marks
// queued casts "no triggers" (AbilityUser+0x258), so they do not fire further
// `on: {cast: ...}` rules: a multi-part spell lists all its parts in `cast`.
#pragma once

#include "conditions.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace soulash_plugins::throw_rules
{
    constexpr std::size_t MaximumRules = 512;
    constexpr std::size_t MaximumItemsPerRule = 64;
    constexpr std::size_t MaximumCastsPerRule = 8;

    struct Rule
    {
        std::string id;
        std::vector<std::string> items;
        std::vector<std::string> casts;
        double chance = 1.0;
        bool consume = true;
    };

    class RuleSet
    {
    public:
        // Strict: unknown keys, wrong types, empty ids and duplicate items are
        // errors (the whole file is rejected, like conditions.json).
        bool Load(const std::string& text, std::string& error)
        {
            rules_.clear();
            byItem_.clear();
            std::vector<Rule> rules;
            std::map<std::string, std::size_t> byItem;
            if (!Parse(text, rules, byItem, error))
            {
                return false;
            }
            rules_ = std::move(rules);
            byItem_ = std::move(byItem);
            return true;
        }

        const Rule* Find(const std::string& item) const
        {
            const auto found = byItem_.find(item);
            return found == byItem_.end() ? nullptr : &rules_[found->second];
        }

        const std::vector<Rule>& Rules() const { return rules_; }

    private:
        static bool Parse(const std::string& text, std::vector<Rule>& outRules, std::map<std::string, std::size_t>& outByItem,
            std::string& error)
        {
            using soulash::conditions::Json;
            Json root;
            if (!soulash::conditions::ParseJson(text, root, error))
            {
                return false;
            }
            if (root.type != Json::Type::Object)
            {
                error = "throws.json must be a JSON object";
                return false;
            }
            for (const auto& member : root.members)
            {
                if (member.first != "throws" && member.first != "note" && member.first != "version")
                {
                    error = "unknown top-level key '" + member.first + "' (expected throws, note, version)";
                    return false;
                }
            }
            const Json* list = root.Find("throws");
            if (list == nullptr || list->type != Json::Type::Array)
            {
                error = "'throws' must be an array";
                return false;
            }
            if (list->items.size() > MaximumRules)
            {
                error = "more than 512 throw rules";
                return false;
            }
            for (std::size_t index = 0; index < list->items.size(); ++index)
            {
                const Json& entry = list->items[index];
                const std::string where = "throws[" + std::to_string(index) + "]";
                if (entry.type != Json::Type::Object)
                {
                    error = where + " must be an object";
                    return false;
                }
                Rule rule;
                for (const auto& member : entry.members)
                {
                    const std::string& key = member.first;
                    const Json& value = member.second;
                    if (key == "id")
                    {
                        if (value.type != Json::Type::String || value.text.empty())
                        {
                            error = where + ".id must be a non-empty string";
                            return false;
                        }
                        rule.id = value.text;
                    }
                    else if (key == "item")
                    {
                        if (!ReadIds(value, MaximumItemsPerRule, rule.items))
                        {
                            error = where + ".item must be a non-empty string or an array of 1-64 of them";
                            return false;
                        }
                    }
                    else if (key == "cast")
                    {
                        if (!ReadIds(value, MaximumCastsPerRule, rule.casts))
                        {
                            error = where + ".cast must be a non-empty string or an array of 1-8 of them";
                            return false;
                        }
                    }
                    else if (key == "chance")
                    {
                        if (value.type != Json::Type::Number || !(value.number >= 0.0 && value.number <= 1.0))
                        {
                            error = where + ".chance must be a number from 0 to 1";
                            return false;
                        }
                        rule.chance = value.number;
                    }
                    else if (key == "consume")
                    {
                        if (value.type != Json::Type::Bool)
                        {
                            error = where + ".consume must be true or false";
                            return false;
                        }
                        rule.consume = value.boolean;
                    }
                    else if (key != "note")
                    {
                        error = where + ": unknown key '" + key + "' (expected id, item, cast, chance, consume, note)";
                        return false;
                    }
                }
                if (rule.items.empty() || rule.casts.empty())
                {
                    error = where + " needs both 'item' and 'cast'";
                    return false;
                }
                if (rule.id.empty()) rule.id = rule.items.front();
                for (const std::string& item : rule.items)
                {
                    if (outByItem.count(item) != 0)
                    {
                        error = where + ": item '" + item + "' already has a throw rule in this file";
                        return false;
                    }
                    outByItem[item] = outRules.size();
                }
                outRules.push_back(std::move(rule));
            }
            return true;
        }

        static bool ReadIds(const soulash::conditions::Json& value, std::size_t maximum, std::vector<std::string>& out)
        {
            using soulash::conditions::Json;
            out.clear();
            if (value.type == Json::Type::String)
            {
                if (value.text.empty()) return false;
                out.push_back(value.text);
                return true;
            }
            if (value.type != Json::Type::Array || value.items.empty() || value.items.size() > maximum) return false;
            for (const Json& item : value.items)
            {
                if (item.type != Json::Type::String || item.text.empty()) return false;
                out.push_back(item.text);
            }
            return true;
        }

        std::vector<Rule> rules_;
        std::map<std::string, std::size_t> byItem_;
    };
}
