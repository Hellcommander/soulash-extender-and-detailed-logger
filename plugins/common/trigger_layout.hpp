// Build-independent location of the game functions the Triggers plugin hooks
// and calls (docs/triggers.md). Header-only, read-only, no game calls: it reads
// code bytes of functions found through the extender's string anchors and
// follows rel32 fields, exactly like plugins/common/ability_bar_layout.hpp.
//
// Facts (build 526b6ab4 / 0.10.6.1, static analysis; names are ours):
//
//   UseAbility::update (the function referencing
//   "UseAbility: Entity {} is casting {}. Applying effects.") calls the
//   engine's on-cast passive trigger routine right after the cast:
//     80 B8 58 02 00 00 00     cmp byte ptr [rax+258h],0   (AbilityUser flag)
//     75 ??                    jne skip
//     4C 8D 45 ??              lea r8,[rbp-??]             (&std::string abilityId)
//     41 8B D6                 mov edx,r14d                (caster entity id)
//     49 8B 4C 24 08           mov rcx,[r12+8]             (game context)
//     E8 rel32                 call TriggerOnCast          <- hooked
//
//   TriggerOnCast(ctx, casterId, const std::string* abilityId) collects the
//   caster's passives carrying effect 117 (trigger_ability_on_cast), rolls the
//   chance and queues the secondary ability. Inside it:
//     C7 44 24 20 75 00 00 00  mov [rsp+20h],75h           (passive effect 117)
//     83 3F 75                 cmp dword ptr [rdi],75h     (row enum)
//     E8 rel32 48 8B C8 4C 8D 47 40 48 8D 54 24 60 E8 rel32
//                              AbilityRegistry() ; map::at(registry, out, &id)
//     B9 98 01 00 00 E8 rel32  operator new(0x198)         (sizeof(Ability))
//     48 39 8E 40 02 00 00 77 08 48 8B CD E8 rel32
//                              deque<Ability*>::_Growmap   (queue at AbilityUser+0x230)
//     48 FF 86 50 02 00 00     inc qword ptr [rsi+250h]    (queue size)
//
//   EntityAttack (the function referencing "EntityAttack: Attacked entity {}")
//   calls the on-attack routine after every attack:
//     44 8B 4E 60              mov r9d,[rsi+60h]           (weapon entity id)
//     45 8B 45 00              mov r8d,[r13]               (target entity id)
//     8B 16                    mov edx,[rsi]               (attacker entity id)
//     49 8B 4F 08              mov rcx,[r15+8]             (game context)
//     E8 rel32                 call TriggerOnAttack        <- hooked
//   TriggerOnAttack contains C7 44 24 20 64 00 00 00 (effect 100, on_attack_cast)
//   and 41 B8 4C 00 00 00 (hasComponent(..., 76 = Weapon)).
//
//   The queued-ability consumer (the function referencing "Queued Ability")
//   destroys a popped Ability with
//     48 8B CB E8 rel32        call Ability::~Ability
//     BA 98 01 00 00 48 8B CB E8 rel32
//                              operator delete(p, 0x198)
//   and starts with 49 83 B8 50 02 00 00 00 (cmp [r8+250h],0: queue empty?).
//
//   Further events (docs/triggers.md 3.1; all optional, each found on its own):
//
//   kill: OnDeath::update (the function referencing "killing an ally") runs the
//   kill passives for every dead entity twice, effect 101 then 102:
//     C7 44 24 20 65 00 00 00  mov [rsp+20h],65h           (effect 101, on_kill_any_create_item)
//     4C 8D 4D D8 45 8B C6 41 8B D7 48 8B CE E8 rel32
//                              lea r9,[rbp-28h]; mov r8d,r14d (victim); mov edx,r15d (killer);
//                              mov rcx,rsi (system); call KillTrigger(system, killer, victim, pos*, effect)
//   KillTrigger checks hasComponent(killer, 44 = Name) and (killer, 21 = Equipment):
//     41 B8 2C 00 00 00 / 41 B8 15 00 00 00 within its first 0x120 bytes.
//
//   turn: ProgressEffects::update (RTTI slot 1 of ProgressEffects) ticks every
//   entity with an Effects component once per game turn:
//     E8 rel32 F3 0F 10 40 18 F3 0F 5C C7 F3 0F 5F 45 6C
//                              call GameClock(); movss xmm0,[rax+18h]; subss xmm0,xmm7; maxss xmm0,[rbp+6Ch]
//     4C 8B C5 41 8B D7 49 8B CD E8 rel32 41 8B D7 49 8B CD E8 rel32
//                              call A(system, entity, effects); call TurnTick(system, entity)  <- hooked
//   TurnTick collects effect 63: C7 44 24 20 3F 00 00 00 within 0x80 bytes. The
//   clock getter starts with 48 83 EC 28 65 48 8B 04 25 58 00 00 00.
//
//   dodge (an attack parried / missed): the attack resolution calls the
//   defender's parry-skill experience routine (the function referencing
//   "core_2_Parry") only on the miss path:
//     45 8B 45 00 8B 16 49 8B 4F 08 E8 rel32
//                              mov r8d,[r13] (defender); mov edx,[rsi] (attacker); mov rcx,[r15+8]; call
//   The routine starts with hasComponent(defender, 3 = Actor): 41 B8 03 00 00 00 8B D3 E8.
//
//   damaged: right after hit and miss paths converge the attack resolution calls
//     44 8B 4D 60 45 8B 45 00 8B 16 49 8B 4F 08 E8 rel32
//                              mov r9d,[rbp+60h] (damage dealt); mov r8d,[r13]; mov edx,[rsi]; mov rcx,[r15+8];
//                              call DamageDealt(ctx, attacker, target, damage)
//   (also called by the ability-damage code; starts with the same hasComponent(target, 3)).
//
//   crit: the attack resolution ends with
//     83 7D 64 00 41 0F 9F C1 44 8B C7 48 8B D6 49 8B CF E8 rel32
//                              cmp [rbp+64h],0; setg r9b (critical); mov r8d,edi (weapon type);
//                              mov rdx,rsi (attack: +0 attacker, +0x90 target); mov rcx,r15; call AttackEnd
//   AttackEnd collects effect 30 (41 B9 1E 00 00 00).
//
//   block (attack deflected): EntityAttack::update (the function referencing
//   "EntityAttack: Attack performed.") asks the deflection check before resolving:
//     E8 rel32 0F B6 D8 84 C0 74 ?? 48 8D 95
//                              call Deflect(system, attacker, queue*, r9) -> al; movzx ebx,al; test al,al; je
//   Deflect reads the current attack entry and its target the way the plugin does:
//     49 8B 48 20 48 FF C9 49 23 48 28 49 8B 40 18 48 8B 0C C8 8B 59 4C
//                              rcx = queue.data[(queue.cap-1) & queue.index]; ebx = entry.target (+0x4C)
#pragma once

#include "ability_bar_layout.hpp"

#include <cstddef>
#include <cstdint>

namespace soulash_plugins
{
    struct TriggerLayout
    {
        bool verified = false;
        const char* failure = "not checked";
        const unsigned char* onCastTrigger = nullptr;      // TriggerOnCast(ctx, id, const std::string*)
        const unsigned char* onAttackTrigger = nullptr;    // TriggerOnAttack(ctx, attacker, target, weapon)
        const unsigned char* abilityRegistry = nullptr;    // AbilityRegistry* ()  (map at +0)
        const unsigned char* abilityMapAt = nullptr;       // Ability* (map*, Ability* out, const std::string* id)
        const unsigned char* gameNew = nullptr;            // void* operator new(size_t)
        const unsigned char* gameDelete = nullptr;         // void operator delete(void*, size_t)
        const unsigned char* dequeGrowMap = nullptr;       // void deque<Ability*>::_Growmap(deque*)
        const unsigned char* abilityDestructor = nullptr;  // void Ability::~Ability(Ability*)
        // Optional event hooks (null = that event is unavailable on this build).
        const unsigned char* onKillTrigger = nullptr;      // KillTrigger(system, killer, victim, pos*, effectId)
        const unsigned char* onTurnTick = nullptr;         // TurnTick(system, entity)
        const unsigned char* onParry = nullptr;            // ParryExperience(ctx, attacker, defender, float)
        const unsigned char* onDamageDealt = nullptr;      // DamageDealt(ctx, attacker, target, int damage)
        const unsigned char* onAttackEnd = nullptr;        // AttackEnd(system, attack*, weaponType, bool critical)
        const unsigned char* onDeflect = nullptr;          // Deflect(system, attacker, queue*, r9) -> bool
        const unsigned char* gameClock = nullptr;          // const void* GameClock()
    };

    // Starts of the anchor functions (any may be null).
    struct TriggerAnchors
    {
        const unsigned char* useAbilityUpdate = nullptr;      // "UseAbility: Entity {} is casting {}. Applying effects."
        const unsigned char* entityAttack = nullptr;          // "EntityAttack: Attacked entity {}"
        const unsigned char* queuedAbility = nullptr;         // "Queued Ability"
        const unsigned char* onDeathUpdate = nullptr;         // "killing an ally"
        const unsigned char* entityAttackUpdate = nullptr;    // "EntityAttack: Attack performed."
        const unsigned char* parryExperience = nullptr;       // "core_2_Parry"
        const unsigned char* progressEffectsUpdate = nullptr; // RTTI ProgressEffects, vtable slot 1
    };

    namespace trigger_layout_detail
    {
        using ability_bar_layout_detail::Any;
        using ability_bar_layout_detail::Find;
        using ability_bar_layout_detail::InImage;
        using ability_bar_layout_detail::Region;
        using ability_bar_layout_detail::RelTarget;

        template <std::size_t N>
        bool Has(const Region& image, const unsigned char* from, std::size_t window, const int (&pattern)[N])
        {
            return Find(image, from, window, pattern) != nullptr;
        }

        // Target of the rel32 that follows `pattern` (the E8/E9 opcode is the
        // pattern's last byte).
        template <std::size_t N>
        const unsigned char* CallTarget(const Region& image, const unsigned char* from, std::size_t window,
            const int (&pattern)[N])
        {
            const unsigned char* at = Find(image, from, window, pattern);
            if (at == nullptr || !InImage(image, at + N, 4))
            {
                return nullptr;
            }
            const unsigned char* target = RelTarget(at + N, at + N + 4);
            return InImage(image, target, 16) ? target : nullptr;
        }

        // Whether some occurrence of `pattern` (ending in its E8) within the
        // window calls `expected`.
        template <std::size_t N>
        bool CallsTarget(const Region& image, const unsigned char* from, std::size_t window,
            const int (&pattern)[N], const unsigned char* expected)
        {
            const unsigned char* cursor = from;
            const unsigned char* end = from + window;
            while (cursor < end)
            {
                const unsigned char* at = Find(image, cursor, static_cast<std::size_t>(end - cursor), pattern);
                if (at == nullptr) return false;
                if (InImage(image, at + N, 4) && RelTarget(at + N, at + N + 4) == expected) return true;
                cursor = at + 1;
            }
            return false;
        }

        // The optional event hooks; each is found and verified on its own.
        inline void LocateEventHooks(const Region& region, const TriggerAnchors& anchors, TriggerLayout& result)
        {
            constexpr std::size_t UpdateWindow = 0x2000;
            constexpr std::size_t HeadWindow = 0x120;

            // kill
            if (anchors.onDeathUpdate != nullptr && InImage(region, anchors.onDeathUpdate, 1))
            {
                static const int kill101[] = {0xC7, 0x44, 0x24, 0x20, 0x65, 0x00, 0x00, 0x00,
                    0x4C, 0x8D, 0x4D, 0xD8, 0x45, 0x8B, 0xC6, 0x41, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8};
                static const int kill102[] = {0xC7, 0x44, 0x24, 0x20, 0x66, 0x00, 0x00, 0x00,
                    0x4C, 0x8D, 0x4D, 0xD8, 0x45, 0x8B, 0xC6, 0x41, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8};
                static const int nameCheck[] = {0x41, 0xB8, 0x2C, 0x00, 0x00, 0x00};
                static const int equipmentCheck[] = {0x41, 0xB8, 0x15, 0x00, 0x00, 0x00};
                const unsigned char* kill = CallTarget(region, anchors.onDeathUpdate, UpdateWindow, kill101);
                if (kill != nullptr && CallsTarget(region, anchors.onDeathUpdate, UpdateWindow, kill102, kill)
                    && Has(region, kill, HeadWindow, nameCheck) && Has(region, kill, HeadWindow, equipmentCheck))
                {
                    result.onKillTrigger = kill;
                }
            }

            // turn + clock
            if (anchors.progressEffectsUpdate != nullptr && InImage(region, anchors.progressEffectsUpdate, 1))
            {
                static const int tickSite[] = {0x4C, 0x8B, 0xC5, 0x41, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8, Any, Any, Any, Any,
                    0x41, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8};
                static const int effect63[] = {0xC7, 0x44, 0x24, 0x20, 0x3F, 0x00, 0x00, 0x00};
                const unsigned char* tick = CallTarget(region, anchors.progressEffectsUpdate, 0x800, tickSite);
                if (tick != nullptr && Has(region, tick, 0x80, effect63))
                {
                    result.onTurnTick = tick;
                }
                static const int clockSite[] = {0xE8, Any, Any, Any, Any, 0xF3, 0x0F, 0x10, 0x40, 0x18,
                    0xF3, 0x0F, 0x5C, 0xC7, 0xF3, 0x0F, 0x5F, 0x45, 0x6C};
                static const int clockHead[] = {0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00};
                const unsigned char* clockCall = Find(region, anchors.progressEffectsUpdate, 0x800, clockSite);
                if (clockCall != nullptr)
                {
                    const unsigned char* clock = RelTarget(clockCall + 1, clockCall + 5);
                    if (InImage(region, clock, 16) && Has(region, clock, 16, clockHead))
                    {
                        result.gameClock = clock;
                    }
                }
            }

            // dodge, damaged, crit: all from the attack resolution
            static const int actorCheck[] = {0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x8B, 0xD3, 0xE8};
            if (anchors.entityAttack != nullptr && InImage(region, anchors.entityAttack, 1))
            {
                if (anchors.parryExperience != nullptr && InImage(region, anchors.parryExperience, 1))
                {
                    static const int parrySite[] = {0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16, 0x49, 0x8B, 0x4F, 0x08, 0xE8};
                    if (Has(region, anchors.parryExperience, 0x60, actorCheck)
                        && CallsTarget(region, anchors.entityAttack, UpdateWindow, parrySite, anchors.parryExperience))
                    {
                        result.onParry = anchors.parryExperience;
                    }
                }
                static const int damageSite[] = {0x44, 0x8B, 0x4D, 0x60, 0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16,
                    0x49, 0x8B, 0x4F, 0x08, 0xE8};
                const unsigned char* damage = CallTarget(region, anchors.entityAttack, UpdateWindow, damageSite);
                if (damage != nullptr && Has(region, damage, 0x60, actorCheck))
                {
                    result.onDamageDealt = damage;
                }
                static const int critSite[] = {0x83, 0x7D, 0x64, 0x00, 0x41, 0x0F, 0x9F, 0xC1, 0x44, 0x8B, 0xC7,
                    0x48, 0x8B, 0xD6, 0x49, 0x8B, 0xCF, 0xE8};
                static const int effect30[] = {0x41, 0xB9, 0x1E, 0x00, 0x00, 0x00};
                const unsigned char* attackEnd = CallTarget(region, anchors.entityAttack, UpdateWindow, critSite);
                if (attackEnd != nullptr && Has(region, attackEnd, 0x60, effect30))
                {
                    result.onAttackEnd = attackEnd;
                }
            }

            // block
            if (anchors.entityAttackUpdate != nullptr && InImage(region, anchors.entityAttackUpdate, 1))
            {
                static const int deflectSite[] = {0x0F, 0xB6, 0xD8, 0x84, 0xC0, 0x74, Any, 0x48, 0x8D, 0x95};
                static const int entryRead[] = {0x49, 0x8B, 0x48, 0x20, 0x48, 0xFF, 0xC9, 0x49, 0x23, 0x48, 0x28,
                    0x49, 0x8B, 0x40, 0x18, 0x48, 0x8B, 0x0C, 0xC8, 0x8B, 0x59, 0x4C};
                const unsigned char* site = Find(region, anchors.entityAttackUpdate, UpdateWindow, deflectSite);
                if (site != nullptr && site - anchors.entityAttackUpdate >= 5 && site[-5] == 0xE8)
                {
                    const unsigned char* deflect = RelTarget(site - 4, site);
                    if (InImage(region, deflect, 16) && Has(region, deflect, 0x80, entryRead))
                    {
                        result.onDeflect = deflect;
                    }
                }
            }
        }
    }

    // image: the mapped game executable. useAbilityUpdate / entityAttack /
    // queuedAbility: starts of the functions referencing the three strings
    // above (from findFunctionsReferencingString). Any of the three may be
    // null; the corresponding fields stay null and `failure` names it.
    inline TriggerLayout LocateTriggerLayout(const unsigned char* image, std::size_t imageSize, const TriggerAnchors& anchors)
    {
        using namespace trigger_layout_detail;
        TriggerLayout result;
        if (image == nullptr || imageSize == 0)
        {
            result.failure = "no game image";
            return result;
        }
        const Region region{image, image + imageSize};
        const unsigned char* useAbilityUpdate = anchors.useAbilityUpdate;
        const unsigned char* entityAttack = anchors.entityAttack;
        const unsigned char* queuedAbility = anchors.queuedAbility;
        LocateEventHooks(region, anchors, result);
        if (useAbilityUpdate == nullptr || !InImage(region, useAbilityUpdate, 1))
        {
            result.failure = "UseAbility::update not found";
            return result;
        }

        // --- UseAbility::update -> TriggerOnCast ----------------------------
        constexpr std::size_t UpdateWindow = 0x2000;
        static const int castSite[] = {0x80, 0xB8, 0x58, 0x02, 0x00, 0x00, 0x00, 0x75, Any,
            0x4C, 0x8D, 0x45, Any, 0x41, 0x8B, 0xD6, 0x49, 0x8B, 0x4C, 0x24, 0x08, 0xE8};
        const unsigned char* onCast = CallTarget(region, useAbilityUpdate, UpdateWindow, castSite);
        if (onCast == nullptr)
        {
            result.failure = "UseAbility::update: call to the on-cast trigger routine not found";
            return result;
        }

        // --- TriggerOnCast: effect 117, registry lookup, new, growmap, queue --
        constexpr std::size_t TriggerWindow = 0x400;
        static const int effect117[] = {0xC7, 0x44, 0x24, 0x20, 0x75, 0x00, 0x00, 0x00};
        static const int rowEnum[] = {0x83, 0x3F, 0x75};
        if (!Has(region, onCast, TriggerWindow, effect117) || !Has(region, onCast, TriggerWindow, rowEnum))
        {
            result.failure = "on-cast trigger routine does not check passive effect 117";
            return result;
        }
        static const int mapAtSite[] = {0x48, 0x8B, 0xC8, 0x4C, 0x8D, 0x47, 0x40, 0x48, 0x8D, 0x54, 0x24, 0x60, 0xE8};
        const unsigned char* mapAtAt = Find(region, onCast, TriggerWindow, mapAtSite);
        if (mapAtAt == nullptr || mapAtAt - onCast < 5 || mapAtAt[-5] != 0xE8)
        {
            result.failure = "on-cast trigger routine: ability registry lookup not found";
            return result;
        }
        result.abilityRegistry = RelTarget(mapAtAt - 4, mapAtAt);
        result.abilityMapAt = CallTarget(region, mapAtAt, sizeof(mapAtSite) / sizeof(mapAtSite[0]) + 4, mapAtSite);
        static const int newAbility[] = {0xB9, 0x98, 0x01, 0x00, 0x00, 0xE8};
        result.gameNew = CallTarget(region, onCast, TriggerWindow, newAbility);
        static const int growSite[] = {0x48, 0x39, 0x8E, 0x40, 0x02, 0x00, 0x00, 0x77, 0x08, 0x48, 0x8B, 0xCD, 0xE8};
        result.dequeGrowMap = CallTarget(region, onCast, TriggerWindow, growSite);
        static const int incSize[] = {0x48, 0xFF, 0x86, 0x50, 0x02, 0x00, 0x00};
        static const int newBlock[] = {0xB9, 0x10, 0x00, 0x00, 0x00, 0xE8};
        const unsigned char* blockNew = CallTarget(region, onCast, TriggerWindow, newBlock);
        if (!InImage(region, result.abilityRegistry, 16) || result.abilityMapAt == nullptr || result.gameNew == nullptr
            || result.dequeGrowMap == nullptr || !Has(region, onCast, TriggerWindow, incSize)
            || blockNew != result.gameNew)
        {
            result.failure = "on-cast trigger routine: ability allocation / queue code does not match";
            return result;
        }
        result.onCastTrigger = onCast;

        // --- EntityAttack -> TriggerOnAttack ---------------------------------
        if (entityAttack != nullptr && InImage(region, entityAttack, 1))
        {
            static const int attackSite[] = {0x44, 0x8B, 0x4E, 0x60, 0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16,
                0x49, 0x8B, 0x4F, 0x08, 0xE8};
            const unsigned char* onAttack = CallTarget(region, entityAttack, UpdateWindow, attackSite);
            static const int effect100[] = {0xC7, 0x44, 0x24, 0x20, 0x64, 0x00, 0x00, 0x00};
            static const int weaponType[] = {0x41, 0xB8, 0x4C, 0x00, 0x00, 0x00};
            if (onAttack != nullptr && Has(region, onAttack, TriggerWindow, effect100)
                && Has(region, onAttack, 0x100, weaponType))
            {
                result.onAttackTrigger = onAttack;
            }
        }

        // --- Queued Ability -> ~Ability, operator delete ---------------------
        if (queuedAbility != nullptr && InImage(region, queuedAbility, 1))
        {
            static const int emptyCheck[] = {0x49, 0x83, 0xB8, 0x50, 0x02, 0x00, 0x00, 0x00};
            static const int deleteSite[] = {0xBA, 0x98, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8};
            const unsigned char* del = Find(region, queuedAbility, 0x200, deleteSite);
            if (Has(region, queuedAbility, 0x100, emptyCheck) && del != nullptr && del - queuedAbility >= 8
                && del[-8] == 0x48 && del[-7] == 0x8B && del[-6] == 0xCB && del[-5] == 0xE8)
            {
                result.abilityDestructor = RelTarget(del - 4, del);
                result.gameDelete = CallTarget(region, del, sizeof(deleteSite) / sizeof(deleteSite[0]) + 4, deleteSite);
                if (!InImage(region, result.abilityDestructor, 16))
                {
                    result.abilityDestructor = nullptr;
                }
            }
        }

        if (result.onAttackTrigger == nullptr)
        {
            result.failure = "on-attack trigger routine not found (attack events unavailable)";
        }
        else if (result.abilityDestructor == nullptr || result.gameDelete == nullptr)
        {
            result.failure = "Ability destructor / operator delete not found (unknown ability ids cannot be released)";
        }
        else
        {
            result.failure = "";
        }
        result.verified = true;
        return result;
    }

    inline TriggerLayout LocateTriggerLayout(
        const unsigned char* image, std::size_t imageSize,
        const unsigned char* useAbilityUpdate, const unsigned char* entityAttack, const unsigned char* queuedAbility)
    {
        TriggerAnchors anchors;
        anchors.useAbilityUpdate = useAbilityUpdate;
        anchors.entityAttack = entityAttack;
        anchors.queuedAbility = queuedAbility;
        return LocateTriggerLayout(image, imageSize, anchors);
    }
}
