// Build-independent location of the game's item throw, and the small pieces of
// game memory the Triggers plugin reads or writes for the `throws` rules
// (docs/triggers.md section 8). Header-only, no game calls, no Windows calls:
// the locator reads code bytes of functions found through RTTI / string
// anchors, the helpers work on plain memory so the unit tests can drive them.
//
// Facts (build 526b6ab4 / 0.10.6.1, static analysis; names are ours):
//
//   Throw::update (RTTI `Throw`, vtable slot 1, 0x8D7B0) handles the PLAYER's
//   throw action, once per throw, and returns 1 when it threw (0 otherwise):
//     41 B8 03 00 00 00 8B 15 rel32 48 8B 49 08 E8
//                              hasComponent(ctx, [player id global], 3 = Actor)
//     48 8B 88 00 01 00 00 8B 15 rel32 48 8B 49 18 E8
//                              registry[3] (Actor storage) -> Actor getter (stride 0x60)
//     48 8B 50 20 48 FF CA 48 23 50 28 48 8B 48 18 48 8B 34 D1 83 3E 28 0F 85
//                              front of the Actor's action deque (+0x10, one
//                              action per block), action type 0x28 = throw
//     48 8B 86 90 00 00 00 48 39 86 88 00 00 00 0F 84
//                              the action's path vector<{int x,y,z}> +0x88/+0x90 non-empty
//     8B 56 4C 48 8B 89 A8 01 00 00 E8 rel32 48 89 44 24 70 F2 0F 10 70 18
//                              thrown item = action +0x4C; registry[53] (Position2D,
//                              stride 0x30): x,y +0x18, z +0x20
//     48 8B 8E 90 00 00 00 F2 0F 10 79 F4 F2 0F 11 7C 24 58 8B 59 FC 89 5C 24 60 E8 rel32
//                              landing = last path point; WorldMap() (static object)
//     4C 8D 44 24 50 8B 56 4C E8 rel32
//                              RemoveFromTile(layer, item, const int xy[2]) at the old position
//     4C 8B 7C 24 70 F2 41 0F 11 7F 18 41 89 5F 20
//                              item Position2D = landing
//     4D 39 75 30 74 ?? 49 8D 4D 10 E8
//                              pops the action
//   The layer of a z level (WorldMap 0x7C4930 + the inline loop, also
//   0x7BD010): 0x42 entries of {int, int, int z} at map+0x74 (stride 12); the
//   first with (0, 0, z) selects the layer pointer at map+0x390+i*8, none
//   selects map+0x5A0. A layer is in bounds for 0 <= x < layer+8, 0 <= y < layer+0xC.
//
//   BaseAttack (the function referencing "BaseAttack {}: Deleted entity {}")
//   deletes spent ammo:
//     48 83 E9 80 48 8D 55 F8 E8 rel32 8B D6 49 8B CE E8 rel32
//                              ...; DestroyEntity(ctx, entity)
//   DestroyEntity (0x7CBD50) hashes the id with FNV-1a (48 B8 25 23 22 84 E4
//   9C F2 CB) into the context's entity map and removes every component.
//
//   The queued-ability consumer (the function referencing "Queued Ability",
//   0x744050) pops AbilityUser+0x230 and aims at the LAST point of the
//   vector<{x,y,z}> at AbilityUser+0x188/+0x190/+0x198 when it is non-empty,
//   else at the position of the target entity (+0x1A0). For an aoe_tile
//   ability (target enum 9 at Ability+0xB8) it computes the area around that
//   point and stores it back into +0x188.
#pragma once

#include "ability_bar_layout.hpp"
#include "ability_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace soulash_plugins
{
    struct ThrowLayout
    {
        bool verified = false;
        const char* failure = "not checked";
        const unsigned char* throwUpdate = nullptr;      // Throw::update(system) -> 1 when it threw (hooked)
        const std::int32_t* playerEntityId = nullptr;    // global int: the player entity
        unsigned int actorIndex = 0;                     // registry index of the Actor storage
        unsigned int positionIndex = 0;                  // registry index of the Position2D storage
        unsigned char throwActionType = 0;               // 0x28
        const unsigned char* worldMap = nullptr;         // void* WorldMap()
        const unsigned char* removeFromTile = nullptr;   // void RemoveFromTile(layer*, int entity, const int xy[2])
        const unsigned char* destroyEntity = nullptr;    // void DestroyEntity(ctx*, int entity); optional
        const char* consumeFailure = "not checked";      // why destroyEntity is null
    };

    // Component / object offsets the throw code relies on (see above).
    constexpr std::size_t ActorStride = 0x60;
    constexpr std::size_t ActorActionsOffset = 0x10;     // std::deque<Action> (one action per block)
    constexpr std::size_t ActionTypeOffset = 0x00;
    constexpr std::size_t ActionItemOffset = 0x4C;
    constexpr std::size_t ActionPathBeginOffset = 0x88;
    constexpr std::size_t ActionPathEndOffset = 0x90;
    constexpr std::size_t Position2DStride = 0x30;
    constexpr std::size_t Position2DXOffset = 0x18;      // int x, int y, int z
    constexpr std::size_t PointSize = 12;
    constexpr std::size_t MaximumPathPoints = 4096;
    constexpr std::size_t AbilityUserPositionsOffset = 0x188;   // vector<{x,y,z}> begin, end +8, capacity +16
    constexpr std::size_t WorldMapLevelsOffset = 0x74;
    constexpr std::size_t WorldMapLevelCount = 0x42;
    constexpr std::size_t WorldMapLayersOffset = 0x390;
    constexpr std::size_t WorldMapDefaultLayerOffset = 0x5A0;
    constexpr std::size_t LayerWidthOffset = 0x08;
    constexpr std::size_t LayerHeightOffset = 0x0C;

    struct TilePoint
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        bool operator==(const TilePoint& other) const { return x == other.x && y == other.y && z == other.z; }
        bool operator!=(const TilePoint& other) const { return !(*this == other); }
    };

    // What the front action of the player says about a throw.
    struct ThrowAction
    {
        std::uint32_t item = 0;
        TilePoint landing;
    };

    // The front action, given the Actor component (nullptr / false when the
    // deque is empty, the front is not a throw, or anything looks off).
    inline const unsigned char* ActorFrontAction(const unsigned char* actor)
    {
        if (actor == nullptr) return nullptr;
        const unsigned char* deque = actor + ActorActionsOffset;
        void** const* map = nullptr;
        std::size_t mapSize = 0, offset = 0, size = 0;
        std::memcpy(&map, deque + 0x08, sizeof(map));
        std::memcpy(&mapSize, deque + 0x10, sizeof(mapSize));
        std::memcpy(&offset, deque + 0x18, sizeof(offset));
        std::memcpy(&size, deque + 0x20, sizeof(size));
        if (map == nullptr || size == 0 || mapSize == 0 || mapSize > 0x100000 || (mapSize & (mapSize - 1)) != 0)
        {
            return nullptr;
        }
        return reinterpret_cast<const unsigned char*>(map[(mapSize - 1) & offset]);
    }

    inline bool ReadThrowAction(const unsigned char* action, unsigned char throwType, ThrowAction& out)
    {
        if (action == nullptr) return false;
        std::int32_t type = 0;
        std::memcpy(&type, action + ActionTypeOffset, sizeof(type));
        if (type != throwType) return false;
        const unsigned char* begin = nullptr;
        const unsigned char* end = nullptr;
        std::memcpy(&begin, action + ActionPathBeginOffset, sizeof(begin));
        std::memcpy(&end, action + ActionPathEndOffset, sizeof(end));
        if (begin == nullptr || end <= begin || static_cast<std::size_t>(end - begin) % PointSize != 0
            || static_cast<std::size_t>(end - begin) / PointSize > MaximumPathPoints)
        {
            return false;
        }
        std::memcpy(&out.item, action + ActionItemOffset, sizeof(out.item));
        std::memcpy(&out.landing, end - PointSize, PointSize);
        return out.item != 0;
    }

    inline TilePoint ReadPosition(const unsigned char* position2d)
    {
        TilePoint point;
        std::memcpy(&point, position2d + Position2DXOffset, PointSize);
        return point;
    }

    // The tile layer of a z level, or nullptr; `inBounds` also checks x / y.
    inline const unsigned char* FindLayer(const unsigned char* worldMap, const TilePoint& at, bool* inBounds = nullptr)
    {
        if (inBounds != nullptr) *inBounds = false;
        if (worldMap == nullptr) return nullptr;
        std::size_t slot = WorldMapDefaultLayerOffset;
        for (std::size_t index = 0; index < WorldMapLevelCount; ++index)
        {
            std::int32_t level[3];
            std::memcpy(level, worldMap + WorldMapLevelsOffset + index * PointSize, sizeof(level));
            if (level[0] == 0 && level[1] == 0 && level[2] == at.z)
            {
                slot = WorldMapLayersOffset + index * sizeof(void*);
                break;
            }
        }
        const unsigned char* layer = nullptr;
        std::memcpy(&layer, worldMap + slot, sizeof(layer));
        if (layer != nullptr && inBounds != nullptr)
        {
            std::int32_t width = 0, height = 0;
            std::memcpy(&width, layer + LayerWidthOffset, sizeof(width));
            std::memcpy(&height, layer + LayerHeightOffset, sizeof(height));
            *inBounds = at.x >= 0 && at.x < width && at.y >= 0 && at.y < height;
        }
        return layer;
    }

    // MSVC std::vector<{int x,y,z}>: begin, end, capacity.
    struct PointVector
    {
        unsigned char* begin;
        unsigned char* end;
        unsigned char* capacity;
    };

    // Makes the vector hold exactly `point`, reusing its buffer when it has
    // room, else allocate(12) and release(old, oldCapacityBytes) - the game's
    // operator new / operator delete(p, size), as the vector itself would.
    template <class Allocate, class Release>
    bool SetSinglePoint(PointVector& vector, const TilePoint& point, Allocate allocate, Release release)
    {
        if (vector.begin != nullptr && vector.capacity >= vector.begin
            && static_cast<std::size_t>(vector.capacity - vector.begin) >= PointSize)
        {
            std::memcpy(vector.begin, &point, PointSize);
            vector.end = vector.begin + PointSize;
            return true;
        }
        auto* memory = static_cast<unsigned char*>(allocate(PointSize));
        if (memory == nullptr) return false;
        std::memcpy(memory, &point, PointSize);
        if (vector.begin != nullptr)
        {
            release(vector.begin, static_cast<std::size_t>(vector.capacity - vector.begin));
        }
        vector.begin = memory;
        vector.end = memory + PointSize;
        vector.capacity = memory + PointSize;
        return true;
    }

    // Front element of a std::deque<T*> (block size 2), or nullptr.
    inline void* DequeFront(const DequeOfPointers& deque)
    {
        if (deque.size == 0 || deque.map == nullptr || deque.mapSize == 0 || (deque.mapSize & (deque.mapSize - 1)) != 0)
        {
            return nullptr;
        }
        void** block = deque.map[(deque.offset / DequeBlockElements) & (deque.mapSize - 1)];
        return block == nullptr ? nullptr : block[deque.offset % DequeBlockElements];
    }

    namespace throw_layout_detail
    {
        using ability_bar_layout_detail::Any;
        using ability_bar_layout_detail::Find;
        using ability_bar_layout_detail::InImage;
        using ability_bar_layout_detail::Region;
        using ability_bar_layout_detail::Rel32;
        using ability_bar_layout_detail::RelTarget;

        template <std::size_t N>
        bool Has(const Region& image, const unsigned char* from, std::size_t window, const int (&pattern)[N])
        {
            return Find(image, from, window, pattern) != nullptr;
        }

        template <std::size_t N>
        const unsigned char* CallAfter(const Region& image, const unsigned char* from, std::size_t window,
            const int (&pattern)[N])
        {
            const unsigned char* at = Find(image, from, window, pattern);
            if (at == nullptr || !InImage(image, at + N, 4)) return nullptr;
            const unsigned char* target = RelTarget(at + N, at + N + 4);
            return InImage(image, target, 16) ? target : nullptr;
        }
    }

    // throwUpdate: slot 1 of the `Throw` vtable. baseAttack: the function
    // referencing "BaseAttack {}: Deleted entity {}" (may be null: then thrown
    // items cannot be consumed, everything else still works).
    inline ThrowLayout LocateThrowLayout(const unsigned char* image, std::size_t imageSize,
        const unsigned char* throwUpdate, const unsigned char* baseAttack)
    {
        using namespace throw_layout_detail;
        ThrowLayout result;
        if (image == nullptr || imageSize == 0)
        {
            result.failure = "no game image";
            return result;
        }
        const Region region{image, image + imageSize};
        if (throwUpdate == nullptr || !InImage(region, throwUpdate, 16))
        {
            result.failure = "Throw::update (RTTI Throw, slot 1) not found";
            return result;
        }
        constexpr std::size_t Window = 0x800;

        static const int playerSite[] = {0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x8B, 0x15, Any, Any, Any, Any,
            0x48, 0x8B, 0x49, 0x08, 0xE8};
        const unsigned char* player = Find(region, throwUpdate, 0x80, playerSite);
        if (player == nullptr)
        {
            result.failure = "Throw::update: player Actor check not found";
            return result;
        }
        const unsigned char* playerGlobal = RelTarget(player + 8, player + 12);
        static const int actorSite[] = {0x48, 0x8B, 0x88, 0x00, 0x01, 0x00, 0x00, 0x8B, 0x15, Any, Any, Any, Any,
            0x48, 0x8B, 0x49, Any, 0xE8};
        const unsigned char* actor = Find(region, player, 0x60, actorSite);
        if (actor == nullptr || RelTarget(actor + 9, actor + 13) != playerGlobal || actor[16] % 8 != 0
            || actor[16] == 0)
        {
            result.failure = "Throw::update: Actor storage lookup not found";
            return result;
        }
        static const int frontSite[] = {0x48, 0x8B, 0x50, 0x20, 0x48, 0xFF, 0xCA, 0x48, 0x23, 0x50, 0x28,
            0x48, 0x8B, 0x48, 0x18, 0x48, 0x8B, 0x34, 0xD1, 0x83, 0x3E, Any, 0x0F, 0x85};
        static const int pathSite[] = {0x48, 0x8B, 0x86, 0x90, 0x00, 0x00, 0x00, 0x48, 0x39, 0x86, 0x88, 0x00, 0x00, 0x00,
            0x0F, 0x84};
        const unsigned char* front = Find(region, actor, 0x40, frontSite);
        if (front == nullptr || front[21] != 0x28 || !Has(region, front, 0x40, pathSite))
        {
            result.failure = "Throw::update: throw action check (type 0x28, path +0x88) not found";
            return result;
        }
        static const int positionSite[] = {0x8B, 0x56, 0x4C, 0x48, 0x8B, 0x89, Any, Any, 0x00, 0x00, 0xE8, Any, Any, Any, Any,
            0x48, 0x89, 0x44, 0x24, 0x70, 0xF2, 0x0F, 0x10, 0x70, 0x18};
        const unsigned char* position = Find(region, front, 0x80, positionSite);
        std::int32_t positionSlot = position != nullptr ? Rel32(position + 6) : 0;
        if (position == nullptr || positionSlot <= 0 || positionSlot % 8 != 0 || positionSlot > 0x400)
        {
            result.failure = "Throw::update: item Position2D lookup (+0x4C, x/y +0x18) not found";
            return result;
        }
        static const int landingSite[] = {0x48, 0x8B, 0x8E, 0x90, 0x00, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x79, 0xF4,
            0xF2, 0x0F, 0x11, 0x7C, 0x24, 0x58, 0x8B, 0x59, 0xFC, 0x89, 0x5C, 0x24, 0x60, 0xE8};
        static const int boundsSite[] = {0x4C, 0x8B, 0xD0, 0x48, 0x8D, 0x54, 0x24, 0x78, 0x48, 0x8B, 0xC8, 0xE8};
        const unsigned char* worldMap = CallAfter(region, position, 0x60, landingSite);
        if (worldMap == nullptr || !Has(region, position, 0x80, boundsSite))
        {
            result.failure = "Throw::update: landing point / world map lookup not found";
            return result;
        }
        static const int levelLoop[] = {0x48, 0x83, 0xF9, 0x42};
        static const int defaultLayer[] = {0x49, 0x8D, 0x8A, 0xA0, 0x05, 0x00, 0x00};
        static const int levelLayer[] = {0x48, 0x83, 0xC1, 0x72};
        static const int levelCompare[] = {0x41, 0x83, 0x7C, 0x82, 0x74, 0x00, 0x75, Any, 0x41, 0x83, 0x7C, 0x82, 0x78, 0x00};
        if (!Has(region, position, 0x100, levelLoop) || !Has(region, position, 0x100, defaultLayer)
            || !Has(region, position, 0x100, levelCompare) || !Has(region, position, 0x100, levelLayer))
        {
            result.failure = "Throw::update: z-level layer lookup does not match";
            return result;
        }
        static const int removeSite[] = {0x4C, 0x8D, 0x44, 0x24, 0x50, 0x8B, 0x56, 0x4C, 0xE8};
        static const int removeHead[] = {0x4C, 0x8B, 0x91, 0xB0, 0x00, 0x00, 0x00};
        const unsigned char* remove = CallAfter(region, position, 0x100, removeSite);
        if (remove == nullptr || !Has(region, remove, 0x20, removeHead))
        {
            result.failure = "Throw::update: remove-from-tile call not found";
            return result;
        }
        static const int writeSite[] = {0x4C, 0x8B, 0x7C, 0x24, 0x70, 0xF2, 0x41, 0x0F, 0x11, 0x7F, 0x18,
            0x41, 0x89, 0x5F, 0x20};
        static const int popSite[] = {0x4D, 0x39, 0x75, 0x30, 0x74, Any, 0x49, 0x8D, 0x4D, 0x10, 0xE8};
        if (!Has(region, throwUpdate, Window, writeSite) || !Has(region, throwUpdate, Window, popSite))
        {
            result.failure = "Throw::update: landing write / action pop not found";
            return result;
        }

        result.throwUpdate = throwUpdate;
        result.playerEntityId = reinterpret_cast<const std::int32_t*>(playerGlobal);
        result.actorIndex = actor[16] / 8u;
        result.positionIndex = static_cast<unsigned int>(positionSlot / 8);
        result.throwActionType = front[21];
        result.worldMap = worldMap;
        result.removeFromTile = remove;
        result.verified = InImage(region, playerGlobal, 4);
        result.failure = result.verified ? "" : "Throw::update: player id global outside the image";

        // --- consumption (optional) ------------------------------------------
        if (baseAttack == nullptr || !InImage(region, baseAttack, 16))
        {
            result.consumeFailure = "BaseAttack (\"Deleted entity\") not found";
            return result;
        }
        static const int deleteSite[] = {0x48, 0x83, 0xE9, 0x80, 0x48, 0x8D, 0x55, 0xF8, 0xE8, Any, Any, Any, Any,
            0x8B, 0xD6, 0x49, 0x8B, 0xCE, 0xE8};
        static const int fnvBasis[] = {0x48, 0xB8, 0x25, 0x23, 0x22, 0x84, 0xE4, 0x9C, 0xF2, 0xCB};
        static const int fnvPrime[] = {0x49, 0xBF, 0xB3, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
        const unsigned char* destroy = CallAfter(region, baseAttack, 0x1800, deleteSite);
        if (destroy == nullptr || !Has(region, destroy, 0x40, fnvBasis) || !Has(region, destroy, 0x60, fnvPrime))
        {
            result.consumeFailure = "BaseAttack: DestroyEntity call does not match";
            return result;
        }
        result.destroyEntity = destroy;
        result.consumeFailure = "";
        return result;
    }
}
