// Game-state source for the conditions evaluator (docs/triggers.md section 3).
// Header-only, read-only: given the component storages of the game's entity
// registry it fills a soulash::conditions::Snapshot for a holder (and its
// target) by reading component memory the same way the engine's own getters
// do. Nothing here calls game code. Every read the plugin makes through this
// header runs inside a __try block in the plugin.
//
// Facts (build 526b6ab4 / 0.10.6.1, static analysis; docs/triggers.md 3):
//   * The game context holds a std::vector<ECStorageBase*> at ctx+0x100 whose
//     index is the component-type id (core_2 messages.json `component_type`
//     order: 1 Ability user, 21 Equipment, 28 Health, 32 Item, 63 Skills,
//     65 Stamina, 68 Tags, 76 Weapon ...). The plugin verifies each storage
//     by its RTTI vtable before use.
//   * ECStorage<T>: +0x08/+0x10 pages vector (24-byte {int* begin,end,cap}
//     entries, 0x2000 ints each: page = id >> 13, slot = id & 0x1FFF, -1 =
//     absent), +0x38/+0x40 dense component array (stride sizeof(T)).
//   * Component layouts below come from the components' own serialisers
//     (vtable slot 1), checked against the owner's player.sav.
#pragma once

#include "conditions.hpp"
#include "ability_bar_layout.hpp"   // ReadMsvcString

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace soulash_plugins::game_state
{
    // ---- storage layout -------------------------------------------------------
    constexpr std::uintptr_t ContextRegistryOffset = 0x100;   // std::vector<ECStorageBase*> begin
    constexpr std::uintptr_t ContextRegistryEndOffset = 0x108;
    constexpr std::uintptr_t StoragePagesBeginOffset = 0x08;
    constexpr std::uintptr_t StoragePagesEndOffset = 0x10;
    constexpr std::uintptr_t StorageDenseBeginOffset = 0x38;
    constexpr std::uintptr_t StorageDenseEndOffset = 0x40;
    constexpr std::size_t StoragePageEntrySize = 24;
    constexpr unsigned int StoragePageShift = 13;
    constexpr unsigned int StoragePageMask = 0x1FFF;

    // ---- component types (registry index) and sizes (getter strides) ----------
    enum ComponentType : unsigned int
    {
        TypeAbilityUser = 1,
        TypeEquipment = 21,
        TypeHealth = 28,
        TypeItem = 32,
        TypeSkills = 63,
        TypeStamina = 65,
        TypeTags = 68,
        TypeWeapon = 76,
        TypeCount = 78
    };
    constexpr std::size_t AbilityUserSize = 0x260;   // getter 0x92360: imul 0x260
    constexpr std::size_t EquipmentSize = 0x150;     // getter 0x284b0
    constexpr std::size_t HealthSize = 0x40;         // getter 0x282e0: shl 6
    constexpr std::size_t ItemSize = 0x18;           // getter 0x3b030: x3, x8
    constexpr std::size_t SkillsSize = 0x88;         // getter 0x12df40
    constexpr std::size_t StaminaSize = 0x28;        // getter 0x85450: x5, x8
    constexpr std::size_t TagsSize = 0x20;           // getter 0x42e80: shl 5
    constexpr std::size_t WeaponSize = 0x68;         // getter 0x1838a0

    // ---- component fields -----------------------------------------------------
    // Health;cur|max|..  (serialiser 0x7b2a0: floats at +8 and +0xC)
    constexpr std::uintptr_t HealthCurrentOffset = 0x08;
    constexpr std::uintptr_t HealthMaximumOffset = 0x0C;
    // Stamina: the attack code refuses when cost + [+8] >= [+0xC]: +8 is the
    // spent part, +0xC the pool (serialiser 0x4368a0 writes +8,+C,+10,+14,+1C x100).
    constexpr std::uintptr_t StaminaUsedOffset = 0x08;
    constexpr std::uintptr_t StaminaMaximumOffset = 0x0C;
    // Tags: std::vector<std::string> at +8/+0x10 (serialiser 0x82500, stride 0x20);
    // the strings hold the tag's index in core_2 `tag` ("5" = Player).
    constexpr std::uintptr_t TagsBeginOffset = 0x08;
    constexpr std::uintptr_t TagsEndOffset = 0x10;
    constexpr std::size_t MsvcStringSize = 0x20;
    // Equipment: int slots[20] at +8 (entity id of the worn item, 0 = empty),
    // in core_2 `eq_slot` order (serialiser 0x77900).
    constexpr std::uintptr_t EquipmentSlotsOffset = 0x08;
    constexpr unsigned int EquipmentSlotCount = 20;
    // Skills: std::map<std::string,int> at +8 = skill id -> level (serialiser
    // 0x80cd0 writes id*level*potential*progress from three maps at +8/+0x18/+0x28).
    constexpr std::uintptr_t SkillsLevelMapOffset = 0x08;
    constexpr std::uintptr_t MapNodeLeft = 0x00;
    constexpr std::uintptr_t MapNodeParent = 0x08;
    constexpr std::uintptr_t MapNodeRight = 0x10;
    constexpr std::uintptr_t MapNodeIsNil = 0x19;
    constexpr std::uintptr_t MapNodeKey = 0x20;      // std::string
    constexpr std::uintptr_t MapNodeValue = 0x40;    // int
    constexpr unsigned int MaximumMapNodes = 512;
    // Weapon +8: index into core_2 `weapon_type` (the on-attack routine passes
    // 9 = Any when the attacker has no weapon entity). Item +8: `item_type` index.
    constexpr std::uintptr_t WeaponTypeOffset = 0x08;
    constexpr std::uintptr_t ItemTypeOffset = 0x08;
    // AbilityUser: +0x1A0 current target entity id (the queued-ability consumer
    // casts at it), +0x230 std::deque<Ability*> queue, +0x258 byte flag that
    // suppresses passive triggers while set.
    constexpr std::uintptr_t AbilityUserTargetOffset = 0x1A0;
    constexpr std::uintptr_t AbilityUserQueueOffset = 0x230;
    constexpr std::uintptr_t AbilityUserNoTriggersOffset = 0x258;

    // ---- core_2 lists (messages.json, 0.10.6.1) -------------------------------
    constexpr const char* WeaponTypeNames[] = {
        "Axe", "Sword", "Mace", "Dagger", "Polearm", "Bow", "Unarmed", "Crossbow", "Improvised", "Any",
        "Natural", "Throwing"};
    constexpr unsigned int WeaponTypeCount = 12;
    constexpr unsigned int WeaponTypeAny = 9;
    constexpr const char* ItemTypeNames[] = {
        "Any", "Head Armor", "Head Cloth", "Amulet", "Torso Armor", "Torso Cloth", "Belt", "Legs Armor",
        "Legs Cloth", "Boots", "Cape", "Gloves", "Ring", "Weapon", "Shield", "Arrow", "Resource", "Usable",
        "Corpse", "Fish", "Food", "Bolt", "Bag", "Light Source", "Seed", "Note", "Golem Head", "Golem Torso",
        "Golem Legs", "Golem Hand", "Quiver", "Soul", "Meat Head", "Meat Legs", "Meat Hand"};
    constexpr unsigned int ItemTypeCount = 35;
    constexpr unsigned int ItemTypeShield = 14;
    constexpr const char* EquipmentSlotNames[EquipmentSlotCount] = {
        "backpack", "head_armor", "head_cloth", "amulet", "cape", "ammo", "torso_armor", "torso_cloth",
        "left_hand", "right_hand", "gloves", "ring1", "ring2", "belt", "legs_armor", "legs_cloth", "boots",
        "second_right", "second_left", "bag"};
    constexpr unsigned int SlotLeftHand = 8;
    constexpr unsigned int SlotRightHand = 9;
    constexpr unsigned int SlotSecondRight = 17;
    constexpr unsigned int SlotSecondLeft = 18;
    constexpr const char* TagNames[] = {
        "Goblin", "Human", "Animal", "Magic", "Undead", "Player", "Dwarf", "Elf", "Owlman", "Alive",
        "Troglodyte", "Troll", "Orc", "Fishman", "Demon", "Satyr", "Centaur", "Mutant", "Insectoid", "Dragon",
        "Skeleton", "Ghoul", "Zombie"};
    constexpr unsigned int TagCount = 23;

    // One storage the plugin has verified (vtable of the storage and of its
    // components checked against RTTI).
    struct StorageRef
    {
        const unsigned char* storage = nullptr;
        std::size_t stride = 0;
        const void* componentVtable = nullptr;   // required first qword of every component
    };

    struct Access
    {
        StorageRef abilityUser, equipment, health, item, skills, stamina, tags, weapon;
    };

    template <typename T>
    T Read(const void* base, std::uintptr_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
        return value;
    }

    // Element `index` of the registry vector at ctx+0x100 (nullptr if absent).
    inline const unsigned char* RegistryStorage(const void* context, unsigned int index)
    {
        const auto begin = Read<const unsigned char* const*>(context, ContextRegistryOffset);
        const auto end = Read<const unsigned char* const*>(context, ContextRegistryEndOffset);
        if (begin == nullptr || end < begin || static_cast<std::size_t>(end - begin) <= index)
        {
            return nullptr;
        }
        return begin[index];
    }

    // Replicates ECStorage<T>::get without calling game code. nullptr when the
    // entity has no such component or the component's vtable is not the
    // expected one (layout assumption broken: treat as absent).
    inline const unsigned char* FindComponent(const StorageRef& ref, std::uint32_t entityId)
    {
        if (ref.storage == nullptr || ref.stride == 0)
        {
            return nullptr;
        }
        const auto pagesBegin = Read<const unsigned char*>(ref.storage, StoragePagesBeginOffset);
        const auto pagesEnd = Read<const unsigned char*>(ref.storage, StoragePagesEndOffset);
        if (pagesBegin == nullptr || pagesEnd < pagesBegin)
        {
            return nullptr;
        }
        const std::size_t pageCount = static_cast<std::size_t>(pagesEnd - pagesBegin) / StoragePageEntrySize;
        const std::size_t page = entityId >> StoragePageShift;
        if (page >= pageCount)
        {
            return nullptr;
        }
        const auto pageBegin = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize);
        const auto pageEnd = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize + 8);
        if (pageBegin == nullptr || pageBegin == pageEnd)
        {
            return nullptr;
        }
        const std::int32_t denseIndex = pageBegin[entityId & StoragePageMask];
        if (denseIndex < 0)
        {
            return nullptr;
        }
        const auto denseBegin = Read<const unsigned char*>(ref.storage, StorageDenseBeginOffset);
        const auto denseEnd = Read<const unsigned char*>(ref.storage, StorageDenseEndOffset);
        if (denseBegin == nullptr || denseEnd < denseBegin)
        {
            return nullptr;
        }
        const std::size_t denseCount = static_cast<std::size_t>(denseEnd - denseBegin) / ref.stride;
        if (static_cast<std::size_t>(denseIndex) >= denseCount)
        {
            return nullptr;
        }
        const unsigned char* component = denseBegin + static_cast<std::size_t>(denseIndex) * ref.stride;
        if (ref.componentVtable != nullptr && Read<const void*>(component, 0) != ref.componentVtable)
        {
            return nullptr;
        }
        return component;
    }

    inline bool Percent(float current, float maximum, double& out)
    {
        if (!(maximum > 0.0f) || current != current || maximum != maximum)
        {
            return false;
        }
        out = 100.0 * static_cast<double>(current) / static_cast<double>(maximum);
        if (out < 0.0) out = 0.0;
        if (out > 100.0) out = 100.0;
        return true;
    }

    inline bool EqualsIgnoreCase(const char* a, const char* b)
    {
        for (;; ++a, ++b)
        {
            const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
            if (ca != cb) return false;
            if (ca == 0) return true;
        }
    }

    // The game stores a tag as the decimal index into core_2 `tag`; the
    // snapshot wants names. Unknown indices keep their digits so a mod-defined
    // tag can still be matched by number.
    inline std::string TagName(const char* stored)
    {
        char* end = nullptr;
        const long index = std::strtol(stored, &end, 10);
        if (end != stored && *end == '\0' && index >= 0 && static_cast<unsigned long>(index) < TagCount)
        {
            return TagNames[index];
        }
        return stored;
    }

    inline bool ReadTags(const StorageRef& tags, std::uint32_t entityId, std::set<std::string>& out)
    {
        const unsigned char* component = FindComponent(tags, entityId);
        if (component == nullptr)
        {
            return false;
        }
        const auto begin = Read<const unsigned char*>(component, TagsBeginOffset);
        const auto end = Read<const unsigned char*>(component, TagsEndOffset);
        if (begin == nullptr || end < begin || static_cast<std::size_t>(end - begin) > 64 * MsvcStringSize)
        {
            return false;
        }
        for (const unsigned char* item = begin; item < end; item += MsvcStringSize)
        {
            char text[64];
            if (ReadMsvcString(item, text, sizeof(text)))
            {
                out.insert(TagName(text));
            }
        }
        return true;
    }

    inline bool ReadHealthPercent(const StorageRef& health, std::uint32_t entityId, double& out)
    {
        const unsigned char* component = FindComponent(health, entityId);
        return component != nullptr
            && Percent(Read<float>(component, HealthCurrentOffset), Read<float>(component, HealthMaximumOffset), out);
    }

    inline bool ReadStaminaPercent(const StorageRef& stamina, std::uint32_t entityId, double& out)
    {
        const unsigned char* component = FindComponent(stamina, entityId);
        if (component == nullptr)
        {
            return false;
        }
        const float used = Read<float>(component, StaminaUsedOffset);
        const float maximum = Read<float>(component, StaminaMaximumOffset);
        return Percent(maximum - used, maximum, out);
    }

    // In-order walk of std::map<std::string,int> (bounded; false on a malformed tree).
    inline bool ReadStringIntMap(const unsigned char* map, std::map<std::string, double>& out)
    {
        const auto head = Read<const unsigned char*>(map, 0);
        if (head == nullptr)
        {
            return false;
        }
        const unsigned char* node = Read<const unsigned char*>(head, MapNodeParent);   // root
        const unsigned char* stack[64];
        unsigned int depth = 0;
        unsigned int visited = 0;
        while ((node != nullptr && Read<std::uint8_t>(node, MapNodeIsNil) == 0) || depth > 0)
        {
            if (node != nullptr && Read<std::uint8_t>(node, MapNodeIsNil) == 0)
            {
                if (depth >= 64) return false;
                stack[depth++] = node;
                node = Read<const unsigned char*>(node, MapNodeLeft);
                continue;
            }
            node = stack[--depth];
            if (++visited > MaximumMapNodes) return false;
            char key[128];
            if (ReadMsvcString(node + MapNodeKey, key, sizeof(key)))
            {
                out[key] = Read<std::int32_t>(node, MapNodeValue);
            }
            node = Read<const unsigned char*>(node, MapNodeRight);
        }
        return true;
    }

    inline bool ReadSkills(const StorageRef& skills, std::uint32_t entityId, std::map<std::string, double>& out)
    {
        const unsigned char* component = FindComponent(skills, entityId);
        return component != nullptr && ReadStringIntMap(component + SkillsLevelMapOffset, out);
    }

    struct EquipmentView
    {
        bool present = false;
        std::int32_t slots[EquipmentSlotCount]{};
    };

    inline EquipmentView ReadEquipment(const StorageRef& equipment, std::uint32_t entityId)
    {
        EquipmentView view;
        const unsigned char* component = FindComponent(equipment, entityId);
        if (component != nullptr)
        {
            view.present = true;
            std::memcpy(view.slots, component + EquipmentSlotsOffset, sizeof(view.slots));
        }
        return view;
    }

    // weapon_type index of the item entity, or -1 when it has no Weapon component.
    inline int WeaponType(const StorageRef& weapon, std::int32_t itemEntity)
    {
        if (itemEntity <= 0) return -1;
        const unsigned char* component = FindComponent(weapon, static_cast<std::uint32_t>(itemEntity));
        return component == nullptr ? -1 : Read<std::int32_t>(component, WeaponTypeOffset);
    }

    inline int ItemType(const StorageRef& item, std::int32_t itemEntity)
    {
        if (itemEntity <= 0) return -1;
        const unsigned char* component = FindComponent(item, static_cast<std::uint32_t>(itemEntity));
        return component == nullptr ? -1 : Read<std::int32_t>(component, ItemTypeOffset);
    }

    // Fills what the registry can answer; everything else stays unknown. The
    // target (0 = none) only contributes target_* predicates.
    inline void BuildSnapshot(const Access& access, std::uint32_t holder, std::uint32_t target,
        soulash::conditions::Snapshot& snapshot)
    {
        double value = 0;
        if (ReadHealthPercent(access.health, holder, value)) snapshot.healthPercent = value;
        if (ReadStaminaPercent(access.stamina, holder, value)) snapshot.staminaPercent = value;
        std::set<std::string> tags;
        if (ReadTags(access.tags, holder, tags)) snapshot.holderTags = tags;
        std::map<std::string, double> skills;
        if (ReadSkills(access.skills, holder, skills)) snapshot.skills = skills;

        const EquipmentView equipment = ReadEquipment(access.equipment, holder);
        if (equipment.present)
        {
            std::map<std::string, soulash::conditions::EquippedItem> worn;
            bool shield = false;
            for (unsigned int slot = 0; slot < EquipmentSlotCount; ++slot)
            {
                const std::int32_t entity = equipment.slots[slot];
                if (entity <= 0) continue;
                soulash::conditions::EquippedItem item;
                const int type = ItemType(access.item, entity);
                if (type >= 0 && static_cast<unsigned int>(type) < ItemTypeCount)
                {
                    item.itemType = ItemTypeNames[type];
                }
                worn[EquipmentSlotNames[slot]] = item;
                if (type == static_cast<int>(ItemTypeShield)
                    && (slot == SlotLeftHand || slot == SlotRightHand || slot == SlotSecondRight || slot == SlotSecondLeft))
                {
                    shield = true;
                }
            }
            snapshot.equipped = worn;
            snapshot.shield = shield;
            // Wielded weapon class: right hand, else left hand, else Unarmed.
            int weaponType = WeaponType(access.weapon, equipment.slots[SlotRightHand]);
            if (weaponType < 0) weaponType = WeaponType(access.weapon, equipment.slots[SlotLeftHand]);
            if (weaponType < 0)
            {
                snapshot.weaponClass = std::string("Unarmed");
            }
            else if (static_cast<unsigned int>(weaponType) < WeaponTypeCount)
            {
                snapshot.weaponClass = std::string(WeaponTypeNames[weaponType]);
            }
        }

        if (target != 0 && target != holder)
        {
            if (ReadHealthPercent(access.health, target, value)) snapshot.targetHealthPercent = value;
            std::set<std::string> targetTags;
            if (ReadTags(access.tags, target, targetTags)) snapshot.targetTags = targetTags;
        }
    }
}
