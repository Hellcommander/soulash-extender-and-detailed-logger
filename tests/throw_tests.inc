// Included inside the anonymous namespace of tests.cpp (uses Check/failures).
// Tests for the Triggers plugin's throw support: locating the game's item
// throw (plugins/common/throw_layout.hpp), the memory helpers it uses, and
// throws.json (plugins/common/throw_rules.hpp).

// ---- synthetic image with the byte sequences of build 526b6ab4 --------------
struct ThrowImage
{
    static constexpr std::size_t Size = 0x3000;
    static constexpr std::size_t Throw = 0x1000;
    static constexpr std::size_t HasComponent = 0x2000;
    static constexpr std::size_t ActorGetter = 0x2080;
    static constexpr std::size_t PositionGetter = 0x2100;
    static constexpr std::size_t WorldMap = 0x2180;
    static constexpr std::size_t InBounds = 0x2200;
    static constexpr std::size_t Remove = 0x2280;
    static constexpr std::size_t Pop = 0x2300;
    static constexpr std::size_t BaseAttack = 0x2400;
    static constexpr std::size_t Erase = 0x2700;
    static constexpr std::size_t Destroy = 0x2780;
    static constexpr std::size_t PlayerGlobal = 0x2F00;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(Size, 0xCC);
    std::size_t frontTypeAt = 0;
    std::size_t popAt = 0;
    std::size_t actorGlobalAt = 0;
    std::size_t positionSlotAt = 0;

    std::size_t Put(std::size_t at, std::initializer_list<std::uint8_t> code)
    {
        std::copy(code.begin(), code.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        return at + code.size();
    }

    std::size_t Rel(std::size_t at, std::size_t target)
    {
        const auto value = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + 4));
        std::memcpy(bytes.data() + at, &value, sizeof(value));
        return at + 4;
    }

    ThrowImage()
    {
        std::size_t at = Throw;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8B, 0xF9});
        // hasComponent(ctx, player, 3)
        at = Put(at, {0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x8B, 0x15});
        at = Rel(at, PlayerGlobal);
        at = Put(at, {0x48, 0x8B, 0x49, 0x08, 0xE8});
        at = Rel(at, HasComponent);
        at = Put(at, {0x84, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x47, 0x08});
        // registry[3] -> Actor getter
        at = Put(at, {0x48, 0x8B, 0x88, 0x00, 0x01, 0x00, 0x00, 0x8B, 0x15});
        actorGlobalAt = at;
        at = Rel(at, PlayerGlobal);
        at = Put(at, {0x48, 0x8B, 0x49, 0x18, 0xE8});
        at = Rel(at, ActorGetter);
        at = Put(at, {0x4C, 0x8B, 0xE8, 0x48, 0x83, 0x78, 0x30, 0x00, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
        // front action, type 0x28
        at = Put(at, {0x48, 0x8B, 0x50, 0x20, 0x48, 0xFF, 0xCA, 0x48, 0x23, 0x50, 0x28, 0x48, 0x8B, 0x48, 0x18,
            0x48, 0x8B, 0x34, 0xD1, 0x83, 0x3E});
        frontTypeAt = at;
        at = Put(at, {0x28, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00});
        at = Put(at, {0x48, 0x8B, 0x86, 0x90, 0x00, 0x00, 0x00, 0x48, 0x39, 0x86, 0x88, 0x00, 0x00, 0x00, 0x0F, 0x84,
            0x00, 0x00, 0x00, 0x00});
        at = Put(at, {0x41, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x47, 0x08});
        // item Position2D (registry +0x1A8)
        at = Put(at, {0x8B, 0x56, 0x4C, 0x48, 0x8B, 0x89});
        positionSlotAt = at;
        at = Put(at, {0xA8, 0x01, 0x00, 0x00, 0xE8});
        at = Rel(at, PositionGetter);
        at = Put(at, {0x48, 0x89, 0x44, 0x24, 0x70, 0xF2, 0x0F, 0x10, 0x70, 0x18, 0x44, 0x8B, 0x60, 0x20});
        // landing point; WorldMap(); in bounds
        at = Put(at, {0x48, 0x8B, 0x8E, 0x90, 0x00, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x79, 0xF4, 0xF2, 0x0F, 0x11, 0x7C,
            0x24, 0x58, 0x8B, 0x59, 0xFC, 0x89, 0x5C, 0x24, 0x60, 0xE8});
        at = Rel(at, WorldMap);
        at = Put(at, {0x4C, 0x8B, 0xD0, 0x48, 0x8D, 0x54, 0x24, 0x78, 0x48, 0x8B, 0xC8, 0xE8});
        at = Rel(at, InBounds);
        // z-level loop, default layer, remove from the old tile
        at = Put(at, {0x33, 0xC9, 0x48, 0x8D, 0x04, 0x49, 0x41, 0x83, 0x7C, 0x82, 0x74, 0x00, 0x75, 0x0F, 0x41, 0x83,
            0x7C, 0x82, 0x78, 0x00, 0x75, 0x07, 0x45, 0x3B, 0x64, 0x82, 0x7C, 0x74, 0x74, 0x48, 0xFF, 0xC1,
            0x48, 0x83, 0xF9, 0x42, 0x72, 0xDC, 0x49, 0x8D, 0x8A, 0xA0, 0x05, 0x00, 0x00, 0x48, 0x8B, 0x09});
        at = Put(at, {0x4C, 0x8D, 0x44, 0x24, 0x50, 0x8B, 0x56, 0x4C, 0xE8});
        at = Rel(at, Remove);
        at = Put(at, {0x48, 0x83, 0xC1, 0x72, 0x49, 0x8D, 0x0C, 0xCA});
        // landing write, then the action pop
        at = Put(at, {0x4C, 0x8B, 0x7C, 0x24, 0x70, 0xF2, 0x41, 0x0F, 0x11, 0x7F, 0x18, 0x41, 0x89, 0x5F, 0x20});
        popAt = at;
        at = Put(at, {0x4D, 0x39, 0x75, 0x30, 0x74, 0x0A, 0x49, 0x8D, 0x4D, 0x10, 0xE8});
        at = Rel(at, Pop);
        Put(at, {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});

        Put(Remove, {0x53, 0x55, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8B, 0x91, 0xB0, 0x00, 0x00, 0x00});

        at = BaseAttack + 0x180;
        at = Put(at, {0x89, 0x75, 0xF8, 0x48, 0x83, 0xE9, 0x80, 0x48, 0x8D, 0x55, 0xF8, 0xE8});
        at = Rel(at, Erase);
        at = Put(at, {0x8B, 0xD6, 0x49, 0x8B, 0xCE, 0xE8});
        Rel(at, Destroy);
        Put(Destroy, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x63, 0xFA,
            0x48, 0x8B, 0xF1, 0x48, 0xB8, 0x25, 0x23, 0x22, 0x84, 0xE4, 0x9C, 0xF2, 0xCB, 0x48, 0x33, 0xE8,
            0x49, 0xBF, 0xB3, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00});
    }

    const std::uint8_t* At(std::size_t offset) const { return bytes.data() + offset; }

    soulash_plugins::ThrowLayout Locate(bool withBaseAttack = true) const
    {
        return soulash_plugins::LocateThrowLayout(bytes.data(), bytes.size(), At(Throw),
            withBaseAttack ? At(BaseAttack) : nullptr);
    }
};

void TestThrowLayout()
{
    {
        const ThrowImage image;
        const auto layout = image.Locate();
        Check(layout.verified && layout.failure[0] == '\0', "throw layout: synthetic Throw::update verifies");
        Check(layout.throwUpdate == image.At(ThrowImage::Throw), "throw layout: hooked routine is Throw::update");
        Check(reinterpret_cast<const std::uint8_t*>(layout.playerEntityId) == image.At(ThrowImage::PlayerGlobal),
            "throw layout: player id global from the Actor check");
        Check(layout.actorIndex == 3 && layout.positionIndex == 53, "throw layout: Actor = registry 3, Position2D = 53");
        Check(layout.throwActionType == 0x28, "throw layout: throw action type 0x28");
        Check(layout.worldMap == image.At(ThrowImage::WorldMap), "throw layout: world map getter");
        Check(layout.removeFromTile == image.At(ThrowImage::Remove), "throw layout: remove-from-tile routine");
        Check(layout.destroyEntity == image.At(ThrowImage::Destroy) && layout.consumeFailure[0] == '\0',
            "throw layout: DestroyEntity from BaseAttack");
    }
    {
        const ThrowImage image;
        const auto layout = image.Locate(false);
        Check(layout.verified && layout.destroyEntity == nullptr && layout.consumeFailure[0] != '\0',
            "throw layout: without BaseAttack the event works, items cannot be used up");
    }
    {
        ThrowImage image;
        image.bytes[ThrowImage::Destroy + 0x16] = 0x00;   // FNV basis changed
        const auto layout = image.Locate();
        Check(layout.verified && layout.destroyEntity == nullptr, "throw layout: a DestroyEntity without FNV hashing is rejected");
    }
    {
        ThrowImage image;
        image.bytes[image.frontTypeAt] = 0x29;
        Check(!image.Locate().verified, "throw layout: another action type is rejected");
    }
    {
        ThrowImage image;
        image.Rel(image.actorGlobalAt, ThrowImage::PlayerGlobal + 4);
        Check(!image.Locate().verified, "throw layout: the two player id reads must agree");
    }
    {
        ThrowImage image;
        image.bytes[image.positionSlotAt] = 0xA9;   // slot not a multiple of 8
        Check(!image.Locate().verified, "throw layout: a misaligned Position2D slot is rejected");
    }
    {
        ThrowImage image;
        image.bytes[image.popAt] = 0x90;
        Check(!image.Locate().verified, "throw layout: without the action pop it is rejected");
    }
    {
        ThrowImage image;
        image.bytes[ThrowImage::Remove + 12] = 0x92;
        Check(!image.Locate().verified, "throw layout: a different remove-from-tile routine is rejected");
    }
    {
        const ThrowImage image;
        Check(!soulash_plugins::LocateThrowLayout(image.bytes.data(), image.bytes.size(), nullptr, nullptr).verified,
            "throw layout: no Throw vtable, no layout");
    }
}

void TestThrowMemory()
{
    using soulash_plugins::TilePoint;
    // Actor component with one throw action at the front of its deque.
    {
        std::vector<std::uint8_t> action(0xA0, 0);
        const std::int32_t type = 0x28, item = 77;
        std::memcpy(action.data(), &type, 4);
        std::memcpy(action.data() + soulash_plugins::ActionItemOffset, &item, 4);
        std::vector<TilePoint> path = {{10, 10, 0}, {11, 12, 0}, {13, 14, -1}};
        const TilePoint* begin = path.data();
        const TilePoint* end = path.data() + path.size();
        std::memcpy(action.data() + soulash_plugins::ActionPathBeginOffset, &begin, 8);
        std::memcpy(action.data() + soulash_plugins::ActionPathEndOffset, &end, 8);
        std::vector<void*> map(8, nullptr);
        map[5] = action.data();
        std::vector<std::uint8_t> actor(soulash_plugins::ActorStride, 0);
        void** mapData = map.data();
        const std::size_t mapSize = 8, offset = 5, size = 1;
        std::memcpy(actor.data() + 0x18, &mapData, 8);
        std::memcpy(actor.data() + 0x20, &mapSize, 8);
        std::memcpy(actor.data() + 0x28, &offset, 8);
        std::memcpy(actor.data() + 0x30, &size, 8);
        const unsigned char* front = soulash_plugins::ActorFrontAction(actor.data());
        Check(front == action.data(), "throw memory: front action of the Actor deque");
        soulash_plugins::ThrowAction read;
        Check(soulash_plugins::ReadThrowAction(front, 0x28, read) && read.item == 77
                && read.landing == TilePoint{13, 14, -1},
            "throw memory: item and landing (last path point) read");
        Check(!soulash_plugins::ReadThrowAction(front, 0x27, read), "throw memory: another action type is not a throw");
        std::memcpy(action.data() + soulash_plugins::ActionPathEndOffset, &begin, 8);
        Check(!soulash_plugins::ReadThrowAction(front, 0x28, read), "throw memory: an empty path is not a throw");
        const std::size_t none = 0;
        std::memcpy(actor.data() + 0x30, &none, 8);
        Check(soulash_plugins::ActorFrontAction(actor.data()) == nullptr, "throw memory: empty deque, no front");
        Check(soulash_plugins::ActorFrontAction(nullptr) == nullptr, "throw memory: no Actor, no front");
    }
    // Position2D and the z-level layer lookup.
    {
        std::vector<std::uint8_t> position(soulash_plugins::Position2DStride, 0);
        const TilePoint at{5, 6, -2};
        std::memcpy(position.data() + soulash_plugins::Position2DXOffset, &at, 12);
        Check(soulash_plugins::ReadPosition(position.data()) == at, "throw memory: Position2D x, y, z");

        std::vector<std::uint8_t> map(0x5A8, 0);
        std::vector<std::uint8_t> surface(0x20, 0), cave(0x20, 0);
        const std::int32_t width = 100, height = 50, caveSize = 20;
        std::memcpy(surface.data() + 8, &width, 4);
        std::memcpy(surface.data() + 12, &height, 4);
        std::memcpy(cave.data() + 8, &caveSize, 4);
        std::memcpy(cave.data() + 12, &caveSize, 4);
        for (std::size_t index = 0; index < soulash_plugins::WorldMapLevelCount; ++index)
        {
            const std::int32_t unused[3] = {1, 1, 0};   // not (0, 0, z)
            std::memcpy(map.data() + soulash_plugins::WorldMapLevelsOffset + index * 12, unused, 12);
        }
        const std::int32_t level[3] = {0, 0, -2};
        std::memcpy(map.data() + soulash_plugins::WorldMapLevelsOffset + 3 * 12, level, 12);
        const unsigned char* cavePointer = cave.data();
        const unsigned char* surfacePointer = surface.data();
        std::memcpy(map.data() + soulash_plugins::WorldMapLayersOffset + 3 * 8, &cavePointer, 8);
        std::memcpy(map.data() + soulash_plugins::WorldMapDefaultLayerOffset, &surfacePointer, 8);
        bool inBounds = false;
        Check(soulash_plugins::FindLayer(map.data(), {5, 6, -2}, &inBounds) == cave.data() && inBounds,
            "throw memory: a listed z level selects its layer");
        Check(soulash_plugins::FindLayer(map.data(), {25, 6, -2}, &inBounds) == cave.data() && !inBounds,
            "throw memory: x outside that layer is out of bounds");
        Check(soulash_plugins::FindLayer(map.data(), {60, 40, 0}, &inBounds) == surface.data() && inBounds,
            "throw memory: other z levels use the default layer");
        Check(soulash_plugins::FindLayer(map.data(), {60, 50, 0}, &inBounds) == surface.data() && !inBounds,
            "throw memory: y == height is out of bounds");
    }
    // The positions vector, against a real std::vector of 12-byte points.
    {
        using Points = std::vector<std::array<std::int32_t, 3>>;
        static_assert(sizeof(Points) == sizeof(soulash_plugins::PointVector), "std::vector layout assumption");
        auto allocate = [](std::size_t bytes) { return ::operator new(bytes); };
        std::size_t released = 0;
        auto release = [&released](void* memory, std::size_t bytes) {
            released = bytes;
            ::operator delete(memory, bytes);
        };
        const TilePoint aim{7, 8, -1};
        Points empty;
        Check(soulash_plugins::SetSinglePoint(*reinterpret_cast<soulash_plugins::PointVector*>(&empty), aim, allocate, release)
                && empty.size() == 1 && empty[0] == std::array<std::int32_t, 3>{7, 8, -1} && released == 0,
            "throw memory: an empty vector gets a fresh one-point buffer");
        empty.push_back({1, 2, 3});
        Check(empty.size() == 2 && empty[0][0] == 7, "throw memory: the vector keeps working (grows from our buffer)");
        Points full = {{1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
        const std::size_t capacity = full.capacity();
        const auto* data = full.data();
        Check(soulash_plugins::SetSinglePoint(*reinterpret_cast<soulash_plugins::PointVector*>(&full), aim, allocate, release)
                && full.size() == 1 && full.capacity() == capacity && full.data() == data
                && full[0] == std::array<std::int32_t, 3>{7, 8, -1},
            "throw memory: a vector with room is reused in place");
        Points reserved;
        reserved.reserve(0);
        Check(soulash_plugins::SetSinglePoint(*reinterpret_cast<soulash_plugins::PointVector*>(&reserved), aim, allocate, release)
                && reserved.size() == 1,
            "throw memory: a vector without a buffer gets one");
    }
    // Front of the queued-ability deque, against a real std::deque<void*>.
    {
        std::vector<int> cells(64);
        std::deque<void*> d;
        Check(soulash_plugins::DequeFront(*reinterpret_cast<const soulash_plugins::DequeOfPointers*>(&d)) == nullptr,
            "throw memory: empty deque has no front");
        bool agree = true;
        for (int i = 0; i < 40; ++i)
        {
            d.push_back(&cells[i]);
            if (i % 3 == 2) d.pop_front();
            agree = agree && soulash_plugins::DequeFront(*reinterpret_cast<const soulash_plugins::DequeOfPointers*>(&d)) == d.front();
        }
        Check(agree, "throw memory: deque front agrees with std::deque through pushes and pops");
    }
}

void TestThrowRules()
{
    using soulash_plugins::throw_rules::RuleSet;
    std::string error;
    {
        RuleSet rules;
        const bool ok = rules.Load(R"({
            "note": "runes",
            "throws": [
                {"id": "fire", "item": "m_Rune_of_Fire", "cast": "m_Fire_Burst"},
                {"item": ["m_Runeword_Flame", "m_Runeword_Flame_B"], "cast": ["m_Flame_A", "m_Flame_B"],
                 "chance": 0.5, "consume": false, "note": "two parts"}
            ]})", error);
        Check(ok, "throw rules: a valid file loads");
        const auto* fire = rules.Find("m_Rune_of_Fire");
        Check(fire != nullptr && fire->id == "fire" && fire->casts == std::vector<std::string>{"m_Fire_Burst"}
                && fire->consume && fire->chance == 1.0,
            "throw rules: string forms and defaults (consume, chance 1)");
        const auto* flame = rules.Find("m_Runeword_Flame_B");
        Check(flame != nullptr && flame->id == "m_Runeword_Flame" && flame->casts.size() == 2 && !flame->consume
                && flame->chance == 0.5,
            "throw rules: array forms, id defaults to the first item");
        Check(rules.Find("m_Unknown") == nullptr && rules.Rules().size() == 2, "throw rules: unknown items have no rule");
    }
    const char* bad[][2] = {
        {R"([])", "not an object"},
        {R"({"throw": []})", "unknown top-level key"},
        {R"({"throws": {}})", "throws not an array"},
        {R"({"throws": [{"item": "a"}]})", "cast missing"},
        {R"({"throws": [{"cast": "a"}]})", "item missing"},
        {R"({"throws": [{"item": "a", "cast": "b", "when": {}}]})", "unknown rule key"},
        {R"({"throws": [{"item": "a", "cast": "b", "chance": 1.5}]})", "chance above 1"},
        {R"({"throws": [{"item": "a", "cast": "b", "consume": 1}]})", "consume not a bool"},
        {R"({"throws": [{"item": "", "cast": "b"}]})", "empty item id"},
        {R"({"throws": [{"item": "a", "cast": []}]})", "empty cast list"},
        {R"({"throws": [{"item": "a", "cast": ["1","2","3","4","5","6","7","8","9"]}]})", "more than 8 casts"},
        {R"({"throws": [{"item": "a", "cast": "b"}, {"item": ["c", "a"], "cast": "d"}]})", "duplicate item"},
        {R"({"throws": [{"item": "a", "cast": "b",}]})", "trailing comma"},
    };
    for (const auto& entry : bad)
    {
        RuleSet rules;
        error.clear();
        Check(!rules.Load(entry[0], error) && !error.empty() && rules.Find("a") == nullptr,
            (std::string("throw rules: rejected - ") + entry[1]).c_str());
    }
}
