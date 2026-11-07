// push_back on the game's std::deque<Ability*> (the queued-ability list at
// AbilityUser+0x230), written exactly as the engine's own on-cast trigger
// routine does it inline (build 526b6ab4, 0x414032-0x4140D3), with the game's
// allocator and the game's _Growmap so the structure stays the engine's.
//
// MSVC std::deque<T*> layout (unchanged since VS2015):
//   +0x00 _Container_proxy*   +0x08 T** _Map (block pointers)   +0x10 size_t _Mapsize
//   +0x18 size_t _Myoff       +0x20 size_t _Mysize
// Block size for 8-byte elements is 2, so a block is 16 bytes.
//
// Header-only and allocator-agnostic so the unit test can drive it against a
// real std::deque<void*> with the CRT's allocator and _Growmap substitute.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace soulash_plugins
{
    struct DequeOfPointers
    {
        void* proxy;
        void*** map;   // blocks of DequeBlockElements pointers
        std::size_t mapSize;
        std::size_t offset;
        std::size_t size;
    };

    constexpr std::size_t DequeBlockElements = 2;
    constexpr std::size_t DequeBlockBytes = DequeBlockElements * sizeof(void*);

    // allocate(bytes) -> block memory; growMap(deque) -> grows the map by at
    // least one block (the engine's _Growmap(1)). Returns false without touching
    // the deque when an allocation fails.
    template <class Allocate, class GrowMap>
    bool DequePushBack(DequeOfPointers& deque, void* value, Allocate allocate, GrowMap growMap)
    {
        // if ((_Myoff + _Mysize) % block == 0 && _Mapsize <= (_Mysize + block) / block) _Growmap(1)
        if (((deque.offset + deque.size) & (DequeBlockElements - 1)) == 0
            && deque.mapSize <= (deque.size + DequeBlockElements) / DequeBlockElements)
        {
            growMap(deque);
            if (deque.map == nullptr || deque.mapSize == 0)
            {
                return false;
            }
        }
        deque.offset &= deque.mapSize * DequeBlockElements - 1;   // wrap into the (grown) ring
        const std::size_t newOffset = deque.offset + deque.size;
        const std::size_t block = (newOffset / DequeBlockElements) & (deque.mapSize - 1);
        if (deque.map[block] == nullptr)
        {
            void* memory = allocate(DequeBlockBytes);
            if (memory == nullptr)
            {
                return false;
            }
            deque.map[block] = static_cast<void**>(memory);
        }
        deque.map[block][newOffset % DequeBlockElements] = value;
        ++deque.size;
        return true;
    }
}
