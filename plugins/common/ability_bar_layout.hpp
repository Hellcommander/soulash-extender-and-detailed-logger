// Build-independent verification of the ability-bar facts the AbilityBar*
// plugins rely on. Header-only, no game calls, read-only.
//
// The plugins used to accept exactly one game build (by exe SHA-256) and carry
// that build's address of the player-entity-id global. Game updates move that
// global (0xAFE668 in 924e1ef3 / 0.10.4.0.1, 0xB07668 in 60b72c10 / 0.10.5.1)
// while every struct offset stayed the same. So instead of a hash list, the
// plugins read DrawActionBar's own code (found through RTTI) and:
//   - take the player-id global from its `mov edx,[rip+X]` loads, and
//   - check, byte for byte, every instruction pattern whose offsets they use
//     (docs/ability-bar.md section 1 explains each one).
// If any pattern is missing the build is treated as unknown and the plugin
// stays inactive, exactly as an unknown hash did before.
//
// Patterns (x64, as emitted for 924e1ef3 and 60b72c10; `??` = any byte):
//   DrawActionBar::update (vtable slot 1)
//     8B 15 rel32                          mov edx,[playerId]  (all loads agree)
//     48 8B 88 00 01 00 00 48 8B 89 A0 01 00 00 E8 rel32
//                                          context+0x100 -> registry+0x1A0 (UI state storage), getter call
//     83 78 60 29                          cmp [ui+0x60],29h   (UI mode at +0x60)
//     48 8B 89 00 01 00 00 48 8B 49 08 E8 rel32
//                                          context+0x100 -> registry+0x08 (AbilityUser storage), getter call
//     F3 0F 58 ?? B0 00 00 00              addss xmm,[abilityUser+0xB0]
//   both getters (ECStorage<T>::get)
//     ?? C1 E8 0D                          page = id >> 13
//     48 8B ?? 08 48 8B ?? 10              pages vector at +0x08/+0x10
//     81 ?? FF 1F 00 00                    slot = id & 0x1FFF
//     48 8B ?? 38 48 8B ?? 40              dense array at +0x38/+0x40
//     48 69 ?? <stride>                    stride 0x260 (AbilityUser) / 0x130 (UI state)
//   DrawActionBar::handleInput (vtable slot 4)
//     48 BA 00 04 06 00 32 01 00 00        bar-input blocked-mode mask 0x13200060400
//     8B 15 rel32 48 8B 49 08 E8 rel32 33 C9 39 48 68 0F 94 C1 89 48 68
//                                          set = ([abilityUser+0x68] == 0)  (action 65)
//     optional (Trigger=overbar only): 0F 2F ?? 20 .. F3 0F 10 ?? 28 .. F3 0F 10 ?? 14 .. 0F 2F ?? 24
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace soulash_plugins
{
    struct AbilityBarLayout
    {
        bool verified = false;
        const char* failure = "not checked";       // first failed check when !verified
        const std::uint32_t* playerEntityId = nullptr;
        std::uintptr_t playerEntityIdRva = 0;
        bool barRectVerified = false;               // cursor-over-bar offsets (+0x20/+0x28/+0x14/+0x24)
    };

    namespace ability_bar_layout_detail
    {
        constexpr int Any = -1;

        struct Region
        {
            const unsigned char* imageBegin;
            const unsigned char* imageEnd;
        };

        inline bool InImage(const Region& image, const unsigned char* p, std::size_t length)
        {
            return p >= image.imageBegin && p <= image.imageEnd
                && static_cast<std::size_t>(image.imageEnd - p) >= length;
        }

        // First match of `pattern` (Any = wildcard) in [from, from+window), clamped to the image.
        inline const unsigned char* Find(
            const Region& image, const unsigned char* from, std::size_t window,
            const int* pattern, std::size_t patternLength)
        {
            if (from == nullptr || !InImage(image, from, 1))
            {
                return nullptr;
            }
            const std::size_t available = static_cast<std::size_t>(image.imageEnd - from);
            if (window > available)
            {
                window = available;
            }
            if (window < patternLength)
            {
                return nullptr;
            }
            for (std::size_t i = 0; i + patternLength <= window; ++i)
            {
                std::size_t k = 0;
                while (k < patternLength && (pattern[k] == Any || from[i + k] == pattern[k]))
                {
                    ++k;
                }
                if (k == patternLength)
                {
                    return from + i;
                }
            }
            return nullptr;
        }

        template <std::size_t N>
        const unsigned char* Find(const Region& image, const unsigned char* from, std::size_t window, const int (&pattern)[N])
        {
            return Find(image, from, window, pattern, N);
        }

        inline std::int32_t Rel32(const unsigned char* p)
        {
            std::int32_t value;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // Target of a rel32 field at `field`, where the instruction ends at `next`.
        inline const unsigned char* RelTarget(const unsigned char* field, const unsigned char* next)
        {
            return next + Rel32(field);
        }

        inline bool GetterMatches(const Region& image, const unsigned char* getter, std::uint32_t stride)
        {
            constexpr std::size_t Window = 0x120;
            static const int pageShift[] = {Any, 0xC1, 0xE8, 0x0D};
            static const int pages[] = {0x48, 0x8B, Any, 0x08, 0x48, 0x8B, Any, 0x10};
            static const int slotMask[] = {0x81, Any, 0xFF, 0x1F, 0x00, 0x00};
            static const int dense[] = {0x48, 0x8B, Any, 0x38, 0x48, 0x8B, Any, 0x40};
            const int strideImul[] = {0x48, 0x69, Any,
                static_cast<int>(stride & 0xFF), static_cast<int>((stride >> 8) & 0xFF),
                static_cast<int>((stride >> 16) & 0xFF), static_cast<int>((stride >> 24) & 0xFF)};
            return Find(image, getter, Window, pageShift) != nullptr
                && Find(image, getter, Window, pages) != nullptr
                && Find(image, getter, Window, slotMask) != nullptr
                && Find(image, getter, Window, dense) != nullptr
                && Find(image, getter, Window, strideImul) != nullptr;
        }
    }

    // image: the mapped game executable (imageSize bytes from its base).
    // update / handleInput: DrawActionBar vtable slots 1 and 4, in that mapping.
    inline AbilityBarLayout VerifyAbilityBarLayout(
        const unsigned char* image, std::size_t imageSize,
        const unsigned char* update, const unsigned char* handleInput)
    {
        using namespace ability_bar_layout_detail;
        AbilityBarLayout result;
        if (image == nullptr || imageSize == 0)
        {
            result.failure = "no game image";
            return result;
        }
        const Region region{image, image + imageSize};
        if (update == nullptr || !InImage(region, update, 1)
            || handleInput == nullptr || !InImage(region, handleInput, 1))
        {
            result.failure = "DrawActionBar update/handleInput outside the game image";
            return result;
        }

        // --- update: player-id loads (all must agree) ---------------------------
        constexpr std::size_t UpdateWindow = 0xC0;
        static const int loadPlayerId[] = {0x8B, 0x15, Any, Any, Any, Any};
        const unsigned char* playerId = nullptr;
        unsigned loads = 0;
        for (const unsigned char* at = update;
             static_cast<std::size_t>(at - update) < UpdateWindow
             && (at = Find(region, at, UpdateWindow - static_cast<std::size_t>(at - update), loadPlayerId)) != nullptr;
             at += sizeof(loadPlayerId) / sizeof(loadPlayerId[0]))
        {
            const unsigned char* target = RelTarget(at + 2, at + 6);
            if (playerId != nullptr && target != playerId)
            {
                result.failure = "update: player-id loads disagree";
                return result;
            }
            playerId = target;
            ++loads;
        }
        if (loads < 2 || !InImage(region, playerId, sizeof(std::uint32_t)))
        {
            result.failure = "update: player-id global not found";
            return result;
        }

        static const int uiStorage[] = {0x48, 0x8B, 0x88, 0x00, 0x01, 0x00, 0x00,
            0x48, 0x8B, 0x89, 0xA0, 0x01, 0x00, 0x00, 0xE8};
        const unsigned char* uiCall = Find(region, update, UpdateWindow, uiStorage);
        if (uiCall == nullptr)
        {
            result.failure = "update: context+0x100 -> registry+0x1A0 not found";
            return result;
        }
        const unsigned char* uiCallField = uiCall + sizeof(uiStorage) / sizeof(uiStorage[0]);
        if (!InImage(region, uiCallField, 4)
            || !GetterMatches(region, RelTarget(uiCallField, uiCallField + 4), 0x130))
        {
            result.failure = "UI-state getter does not match (page table / stride 0x130)";
            return result;
        }
        static const int uiMode[] = {0x83, 0x78, 0x60, 0x29};
        if (Find(region, update, UpdateWindow, uiMode) == nullptr)
        {
            result.failure = "update: UI mode compare at +0x60 not found";
            return result;
        }
        static const int abilityStorage[] = {0x48, 0x8B, 0x89, 0x00, 0x01, 0x00, 0x00,
            0x48, 0x8B, 0x49, 0x08, 0xE8};
        const unsigned char* abilityCall = Find(region, update, UpdateWindow, abilityStorage);
        if (abilityCall == nullptr)
        {
            result.failure = "update: context+0x100 -> registry+0x08 not found";
            return result;
        }
        const unsigned char* abilityCallField = abilityCall + sizeof(abilityStorage) / sizeof(abilityStorage[0]);
        if (!InImage(region, abilityCallField, 4)
            || !GetterMatches(region, RelTarget(abilityCallField, abilityCallField + 4), 0x260))
        {
            result.failure = "AbilityUser getter does not match (page table / stride 0x260)";
            return result;
        }
        static const int timer[] = {0xF3, 0x0F, 0x58, Any, 0xB0, 0x00, 0x00, 0x00};
        if (Find(region, update, UpdateWindow, timer) == nullptr)
        {
            result.failure = "update: AbilityUser+0xB0 timer not found";
            return result;
        }

        // --- handleInput ----------------------------------------------------------
        constexpr std::size_t InputWindow = 0x500;
        static const int blockedModes[] = {0x48, Any, 0x00, 0x04, 0x06, 0x00, 0x32, 0x01, 0x00, 0x00};
        if (Find(region, handleInput, InputWindow, blockedModes) == nullptr)
        {
            result.failure = "handleInput: blocked-mode mask 0x13200060400 not found";
            return result;
        }
        static const int toggle[] = {0x8B, 0x15, Any, Any, Any, Any,
            0x48, 0x8B, 0x49, 0x08, 0xE8, Any, Any, Any, Any,
            0x33, 0xC9, 0x39, 0x48, 0x68, 0x0F, 0x94, 0xC1, 0x89, 0x48, 0x68};
        const unsigned char* toggleAt = Find(region, handleInput, InputWindow, toggle);
        if (toggleAt == nullptr)
        {
            result.failure = "handleInput: AbilityUser+0x68 toggle (action 65) not found";
            return result;
        }
        if (RelTarget(toggleAt + 2, toggleAt + 6) != playerId)
        {
            result.failure = "handleInput: toggle reads a different player-id global";
            return result;
        }

        // Optional: the cursor-over-bar rectangle (only Trigger=overbar uses it).
        static const int rectLeft[] = {0x0F, 0x2F, Any, 0x20};
        static const int rectRight[] = {0xF3, 0x0F, 0x10, Any, 0x28};
        static const int rectTop[] = {0xF3, 0x0F, 0x10, Any, 0x14};
        static const int rectBottom[] = {0x0F, 0x2F, Any, 0x24};
        const unsigned char* left = Find(region, handleInput, InputWindow, rectLeft);
        const unsigned char* right = left == nullptr ? nullptr : Find(region, left, 0x20, rectRight);
        const unsigned char* top = right == nullptr ? nullptr : Find(region, right, 0x20, rectTop);
        const unsigned char* bottom = top == nullptr ? nullptr : Find(region, top, 0x20, rectBottom);
        result.barRectVerified = bottom != nullptr;

        result.verified = true;
        result.failure = "";
        result.playerEntityId = reinterpret_cast<const std::uint32_t*>(playerId);
        result.playerEntityIdRva = static_cast<std::uintptr_t>(playerId - image);
        return result;
    }

    // Reads an MSVC x64 std::string (16-byte SSO buffer or heap pointer, size at
    // +0x10, capacity at +0x18) into `out` (NUL-terminated). False when the
    // object does not look like a string or does not fit `outSize - 1` chars.
    // Callers that read game memory wrap this in __try.
    inline bool ReadMsvcString(const void* object, char* out, std::size_t outSize)
    {
        if (object == nullptr || out == nullptr || outSize == 0)
        {
            return false;
        }
        out[0] = '\0';
        const auto* s = static_cast<const unsigned char*>(object);
        std::uint64_t size;
        std::uint64_t capacity;
        std::memcpy(&size, s + 0x10, sizeof(size));
        std::memcpy(&capacity, s + 0x18, sizeof(capacity));
        if (capacity < 15 || size > capacity || size >= outSize)
        {
            return false;
        }
        const unsigned char* data = s;
        if (capacity > 15)
        {
            std::memcpy(&data, s, sizeof(data));
            if (data == nullptr)
            {
                return false;
            }
        }
        std::memcpy(out, data, static_cast<std::size_t>(size));
        out[size] = '\0';
        return true;
    }

    // Where the current save's folder name lives (static data; read-only use).
    // The player-save writer (the function that references "Saving player")
    // builds the save folder as
    //   SDL_GetPrefPath("WizardsOfTheCode", "Soulash2") + "saves/" + <name>
    // with `name` a std::string inside a static singleton:
    //   E8 rel32                 call getSingleton
    //   48 8D 90 imm32           lea rdx,[rax+nameOffset]      (0xC08 in 60b72c10)
    //   48 8D 4C 24 ?? E8        lea rcx,[rsp+..]; call buildSaveFolder
    // and getSingleton constructs it in place:
    //   41 B8 imm32 48 8D 1D rel32   mov r8d,objectSize (0xC30); lea rbx,[object]
    struct SaveNameLocation
    {
        bool found = false;
        const char* failure = "not checked";
        const void* nameString = nullptr;       // MSVC std::string
        std::uintptr_t singletonRva = 0;
        std::uint32_t nameOffset = 0;
    };

    inline SaveNameLocation LocateSaveName(const unsigned char* image, std::size_t imageSize, const unsigned char* saver)
    {
        using namespace ability_bar_layout_detail;
        SaveNameLocation result;
        const Region region{image, image + imageSize};
        if (image == nullptr || saver == nullptr || !InImage(region, saver, 1))
        {
            result.failure = "save writer outside the game image";
            return result;
        }
        static const int access[] = {0xE8, Any, Any, Any, Any, 0x48, 0x8D, 0x90, Any, Any, Any, Any,
            0x48, 0x8D, 0x4C, 0x24, Any, 0xE8};
        const unsigned char* at = Find(region, saver, 0x200, access);
        if (at == nullptr)
        {
            result.failure = "save writer: singleton name access not found";
            return result;
        }
        const unsigned char* getter = RelTarget(at + 1, at + 5);
        std::uint32_t nameOffset;
        std::memcpy(&nameOffset, at + 8, sizeof(nameOffset));
        static const int construct[] = {0x41, 0xB8, Any, Any, Any, Any, 0x48, 0x8D, 0x1D, Any, Any, Any, Any};
        const unsigned char* ctor = InImage(region, getter, 1) ? Find(region, getter, 0x80, construct) : nullptr;
        if (ctor == nullptr)
        {
            result.failure = "singleton getter: static construction not found";
            return result;
        }
        std::uint32_t objectSize;
        std::memcpy(&objectSize, ctor + 2, sizeof(objectSize));
        const unsigned char* object = RelTarget(ctor + 9, ctor + 13);
        if (objectSize > 0x100000 || nameOffset + 0x20u > objectSize || !InImage(region, object, objectSize))
        {
            result.failure = "singleton: name offset outside the object";
            return result;
        }
        result.found = true;
        result.failure = "";
        result.nameString = object + nameOffset;
        result.singletonRva = static_cast<std::uintptr_t>(object - image);
        result.nameOffset = nameOffset;
        return result;
    }

    // SizeOfImage from the PE optional header of a mapped image (0 if malformed).
    inline std::size_t MappedImageSize(const void* base)
    {
        const auto* b = static_cast<const unsigned char*>(base);
        if (b == nullptr || b[0] != 'M' || b[1] != 'Z')
        {
            return 0;
        }
        std::int32_t peOffset;
        std::memcpy(&peOffset, b + 0x3C, sizeof(peOffset));
        if (peOffset <= 0 || peOffset > 0x1000 || std::memcmp(b + peOffset, "PE\0\0", 4) != 0)
        {
            return 0;
        }
        std::uint16_t magic;
        std::memcpy(&magic, b + peOffset + 24, sizeof(magic));
        if (magic != 0x20B)
        {
            return 0;
        }
        std::uint32_t sizeOfImage;
        std::memcpy(&sizeOfImage, b + peOffset + 24 + 56, sizeof(sizeOfImage));
        return sizeOfImage;
    }
}
