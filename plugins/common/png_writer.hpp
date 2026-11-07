// Minimal PNG encoder for TestBot snapshots (24-bit RGB, no interlace, filter 0).
// Pure and header-only so tests/tests.cpp can check it. Compression comes from
// zlib's compress2 when the caller passes it (the game ships and loads
// zlib1.dll); without it the image data is stored in uncompressed deflate
// blocks, which every PNG reader accepts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace soulash_plugins::png
{
    // zlib: int compress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen, int level)
    using Compress2Fn = int (*)(unsigned char* dest, unsigned long* destLength,
        const unsigned char* source, unsigned long sourceLength, int level);

    constexpr int MaxDimension = 16384;

    inline std::uint32_t Crc32(const unsigned char* data, std::size_t size, std::uint32_t crc = 0)
    {
        crc = ~crc;
        for (std::size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    }

    inline std::uint32_t Adler32(const unsigned char* data, std::size_t size)
    {
        std::uint32_t a = 1, b = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            a = (a + data[i]) % 65521u;
            b = (b + a) % 65521u;
        }
        return (b << 16) | a;
    }

    inline void PutBigEndian(std::vector<unsigned char>& out, std::uint32_t value)
    {
        out.push_back(static_cast<unsigned char>(value >> 24));
        out.push_back(static_cast<unsigned char>(value >> 16));
        out.push_back(static_cast<unsigned char>(value >> 8));
        out.push_back(static_cast<unsigned char>(value));
    }

    inline void PutChunk(std::vector<unsigned char>& out, const char type[4], const unsigned char* data, std::size_t size)
    {
        PutBigEndian(out, static_cast<std::uint32_t>(size));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        if (size > 0)
        {
            out.insert(out.end(), data, data + size);
        }
        PutBigEndian(out, Crc32(out.data() + start, size + 4));
    }

    // zlib stream with stored (uncompressed) deflate blocks.
    inline std::vector<unsigned char> StoredZlib(const unsigned char* data, std::size_t size)
    {
        std::vector<unsigned char> out;
        out.reserve(size + size / 65535 * 5 + 16);
        out.push_back(0x78);
        out.push_back(0x01);
        std::size_t position = 0;
        do
        {
            const std::size_t length = size - position > 65535 ? 65535 : size - position;
            const bool last = position + length == size;
            out.push_back(last ? 1 : 0);
            out.push_back(static_cast<unsigned char>(length));
            out.push_back(static_cast<unsigned char>(length >> 8));
            out.push_back(static_cast<unsigned char>(~length));
            out.push_back(static_cast<unsigned char>(~length >> 8));
            out.insert(out.end(), data + position, data + position + length);
            position += length;
        } while (position < size);
        PutBigEndian(out, Adler32(data, size));
        return out;
    }

    // pixels: width*height RGB triplets, rows tightly packed. bottomUp: the
    // first row in memory is the bottom of the image (OpenGL glReadPixels).
    // Returns an empty vector for invalid sizes.
    inline std::vector<unsigned char> EncodeRgb(const unsigned char* pixels, int width, int height, bool bottomUp,
        Compress2Fn compress2 = nullptr, int level = 1)
    {
        if (pixels == nullptr || width <= 0 || height <= 0 || width > MaxDimension || height > MaxDimension)
        {
            return {};
        }
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 3;
        std::vector<unsigned char> raw((rowBytes + 1) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
        {
            const int sourceRow = bottomUp ? height - 1 - y : y;
            unsigned char* row = raw.data() + (rowBytes + 1) * static_cast<std::size_t>(y);
            row[0] = 0;   // filter: none
            std::memcpy(row + 1, pixels + rowBytes * static_cast<std::size_t>(sourceRow), rowBytes);
        }

        std::vector<unsigned char> zlib;
        if (compress2 != nullptr)
        {
            const std::size_t n = raw.size();
            unsigned long bound = static_cast<unsigned long>(n + (n >> 12) + (n >> 14) + (n >> 25) + 13);
            zlib.resize(bound);
            if (compress2(zlib.data(), &bound, raw.data(), static_cast<unsigned long>(n), level) == 0)
            {
                zlib.resize(bound);
            }
            else
            {
                zlib.clear();
            }
        }
        if (zlib.empty())
        {
            zlib = StoredZlib(raw.data(), raw.size());
        }

        std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<unsigned char> header;
        PutBigEndian(header, static_cast<std::uint32_t>(width));
        PutBigEndian(header, static_cast<std::uint32_t>(height));
        header.push_back(8);   // bit depth
        header.push_back(2);   // colour type: RGB
        header.push_back(0);   // compression
        header.push_back(0);   // filter method
        header.push_back(0);   // no interlace
        PutChunk(out, "IHDR", header.data(), header.size());
        PutChunk(out, "IDAT", zlib.data(), zlib.size());
        PutChunk(out, "IEND", nullptr, 0);
        return out;
    }
}
