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
//     order: 1 Ability user, 20 Effects, 21 Equipment, 28 Health, 32 Item,
//     50 Party, 52 Player, 63 Skills, 65 Stamina, 68 Tags, 76 Weapon ...). The
//     plugin verifies each storage by its RTTI vtable before use.
//   * ECStorage<T>: +0x08/+0x10 pages vector (24-byte {int* begin,end,cap}
//     entries, 0x2000 ints each: page = id >> 13, slot = id & 0x1FFF, -1 =
//     absent), +0x20/+0x28 vector<int> of the entity ids that have the
//     component (the dense order), +0x38/+0x40 dense component array (stride
//     sizeof(T)).
//   * Component layouts below come from the components' own serialisers
//     (vtable slot 1), checked against the owner's player.sav.
//   * The game clock is a static struct returned by a tiny getter (0x205F0 in
//     526b6ab4; the Triggers plugin finds it from ProgressEffects::update):
//     +0x14 int 1440 (turns per day), +0x18 float time in turns, +0x20 int
//     day-part index (core_2 `day_part`, passed to the day-part translator by
//     the UI), +0x28 int year-part index (core_2 `year_part`).
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
    constexpr std::uintptr_t StorageIdsBeginOffset = 0x20;   // std::vector<int> entity ids
    constexpr std::uintptr_t StorageIdsEndOffset = 0x28;
    constexpr std::uintptr_t StorageDenseBeginOffset = 0x38;
    constexpr std::uintptr_t StorageDenseEndOffset = 0x40;
    constexpr std::size_t StoragePageEntrySize = 24;
    constexpr unsigned int StoragePageShift = 13;
    constexpr unsigned int StoragePageMask = 0x1FFF;

    // ---- component types (registry index) and sizes (getter strides) ----------
    enum ComponentType : unsigned int
    {
        TypeAbilityUser = 1,
        TypeEffects = 20,
        TypeEquipment = 21,
        TypeHealth = 28,
        TypeItem = 32,
        TypeName = 44,
        TypeParty = 50,
        TypePlayer = 52,
        TypeSkills = 63,
        TypeStackable = 64,
        TypeStamina = 65,
        TypeTags = 68,
        TypeWeapon = 76,
        TypeCount = 78
    };
    constexpr std::size_t AbilityUserSize = 0x260;   // getter 0x92360: imul 0x260
    constexpr std::size_t EffectsSize = 0x70;        // getter 0x92160
    constexpr std::size_t PartySize = 0x10;          // getter 0x27cc0: sar 4
    constexpr std::size_t PlayerSize = 0x130;        // getter 0x28100
    constexpr std::size_t NameSize = 0xF0;           // getter 0x27eb0
    constexpr std::size_t StackableSize = 0x10;      // getter 0x27cc0 (shared with Party)
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
    // Equipment +0x70/+0x78: std::vector<int> of the item entities carried in
    // the backpack (the serialiser writes their count and ids after the slots;
    // AStashItems walks it and asks hasComponent(item, 64 = Stackable)).
    constexpr std::uintptr_t EquipmentInventoryBeginOffset = 0x70;
    constexpr std::uintptr_t EquipmentInventoryEndOffset = 0x78;
    constexpr unsigned int MaximumInventoryItems = 1024;
    // Name: +0x08 std::string data id (core_2 entity "803" = Wheat Bread,
    // "core_2_Fancy_Flask", ...), +0x28 std::string display name (serialiser
    // 0x7bf40: `Name;803|Wheat Bread|...`).
    constexpr std::uintptr_t NameDataIdOffset = 0x08;
    constexpr std::uintptr_t NameDisplayOffset = 0x28;
    // Stackable: +0x08 int stack count (serialiser 0x1e0080).
    constexpr std::uintptr_t StackableCountOffset = 0x08;
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
    // Effects: +0x08/+0x10 vector of active effects (0x1A0 each: an Ability
    // plus its end time), +0x38 std::map<std::string,int> stacker id -> stacks,
    // +0x48 std::map<std::string,float> stacker id -> expiry time (the engine's
    // on_max_stacks code resets a stack count to 0 once time passes its
    // expiry), +0x6C float: the time the entity's effects last ticked.
    constexpr std::uintptr_t EffectsStacksMapOffset = 0x38;
    constexpr std::uintptr_t EffectsStackExpiryMapOffset = 0x48;
    constexpr std::uintptr_t EffectsLastTickOffset = 0x6C;
    // Party: carried by the player's companions (not by the player: the owner's
    // player.sav has no Party line). +0x08 int, +0x0C bool "hold position"
    // (PartyActions toggles it). The party UI lists the Party storage's ids.
    constexpr std::uintptr_t PartyHoldPositionOffset = 0x0C;
    constexpr unsigned int MaximumPartyMembers = 64;
    // Game clock struct (see the header comment).
    constexpr std::uintptr_t ClockTurnsPerDayOffset = 0x14;
    constexpr std::uintptr_t ClockTimeOffset = 0x18;
    constexpr std::uintptr_t ClockDayPartOffset = 0x20;
    // +0x00 year, +0x04 day of the year (1-based), +0x10 days per year (100).
    // The exe has no year-part translator (only `day_part` is looked up), so
    // the season is derived: four equal parts of the year.
    constexpr std::uintptr_t ClockDayOffset = 0x04;
    constexpr std::uintptr_t ClockDaysPerYearOffset = 0x10;

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
    constexpr const char* DayPartNames[] = {
        "Midnight", "After Midnight", "Small Hours", "Early Morning", "Morning", "Midday", "Afternoon",
        "Late Afternoon", "Evening", "Late Evening", "Night", "Late Night"};
    constexpr unsigned int DayPartCount = 12;
    constexpr const char* YearPartNames[] = {"Spring", "Summer", "Autumn", "Winter"};
    constexpr unsigned int YearPartCount = 4;

    // What the game clock says; `has` is false when the getter was not located.
    struct GameClock
    {
        bool has = false;
        float time = 0.0f;   // turns since the start of the game
        int dayPart = -1;    // index into DayPartNames, -1 = out of range
        int yearPart = -1;   // index into YearPartNames, derived from day / daysPerYear
        int day = 0;         // day of the year, 1-based
        int daysPerYear = 0;
    };

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
        StorageRef effects, party, player, name, stackable;
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

    inline GameClock ReadClock(const void* clock)
    {
        GameClock out;
        if (clock == nullptr) return out;
        const auto turnsPerDay = Read<std::int32_t>(clock, ClockTurnsPerDayOffset);
        const float time = Read<float>(clock, ClockTimeOffset);
        if (turnsPerDay <= 0 || time != time || time < 0.0f)
        {
            return out;   // not the struct we know
        }
        out.has = true;
        out.time = time;
        const auto dayPart = Read<std::int32_t>(clock, ClockDayPartOffset);
        out.dayPart = dayPart >= 0 && static_cast<unsigned int>(dayPart) < DayPartCount ? dayPart : -1;
        out.day = Read<std::int32_t>(clock, ClockDayOffset);
        out.daysPerYear = Read<std::int32_t>(clock, ClockDaysPerYearOffset);
        if (out.daysPerYear > 0 && out.day >= 1 && out.day <= out.daysPerYear)
        {
            out.yearPart = static_cast<int>((static_cast<long long>(out.day - 1) * YearPartCount) / out.daysPerYear);
        }
        return out;
    }

    // In-order walk of std::map<std::string,V> with V an int or a float
    // (bounded; false on a malformed tree).
    template <typename V>
    bool ReadStringMap(const unsigned char* map, std::map<std::string, double>& out)
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
                out[key] = static_cast<double>(Read<V>(node, MapNodeValue));
            }
            node = Read<const unsigned char*>(node, MapNodeRight);
        }
        return true;
    }

    inline bool ReadStringIntMap(const unsigned char* map, std::map<std::string, double>& out)
    {
        return ReadStringMap<std::int32_t>(map, out);
    }

    // Stacker id -> live stacks of the holder. A stack whose expiry time lies
    // before `now` counts as 0, as in the engine's own on_max_stacks code. `now`
    // comes from the game clock, else from the entity's last effects tick (at
    // most one turn behind). False when the holder has no Effects component.
    inline bool ReadStackers(const StorageRef& effects, std::uint32_t entityId, const GameClock& clock,
        std::map<std::string, double>& out)
    {
        const unsigned char* component = FindComponent(effects, entityId);
        if (component == nullptr)
        {
            return false;
        }
        std::map<std::string, double> counts;
        std::map<std::string, double> expiry;
        if (!ReadStringMap<std::int32_t>(component + EffectsStacksMapOffset, counts)
            || !ReadStringMap<float>(component + EffectsStackExpiryMapOffset, expiry))
        {
            return false;
        }
        const double now = clock.has ? clock.time : Read<float>(component, EffectsLastTickOffset);
        for (const auto& [id, count] : counts)
        {
            if (count <= 0) continue;
            const auto expires = expiry.find(id);
            if (expires != expiry.end() && expires->second < now) continue;
            out[id] = count;
        }
        return true;
    }

    // Entities that currently have the component (the storage's id vector,
    // cross-checked against the page table). -1 when the vector looks wrong.
    inline int CountStorageEntities(const StorageRef& ref, unsigned int maximum)
    {
        if (ref.storage == nullptr) return -1;
        const auto begin = Read<const std::int32_t*>(ref.storage, StorageIdsBeginOffset);
        const auto end = Read<const std::int32_t*>(ref.storage, StorageIdsEndOffset);
        if (begin == nullptr || end < begin || static_cast<std::size_t>(end - begin) > maximum)
        {
            return begin == end ? 0 : -1;
        }
        int count = 0;
        for (const std::int32_t* id = begin; id < end; ++id)
        {
            if (*id > 0 && FindComponent(ref, static_cast<std::uint32_t>(*id)) != nullptr) ++count;
        }
        return count;
    }

    // party / companions for the holder. The player has no Party component;
    // the companions do. For the player: in a party while any companion has
    // one, companions = their number. For a companion: in a party, companions =
    // the other members plus the player. Anyone else is alone with none.
    inline bool ReadParty(const StorageRef& party, const StorageRef& player, std::uint32_t holder,
        soulash::conditions::Snapshot& snapshot)
    {
        if (party.storage == nullptr || player.storage == nullptr) return false;
        const int members = CountStorageEntities(party, MaximumPartyMembers);
        if (members < 0) return false;
        const bool holderIsMember = FindComponent(party, holder) != nullptr;
        const bool holderIsPlayer = FindComponent(player, holder) != nullptr;
        if (holderIsMember)
        {
            snapshot.party = std::string("in_party");
            snapshot.companions = static_cast<double>(members);   // the others plus the player
        }
        else if (holderIsPlayer)
        {
            snapshot.party = std::string(members > 0 ? "in_party" : "alone");
            snapshot.companions = static_cast<double>(members);
        }
        else
        {
            snapshot.party = std::string("alone");
            snapshot.companions = 0.0;
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

    // The entity's data id (its definition's `id`) from its Name component.
    inline bool ReadDataId(const StorageRef& name, std::int32_t entity, std::string& out)
    {
        if (entity <= 0) return false;
        const unsigned char* component = FindComponent(name, static_cast<std::uint32_t>(entity));
        char text[160];
        if (component == nullptr || !ReadMsvcString(component + NameDataIdOffset, text, sizeof(text)) || text[0] == '\0')
        {
            return false;
        }
        out = text;
        return true;
    }

    // Stack count of an item entity: its Stackable count, 1 without the component.
    inline int StackCount(const StorageRef& stackable, std::int32_t entity)
    {
        if (entity <= 0) return 0;
        const unsigned char* component = FindComponent(stackable, static_cast<std::uint32_t>(entity));
        if (component == nullptr) return 1;
        const auto count = Read<std::int32_t>(component, StackableCountOffset);
        return count > 0 ? count : 0;
    }

    // Data id -> carried count over the holder's backpack list. False when the
    // holder has no Equipment component or the Name storage is unavailable
    // (then the whole predicate stays unknown rather than reading zero).
    inline bool ReadInventory(const Access& access, std::uint32_t holder, std::map<std::string, double>& out)
    {
        if (access.name.storage == nullptr) return false;
        const unsigned char* equipment = FindComponent(access.equipment, holder);
        if (equipment == nullptr) return false;
        const auto begin = Read<const std::int32_t*>(equipment, EquipmentInventoryBeginOffset);
        const auto end = Read<const std::int32_t*>(equipment, EquipmentInventoryEndOffset);
        if (begin == nullptr || end < begin || static_cast<std::size_t>(end - begin) > MaximumInventoryItems)
        {
            return begin == end;
        }
        for (const std::int32_t* item = begin; item < end; ++item)
        {
            std::string id;
            if (!ReadDataId(access.name, *item, id)) continue;
            out[id] += StackCount(access.stackable, *item);
        }
        return true;
    }

    // Fills what the registry can answer; everything else stays unknown. The
    // target (0 = none) only contributes target_* predicates; the clock (may
    // be a default-constructed GameClock) the time predicates.
    inline void BuildSnapshot(const Access& access, std::uint32_t holder, std::uint32_t target,
        soulash::conditions::Snapshot& snapshot, const GameClock& clock = GameClock{})
    {
        double value = 0;
        if (ReadHealthPercent(access.health, holder, value)) snapshot.healthPercent = value;
        if (ReadStaminaPercent(access.stamina, holder, value)) snapshot.staminaPercent = value;
        std::set<std::string> tags;
        if (ReadTags(access.tags, holder, tags)) snapshot.holderTags = tags;
        std::map<std::string, double> skills;
        if (ReadSkills(access.skills, holder, skills)) snapshot.skills = skills;
        std::map<std::string, double> stackers;
        if (ReadStackers(access.effects, holder, clock, stackers)) snapshot.stackers = stackers;
        std::map<std::string, double> inventory;
        if (ReadInventory(access, holder, inventory)) snapshot.inventory = inventory;
        ReadParty(access.party, access.player, holder, snapshot);
        if (clock.has && clock.dayPart >= 0) snapshot.dayPart = std::string(DayPartNames[clock.dayPart]);
        if (clock.has && clock.yearPart >= 0) snapshot.season = std::string(YearPartNames[clock.yearPart]);

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
                ReadDataId(access.name, entity, item.item);
                std::set<std::string> itemTags;
                if (ReadTags(access.tags, static_cast<std::uint32_t>(entity), itemTags)) item.itemTags = itemTags;
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
