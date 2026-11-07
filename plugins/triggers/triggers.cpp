// Triggers: true conditional-trigger support, executed by the extender
// (docs/triggers.md). The engine can chain abilities only through a few passive
// effects with a flat chance and no condition, and the in-game editor exposes
// only stackers. This plugin hooks the places where the engine itself resolves
// passive triggers (and a few neighbouring routines) and runs the extender's
// own rules there:
//
//   * TriggerOnCast(ctx, caster, &abilityId)   - called by UseAbility::update
//     right after an ability is cast (the engine's trigger_ability_on_cast);
//   * TriggerOnAttack(ctx, attacker, target, weapon) - called after every
//     melee / ranged attack (the engine's on_attack_cast);
//   * KillTrigger(system, killer, victim, ...)  - OnDeath::update, effect 101;
//   * TurnTick(system, entity)                  - ProgressEffects::update, once
//     per game turn for every entity with effects (the turn source);
//   * ParryExperience(ctx, attacker, defender)  - the attack resolution's miss
//     path (dodge);
//   * DamageDealt(ctx, attacker, target, damage) - after an attack or ability
//     damage lands (damaged);
//   * AttackEnd(system, attack, weapon, critical) - the end of every attack
//     resolution (crit);
//   * Deflect(system, attacker, queue) -> bool  - EntityAttack::update's
//     deflection check (block).
//
// Rules live in <mod>\extender\conditions.json (`triggers`: on <event>, cast
// B, chance, when) and <mod>\extender\effects.json. For each event the plugin
// builds a state snapshot of the holder from the game's own component registry
// (plugins/common/game_state.hpp), evaluates the rules
// (plugins/common/trigger_rules.hpp, fail closed) and queues each resulting
// ability on the holder with the engine's own primitive: a fresh Ability copied
// from the ability registry, pushed onto the AbilityUser's queued-ability deque,
// which the game casts at the holder's current target on its own thread.
// Delayed casts (effects.json `delay`) wait for that many turn ticks of the
// holder.
//
// Nothing here trusts a build hash. Every game function is located from its
// own code (plugins/common/trigger_layout.hpp, anchored by the engine's log
// strings and RTTI) and every component storage is checked against RTTI before
// use. Mode=log (default) installs the hooks and logs what would happen;
// Mode=active queues. Any fault inside a hook disables the plugin and leaves
// the game alone.
#include "plugin_api.h"
#include "../common/ability_queue.hpp"
#include "../common/game_state.hpp"
#include "../common/plugin_status.hpp"
#include "../common/trigger_layout.hpp"
#include "../common/trigger_rules.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
    namespace gs = soulash_plugins::game_state;
    namespace rules = soulash_plugins::trigger_rules;

    constexpr std::size_t AbilitySize = 0x198;
    constexpr std::size_t MaximumConditionsFile = 64 * 1024;
    constexpr std::size_t MaximumEffectsFile = 256 * 1024;
    constexpr std::size_t MaximumMods = 512;
    constexpr std::size_t MaximumPendingCasts = 256;
    constexpr int KillEffectAny = 101;   // on_kill_any_create_item: the first of the two kill passes
    // Attack struct of the attack resolution (docs/triggers.md 3.1).
    constexpr std::size_t AttackAttackerOffset = 0x00;
    constexpr std::size_t AttackTargetOffset = 0x90;
    // The EntityAttack system's attack queue: data +0x18, capacity +0x20, index
    // +0x28; the current entry's target entity id at +0x4C.
    constexpr std::size_t QueueDataOffset = 0x18;
    constexpr std::size_t QueueCapacityOffset = 0x20;
    constexpr std::size_t QueueIndexOffset = 0x28;
    constexpr std::size_t QueueEntryTargetOffset = 0x4C;
    constexpr std::size_t SystemContextOffset = 0x08;   // every system holds the game context at +8

    using OnCastFn = void* (*)(void* context, unsigned int caster, const void* abilityId);
    using OnAttackFn = void* (*)(void* context, unsigned int attacker, unsigned int target, unsigned int weapon);
    using KillFn = std::uint64_t (*)(void* system, unsigned int killer, unsigned int victim, void* position, int effectId);
    using TurnFn = std::uint64_t (*)(void* system, unsigned int entity);
    using ParryFn = std::uint64_t (*)(void* context, unsigned int attacker, unsigned int defender, float value);
    using DamageFn = std::uint64_t (*)(void* context, unsigned int attacker, unsigned int target, int damage);
    using AttackEndFn = std::uint64_t (*)(void* system, void* attack, unsigned int weaponType, unsigned char critical);
    using DeflectFn = unsigned char (*)(void* system, unsigned int attacker, void* queue, void* fourth);
    using ClockFn = const void* (*)();
    using RegistryFn = void* (*)();
    using MapAtFn = void* (*)(void* registry, void* out, const std::string* key);
    using NewFn = void* (*)(std::size_t);
    using DeleteFn = void (*)(void*, std::size_t);
    using GrowMapFn = void (*)(void* deque);
    using DestructorFn = void (*)(void* ability);

    const SoulashExtenderInterface* g_extender = nullptr;
    soulash_plugins::StartupReason g_startupReason;

    // ---- configuration --------------------------------------------------------
    bool g_enabled = true;
    bool g_active = false;          // Mode=active
    bool g_logEvents = true;
    bool g_logTurnEvents = false;
    unsigned int g_maxLogLines = 400;
    unsigned int g_maxCastsPerEvent = 8;

    // ---- game functions (from trigger_layout) --------------------------------
    soulash_plugins::TriggerLayout g_layout;
    OnCastFn g_onCastOriginal = nullptr;
    OnAttackFn g_onAttackOriginal = nullptr;
    KillFn g_onKillOriginal = nullptr;
    TurnFn g_onTurnOriginal = nullptr;
    ParryFn g_onParryOriginal = nullptr;
    DamageFn g_onDamageOriginal = nullptr;
    AttackEndFn g_onAttackEndOriginal = nullptr;
    DeflectFn g_onDeflectOriginal = nullptr;
    ClockFn g_clock = nullptr;
    RegistryFn g_abilityRegistry = nullptr;
    MapAtFn g_abilityMapAt = nullptr;
    NewFn g_gameNew = nullptr;
    DeleteFn g_gameDelete = nullptr;
    GrowMapFn g_growMap = nullptr;
    DestructorFn g_abilityDestructor = nullptr;
    bool g_hookInstalled[rules::EventKindCount] = {};

    // ---- state source ---------------------------------------------------------
    struct StorageSpec
    {
        const char* name;
        unsigned int index;
        std::size_t stride;
        const char* storageRtti;
        const char* componentRtti;
        gs::StorageRef* ref;
        const void* storageVtable = nullptr;
        const void* componentVtable = nullptr;
        bool ok = false;
    };
    gs::Access g_access;
    StorageSpec g_storages[] = {
        {"AbilityUser", gs::TypeAbilityUser, gs::AbilityUserSize, ".?AV?$ECStorage@UAbilityUser@@@ASEngine@@", ".?AUAbilityUser@@", &g_access.abilityUser},
        {"Effects", gs::TypeEffects, gs::EffectsSize, ".?AV?$ECStorage@UEffects@@@ASEngine@@", ".?AUEffects@@", &g_access.effects},
        {"Equipment", gs::TypeEquipment, gs::EquipmentSize, ".?AV?$ECStorage@UEquipment@@@ASEngine@@", ".?AUEquipment@@", &g_access.equipment},
        {"Health", gs::TypeHealth, gs::HealthSize, ".?AV?$ECStorage@UHealth@@@ASEngine@@", ".?AUHealth@@", &g_access.health},
        {"Item", gs::TypeItem, gs::ItemSize, ".?AV?$ECStorage@UItem@@@ASEngine@@", ".?AUItem@@", &g_access.item},
        {"Party", gs::TypeParty, gs::PartySize, ".?AV?$ECStorage@UParty@@@ASEngine@@", ".?AUParty@@", &g_access.party},
        {"Player", gs::TypePlayer, gs::PlayerSize, ".?AV?$ECStorage@UPlayer@@@ASEngine@@", ".?AUPlayer@@", &g_access.player},
        {"Skills", gs::TypeSkills, gs::SkillsSize, ".?AV?$ECStorage@USkills@@@ASEngine@@", ".?AUSkills@@", &g_access.skills},
        {"Stamina", gs::TypeStamina, gs::StaminaSize, ".?AV?$ECStorage@UStamina@@@ASEngine@@", ".?AUStamina@@", &g_access.stamina},
        {"Tags", gs::TypeTags, gs::TagsSize, ".?AV?$ECStorage@UTags@@@ASEngine@@", ".?AUTags@@", &g_access.tags},
        {"Weapon", gs::TypeWeapon, gs::WeaponSize, ".?AV?$ECStorage@UWeapon@@@ASEngine@@", ".?AUWeapon@@", &g_access.weapon},
    };
    bool g_storagesResolved = false;
    bool g_queueUsable = false;

    // ---- rules ----------------------------------------------------------------
    std::vector<rules::ModRules>* g_mods = nullptr;   // heap: never destroyed at process exit
    unsigned int g_ruleCount = 0;
    unsigned int g_modCount = 0;

    // A cast waiting for turn ticks of its holder (effects.json `delay`).
    struct PendingCast
    {
        std::uint32_t holder = 0;
        std::string abilityId;
        std::string rule;
        unsigned int turnsLeft = 0;
    };
    std::vector<PendingCast>* g_pending = nullptr;

    // ---- runtime --------------------------------------------------------------
    std::mt19937_64* g_rng = nullptr;
    bool g_disabled = false;          // after a fault
    unsigned int g_logLines = 0;
    unsigned long long g_eventCounts[rules::EventKindCount] = {};
    unsigned long long g_queued = 0;
    unsigned long long g_wouldQueue = 0;
    unsigned int g_badIdLogs = 0;
    bool g_clockLogged = false;

    void Log(const char* format, ...)
    {
        char line[600];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        g_startupReason.Capture(line);
        char prefixed[640];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "Triggers: %s", line);
        g_extender->log(prefixed);
    }

    // Event lines are capped per session; firings and problems always log.
    bool EventLogBudget()
    {
        if (g_logLines >= g_maxLogLines)
        {
            if (g_logLines == g_maxLogLines)
            {
                ++g_logLines;
                Log("event log cap (%u lines) reached; only firings and problems from here on", g_maxLogLines);
            }
            return false;
        }
        ++g_logLines;
        return true;
    }

    std::uintptr_t Rva(const void* address)
    {
        return address == nullptr ? 0
            : static_cast<std::uintptr_t>(static_cast<const unsigned char*>(address)
                - static_cast<const unsigned char*>(g_extender->gameBase));
    }

    // ---- configuration --------------------------------------------------------
    void ReadConfiguration()
    {
        wchar_t iniPath[MAX_PATH * 2];
        _snwprintf_s(iniPath, _TRUNCATE, L"%sTriggers.ini", g_extender->pluginDirectory);
        g_enabled = GetPrivateProfileIntW(L"Triggers", L"Enabled", 1, iniPath) != 0;
        wchar_t mode[32];
        GetPrivateProfileStringW(L"Triggers", L"Mode", L"log", mode, 32, iniPath);
        g_active = _wcsicmp(mode, L"active") == 0;
        g_logEvents = GetPrivateProfileIntW(L"Triggers", L"LogEvents", 1, iniPath) != 0;
        g_logTurnEvents = GetPrivateProfileIntW(L"Triggers", L"LogTurnEvents", 0, iniPath) != 0;
        g_maxLogLines = GetPrivateProfileIntW(L"Triggers", L"MaxLogLines", 400, iniPath);
        g_maxCastsPerEvent = GetPrivateProfileIntW(L"Triggers", L"MaxCastsPerEvent", 8, iniPath);
        if (g_maxCastsPerEvent == 0) g_maxCastsPerEvent = 1;
        if (g_maxCastsPerEvent > 64) g_maxCastsPerEvent = 64;
    }

    // ---- mod files ------------------------------------------------------------
    bool ReadFileText(const std::wstring& path, std::size_t maximum, std::string& out)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
            && static_cast<unsigned long long>(size.QuadPart) <= maximum;
        if (ok)
        {
            out.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != FALSE
                && read == out.size());
        }
        CloseHandle(file);
        return ok;
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        return out;
    }

    void LoadModRules()
    {
        g_mods = new std::vector<rules::ModRules>();
        g_pending = new std::vector<PendingCast>();
        std::wstring root = std::wstring(g_extender->gameDirectory) + L"data\\mods\\";
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((root + L"*").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE)
        {
            Log("no mods folder at %S; no rules loaded", root.c_str());
            return;
        }
        do
        {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || entry.cFileName[0] == L'.')
            {
                continue;
            }
            if (g_mods->size() >= MaximumMods) break;
            const std::wstring mod = entry.cFileName;
            const std::string name = Narrow(mod);
            rules::ModRules data;
            data.mod = name;
            std::string text;
            std::string error;
            const std::wstring conditionsPath = root + mod + L"\\extender\\conditions.json";
            if (GetFileAttributesW(conditionsPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (!ReadFileText(conditionsPath, MaximumConditionsFile, text))
                {
                    Log("%s: conditions.json unreadable or over 64 KiB; skipped", name.c_str());
                }
                else if (!data.conditions.Load(text, error))
                {
                    Log("%s: conditions.json rejected: %s", name.c_str(), error.c_str());
                }
                else
                {
                    data.hasConditions = true;
                    g_ruleCount += static_cast<unsigned int>(data.conditions.Triggers().size());
                }
            }
            const std::wstring effectsPath = root + mod + L"\\extender\\effects.json";
            if (GetFileAttributesW(effectsPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (!ReadFileText(effectsPath, MaximumEffectsFile, text))
                {
                    Log("%s: effects.json unreadable or over 256 KiB; skipped", name.c_str());
                }
                else if (!data.effects.Load(text, error))
                {
                    Log("%s: effects.json rejected: %s", name.c_str(), error.c_str());
                }
                else
                {
                    data.hasEffects = true;
                    g_ruleCount += static_cast<unsigned int>(data.effects.Triggers().size());
                }
            }
            if (data.hasConditions || data.hasEffects)
            {
                Log("%s: %zu conditional trigger(s), %zu effects trigger(s)", name.c_str(),
                    data.hasConditions ? data.conditions.Triggers().size() : 0,
                    data.hasEffects ? data.effects.Triggers().size() : 0);
                g_mods->push_back(std::move(data));
                ++g_modCount;
            }
        } while (FindNextFileW(find, &entry) != FALSE);
        FindClose(find);
    }

    // ---- state source resolution (first event, game thread) -------------------
    void ResolveStorages(const void* context)
    {
        g_storagesResolved = true;
        char summary[600] = "";
        for (StorageSpec& spec : g_storages)
        {
            const unsigned char* storage = gs::RegistryStorage(context, spec.index);
            const bool vtableOk = storage != nullptr && spec.storageVtable != nullptr
                && gs::Read<const void*>(storage, 0) == spec.storageVtable;
            spec.ok = vtableOk;
            if (vtableOk)
            {
                spec.ref->storage = storage;
                spec.ref->stride = spec.stride;
                spec.ref->componentVtable = spec.componentVtable;
            }
            char part[64];
            _snprintf_s(part, _TRUNCATE, "%s%s=%s", summary[0] == '\0' ? "" : " ", spec.name,
                vtableOk ? "ok" : (storage == nullptr ? "missing" : "WRONG VTABLE"));
            strncat_s(summary, part, _TRUNCATE);
        }
        g_queueUsable = g_storages[0].ok && g_gameNew != nullptr && g_abilityRegistry != nullptr
            && g_abilityMapAt != nullptr && g_growMap != nullptr;
        Log("state source (registry at ctx+0x100, indices = component_type ids): %s; queueing %s", summary,
            g_queueUsable ? "possible" : "NOT possible (AbilityUser storage or game functions missing)");
    }

    gs::GameClock ReadGameClock()
    {
        if (g_clock == nullptr) return gs::GameClock{};
        const gs::GameClock clock = gs::ReadClock(g_clock());
        if (!g_clockLogged)
        {
            g_clockLogged = true;
            if (clock.has)
            {
                Log("game clock: time %.1f turns, day part %d (%s), year part %d (%s)", clock.time, clock.dayPart,
                    clock.dayPart >= 0 ? gs::DayPartNames[clock.dayPart] : "?", clock.yearPart,
                    clock.yearPart >= 0 ? gs::YearPartNames[clock.yearPart] : "?");
            }
            else
            {
                Log("game clock: the clock struct does not look as expected; time_of_day and season stay unknown");
            }
        }
        return clock;
    }

    // ---- the cast primitive ---------------------------------------------------
    // Returns false (and logs) when the ability id is unknown or anything about
    // the queue does not look as expected. Memory stays in the game's heap.
    bool QueueAbility(const void* context, std::uint32_t holder, const std::string& abilityId, const char* rule)
    {
        (void)context;
        const unsigned char* abilityUser = gs::FindComponent(g_access.abilityUser, holder);
        if (abilityUser == nullptr)
        {
            Log("rule %s: entity %u has no AbilityUser component; nothing queued", rule, holder);
            return false;
        }
        void* ability = g_gameNew(AbilitySize);
        if (ability == nullptr)
        {
            Log("rule %s: the game's allocator returned null; nothing queued", rule);
            return false;
        }
        std::memset(ability, 0, AbilitySize);
        g_abilityMapAt(g_abilityRegistry(), ability, &abilityId);
        char copiedId[160];
        if (!soulash_plugins::ReadMsvcString(ability, copiedId, sizeof(copiedId)) || abilityId != copiedId)
        {
            if (g_abilityDestructor != nullptr && g_gameDelete != nullptr)
            {
                g_abilityDestructor(ability);
                g_gameDelete(ability, AbilitySize);
            }
            if (g_badIdLogs < 50)
            {
                ++g_badIdLogs;
                Log("rule %s: ability '%s' is not in the game's ability registry (got '%s'); nothing queued%s",
                    rule, abilityId.c_str(), copiedId,
                    g_abilityDestructor == nullptr ? " (object leaked: destructor not located)" : "");
            }
            return false;
        }
        auto* queue = reinterpret_cast<soulash_plugins::DequeOfPointers*>(
            const_cast<unsigned char*>(abilityUser) + gs::AbilityUserQueueOffset);
        if (queue->mapSize > 0x100000 || queue->size > 0x100000)
        {
            g_abilityDestructor(ability);
            g_gameDelete(ability, AbilitySize);
            Log("rule %s: the queued-ability deque looks corrupt (map %zu, size %zu); nothing queued", rule,
                queue->mapSize, queue->size);
            return false;
        }
        const bool pushed = soulash_plugins::DequePushBack(*queue, ability,
            [](std::size_t bytes) { return g_gameNew(bytes); },
            [](soulash_plugins::DequeOfPointers& deque) { g_growMap(&deque); });
        if (!pushed)
        {
            g_abilityDestructor(ability);
            g_gameDelete(ability, AbilitySize);
            Log("rule %s: queue push failed; nothing queued", rule);
            return false;
        }
        ++g_queued;
        return true;
    }

    // Queue (or, in log mode, report) one cast for a holder.
    void FireCast(const void* context, std::uint32_t holder, std::uint32_t target, const std::string& abilityId,
        const std::string& rule, const char* why)
    {
        if (!g_active)
        {
            ++g_wouldQueue;
            Log("rule %s: would queue '%s' on entity %u%s (Mode=log; set Mode=active to queue)", rule.c_str(),
                abilityId.c_str(), holder, why);
            return;
        }
        if (!g_queueUsable)
        {
            Log("rule %s: cannot queue '%s' (queue not usable on this build)", rule.c_str(), abilityId.c_str());
            return;
        }
        if (QueueAbility(context, holder, abilityId, rule.c_str()))
        {
            Log("rule %s: queued '%s' on entity %u (target %u)%s", rule.c_str(), abilityId.c_str(), holder, target, why);
        }
    }

    // Delayed casts of this holder: one turn closer; fire the due ones.
    void TickPending(const void* context, std::uint32_t holder, std::uint32_t target)
    {
        for (std::size_t i = 0; i < g_pending->size();)
        {
            PendingCast& pending = (*g_pending)[i];
            if (pending.holder != holder)
            {
                ++i;
                continue;
            }
            if (--pending.turnsLeft > 0)
            {
                ++i;
                continue;
            }
            const PendingCast due = pending;
            g_pending->erase(g_pending->begin() + static_cast<std::ptrdiff_t>(i));
            FireCast(context, holder, target, due.abilityId, due.rule, " (delayed cast due)");
        }
    }

    // ---- event handling (game thread) -----------------------------------------
    // `extra` is the weapon entity (attack) or the damage (damaged).
    void DescribeEvent(rules::EventKind kind, std::uint32_t holder, std::uint32_t target, std::uint32_t extra,
        const char* abilityId, char* out, std::size_t size)
    {
        switch (kind)
        {
        case rules::EventKind::Cast:
            _snprintf_s(out, size, _TRUNCATE, "cast event: entity %u cast '%s' (target %u)", holder, abilityId, target);
            break;
        case rules::EventKind::Attack:
            _snprintf_s(out, size, _TRUNCATE, "attack event: entity %u attacked %u (weapon entity %u)", holder, target, extra);
            break;
        case rules::EventKind::Kill:
            _snprintf_s(out, size, _TRUNCATE, "kill event: entity %u killed %u", holder, target);
            break;
        case rules::EventKind::Turn:
            _snprintf_s(out, size, _TRUNCATE, "turn event: entity %u (target %u)", holder, target);
            break;
        case rules::EventKind::Damaged:
            _snprintf_s(out, size, _TRUNCATE, "damaged event: entity %u took %u damage from %u", holder, extra, target);
            break;
        case rules::EventKind::Dodge:
            _snprintf_s(out, size, _TRUNCATE, "dodge event: entity %u parried an attack by %u", holder, target);
            break;
        case rules::EventKind::Block:
            _snprintf_s(out, size, _TRUNCATE, "block event: entity %u deflected an attack by %u", holder, target);
            break;
        case rules::EventKind::Crit:
            _snprintf_s(out, size, _TRUNCATE, "crit event: entity %u critically hit %u", holder, target);
            break;
        }
    }

    void HandleEvent(const void* context, rules::EventKind kind, std::uint32_t holder, std::uint32_t target,
        std::uint32_t extra, const void* abilityIdString)
    {
        if (!g_storagesResolved)
        {
            ResolveStorages(context);
        }
        ++g_eventCounts[static_cast<unsigned int>(kind)];
        rules::Event event;
        event.kind = kind;
        char abilityId[160] = "";
        if (kind == rules::EventKind::Cast)
        {
            if (abilityIdString == nullptr || !soulash_plugins::ReadMsvcString(abilityIdString, abilityId, sizeof(abilityId)))
            {
                if (g_logEvents && EventLogBudget()) Log("cast event: entity %u, ability id unreadable", holder);
                return;
            }
            event.abilityId = abilityId;
        }
        if (kind == rules::EventKind::Cast || kind == rules::EventKind::Turn)
        {
            const unsigned char* abilityUser = gs::FindComponent(g_access.abilityUser, holder);
            if (abilityUser != nullptr)
            {
                const auto current = gs::Read<std::int32_t>(abilityUser, gs::AbilityUserTargetOffset);
                target = current > 0 ? static_cast<std::uint32_t>(current) : 0;
            }
        }
        const bool turn = kind == rules::EventKind::Turn;
        if (turn && !g_pending->empty())
        {
            TickPending(context, holder, target);
        }
        const bool logThis = g_logEvents && (!turn || g_logTurnEvents);

        char description[240];
        DescribeEvent(kind, holder, target, extra, abilityId, description, sizeof(description));
        const bool anyRule = rules::AnyRuleFor(*g_mods, event);
        if (!anyRule)
        {
            // Turn ticks without a rule are never logged: hundreds per turn.
            if (logThis && !turn && EventLogBudget()) Log("%s; no rules", description);
            return;
        }

        const gs::GameClock clock = ReadGameClock();
        soulash::conditions::Snapshot state;
        gs::BuildSnapshot(g_access, holder, target, state, clock);
        const rules::Outcome outcome = rules::Evaluate(*g_mods, event, state,
            [] { return std::uniform_real_distribution<double>(0.0, 1.0)(*g_rng); }, g_maxCastsPerEvent,
            g_onTurnOriginal != nullptr);

        if (logThis && EventLogBudget())
        {
            char stateText[300];
            _snprintf_s(stateText, _TRUNCATE,
                "health=%s%.0f stamina=%s%.0f weapon=%s shield=%s tags=%s%zu skills=%s%zu stacks=%s%zu party=%s companions=%s%.0f time=%s/%s",
                state.healthPercent.has ? "" : "?", state.healthPercent.value,
                state.staminaPercent.has ? "" : "?", state.staminaPercent.value,
                state.weaponClass.has ? state.weaponClass.value.c_str() : "?",
                state.shield.has ? (state.shield.value ? "yes" : "no") : "?",
                state.holderTags.has ? "" : "?", state.holderTags.has ? state.holderTags.value.size() : 0,
                state.skills.has ? "" : "?", state.skills.has ? state.skills.value.size() : 0,
                state.stackers.has ? "" : "?", state.stackers.has ? state.stackers.value.size() : 0,
                state.party.has ? state.party.value.c_str() : "?",
                state.companions.has ? "" : "?", state.companions.value,
                state.dayPart.has ? state.dayPart.value.c_str() : "?", state.season.has ? state.season.value.c_str() : "?");
            Log("%s: %u rule(s) matched, %u allowed, %u blocked by condition, %u by chance; %s", description,
                outcome.matched, outcome.allowed, outcome.blockedByCondition, outcome.blockedByChance, stateText);
        }
        for (const std::string& note : outcome.notes)
        {
            if (EventLogBudget()) Log("note: %s", note.c_str());
        }
        for (const rules::Cast& cast : outcome.casts)
        {
            FireCast(context, holder, target, cast.abilityId, cast.mod + "/" + cast.rule, "");
        }
        for (const rules::Cast& cast : outcome.delayed)
        {
            const std::string rule = cast.mod + "/" + cast.rule;
            if (g_pending->size() >= MaximumPendingCasts)
            {
                Log("rule %s: delayed cast of '%s' dropped (%zu casts already pending)", rule.c_str(),
                    cast.abilityId.c_str(), g_pending->size());
                continue;
            }
            g_pending->push_back({holder, cast.abilityId, rule, cast.delayTurns});
            if (EventLogBudget())
            {
                Log("rule %s: '%s' scheduled for entity %u in %u turn(s)", rule.c_str(), cast.abilityId.c_str(), holder,
                    cast.delayTurns);
            }
        }
    }

    // Plain bodies for __try (no C++ objects with destructors in these frames).
    // `base` is the game context, or the system that holds it at +8.
    void GuardedHandleEvent(void* base, bool baseIsSystem, rules::EventKind kind, std::uint32_t holder,
        std::uint32_t target, std::uint32_t extra, const void* abilityIdString)
    {
        __try
        {
            const void* context = baseIsSystem ? gs::Read<const void*>(base, SystemContextOffset) : base;
            if (context != nullptr)
            {
                HandleEvent(context, kind, holder, target, extra, abilityIdString);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_disabled = true;
            Log("fault 0x%08lX while handling a %s event; plugin disabled for this session, game unaffected",
                GetExceptionCode(), rules::EventName(kind));
            soulash_plugins::ReportStatus(g_extender, SOULASH_PLUGIN_STATUS_DEGRADED,
                "memory fault while handling an event; triggers stopped (game unaffected)");
        }
    }

    bool GuardedReadU32(const void* base, std::size_t offset, std::uint32_t* out)
    {
        __try
        {
            if (base == nullptr) return false;
            *out = gs::Read<std::uint32_t>(base, offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The target of the attack the deflection check is about: the queue's
    // current entry, read exactly as the engine does right after.
    bool GuardedDeflectTarget(const void* queue, std::uint32_t* out)
    {
        __try
        {
            if (queue == nullptr) return false;
            const auto data = gs::Read<const unsigned char* const*>(queue, QueueDataOffset);
            const auto capacity = gs::Read<std::size_t>(queue, QueueCapacityOffset);
            const auto index = gs::Read<std::size_t>(queue, QueueIndexOffset);
            if (data == nullptr || capacity == 0 || capacity > 0x100000) return false;
            const unsigned char* entry = data[(capacity - 1) & index];
            if (entry == nullptr) return false;
            *out = gs::Read<std::uint32_t>(entry, QueueEntryTargetOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* OnCastDetour(void* context, unsigned int caster, const void* abilityId)
    {
        void* result = g_onCastOriginal(context, caster, abilityId);
        if (!g_disabled && context != nullptr)
        {
            GuardedHandleEvent(context, false, rules::EventKind::Cast, caster, 0, 0, abilityId);
        }
        return result;
    }

    void* OnAttackDetour(void* context, unsigned int attacker, unsigned int target, unsigned int weapon)
    {
        void* result = g_onAttackOriginal(context, attacker, target, weapon);
        if (!g_disabled && context != nullptr)
        {
            GuardedHandleEvent(context, false, rules::EventKind::Attack, attacker, target, weapon, nullptr);
        }
        return result;
    }

    std::uint64_t OnKillDetour(void* system, unsigned int killer, unsigned int victim, void* position, int effectId)
    {
        const std::uint64_t result = g_onKillOriginal(system, killer, victim, position, effectId);
        // The engine runs this twice per death (effects 101 and 102); fire once.
        if (!g_disabled && system != nullptr && effectId == KillEffectAny && killer != 0)
        {
            GuardedHandleEvent(system, true, rules::EventKind::Kill, killer, victim, 0, nullptr);
        }
        return result;
    }

    std::uint64_t OnTurnDetour(void* system, unsigned int entity)
    {
        const std::uint64_t result = g_onTurnOriginal(system, entity);
        if (!g_disabled && system != nullptr && entity != 0)
        {
            GuardedHandleEvent(system, true, rules::EventKind::Turn, entity, 0, 0, nullptr);
        }
        return result;
    }

    std::uint64_t OnParryDetour(void* context, unsigned int attacker, unsigned int defender, float value)
    {
        const std::uint64_t result = g_onParryOriginal(context, attacker, defender, value);
        if (!g_disabled && context != nullptr && defender != 0)
        {
            GuardedHandleEvent(context, false, rules::EventKind::Dodge, defender, attacker, 0, nullptr);
        }
        return result;
    }

    std::uint64_t OnDamageDetour(void* context, unsigned int attacker, unsigned int target, int damage)
    {
        const std::uint64_t result = g_onDamageOriginal(context, attacker, target, damage);
        if (!g_disabled && context != nullptr && target != 0 && damage > 0)
        {
            GuardedHandleEvent(context, false, rules::EventKind::Damaged, target, attacker,
                static_cast<std::uint32_t>(damage), nullptr);
        }
        return result;
    }

    std::uint64_t OnAttackEndDetour(void* system, void* attack, unsigned int weaponType, unsigned char critical)
    {
        const std::uint64_t result = g_onAttackEndOriginal(system, attack, weaponType, critical);
        if (!g_disabled && system != nullptr && critical != 0)
        {
            std::uint32_t attacker = 0, target = 0;
            if (GuardedReadU32(attack, AttackAttackerOffset, &attacker) && GuardedReadU32(attack, AttackTargetOffset, &target)
                && attacker != 0)
            {
                GuardedHandleEvent(system, true, rules::EventKind::Crit, attacker, target, weaponType, nullptr);
            }
        }
        return result;
    }

    unsigned char OnDeflectDetour(void* system, unsigned int attacker, void* queue, void* fourth)
    {
        std::uint32_t target = 0;
        const bool haveTarget = !g_disabled && GuardedDeflectTarget(queue, &target);
        const unsigned char result = g_onDeflectOriginal(system, attacker, queue, fourth);
        if (!g_disabled && system != nullptr && result != 0 && haveTarget && target != 0)
        {
            GuardedHandleEvent(system, true, rules::EventKind::Block, target, attacker, 0, nullptr);
        }
        return result;
    }

    // ---- install --------------------------------------------------------------
    const unsigned char* FirstFunctionReferencing(const char* text)
    {
        void* results[4]{};
        unsigned int count = 0;
        if (!g_extender->findFunctionsReferencingString(g_extender->pluginHandle, text, results, 4, &count) || count == 0)
        {
            return nullptr;
        }
        if (count > 1)
        {
            Log("%u functions reference \"%s\"; using the first", count, text);
        }
        return static_cast<const unsigned char*>(results[0]);
    }

    // Virtual function `slot` of the class with that RTTI name (the systems'
    // update is slot 1), or null.
    const unsigned char* RttiMethod(const char* rttiName, unsigned int slot)
    {
        void* vtable = nullptr;
        if (!g_extender->findRttiVtable(g_extender->pluginHandle, rttiName, &vtable) || vtable == nullptr)
        {
            return nullptr;
        }
        const unsigned char* method = nullptr;
        __try
        {
            method = static_cast<const unsigned char* const*>(vtable)[slot];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            method = nullptr;
        }
        return method;
    }

    soulash_plugins::TriggerLayout GuardedLocate(const soulash_plugins::TriggerAnchors& anchors)
    {
        soulash_plugins::TriggerLayout layout;
        __try
        {
            const auto* image = static_cast<const unsigned char*>(g_extender->gameBase);
            layout = soulash_plugins::LocateTriggerLayout(image, soulash_plugins::MappedImageSize(image), anchors);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            layout = soulash_plugins::TriggerLayout{};
            layout.failure = "access fault while reading game code";
        }
        return layout;
    }

    void ResolveRtti()
    {
        for (StorageSpec& spec : g_storages)
        {
            void* vtable = nullptr;
            if (g_extender->findRttiVtable(g_extender->pluginHandle, spec.storageRtti, &vtable) && vtable != nullptr)
            {
                spec.storageVtable = vtable;
            }
            vtable = nullptr;
            if (g_extender->findRttiVtable(g_extender->pluginHandle, spec.componentRtti, &vtable) && vtable != nullptr)
            {
                spec.componentVtable = vtable;
            }
            if (spec.storageVtable == nullptr || spec.componentVtable == nullptr)
            {
                Log("RTTI for %s not found (%s / %s); that component stays unknown to conditions", spec.name,
                    spec.storageVtable == nullptr ? "storage missing" : "storage ok",
                    spec.componentVtable == nullptr ? "component missing" : "component ok");
            }
        }
    }

    // Detours one optional routine; records the trampoline in `original`.
    template <typename Fn>
    bool InstallOptional(rules::EventKind kind, const unsigned char* routine, void* detour, Fn& original, const char* what)
    {
        if (routine == nullptr)
        {
            return false;
        }
        void* trampoline = nullptr;
        if (!g_extender->installDetourAuto(g_extender->pluginHandle, const_cast<unsigned char*>(routine), detour, &trampoline))
        {
            Log("detour on the %s routine refused; %s rules will not fire", what, rules::EventName(kind));
            return false;
        }
        original = reinterpret_cast<Fn>(trampoline);
        g_hookInstalled[static_cast<unsigned int>(kind)] = true;
        return true;
    }

    void HookSummary(char* out, std::size_t size)
    {
        char installed[160] = "";
        char missing[160] = "";
        for (unsigned int kind = 0; kind < rules::EventKindCount; ++kind)
        {
            char* list = g_hookInstalled[kind] ? installed : missing;
            if (list[0] != '\0') strncat_s(list, 160, " ", _TRUNCATE);
            strncat_s(list, 160, rules::EventName(static_cast<rules::EventKind>(kind)), _TRUNCATE);
        }
        _snprintf_s(out, size, _TRUNCATE, "events hooked: %s%s%s", installed[0] ? installed : "none",
            missing[0] ? "; not available: " : "", missing);
    }

    bool Install()
    {
        soulash_plugins::TriggerAnchors anchors;
        anchors.useAbilityUpdate = FirstFunctionReferencing("UseAbility: Entity {} is casting {}. Applying effects.");
        anchors.entityAttack = FirstFunctionReferencing("EntityAttack: Attacked entity {}");
        anchors.queuedAbility = FirstFunctionReferencing("Queued Ability");
        anchors.onDeathUpdate = FirstFunctionReferencing("killing an ally");
        anchors.entityAttackUpdate = FirstFunctionReferencing("EntityAttack: Attack performed.");
        anchors.parryExperience = FirstFunctionReferencing("core_2_Parry");
        anchors.progressEffectsUpdate = RttiMethod(".?AVProgressEffects@@", 1);
        if (anchors.useAbilityUpdate == nullptr)
        {
            Log("inactive: the UseAbility cast log string is not in this game build; no hooks installed");
            return false;
        }
        g_layout = GuardedLocate(anchors);
        if (!g_layout.verified)
        {
            Log("inactive: game build %.8s does not match the verified trigger code (%s); no hooks installed",
                g_extender->gameExecutableSha256[0] == '\0' ? "unknown " : g_extender->gameExecutableSha256,
                g_layout.failure);
            return false;
        }
        g_abilityRegistry = reinterpret_cast<RegistryFn>(const_cast<unsigned char*>(g_layout.abilityRegistry));
        g_abilityMapAt = reinterpret_cast<MapAtFn>(const_cast<unsigned char*>(g_layout.abilityMapAt));
        g_gameNew = reinterpret_cast<NewFn>(const_cast<unsigned char*>(g_layout.gameNew));
        g_gameDelete = reinterpret_cast<DeleteFn>(const_cast<unsigned char*>(g_layout.gameDelete));
        g_growMap = reinterpret_cast<GrowMapFn>(const_cast<unsigned char*>(g_layout.dequeGrowMap));
        g_abilityDestructor = reinterpret_cast<DestructorFn>(const_cast<unsigned char*>(g_layout.abilityDestructor));
        g_clock = reinterpret_cast<ClockFn>(const_cast<unsigned char*>(g_layout.gameClock));
        Log("game code located (RVAs): on-cast trigger 0x%llX, on-attack trigger 0x%llX, ability registry 0x%llX, "
            "map::at 0x%llX, operator new 0x%llX, operator delete 0x%llX, deque growmap 0x%llX, ~Ability 0x%llX%s%s",
            static_cast<unsigned long long>(Rva(g_layout.onCastTrigger)),
            static_cast<unsigned long long>(Rva(g_layout.onAttackTrigger)),
            static_cast<unsigned long long>(Rva(g_layout.abilityRegistry)),
            static_cast<unsigned long long>(Rva(g_layout.abilityMapAt)),
            static_cast<unsigned long long>(Rva(g_layout.gameNew)),
            static_cast<unsigned long long>(Rva(g_layout.gameDelete)),
            static_cast<unsigned long long>(Rva(g_layout.dequeGrowMap)),
            static_cast<unsigned long long>(Rva(g_layout.abilityDestructor)),
            g_layout.failure[0] == '\0' ? "" : "; note: ", g_layout.failure);
        Log("event code located (RVAs, 0 = not on this build): kill trigger 0x%llX, turn tick 0x%llX, parry 0x%llX, "
            "damage dealt 0x%llX, attack end 0x%llX, deflect 0x%llX, game clock 0x%llX",
            static_cast<unsigned long long>(Rva(g_layout.onKillTrigger)),
            static_cast<unsigned long long>(Rva(g_layout.onTurnTick)),
            static_cast<unsigned long long>(Rva(g_layout.onParry)),
            static_cast<unsigned long long>(Rva(g_layout.onDamageDealt)),
            static_cast<unsigned long long>(Rva(g_layout.onAttackEnd)),
            static_cast<unsigned long long>(Rva(g_layout.onDeflect)),
            static_cast<unsigned long long>(Rva(g_layout.gameClock)));
        ResolveRtti();

        void* trampoline = nullptr;
        if (!g_extender->installDetourAuto(g_extender->pluginHandle,
                const_cast<unsigned char*>(g_layout.onCastTrigger), reinterpret_cast<void*>(&OnCastDetour), &trampoline))
        {
            Log("inactive: detour on the on-cast trigger routine refused (see the diagnostics log); nothing hooked");
            return false;
        }
        g_onCastOriginal = reinterpret_cast<OnCastFn>(trampoline);
        g_hookInstalled[static_cast<unsigned int>(rules::EventKind::Cast)] = true;
        InstallOptional(rules::EventKind::Attack, g_layout.onAttackTrigger, reinterpret_cast<void*>(&OnAttackDetour),
            g_onAttackOriginal, "on-attack trigger");
        InstallOptional(rules::EventKind::Kill, g_layout.onKillTrigger, reinterpret_cast<void*>(&OnKillDetour),
            g_onKillOriginal, "kill trigger");
        InstallOptional(rules::EventKind::Turn, g_layout.onTurnTick, reinterpret_cast<void*>(&OnTurnDetour),
            g_onTurnOriginal, "turn tick");
        InstallOptional(rules::EventKind::Dodge, g_layout.onParry, reinterpret_cast<void*>(&OnParryDetour),
            g_onParryOriginal, "parry");
        InstallOptional(rules::EventKind::Damaged, g_layout.onDamageDealt, reinterpret_cast<void*>(&OnDamageDetour),
            g_onDamageOriginal, "damage dealt");
        InstallOptional(rules::EventKind::Crit, g_layout.onAttackEnd, reinterpret_cast<void*>(&OnAttackEndDetour),
            g_onAttackEndOriginal, "attack end");
        InstallOptional(rules::EventKind::Block, g_layout.onDeflect, reinterpret_cast<void*>(&OnDeflectDetour),
            g_onDeflectOriginal, "deflection");
        char hooks[400];
        HookSummary(hooks, sizeof(hooks));
        Log("active: game build %.8s, mode=%s, %u rule(s) from %u mod(s); %s; delayed casts %s",
            g_extender->gameExecutableSha256[0] == '\0' ? "unknown " : g_extender->gameExecutableSha256,
            g_active ? "active" : "log", g_ruleCount, g_modCount, hooks,
            g_onTurnOriginal != nullptr ? "wait for the holder's turns" : "are dropped (no turn tick on this build)");
        return true;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize < SOULASH_EXTENDER_INTERFACE_V3_BASE_SIZE
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->gameExecutableSha256 == nullptr
        || extender->gameBase == nullptr || extender->gameDirectory == nullptr || extender->pluginDirectory == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;
    g_startupReason.Begin();
    ReadConfiguration();
    if (!g_enabled)
    {
        Log("disabled: [Triggers] Enabled=0 in %STriggers.ini; nothing hooked", extender->pluginDirectory);
        g_startupReason.End();
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_DISABLED, "Enabled=0 in Triggers.ini");
        return TRUE;
    }
    g_rng = new std::mt19937_64(static_cast<unsigned long long>(GetTickCount64()) ^ 0x9E3779B97F4A7C15ull
        ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32));
    LoadModRules();
    const bool active = Install();
    g_startupReason.End();
    char hooks[400];
    HookSummary(hooks, sizeof(hooks));
    char detail[480];
    _snprintf_s(detail, _TRUNCATE, "mode=%s; %u rule(s) from %u mod(s); %s",
        g_active ? "active" : "log", g_ruleCount, g_modCount, hooks);
    soulash_plugins::ReportStatus(extender,
        active ? (g_layout.failure[0] == '\0' ? SOULASH_PLUGIN_STATUS_ACTIVE : SOULASH_PLUGIN_STATUS_DEGRADED)
               : SOULASH_PLUGIN_STATUS_INACTIVE,
        active ? detail : g_startupReason.Last());
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
