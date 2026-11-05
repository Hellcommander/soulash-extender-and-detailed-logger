// Original (unpatched) value of a pointer slot inside the game image.
//
// Several plugins hook the same DrawActionBar vtable slot; plugins load in
// alphabetical order, so every plugin after the first sees the previous
// plugin's hook (a pointer into that plugin's DLL) instead of the game's
// function. Code verification needs the game's function, so this reads the
// slot's bytes from the executable FILE (.rdata on disk) and rebases them to
// where the image is loaded. Hooks still chain through the CURRENT slot value.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace soulash_plugins
{
    // Pure: PE32+ headers (the first bytes of the file) -> file offset of an RVA
    // and the preferred image base. Requires the 8 bytes at rva to lie in a
    // section's raw data.
    inline bool PeRvaToFileOffset(const unsigned char* headers, std::size_t size, std::uint32_t rva,
        std::uint64_t& fileOffset, std::uint64_t& preferredBase)
    {
        if (headers == nullptr || size < 0x40 || headers[0] != 'M' || headers[1] != 'Z')
        {
            return false;
        }
        std::uint32_t pe = 0;
        std::memcpy(&pe, headers + 0x3C, 4);
        if (pe > size || size - pe < 24 + 0x70 || std::memcmp(headers + pe, "PE\0\0", 4) != 0)
        {
            return false;
        }
        std::uint16_t sections = 0, optionalSize = 0, magic = 0;
        std::memcpy(&sections, headers + pe + 6, 2);
        std::memcpy(&optionalSize, headers + pe + 20, 2);
        const std::size_t optional = pe + 24;
        std::memcpy(&magic, headers + optional, 2);
        if (magic != 0x20B)   // PE32+
        {
            return false;
        }
        std::memcpy(&preferredBase, headers + optional + 24, 8);
        const std::size_t table = optional + optionalSize;
        if (table > size || (size - table) / 40 < sections)
        {
            return false;
        }
        for (std::uint16_t i = 0; i < sections; ++i)
        {
            const unsigned char* section = headers + table + 40u * i;
            std::uint32_t virtualAddress = 0, rawSize = 0, rawPointer = 0;
            std::memcpy(&virtualAddress, section + 12, 4);
            std::memcpy(&rawSize, section + 16, 4);
            std::memcpy(&rawPointer, section + 20, 4);
            if (rva >= virtualAddress && static_cast<std::uint64_t>(rva) + 8 <= static_cast<std::uint64_t>(virtualAddress) + rawSize)
            {
                fileOffset = static_cast<std::uint64_t>(rawPointer) + (rva - virtualAddress);
                return true;
            }
        }
        return false;
    }

    // The value *slot had in the executable file, rebased to `image`. False when
    // the slot is outside the image, the file cannot be read, or the value does
    // not point into the image.
    inline bool ReadOriginalImagePointer(const unsigned char* image, std::size_t imageSize, const void* const* slot,
        const unsigned char*& original)
    {
        const auto* at = reinterpret_cast<const unsigned char*>(slot);
        if (image == nullptr || imageSize < 8 || at < image || at > image + imageSize - 8)
        {
            return false;
        }
        wchar_t path[MAX_PATH * 2];
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(const_cast<unsigned char*>(image)), path,
            static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
        if (length == 0 || length >= sizeof(path) / sizeof(path[0]))
        {
            return false;
        }
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        unsigned char headers[4096];
        DWORD read = 0;
        std::uint64_t offset = 0, preferredBase = 0, value = 0;
        bool ok = ReadFile(file, headers, sizeof(headers), &read, nullptr) != FALSE
            && PeRvaToFileOffset(headers, read, static_cast<std::uint32_t>(at - image), offset, preferredBase);
        if (ok)
        {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE
                && ReadFile(file, &value, sizeof(value), &read, nullptr) != FALSE && read == sizeof(value);
        }
        CloseHandle(file);
        if (!ok || value < preferredBase || value - preferredBase >= imageSize)
        {
            return false;
        }
        original = image + (value - preferredBase);
        return true;
    }

    // The code pointer to VERIFY for a vtable slot: the current value when it
    // still points into the game image, otherwise the value from the file.
    inline const unsigned char* OriginalCodePointer(const unsigned char* image, std::size_t imageSize, const void* const* slot)
    {
        const auto* current = static_cast<const unsigned char*>(*slot);
        if (image != nullptr && current >= image && current < image + imageSize)
        {
            return current;
        }
        const unsigned char* original = nullptr;
        return ReadOriginalImagePointer(image, imageSize, slot, original) ? original : current;
    }
}
