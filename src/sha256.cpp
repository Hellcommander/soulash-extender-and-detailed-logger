#include "sha256.hpp"

#include <bcrypt.h>

#include <cstdint>

namespace soulash
{
    bool Sha256OfHandle(HANDLE file, char (&hex)[65])
    {
        constexpr std::uint64_t MaximumHashBytes = 512ull * 1024 * 1024;

        LARGE_INTEGER size{};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        BYTE hashObject[4096]{};
        DWORD objectLength = 0;
        DWORD ignored = 0;
        BYTE digest[32]{};
        bool ok = GetFileSizeEx(file, &size) != FALSE
            && size.QuadPart >= 0
            && static_cast<std::uint64_t>(size.QuadPart) <= MaximumHashBytes
            && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
            && BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength), &ignored, 0))
            && objectLength <= sizeof(hashObject)
            && BCRYPT_SUCCESS(BCryptCreateHash(
                algorithm, &hash, hashObject, objectLength, nullptr, 0, 0));

        BYTE chunk[16384];
        while (ok)
        {
            DWORD read = 0;
            if (!ReadFile(file, chunk, sizeof(chunk), &read, nullptr))
            {
                ok = false;
            }
            else if (read == 0)
            {
                break;
            }
            else
            {
                ok = BCRYPT_SUCCESS(BCryptHashData(hash, chunk, read, 0));
            }
        }
        ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);

        if (ok)
        {
            static constexpr char Hex[] = "0123456789abcdef";
            for (unsigned i = 0; i < sizeof(digest); ++i)
            {
                hex[i * 2] = Hex[digest[i] >> 4];
                hex[i * 2 + 1] = Hex[digest[i] & 0x0f];
            }
            hex[64] = '\0';
        }
        return ok;
    }
}
