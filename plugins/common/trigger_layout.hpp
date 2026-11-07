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
    }

    // image: the mapped game executable. useAbilityUpdate / entityAttack /
    // queuedAbility: starts of the functions referencing the three strings
    // above (from findFunctionsReferencingString). Any of the three may be
    // null; the corresponding fields stay null and `failure` names it.
    inline TriggerLayout LocateTriggerLayout(
        const unsigned char* image, std::size_t imageSize,
        const unsigned char* useAbilityUpdate, const unsigned char* entityAttack, const unsigned char* queuedAbility)
    {
        using namespace trigger_layout_detail;
        TriggerLayout result;
        if (image == nullptr || imageSize == 0)
        {
            result.failure = "no game image";
            return result;
        }
        const Region region{image, image + imageSize};
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
}
